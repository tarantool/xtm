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
#include "xtm_config.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#ifdef TARANTOOL_XTM_USE_EVENTFD
#include <sys/eventfd.h>
#endif

#define XTM_PIPE_SIZE 4096

union xtm_msg {
	/**
	 * Anonymous structure for storing functions and their arguments,
	 * when using the xtm API in dispatch/invoke pattern.
	 */
	struct {
		xtm_queue_fun_t fun;
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

static inline int
xtm_queue_flush_pipe(struct xtm_queue *xtm_queue)
{
	unsigned char tmp[XTM_PIPE_SIZE];
	ssize_t read_bytes;
	while ((read_bytes = read(xtm_queue->readfd, tmp, sizeof(tmp))) < 0 &&
	        errno == EINTR)
		;
	if (read_bytes <= 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		return -1;
	return 0;
}


struct xtm_queue *
xtm_queue_new(unsigned size)
{
	int save_errno = 0;
	int filedes[2];
	struct xtm_queue *queue = (struct xtm_queue *)
		malloc(sizeof(struct xtm_queue) +
		       size * sizeof(union xtm_msg));
	if (queue == NULL)
		return NULL;

#ifdef TARANTOOL_XTM_USE_EVENTFD
	if ((filedes[0] = eventfd(0, 0)) < 0) {
#else
	if (pipe(filedes) < 0) {
#endif
		save_errno = errno;
		goto free_queue;
	}

#ifdef TARANTOOL_XTM_USE_EVENTFD
	queue->readfd = queue->writefd = filedes[0];
#else
	queue->readfd = filedes[0];
	queue->writefd = filedes[1];
#endif

	if (fcntl(queue->readfd, F_SETFL, O_NONBLOCK) < 0 ||
	    fcntl(queue->writefd, F_SETFL, O_NONBLOCK) < 0) {
		save_errno = errno;
		goto close_fd;
	}
	if (queue->queue.create(size) < 0) {
		save_errno = EINVAL;
		goto close_fd;
	}
	return queue;

close_fd:
	close(queue->readfd);
	if (queue->readfd != queue->writefd)
		close(queue->writefd);
free_queue:
	free(queue);
	errno = save_errno;
	return NULL;
};

int
xtm_queue_delete(struct xtm_queue *queue)
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
	if (queue->queue.free_count() == 0) {
		errno = ENOBUFS;
		return -1;
	}
	return 0;
}

unsigned
xtm_msg_count(struct xtm_queue *queue)
{
	return queue->queue.count();
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
xtm_fun_dispatch(struct xtm_queue *queue, xtm_queue_fun_t fun,
		 void *fun_arg, int delayed)
{
	union xtm_msg xtm_msg;
	xtm_msg.fun = fun;
	xtm_msg.fun_arg = fun_arg;
	if (queue->queue.put(&xtm_msg, 1) != 1) {
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
	struct xtm_scsp_queue_read_iterator<xtm_msg> iter;
	iter.begin(&queue->queue);
	const union xtm_msg *xtm_msg;
	unsigned cnt = 0;

	while((xtm_msg = iter.read()) != nullptr) {
		xtm_msg->fun(xtm_msg->fun_arg);
		cnt++;
	}
	iter.end();
	return cnt;
}

int
xtm_msg_send(struct xtm_queue *queue, void *msg, int delayed)
{
	union xtm_msg xtm_msg;
	xtm_msg.data = msg;
	if (queue->queue.put(&xtm_msg, 1) != 1) {
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
	struct xtm_scsp_queue_read_iterator<xtm_msg> iter;
	iter.begin(&queue->queue);
	const union xtm_msg *xtm_msg;
	void **msg_begin = msg;

	while(msg - msg_begin < count && (xtm_msg = iter.read()) != nullptr) {
		*msg = xtm_msg->data;
		++msg;
	}
	iter.end();
	return msg - msg_begin;
}
