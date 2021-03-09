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
#ifdef XTM_WITH_EVENTFD
#include <sys/eventfd.h>
#endif

#define XTM_PIPE_SIZE 4096

union xtm_msg {
	/**
	 * Anonymous structure for storing functions and their arguments,
	 * when using the xtm API in dispatch/invoke pattern.
	 */
	struct {
		void (*fun)(void *);
		void *fun_arg;
	};
	/**
	 * Pointer to save message, when using xtm API
	 * in send/recv pattern.
	 */
	void *data;
};

struct xtm_queue {
	/**
	 * File descriptor that the consumer thread must poll,
	 * to know when new messages are added to the queue.
	 */
	int readfd;
	/**
	 * The file descriptor to which the producer thread writes
	 * to notify the consumer thread of new messages in the queue.
	 */
	int writefd;
	/** Message queue, it's size must be power of two */
	struct xtm_scsp_queue<union xtm_msg> queue;
};

/**
 * Function gets num data pointers, previously saved in data field of
 * union xtm_msg. Used in case send/recv xtm API pattern.
 * @param[in] queue - xtm queue
 * @param[out] data - array to save data pointers from xtm_msg union data field
 * @param[in] num - count of elements to get
 * @retval return the number of data pointers actually read.
 */
static inline unsigned
xtm_queue_get_data(struct xtm_queue *xtm_queue,
		   void **data, unsigned num)
{
	struct xtm_scsp_queue<union xtm_msg> *queue = &xtm_queue->queue;
	unsigned i = 0;
	unsigned new_read = queue->read;
	unsigned queue_write =
		__atomic_load_n(&queue->write, __ATOMIC_ACQUIRE);

	for (i = 0; i < num; i++) {
		if (new_read == queue_write)
			break;
		data[i] = queue->buffer[new_read].data;
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
	struct xtm_scsp_queue<union xtm_msg> *queue = &xtm_queue->queue;
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

static inline int
xtm_queue_flush_pipe(struct xtm_queue *xtm_queue)
{
	unsigned char tmp[XTM_PIPE_SIZE];
	ssize_t read_bytes;
	while ((read_bytes = read(xtm_queue->readfd, tmp, sizeof(tmp))) < 0 &&
	        errno == EINTR)
		;
	if (read_bytes <= 0 && errno != EAGAIN)
		return -1;
	return 0;
}


struct xtm_queue *
xtm_create(unsigned size)
{
	int filedes[2];
	struct xtm_queue *queue = (struct xtm_queue *)
		malloc(sizeof(struct xtm_queue) +
		       size * sizeof(union xtm_msg));
	if (queue == NULL)
		return NULL;

#ifdef XTM_WITH_EVENTFD
	if ((filedes[0] = eventfd(0, 0)) < 0)
#else
	if (pipe(filedes) < 0)
#endif
		goto free_queue;

#ifdef XTM_WITH_EVENTFD
	queue->readfd = queue->writefd = filedes[0];
#else
	queue->readfd = filedes[0];
	queue->writefd = filedes[1];
#endif

	if (fcntl(queue->readfd, F_SETFL, O_NONBLOCK) < 0 ||
	    fcntl(queue->writefd, F_SETFL, O_NONBLOCK) < 0)
		goto close_fd;
	if (xtm_scsp_queue_init(&queue->queue, size) < 0) {
		errno = EINVAL;
		goto close_fd;
	}
	return queue;

close_fd:
	close(queue->readfd);
	if (queue->readfd != queue->writefd)
		close(queue->writefd);
free_queue:
	free(queue);
	return NULL;
};

int
xtm_delete(struct xtm_queue *queue)
{
	int rc = 0;
	if (close(queue->readfd) < 0)
		rc = -1;
	if (queue->readfd != queue->writefd && close(queue->writefd) < 0)
		rc = -1;
	free(queue);
	return rc;
}

int
xtm_msg_probe(struct xtm_queue *queue)
{
	if (xtm_scsp_queue_free_count(&queue->queue) == 0) {
		errno = ENOBUFS;
		return -1;
	}
	return 0;
}

unsigned
xtm_msg_count(struct xtm_queue *queue)
{
	return xtm_scsp_queue_count(&queue->queue);
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
	union xtm_msg xtm_msg;
	xtm_msg.fun = fun;
	xtm_msg.fun_arg = fun_arg;
	if (xtm_scsp_queue_put(&queue->queue, &xtm_msg, 1) != 1) {
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

int
xtm_fun_invoke(struct xtm_queue *queue, int flushed)
{
	if (flushed != 0 && xtm_queue_flush_pipe(queue) < 0)
		return -1;
	return xtm_queue_execute_funs(queue);
}

int
xtm_msg_send(struct xtm_queue *queue, void *msg, int delayed)
{
	union xtm_msg xtm_msg;
	xtm_msg.data = msg;
	if (xtm_scsp_queue_put(&queue->queue, &xtm_msg, 1) != 1) {
		errno = ENOBUFS;
		return -1;
	}
	if (delayed == 0)
		return xtm_msg_notify(queue);
	return 0;
}

int
xtm_msg_recv(struct xtm_queue *queue, void **msg, unsigned count, int flushed)
{
	if (flushed != 0 && xtm_queue_flush_pipe(queue) < 0)
		return -1;
	return xtm_queue_get_data(queue, msg, count);
}
