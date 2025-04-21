#include "chat.h"
#include "chat_client.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/poll.h>
#include <errno.h>
#include <stdbool.h>

struct buffer {
	struct buffer *next;
	ssize_t size;
	char data[0];
};

struct chat_client {
	int socket;
	char **authors;
	ssize_t authors_size;
	ssize_t authors_capacity;

	struct buffer buffers;
	struct buffer *last_buffer;
	ssize_t buffer_offset;

	struct chat_message messages;
	struct chat_message *last_message;
};

static int
send_to_server(struct chat_client *client);

static int
receive_from_server(struct chat_client *client);

static ssize_t
parse_data(struct chat_client *client, const char *buffer, ssize_t size);

static ssize_t
find_first_char(const char *haystack, ssize_t size, char needle);

static void
append_data(struct chat_message *message, const char *data, ssize_t size);

static const char *
get_author(char *author, ssize_t author_size, struct chat_client *client);

struct chat_client *
chat_client_new(const char *name)
{
	struct chat_client *client = calloc(1, sizeof(*client));
	client->socket = -1;
	client->messages.status = MESSAGE_STATUS_READY;
	client->last_message = &client->messages;

	size_t name_length = strlen(name) + 1;
	struct buffer *buffer = malloc(sizeof(*buffer) + name_length);
	buffer->next = NULL;
	buffer->size = name_length;
	strcpy(buffer->data, name);
	buffer->data[name_length - 1] = '\n';

	client->buffers.next = buffer;
	client->last_buffer = buffer;
	client->buffer_offset = 0;

	return client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (client->socket != -1) {
		close(client->socket);
	}

	while (client->buffers.next != NULL) {
		struct buffer *buffer = client->buffers.next;
		client->buffers.next = buffer->next;

		free(buffer);
	}

	while (client->messages.next != NULL) {
		struct chat_message *message = client->messages.next;
		client->messages.next = message->next;

		chat_message_delete(message);
	}

	for (ssize_t i = 0; i < client->authors_size; i++) {
		free(client->authors[i]);
	}

	free(client->authors);
	free(client);
}

int
chat_client_connect(struct chat_client *client, const char *addr)
{
	if (client->socket != -1) {
		return CHAT_ERR_ALREADY_STARTED;
	}

	client->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (client->socket == -1) {
		return CHAT_ERR_SYS;
	}

	ssize_t colon_index = 0;
	while (addr[colon_index] != ':') {
		colon_index++;
	}

	char *host = malloc(colon_index + 1);
	memcpy(host, addr, colon_index);
	host[colon_index] = '\0';

	struct addrinfo *address;
	if (getaddrinfo(host, addr + colon_index + 1, NULL, &address) != 0) {
		close(client->socket);
		free(host);
		return CHAT_ERR_NO_ADDR;
	}

	free(host);

	struct sockaddr *in_addr = NULL;
	for (struct addrinfo *iter = address; iter != NULL; iter = iter->ai_next) {
		if (iter->ai_family == AF_INET) {
			in_addr = iter->ai_addr;
			break;
		}
	}

	if (in_addr == NULL) {
		close(client->socket);
		freeaddrinfo(address);
		return CHAT_ERR_NO_ADDR;
	}

	if (connect(client->socket, in_addr, sizeof(*in_addr)) != 0) {
		close(client->socket);
		freeaddrinfo(address);
		return CHAT_ERR_SYS;
	}

	freeaddrinfo(address);

	int flags = fcntl(client->socket, F_GETFL);
	if (fcntl(client->socket, F_SETFL, flags | O_NONBLOCK) != 0) {
		close(client->socket);
		return CHAT_ERR_SYS;
	}

	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	if (client->messages.next == NULL || client->messages.next->status != MESSAGE_STATUS_READY) {
		return NULL;
	}

	struct chat_message *message = client->messages.next;
	client->messages.next = client->messages.next->next;
	if (client->last_message == message) {
		client->last_message = &client->messages;
	}

	return message;
}

int
chat_client_update(struct chat_client *client, double timeout)
{
	if (client->socket == -1) {
		return CHAT_ERR_NOT_STARTED;
	}

	struct pollfd fd;
	fd.fd = client->socket;
	fd.events = POLLIN;
	if (client->buffers.next != NULL) {
		fd.events = POLLOUT;
	}

	int poll_result = poll(&fd, 1, (int) (timeout * 1000));
	if (poll_result == -1) {
		return CHAT_ERR_SYS;
	}

	if (poll_result == 0) {
		return CHAT_ERR_TIMEOUT;
	}

	if (fd.revents & POLLOUT) {
		int res = send_to_server(client);
		if (res != 0) {
			return res;
		}
	}

	if (fd.revents & POLLIN) {
		int res = receive_from_server(client);
		if (res != 0) {
			return res;
		}
	}

	return 0;
}

static int
send_to_server(struct chat_client *client)
{
	while (true) {
		struct buffer *buffer = client->buffers.next;
		if (buffer == NULL) {
			return 0;
		}

		ssize_t size = write(client->socket, buffer->data + client->buffer_offset, buffer->size - client->buffer_offset);
		if (size == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
			return 0;
		}

		if (size == -1) {
			return CHAT_ERR_SYS;
		}

		if (client->buffer_offset + size < buffer->size) {
			client->buffer_offset += size;
			return 0;
		}

		client->buffer_offset = 0;
		client->buffers.next = buffer->next;
		if (client->last_buffer == buffer) {
			client->last_buffer = &client->buffers;
		}
		free(buffer);
		return 0;
	}
}

static int
receive_from_server(struct chat_client *client)
{
	while (true) {
		char buffer[1024];
		ssize_t size = read(client->socket, buffer, 1024);
		if (size == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
			return 0;
		}

		if (size == -1) {
			return CHAT_ERR_SYS;
		}

		ssize_t len = 0;
		while (len < size) {
			len += parse_data(client, buffer + len, size - len);
		}
	}
}

static ssize_t
parse_data(struct chat_client *client, const char *buffer, ssize_t size)
{
	if (client->last_message->status == MESSAGE_STATUS_READY) {
		client->last_message->next = calloc(1, sizeof(*client->messages.next));
		client->last_message->next->status = MESSAGE_STATUS_READING_AUTHOR;
		client->last_message = client->last_message->next;
	}

	if (client->last_message->status == MESSAGE_STATUS_READING_AUTHOR) {
		ssize_t colon_index = find_first_char(buffer, size, ':');
		if (colon_index == size) {
			append_data(client->last_message, buffer, size);
			return size;
		}

		append_data(client->last_message, buffer, colon_index);
		client->last_message->status = MESSAGE_STATUS_READING_DATA;
		client->last_message->author = get_author(client->last_message->data, client->last_message->data_size, client);
		free(client->last_message->data);
		client->last_message->data = NULL;
		client->last_message->data_size = 0;

		return colon_index + 1;
	}

	ssize_t new_line_index = find_first_char(buffer, size, '\n');
	if (new_line_index == size) {
		append_data(client->last_message, buffer, size);
		return size;
	}

	append_data(client->last_message, buffer, new_line_index);
	append_data(client->last_message, "\0", 1);
	client->last_message->status = MESSAGE_STATUS_READY;

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

static void
append_data(struct chat_message *message, const char *data, ssize_t size)
{
	message->data = realloc(message->data, message->data_size + size);
	memcpy(message->data + message->data_size, data, size);
	message->data_size += size;
}

static const char *
get_author(char *author, ssize_t author_size, struct chat_client *client)
{
	for (ssize_t i = 0; i < client->authors_size; i++) {
		if ((ssize_t) strlen(client->authors[i]) != author_size) {
			continue;
		}

		if (strncmp(client->authors[i], author, author_size) == 0) {
			return client->authors[i];
		}
	}

	if (client->authors_size == client->authors_capacity) {
		client->authors_capacity = client->authors_capacity * 2 + 1;
		client->authors = realloc(client->authors, client->authors_capacity * sizeof(*client->authors));
	}

	client->authors[client->authors_size] = malloc(author_size + 1);
	memcpy(client->authors[client->authors_size], author, author_size);
	client->authors[client->authors_size][author_size] = '\0';

	return client->authors[client->authors_size++];
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
	if (client->socket == -1) {
		return 0;
	}

	int events = CHAT_EVENT_INPUT;
	if (client->buffers.next != NULL) {
		events |= CHAT_EVENT_OUTPUT;
	}

	return events;
}

int
chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
	if (client->socket == -1) {
		return CHAT_ERR_NOT_STARTED;
	}

	struct buffer *buffer = malloc(sizeof(*buffer) + msg_size);
	buffer->size = msg_size;
	memcpy(buffer->data, msg, msg_size);
	buffer->next = NULL;

	client->last_buffer->next = buffer;
	client->last_buffer = buffer;

	return 0;
}
