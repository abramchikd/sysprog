#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector
{
	unsigned *data;
	size_t size;
	size_t capacity;
};

#if 1 /* Uncomment this if want to use */

/** Append @a count messages in @a data to the end of the vector. */
static void
data_vector_append_many(struct data_vector *vector,
                        const unsigned *data, size_t count)
{
	if (vector->size + count > vector->capacity) {
		if (vector->capacity == 0)
			vector->capacity = 4;
		else
			vector->capacity *= 2;
		if (vector->capacity < vector->size + count)
			vector->capacity = vector->size + count;
		vector->data = realloc(vector->data,
		                       sizeof(vector->data[0]) * vector->capacity);
	}
	memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
	vector->size += count;
}

/** Append a single message to the vector. */
static void
data_vector_append(struct data_vector *vector, unsigned data)
{
	data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void
data_vector_pop_first_many(struct data_vector *vector, unsigned *data, size_t count)
{
	assert(count <= vector->size);
	memcpy(data, vector->data, sizeof(data[0]) * count);
	vector->size -= count;
	memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
}

#endif

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry
{
	struct rlist base;
	struct coro *coro;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue
{
	struct rlist coros;
};

#if 1 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
	                                               struct wakeup_entry, base);
	coro_wakeup(entry->coro);
}

static void
wakeup_queue_wakeup_all(struct wakeup_queue *queue)
{
	struct wakeup_entry *entry;
	rlist_foreach_entry(entry, &queue->coros, base) {
		coro_wakeup(entry->coro);
	}
}

#endif

struct coro_bus_channel
{
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	struct data_vector data;
};

struct coro_bus
{
	struct coro_bus_channel **channels;
	int channel_count;
	int capacity;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = malloc(sizeof(struct coro_bus));
	bus->channel_count = 0;
	bus->capacity = 0;
	bus->channels = NULL;

	return bus;
}

void
coro_bus_delete(struct coro_bus *bus)
{
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] == NULL) {
			continue;
		}

		coro_bus_channel_close(bus, i);
	}

	free(bus->channels);
	free(bus);
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
	struct coro_bus_channel *channel = malloc(sizeof(struct coro_bus_channel));
	channel->size_limit = size_limit;
	channel->data.size = 0;
	channel->data.capacity = 0;
	channel->data.data = NULL;
	rlist_create(&channel->recv_queue.coros);
	rlist_create(&channel->send_queue.coros);

	int free_index = -1;
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] == NULL) {
			free_index = i;
			break;
		}
	}

	if (free_index == -1) {
		if (bus->channel_count == bus->capacity) {
			bus->capacity = (bus->capacity + 1) * 2;
			bus->channels = realloc(bus->channels, bus->capacity * sizeof(struct coro_bus_channel *));
		}

		free_index = bus->channel_count;
		bus->channel_count++;
	}

	bus->channels[free_index] = channel;

	return free_index;
}

static bool
coro_bus_channel_exists(const struct coro_bus *bus, int channel)
{
	if (channel >= bus->channel_count) {
		return false;
	}

	return bus->channels[channel] != NULL;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
	if (!coro_bus_channel_exists(bus, channel)) {
		return;
	}

    struct coro_bus_channel *removed_channel = bus->channels[channel];
    bus->channels[channel] = NULL;

	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
	wakeup_queue_wakeup_all(&removed_channel->recv_queue);
	wakeup_queue_wakeup_all(&removed_channel->send_queue);
    coro_yield();

    free(removed_channel->data.data);
	free(removed_channel);
}

int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	int res = coro_bus_send_v(bus, channel, &data, 1);
	return res > 0 ? 0 : res;
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	int res = coro_bus_try_send_v(bus, channel, &data, 1);
	return res > 0 ? 0 : res;
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	int res = coro_bus_recv_v(bus, channel, data, 1);
	return res > 0 ? 0 : res;
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	int res = coro_bus_try_recv_v(bus, channel, data, 1);
	return res > 0 ? 0 : res;
}


#if NEED_BROADCAST

static bool
coro_bus_channel_any_exist(const struct coro_bus *bus)
{
	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] != NULL) {
			return true;
		}
	}

	return false;
}

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	for (;;) {
		if (coro_bus_try_broadcast(bus, data) == 0) {
			break;
		}

		if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			coro_bus_errno_set(CORO_BUS_ERR_NONE);
			for (int i = 0; i < bus->channel_count; i++) {
				if (bus->channels[i] == NULL) {
					continue;
				}

				if (bus->channels[i]->data.size == bus->channels[i]->size_limit) {
					wakeup_queue_suspend_this(&bus->channels[i]->send_queue);
					break;
				}
			}

			continue;
		}

		return -1;
	}

	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] == NULL) {
			continue;
		}

		if (bus->channels[i]->data.size < bus->channels[i]->size_limit) {
			wakeup_queue_wakeup_first(&bus->channels[i]->send_queue);
		}
	}

	return 0;
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	if (!coro_bus_channel_any_exist(bus)) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] == NULL) {
			continue;
		}

		if (bus->channels[i]->data.size == bus->channels[i]->size_limit) {
			coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
			return -1;
		}
	}

	for (int i = 0; i < bus->channel_count; i++) {
		if (bus->channels[i] == NULL) {
			continue;
		}

		data_vector_append(&bus->channels[i]->data, data);
		wakeup_queue_wakeup_first(&bus->channels[i]->recv_queue);
	}

	return 0;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	/*
	 * Try sending in a loop, until success. If error, then
	 * check which one is that. If 'wouldblock', then suspend
	 * this coroutine and try again when woken up.
	 *
	 * If the channel has space, then wakeup the first
	 * coro in the send-queue. That is needed so when there is
	 * enough space for many messages, and many coroutines are
	 * waiting, they would then wake each other up one by one
	 * as lone as there is still space.
	 */

	int sent_count;
	for (;;) {
		if ((sent_count = coro_bus_try_send_v(bus, channel, data, count)) != -1) {
			break;
		}

		if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			coro_bus_errno_set(CORO_BUS_ERR_NONE);
			wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
			continue;
		}

		return -1;
	}

	if (bus->channels[channel]->data.size < bus->channels[channel]->size_limit) {
		wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);
	}

	return sent_count;
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	if (!coro_bus_channel_exists(bus, channel)) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (bus->channels[channel]->data.size == bus->channels[channel]->size_limit) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	size_t sent_count = bus->channels[channel]->size_limit - bus->channels[channel]->data.size;
	sent_count = sent_count > count ? count : sent_count;

	data_vector_append_many(&bus->channels[channel]->data, data, sent_count);
	wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);

	return sent_count;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	int recv_count;
	for (;;) {
		if ((recv_count = coro_bus_try_recv_v(bus, channel, data, capacity)) != -1) {
			break;
		}

		if (coro_bus_errno() == CORO_BUS_ERR_WOULD_BLOCK) {
			coro_bus_errno_set(CORO_BUS_ERR_NONE);
			wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue);
			continue;
		}

		return -1;
	}

	if (bus->channels[channel]->data.size > 0) {
		wakeup_queue_wakeup_first(&bus->channels[channel]->recv_queue);
	}

	return recv_count;
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (!coro_bus_channel_exists(bus, channel)) {
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (bus->channels[channel]->data.size == 0) {
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	size_t recv_count = bus->channels[channel]->data.size > capacity ? capacity : bus->channels[channel]->data.size;

	data_vector_pop_first_many(&bus->channels[channel]->data, data, recv_count);
	wakeup_queue_wakeup_first(&bus->channels[channel]->send_queue);

	return recv_count;
}

#endif
