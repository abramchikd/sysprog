#include "chat.h"
#include "chat_server.h"

#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <errno.h>
#include <stdbool.h>

struct chat_peer {
	int socket;
	char *name;

	struct chat_message messages;
	struct chat_message *last_message;

	struct chat_peer *next;

	struct chat_message *pending_message;
};

struct chat_server {
	int socket;
	int epoll;
	struct chat_peer peers;
	struct chat_peer *last_peer;

	struct chat_message messages;
	struct chat_message *last_message;

	struct chat_message pending_message;
};

static ssize_t
parse_data(struct chat_message *message, const char *buffer, ssize_t size);

static ssize_t
find_first_char(const char *haystack, ssize_t size, char needle);

static int
accept_clients(struct chat_server *server);

static int
receive_from_client(struct chat_server *server, struct chat_peer *peer);

static int
send_to_client(struct chat_server *server, struct chat_peer *peer);

static void
append_data(struct chat_message *message, const char *data, ssize_t size);

struct chat_server *
chat_server_new(void)
{
	struct chat_server *server = calloc(1, sizeof(*server));
	server->socket = -1;
	server->last_peer = &server->peers;
	server->last_message = &server->messages;
	server->peers.last_message = &server->peers.messages;

	return server;
}

void
chat_server_delete(struct chat_server *server)
{
	if (server->socket == -1) {
		free(server);
		return;
	}

	close(server->socket);
	close(server->epoll);

	while (server->peers.next != NULL) {
		struct chat_peer *peer = server->peers.next;
		server->peers.next = peer->next;

		close(peer->socket);
		free(peer->name);
		while (peer->messages.next != NULL) {
			struct chat_message *message = peer->messages.next;
			peer->messages.next = message->next;

			chat_message_delete(message);
		}

		free(peer->pending_message);
		free(peer);
	}

	while (server->peers.messages.next != NULL) {
		struct chat_message *message = server->peers.messages.next;
		server->peers.messages.next = message->next;

		chat_message_delete(message);
	}

	free(server);
}

int
chat_server_listen(struct chat_server *server, uint16_t port)
{
	if (server->socket != -1) {
		return CHAT_ERR_ALREADY_STARTED;
	}

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_port = htons(port);
	/* Listen on all IPs of this machine. */
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	server->socket = socket(AF_INET, SOCK_STREAM, 0);
	if (server->socket == -1) {
		return CHAT_ERR_SYS;
	}

	int one = 1;
	if (setsockopt(server->socket, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(int)) < 0) {
		close(server->socket);
		return CHAT_ERR_SYS;
	}

	if (bind(server->socket, (struct sockaddr *)(&addr), sizeof(addr)) == -1) {
		close(server->socket);
		return CHAT_ERR_PORT_BUSY;
	}

	if (listen(server->socket, 1000) == -1) {
		close(server->socket);
		return CHAT_ERR_SYS;
	}

	int flags = fcntl(server->socket, F_GETFL);
	if (fcntl(server->socket, F_SETFL, flags | O_NONBLOCK) != 0) {
		close(server->socket);
		return CHAT_ERR_SYS;
	}

	server->epoll = epoll_create1(0);
	if (server->epoll == -1) {
		close(server->socket);
		return CHAT_ERR_SYS;
	}

	struct epoll_event sock_event;
	sock_event.data.ptr = NULL;
	sock_event.events = EPOLLIN | EPOLLET;

	if (epoll_ctl(server->epoll, EPOLL_CTL_ADD, server->socket, &sock_event) != 0) {
		close(server->socket);
		close(server->epoll);

		return CHAT_ERR_SYS;
	}

	return 0;
}

struct chat_message *
chat_server_pop_next(struct chat_server *server)
{
	if (server->messages.next == NULL) {
		return NULL;
	}

	struct chat_message *message = server->messages.next;
	server->messages.next = server->messages.next->next;
	if (server->last_message == message) {
		server->last_message = &server->messages;
	}

	return message;
}

int
chat_server_update(struct chat_server *server, double timeout)
{
	if (server->socket == -1) {
		return CHAT_ERR_NOT_STARTED;
	}

	struct epoll_event events[1000];
	int events_count = epoll_wait(server->epoll, events, 1000, (int) (timeout * 1000));
	if (events_count == 0) {
		return CHAT_ERR_TIMEOUT;
	}

	if (events_count == -1) {
		return CHAT_ERR_SYS;
	}

	for (int i = 0; i < events_count; i++) {
		if (events[i].data.ptr == NULL) {
			int res = accept_clients(server);
			if (res != 0) {
				return res;
			}
			continue;
		}

		if (events[i].events & EPOLLIN) {
			int res = receive_from_client(server, (struct chat_peer *) events[i].data.ptr);
			if (res != 0) {
				return res;
			}
		}

		if (events[i].events & EPOLLOUT) {
			int res = send_to_client(server, (struct chat_peer *) events[i].data.ptr);
			if (res != 0) {
				return res;
			}
		}
	}

	return 0;
}

static int
accept_clients(struct chat_server *server)
{
	while (true) {
		int client_sock = accept(server->socket, NULL, NULL);
		if (client_sock == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return 0;
			}

			return CHAT_ERR_SYS;
		}

		int flags = fcntl(client_sock, F_GETFL);
		if (fcntl(client_sock, F_SETFL, flags | O_NONBLOCK) != 0) {
			close(client_sock);
		}

		struct chat_peer *peer = calloc(1, sizeof(*peer));
		peer->socket = client_sock;
		peer->last_message = &peer->messages;

		struct epoll_event client_event;
		client_event.data.ptr = peer;
		client_event.events = EPOLLIN | EPOLLET;
		if (server->peers.messages.next != NULL) {
			client_event.events |= EPOLLOUT;
		}

		if (epoll_ctl(server->epoll, EPOLL_CTL_ADD, client_sock, &client_event) != 0) {
			close(client_sock);
			continue;
		}

		server->last_peer->next = peer;
		server->last_peer = peer;

		struct chat_message *iter = server->peers.messages.next;
		while (iter != NULL) {
			peer->last_message->next = calloc(1, sizeof(*peer->last_message->next));
			peer->last_message->next->data = malloc(iter->data_size);
			memcpy(peer->last_message->next->data, iter->data, iter->data_size);
			peer->last_message->next->data_size = iter->data_size;
			peer->last_message->next->status = MESSAGE_STATUS_READY;
			peer->last_message = peer->last_message->next;

			iter = iter->next;
		}
	}
}

static int
receive_from_client(struct chat_server *server, struct chat_peer *peer)
{
	struct chat_message *message = peer->pending_message;
	struct chat_message *last_message = peer->pending_message;

	while (true) {
		char buffer[1024];
		ssize_t size = read(peer->socket, buffer, 1024);
		if (size == 0) {
			struct chat_peer *p = peer;
			struct  chat_peer *iter = &server->peers;
			while (iter->next != peer) {
				iter = iter->next;
			}

			iter->next = peer->next;
			if (iter->next == NULL) {
				server->last_peer = iter;
			}

			free(p->name);
			while (p->messages.next != NULL) {
				struct chat_message *message = p->messages.next;
				p->messages.next = message->next;

				chat_message_delete(message);
			}

			while (message != NULL) {
				struct chat_message *m = message;
				message = message->next;
				free(m->data);
				free(m);
			}

			if (epoll_ctl(server->epoll, EPOLL_CTL_DEL, peer->socket, NULL) != 0) {
				close(p->socket);
				free(p->pending_message);
				free(p);

				return CHAT_ERR_SYS;
			}

			close(p->socket);
			free(p->pending_message);
			free(p);

			return 0;
		}

		if (size == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
			break;
		}

		if (size == -1) {
			while (message != NULL) {
				struct chat_message *m = message;
				message = message->next;
				free(m->data);
				free(m);
			}

			return CHAT_ERR_SYS;
		}

		if (message == NULL) {
			message = calloc(1, sizeof(*message));
			message->status = MESSAGE_STATUS_READING_DATA;
			last_message = message;
		}

		ssize_t len = 0;
		while (len < size) {
			len += parse_data(last_message, buffer + len, size - len);
			if (last_message->next != NULL) {
				last_message = last_message->next;
			}
		}
	}

	if (message == NULL) {
		return 0;
	}

	if (peer->name == NULL && message->status != MESSAGE_STATUS_READING_DATA) {
		peer->name = message->data;
		peer->name[message->data_size - 1] = '\0';
		struct chat_message *tmp = message;
		message = message->next;
		free(tmp);
	}

	struct chat_message *iter = message;
	while (iter != NULL && iter->status != MESSAGE_STATUS_READING_DATA) {
		server->last_message->next = calloc(1, sizeof(*server->last_message->next));
		server->last_message->next->data = malloc(iter->data_size);
		memcpy(server->last_message->next->data, iter->data, iter->data_size);
		server->last_message->next->data[iter->data_size - 1] = '\0';
		server->last_message->next->author = peer->name;
		server->last_message->next->status = MESSAGE_STATUS_READY;
		server->last_message = server->last_message->next;

		struct chat_peer *server_peer = server->peers.next;
		while (server_peer != NULL) {
			if (server_peer == peer) {
				server_peer = server_peer->next;
				continue;
			}

			if (server_peer->messages.next == NULL) {
				if (epoll_ctl(server->epoll, EPOLL_CTL_DEL, server_peer->socket, NULL) != 0) {
					while (message != NULL) {
						struct chat_message *m = message;
						message = message->next;
						free(m->data);
						free(m);
					}

					return CHAT_ERR_SYS;
				}

				struct epoll_event client_event;
				client_event.data.ptr = server_peer;
				client_event.events = EPOLLIN | EPOLLOUT | EPOLLET;

				if (epoll_ctl(server->epoll, EPOLL_CTL_ADD, server_peer->socket, &client_event) != 0) {
					while (message != NULL) {
						struct chat_message *m = message;
						message = message->next;
						free(m->data);
						free(m);
					}

					return CHAT_ERR_SYS;
				}
			}

			server_peer->last_message->next = calloc(1, sizeof(*server_peer->last_message->next));
			server_peer->last_message->next->data = malloc(strlen(peer->name) + 1);
			memcpy(server_peer->last_message->next->data, peer->name, strlen(peer->name));
			server_peer->last_message->next->data[strlen(peer->name)] = ':';
			server_peer->last_message->next->data_size = strlen(peer->name) + 1;
			server_peer->last_message->next->status = MESSAGE_STATUS_READING_AUTHOR;
			server_peer->last_message = server_peer->last_message->next;

			server_peer->last_message->next = calloc(1, sizeof(*server_peer->last_message->next));
			server_peer->last_message->next->data = malloc(iter->data_size);
			memcpy(server_peer->last_message->next->data, iter->data, iter->data_size);
			server_peer->last_message->next->data_size = iter->data_size;
			server_peer->last_message->next->status = MESSAGE_STATUS_READY;
			server_peer->last_message = server_peer->last_message->next;

			server_peer = server_peer->next;
		}

		iter = iter->next;
	}

	peer->pending_message = iter;

	while (message != NULL && message->status != MESSAGE_STATUS_READING_DATA) {
		struct chat_message *m = message;
		message = message->next;
		free(m->data);
		free(m);
	}

	return 0;
}

static ssize_t
parse_data(struct chat_message *message, const char *buffer, ssize_t size)
{
	if (message->status == MESSAGE_STATUS_READY) {
		message->next = calloc(1, sizeof(*message->next));
		message->next->status = MESSAGE_STATUS_READING_DATA;
		message = message->next;
	}

	ssize_t new_line_index = find_first_char(buffer, size, '\n');
	if (new_line_index == size) {
		append_data(message, buffer, size);
		return size;
	}

	append_data(message, buffer, new_line_index);
	append_data(message, "\n", 1);
	message->status = MESSAGE_STATUS_READY;

	return new_line_index + 1;
}

static ssize_t
find_first_char(const char *haystack, ssize_t size, char needle)
{
	ssize_t index = 0;
	while (index < size && haystack[index] != needle) {
		index++;
	}

	return index;
}

static int
send_to_client(struct chat_server *server, struct chat_peer *peer)
{
	while (peer->messages.next != NULL) {
		ssize_t len = write(peer->socket, peer->messages.next->data + peer->messages.next->offset, peer->messages.next->data_size - peer->messages.next->offset);
		if (len == peer->messages.next->data_size - peer->messages.next->offset) {
			free(peer->messages.next->data);

			struct chat_message *m = peer->messages.next;
			peer->messages.next = peer->messages.next->next;
			if (peer->messages.next == NULL) {
				peer->last_message = &peer->messages;
			}
			free(m);
			continue;
		}

		if (len == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
			return 0;
		}

		if (len == -1) {
			return CHAT_ERR_SYS;
		}

		peer->messages.next->offset += len;
		return 0;
	}

	if (epoll_ctl(server->epoll, EPOLL_CTL_DEL, peer->socket, NULL) != 0) {
		return CHAT_ERR_SYS;
	}

	struct epoll_event client_event;
	client_event.data.ptr = peer;
	client_event.events = EPOLLIN | EPOLLET;

	if (epoll_ctl(server->epoll, EPOLL_CTL_ADD, peer->socket, &client_event) != 0) {
		return CHAT_ERR_SYS;
	}

	return 0;
}

static void
append_data(struct chat_message *message, const char *data, ssize_t size)
{
	message->data = realloc(message->data, message->data_size + size);
	memcpy(message->data + message->data_size, data, size);
	message->data_size += size;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
	return server->epoll;
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
	if (server->socket == -1) {
		return 0;
	}

	int events = CHAT_EVENT_INPUT;
	struct chat_peer *peer = server->peers.next;
	while (peer != NULL) {
		if (peer->messages.next != NULL) {
			events |= CHAT_EVENT_OUTPUT;
			break;
		}

		peer = peer->next;
	}

	return events;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
	if (server->socket == -1) {
		return CHAT_ERR_NOT_STARTED;
	}

	ssize_t len = 0;
	while (msg_size > len) {
		ssize_t new_line_index = find_first_char(msg + len, msg_size - len, '\n');
		if (new_line_index == msg_size - len) {
			append_data(&server->pending_message, msg + len, msg_size - len);
			len += msg_size;
			continue;
		}

		append_data(&server->pending_message, msg + len, new_line_index + 1);
		len += new_line_index + 1;

		struct chat_peer *server_peer = &server->peers;
		while (server_peer != NULL) {
			if (server_peer->messages.next == NULL && server_peer != &server->peers) {
				if (epoll_ctl(server->epoll, EPOLL_CTL_DEL, server_peer->socket, NULL) != 0) {
					return CHAT_ERR_SYS;
				}

				struct epoll_event client_event;
				client_event.data.ptr = server_peer;
				client_event.events = EPOLLIN | EPOLLOUT | EPOLLET;

				if (epoll_ctl(server->epoll, EPOLL_CTL_ADD, server_peer->socket, &client_event) != 0) {
					return CHAT_ERR_SYS;
				}
			}

			server_peer->last_message->next = calloc(1, sizeof(*server_peer->last_message->next));
			server_peer->last_message->next->data = malloc(strlen("server") + 1);
			memcpy(server_peer->last_message->next->data, "server", strlen("server"));
			server_peer->last_message->next->data[strlen("server")] = ':';
			server_peer->last_message->next->data_size = strlen("server") + 1;
			server_peer->last_message->next->status = MESSAGE_STATUS_READING_AUTHOR;
			server_peer->last_message = server_peer->last_message->next;

			server_peer->last_message->next = calloc(1, sizeof(*server_peer->last_message->next));
			server_peer->last_message->next->data = malloc(server->pending_message.data_size);
			memcpy(server_peer->last_message->next->data, server->pending_message.data, server->pending_message.data_size);
			server_peer->last_message->next->data_size = server->pending_message.data_size;
			server_peer->last_message->next->status = MESSAGE_STATUS_READY;
			server_peer->last_message = server_peer->last_message->next;

			server_peer = server_peer->next;
		}

		free(server->pending_message.data);
		server->pending_message.data = NULL;
		server->pending_message.data_size = 0;
	}

	return 0;
}
