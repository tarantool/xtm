/*
 * Copyright 2010-2021, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */
#include "xtm_api.h"
#include "xtm_scsp_queue.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#ifdef __linux__
#include <sys/eventfd.h>
#endif

#define XTM_PIPE_SIZE 4096

struct xtm_msg {
	/**
	 * Function for call, or NULL if we want to pass only data pointer
	 */
	void (*fun)(void *);
	/**
	 * Function argument (in case fun is NULL, used to save data pointer)
	 */
	void *fun_arg;
};

struct xtm_queue {
#ifdef __linux__
	int filedes;
#else
	int filedes[2];
#endif
	int readfd;
	int writefd;
	/*
	 * Message queue, size must be power of 2
	 */
	struct xtm_scsp_queue<struct xtm_msg> *queue;
};

/**
 * Function gets num data pointers, previously saved in fun_arg field of
 * struct xtm_msg. Used in case send/recv xtm API pattern.
 * @param[in] queue - xtm queue
 * @param[out] fun_args - array to save data pointers from fun_arg field
 * @param[in] num - count of elements to get
 * @retval return the number of data pointers actually read.
 */
static inline unsigned
xtm_queue_get_funargs(struct xtm_queue *xtm_queue,
		      void **fun_args, unsigned num)
{
	struct xtm_scsp_queue<struct xtm_msg> *queue = xtm_queue->queue;
	unsigned i = 0;
	unsigned new_read = queue->read;
	unsigned queue_write =
		__atomic_load_n(&queue->write, __ATOMIC_ACQUIRE);

	for (i = 0; i < num; i++) {
		if (new_read == queue_write)
			break;
		fun_args[i] = queue->buffer[new_read].fun_arg;
		new_read = (new_read + 1) & (queue->len - 1);
	}
	__atomic_store_n(&queue->read, new_read, __ATOMIC_RELEASE);
	return i;
}

/**
 * Function executes all functions contained in all queue elements.
 * Used in case dispatch/invoke xtm API pattern
 * @param[in] queue - xtm queue
 * @retval return count of executed functions
 */
static inline unsigned
xtm_queue_execute_funs(struct xtm_queue *xtm_queue)
{
	struct xtm_scsp_queue<struct xtm_msg> *queue = xtm_queue->queue;
	unsigned i = 0;
	unsigned new_read = queue->read;
	unsigned queue_write =
		__atomic_load_n(&queue->write, __ATOMIC_ACQUIRE);

	for(;;) {
		if (new_read == queue_write)
			break;
		queue->buffer[new_read].fun(queue->buffer[new_read].fun_arg);
		new_read = (new_read + 1) & (queue->len - 1);
		i++;
	}
	__atomic_store_n(&queue->read, new_read, __ATOMIC_RELEASE);
	return i;
}


struct xtm_queue *
xtm_create(unsigned size)
{
	struct xtm_queue *queue = (struct xtm_queue *)
		malloc(sizeof(struct xtm_queue));
	if (queue == NULL)
		goto fail_alloc_queue;
	queue->queue = (struct xtm_scsp_queue<struct xtm_msg> *)
		malloc(sizeof(struct xtm_scsp_queue<struct xtm_msg>) +
		       size * sizeof(struct xtm_msg));
	if (queue->queue == NULL)
		goto fail_alloc_scsp_queue;
#ifdef __linux__
	if ((queue->filedes = eventfd(0, 0)) < 0)
#else
	if (pipe(queue->filedes) < 0)
#endif
		goto fail_alloc_fd;

#ifdef __linux__
	queue->readfd = queue->writefd = queue->filedes;
#else
	queue->readfd = queue->filedes[0];
	queue->writefd = queue->filedes[1];
#endif
	/*
	 * Make pipe read/write nonblock, and decrease
	 * pipe size to minimum (pipe size >= PAGE_SIZE)
	 */
	if (fcntl(queue->readfd, F_SETFL, O_NONBLOCK) < 0 ||
	    fcntl(queue->writefd, F_SETFL, O_NONBLOCK) < 0)
		goto fail_alloc_fd;
	if (xtm_scsp_queue_init(queue->queue, size) < 0) {
		errno = EINVAL;
		goto fail_scsp_queue_init;
	}
	return queue;

fail_scsp_queue_init:
	close(queue->readfd);
	if (queue->readfd != queue->writefd)
		close(queue->writefd);
fail_alloc_fd:
	free(queue->queue);
fail_alloc_scsp_queue:
	free(queue);
fail_alloc_queue:
	return NULL;
};

int
xtm_delete(struct xtm_queue *queue)
{
	int err = 0;
	int save_errno = errno;
	errno = 0;
	if (close(queue->readfd) < 0)
		err = errno;
	if (queue->readfd != queue->writefd && close(queue->writefd) == 0)
		errno = err;
	free(queue->queue);
	free(queue);
	err = errno;
	if (err == 0)
		errno = save_errno;
	return (err == 0 ? 0 : -1);
}

int
xtm_msg_probe(struct xtm_queue *queue)
{
	if (xtm_scsp_queue_free_count(queue->queue) == 0) {
		errno = ENOBUFS;
		return -1;
	}
	return 0;
}

unsigned
xtm_msg_count(struct xtm_queue *queue)
{
	return xtm_scsp_queue_count(queue->queue);
}

int
xtm_msg_notify(struct xtm_queue *queue)
{
	uint64_t tmp = 1;
	ssize_t cnt;
	/*
	 * We must write 8 byte value, because for linux we
	 * used eventfd, which require to write >= 8 byte at once.
	 * Also in case of EINTR we retry to write
	 */
	while ((cnt = write(queue->writefd, &tmp, sizeof(tmp))) < 0 &&
	       errno == EINTR)
		;
	return (cnt == sizeof(tmp) ? 0 : -1);
}

int
xtm_fun_dispatch(struct xtm_queue *queue, void (*fun)(void *),
		 void *fun_arg, int delayed)
{
	struct xtm_msg msg;
	msg.fun = fun;
	msg.fun_arg = fun_arg;
	if (xtm_scsp_queue_put(queue->queue, &msg, 1) != 1) {
		errno = ENOBUFS;
		return -1;
	}
	if (delayed == 0)
		return xtm_msg_notify(queue);
	return 0;
}

int
xtm_fd(struct xtm_queue *queue)
{
	return queue->readfd;
}

unsigned
xtm_fun_invoke(struct xtm_queue *queue)
{
	xtm_queue_execute_funs(queue);
	return xtm_scsp_queue_count(queue->queue);
}

int
xtm_fun_invoke_with_pipe_flushing(struct xtm_queue *queue)
{
	unsigned char tmp[XTM_PIPE_SIZE];
	ssize_t read_bytes;
	int save_errno = errno;
	while ((read_bytes = read(queue->readfd, tmp, sizeof(tmp))) < 0 &&
	        errno == EINTR)
		;
	if (read_bytes <= 0 && errno != EAGAIN)
		return -1;
	errno = save_errno;
	return xtm_fun_invoke(queue);
}

unsigned
xtm_msg_recv(struct xtm_queue *queue, void **msg, unsigned count)
{
	return xtm_queue_get_funargs(queue, msg, count);
}

int
xtm_msg_recv_with_pipe_flushing(struct xtm_queue *queue,
				void **msg, unsigned count)
{
	unsigned char tmp[XTM_PIPE_SIZE];
	ssize_t read_bytes;
	int save_errno = errno;
	while ((read_bytes = read(queue->readfd, tmp, sizeof(tmp))) < 0 &&
	       errno == EINTR)
		;
	if (read_bytes <= 0 && errno != EAGAIN)
		return -1;
	errno = save_errno;
	return xtm_msg_recv(queue, msg, count);
}
