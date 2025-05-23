#pragma once

/**
 * Here you should specify which features do you want to implement via macros:
 * If you want to enable author name support, do:
 *
 *     #define NEED_AUTHOR 1
 *
 * To enable server-feed from admin do:
 *
 *     #define NEED_SERVER_FEED 1
 *
 * It is important to define these macros here, in the header, because it is
 * used by tests.
 */
#include <stddef.h>
#include <unistd.h>

#define NEED_AUTHOR 1
#define NEED_SERVER_FEED 1

enum chat_errcode {
	CHAT_ERR_INVALID_ARGUMENT = 1,
	CHAT_ERR_TIMEOUT,
	CHAT_ERR_PORT_BUSY,
	CHAT_ERR_NO_ADDR,
	CHAT_ERR_ALREADY_STARTED,
	CHAT_ERR_NOT_IMPLEMENTED,
	CHAT_ERR_NOT_STARTED,
	CHAT_ERR_SYS,
};

enum chat_events {
	CHAT_EVENT_INPUT = 1,
	CHAT_EVENT_OUTPUT = 2,
};

enum message_status {
	MESSAGE_STATUS_READING_AUTHOR,
	MESSAGE_STATUS_READING_DATA,
	MESSAGE_STATUS_READY
};

struct chat_message {
#if NEED_AUTHOR
	/** Author's name. */
	const char *author;
#endif
	/** 0-terminate text. */
	char *data;
	ssize_t data_size;
	ssize_t offset;

	struct chat_message *next;
	enum message_status status;
};

/** Free message's memory. */
void
chat_message_delete(struct chat_message *msg);

/** Convert chat_events mask to events suitable for poll(). */
int
chat_events_to_poll_events(int mask);
