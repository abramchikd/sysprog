#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>

enum task_status
{
	TPOOL_TASK_NEW = 1,
	TPOOL_TASK_QUEUED = 2,
	TPOOL_TASK_RUNNING = 3,
	TPOOL_TASK_FINISHED = 4
};

struct thread_task
{
	struct thread_task *next;

	thread_task_f function;
	void *arg;
	void *result;

	struct thread_pool *pool;
	enum task_status status;

	bool is_joined;
	bool is_detached;
};

struct thread_pool
{
	pthread_t *threads;
	int thread_count;
	int max_thread_count;

	int idle_thread_count;
	struct thread_task *task_queue;
	int queued_tasks_count;
	struct thread_task *task_queue_last;

	pthread_mutex_t tasks_mutex;
	pthread_cond_t tasks_cv;
	pthread_cond_t ready_tasks_cv;

	bool is_active;
};

int
thread_pool_new(int max_thread_count, struct thread_pool **pool)
{
	if (max_thread_count < 1 || max_thread_count > TPOOL_MAX_THREADS) {
		return TPOOL_ERR_INVALID_ARGUMENT;
	}

	*pool = calloc(1, sizeof(struct thread_pool));
	(*pool)->threads = malloc(sizeof(struct thread_pool) * max_thread_count);
	(*pool)->max_thread_count = max_thread_count;
	(*pool)->is_active = true;

	pthread_mutex_init(&(*pool)->tasks_mutex, NULL);
	pthread_cond_init(&(*pool)->tasks_cv, NULL);
	pthread_cond_init(&(*pool)->ready_tasks_cv, NULL);

	return 0;
}

int
thread_pool_thread_count(const struct thread_pool *pool)
{
	return pool->thread_count;
}

int
thread_pool_delete(struct thread_pool *pool)
{
	if (pool->task_queue != NULL) {
		return TPOOL_ERR_HAS_TASKS;
	}

	if (pool->idle_thread_count != pool->thread_count) {
		return TPOOL_ERR_HAS_TASKS;
	}

	pool->is_active = false;
	pthread_cond_broadcast(&pool->tasks_cv);

	for (int i = 0; i < pool->thread_count; i++) {
		pthread_join(pool->threads[i], NULL);
	}

	pthread_mutex_destroy(&pool->tasks_mutex);
	pthread_cond_destroy(&pool->tasks_cv);
	pthread_cond_destroy(&pool->ready_tasks_cv);

	free(pool);

	return 0;
}

static void *
run(void *pool_pointer);


int
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task)
{
	if (pool->queued_tasks_count >= TPOOL_MAX_TASKS) {
		return TPOOL_ERR_TOO_MANY_TASKS;
	}

	if (pool->idle_thread_count == 0 && pool->thread_count < pool->max_thread_count) {
		pthread_mutex_lock(&pool->tasks_mutex);
		if (pool->idle_thread_count == 0 && pool->thread_count < pool->max_thread_count) {
			pthread_create(&pool->threads[pool->thread_count++], NULL, run, pool);
			pool->idle_thread_count++;
		}
		pthread_mutex_unlock(&pool->tasks_mutex);
	}

	pthread_mutex_lock(&pool->tasks_mutex);
	if (pool->task_queue == NULL) {
		task->next = NULL;
		pool->task_queue = task;
		pool->task_queue_last = task;
	} else {
		task->next = NULL;
		pool->task_queue_last->next = task;
		pool->task_queue_last = task;
	}

	task->pool = pool;
	task->is_joined = false;
	task->status = TPOOL_TASK_QUEUED;
	pool->queued_tasks_count++;

	pthread_cond_signal(&pool->tasks_cv);
	pthread_mutex_unlock(&pool->tasks_mutex);

	return 0;
}

static void *
run(void *pool_pointer)
{
	struct thread_pool *pool = pool_pointer;
	pthread_mutex_lock(&pool->tasks_mutex);

	while (pool->is_active) {
		while (pool->task_queue == NULL && pool->is_active) {
			pthread_cond_wait(&pool->tasks_cv, &pool->tasks_mutex);
		}

		if (!pool->is_active) {
			break;
		}

		struct thread_task *task = pool->task_queue;
		pool->task_queue = pool->task_queue->next;
		if (pool->task_queue == NULL) {
			pool->task_queue_last = NULL;
		}

		pool->queued_tasks_count--;

		pool->idle_thread_count--;
		pthread_mutex_unlock(&pool->tasks_mutex);
		task->status = TPOOL_TASK_RUNNING;
		task->result = task->function(task->arg);

		pthread_mutex_lock(&pool->tasks_mutex);
		task->status = TPOOL_TASK_FINISHED;
		pool->idle_thread_count++;
		if (task->is_detached) {
			free(task);
		} else {
			pthread_cond_broadcast(&pool->ready_tasks_cv);
		}
	}

	pthread_mutex_unlock(&pool->tasks_mutex);

	return NULL;
}

int
thread_task_new(struct thread_task **task, thread_task_f function, void *arg)
{
	*task = calloc(1, sizeof(struct thread_task));
	(*task)->function = function;
	(*task)->arg = arg;
	(*task)->status = TPOOL_TASK_NEW;
	(*task)->is_joined = false;

	return 0;
}

bool
thread_task_is_finished(const struct thread_task *task)
{
	return task->status == TPOOL_TASK_FINISHED;
}

bool
thread_task_is_running(const struct thread_task *task)
{
	return task->status == TPOOL_TASK_RUNNING;
}

int
thread_task_join(struct thread_task *task, void **result)
{
	if (task->status == TPOOL_TASK_NEW) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	pthread_mutex_lock(&task->pool->tasks_mutex);
	while (task->status != TPOOL_TASK_FINISHED) {
		pthread_cond_wait(&task->pool->ready_tasks_cv, &task->pool->tasks_mutex);
	}

	task->is_joined = true;
	pthread_mutex_unlock(&task->pool->tasks_mutex);

	*result = task->result;

	return 0;
}

#if NEED_TIMED_JOIN

int
thread_task_timed_join(struct thread_task *task, double timeout, void **result)
{
	if (task->status == TPOOL_TASK_NEW) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	struct timespec timeout_time;
	clock_gettime(CLOCK_MONOTONIC, &timeout_time);
	timeout_time.tv_sec += (time_t) timeout;
	timeout_time.tv_nsec += (time_t) ((timeout - (int) timeout) * 1000000000);

	pthread_mutex_lock(&task->pool->tasks_mutex);
	while (task->status != TPOOL_TASK_FINISHED) {
		pthread_cond_timedwait(&task->pool->ready_tasks_cv, &task->pool->tasks_mutex, &timeout_time);

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec * 1000000000 + now.tv_nsec > timeout_time.tv_sec * 1000000000 + timeout_time.tv_nsec) {
			break;
		}
	}

	pthread_mutex_unlock(&task->pool->tasks_mutex);


	if (task->status == TPOOL_TASK_FINISHED) {
		task->is_joined = true;
		*result = task->result;

		return 0;
	}

	return TPOOL_ERR_TIMEOUT;
}

#endif

int
thread_task_delete(struct thread_task *task)
{
	if (task->status != TPOOL_TASK_NEW && !task->is_joined) {
		return TPOOL_ERR_TASK_IN_POOL;
	}

	free(task);
	return 0;
}

#if NEED_DETACH

int
thread_task_detach(struct thread_task *task)
{
	if (task->status == TPOOL_TASK_NEW) {
		return TPOOL_ERR_TASK_NOT_PUSHED;
	}

	task->is_detached = true;
	return 0;
}

#endif
