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

#include <unistd.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#ifdef TARANTOOL_XTM_USE_EVENTFD
#include <sys/eventfd.h>
#endif /* defined(TARANTOOL_XTM_USE_EVENTFD) */

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
	void *ptr;
};

struct xtm_queue {
	/**
	 * File descriptor that the consumer thread must poll,
	 * to know when new messages are added to the queue.
	 */
	int consumer_read_fd;
	/**
	 * File descriptor to which the producer thread writes
	 * to notify the consumer thread of new messages in the queue.
	 */
	int consumer_write_fd;
	/**
	 * File descriptor that the producer thread must poll,
	 * to know when there is a free space in qeuue.
	 */
	int producer_read_fd;
	/**
	 * File descriptor to which the consumer thread writes
	 * to notify the producer thread about free space in queue.
	 */
	int producer_write_fd;
	/**
	 * Flags, that define queue behaviour.
	 */
	uint64_t flags;
	/**
	 * Flag indicates, that producer couldn't put an item in
	 * the queue, and wait for notification.
	 */
	bool is_notification_expected;
	/** Message queue, it's size must be power of two */
	struct xtm_scsp_queue<union xtm_msg> queue;
};

static inline int
notify_fd(int fd)
{
	static uint64_t tmp = 1;
	ssize_t cnt;
	/*
	 * We must write 8 byte value, because for linux we
	 * used eventfd, which require to write >= 8 byte at once.
	 * Also in case of EINTR we retry to write
	 */
	while ((cnt = write(fd, &tmp, sizeof(tmp))) < 0 && errno == EINTR)
		;
	return ((cnt == sizeof(tmp) || errno == EAGAIN) ? 0 : -1);
}

static inline int
create_fds(int *read_fd, int *write_fd)
{
	int fds[2];
	assert(read_fd != NULL);
	assert(write_fd != NULL);

#ifdef TARANTOOL_XTM_USE_EVENTFD
	if ((fds[0] = eventfd(0, 0)) < 0)
#else /* !defined(TARANTOOL_XTM_USE_EVENTFD) */
	if (pipe(fds) < 0)
#endif /* defined(TARANTOOL_XTM_USE_EVENTFD) */
		return -1;

#ifdef TARANTOOL_XTM_USE_EVENTFD
	*read_fd = *write_fd = fds[0];
#else /* !defined(TARANTOOL_XTM_USE_EVENTFD) */
	*read_fd = fds[0];
	*write_fd = fds[1];
#endif /* defined(TARANTOOL_XTM_USE_EVENTFD) */
	return 0;
}

int
xtm_queue_attr_create(struct xtm_queue_attr *attr)
{
	attr->ex_size = 0;
	attr->flags = 0;
	/*
	 * In future, if we decide to use xtm_queue_attr_ex, we need
	 * to malloc memory for it, so, this function can fails.
	 */
	return 0;
}

void
xtm_queue_attr_set(struct xtm_queue_attr *attr, uint64_t flag)
{
	attr->flags |= flag;
}

void
xtm_queue_attr_destroy(struct xtm_queue_attr *attr)
{
	(void)attr;
	/*
	 * In future, if we decide to use xtm_queue_attr_ex,
	 * we need to free up it's memory.
	 */
}

struct xtm_queue *
xtm_queue_new(unsigned size, struct xtm_queue_attr *attr)
{
	int save_errno = 0;
	struct xtm_queue *queue = (struct xtm_queue *)
		malloc(sizeof(struct xtm_queue) +
		       size * sizeof(union xtm_msg));
	if (queue == NULL)
		return NULL;

	queue->flags = attr->flags;
	queue->is_notification_expected = false;

	if (create_fds(&queue->consumer_read_fd,
		       &queue->consumer_write_fd) < 0) {
		save_errno = errno;
		goto free_queue;
	}

	if (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) {
		if (create_fds(&queue->producer_read_fd,
			       &queue->producer_write_fd) < 0) {
			save_errno = errno;
			goto close_consumer_fds;
		}
	} else {
		queue->producer_read_fd = queue->producer_write_fd = -1;
	}

	if (fcntl(queue->consumer_read_fd, F_SETFL, O_NONBLOCK) < 0 ||
	    fcntl(queue->consumer_write_fd, F_SETFL, O_NONBLOCK) < 0 ||
	    (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS &&
	    (fcntl(queue->producer_read_fd, F_SETFL, O_NONBLOCK) < 0 ||
	     fcntl(queue->producer_write_fd, F_SETFL, O_NONBLOCK) < 0))) {
		save_errno = errno;
		goto close_producer_fds;
	}
	if (queue->queue.create(size) < 0) {
		save_errno = EINVAL;
		goto close_producer_fds;
	}
	return queue;

close_producer_fds:
	if (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) {
		close(queue->producer_read_fd);
		if (queue->producer_read_fd != queue->producer_write_fd)
			close(queue->producer_write_fd);
	}
close_consumer_fds:
	close(queue->consumer_read_fd);
	if (queue->consumer_read_fd != queue->consumer_write_fd)
		close(queue->consumer_write_fd);
free_queue:
	free(queue);
	errno = save_errno;
	return NULL;
};

int
xtm_queue_delete(struct xtm_queue *queue)
{
	int rc = 0;
	if ((queue->flags & XTM_QUEUE_NEED_TO_CLOSE_READFD) &&
	    (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) &&
	    close(queue->producer_read_fd) < 0)
		rc = -1;
	if ((queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) &&
	    queue->producer_write_fd != queue->producer_read_fd &&
	    close(queue->producer_write_fd) < 0)
		rc = -1;
	if ((queue->flags & XTM_QUEUE_NEED_TO_CLOSE_READFD) &&
	    close(queue->consumer_read_fd) < 0)
		rc = -1;
	if (queue->consumer_read_fd != queue->consumer_write_fd &&
	    close(queue->consumer_write_fd) < 0)
		rc = -1;
	free(queue);
	return rc;
}

int
xtm_queue_notify_consumer(struct xtm_queue *queue)
{
	return notify_fd(queue->consumer_write_fd);
}

int
xtm_queue_probe(struct xtm_queue *queue)
{
	if (queue->queue.free_count() == 0) {
		errno = ENOBUFS;
		return -1;
	}
	return 0;
}

unsigned
xtm_queue_count(struct xtm_queue *queue)
{
	return queue->queue.count();
}

int
xtm_queue_push_fun(struct xtm_queue *queue, xtm_queue_fun_t fun,
		   void *fun_arg)
{
	union xtm_msg xtm_msg;
	xtm_msg.fun = fun;
	xtm_msg.fun_arg = fun_arg;
	if (queue->queue.put(&xtm_msg, 1) == 1)
		return 0;
	errno = ENOBUFS;
	if (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) {
		__atomic_store_n(&queue->is_notification_expected,
				 true, __ATOMIC_RELEASE);
	}
	return (queue->queue.put(&xtm_msg, 1) == 1 ? 0 : -1);
}

int
xtm_queue_consumer_fd(struct xtm_queue *queue)
{
	return queue->consumer_read_fd;
}

int
xtm_queue_producer_fd(struct xtm_queue *queue)
{
	return queue->producer_read_fd;
}

int
xtm_queue_invoke_funs_all(struct xtm_queue *queue)
{
	struct xtm_scsp_queue_read_iterator<xtm_msg> iter;
	const union xtm_msg *xtm_msg;
	unsigned cnt = 0;

	iter.begin(&queue->queue);
	while((xtm_msg = iter.read()) != nullptr) {
		xtm_msg->fun(xtm_msg->fun_arg);
		cnt++;
	}
	iter.end();
	if (! (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) ||
	    ! __atomic_load_n(&queue->is_notification_expected,
			      __ATOMIC_ACQUIRE))
		goto finish;

	__atomic_store_n(&queue->is_notification_expected, false,
			 __ATOMIC_RELEASE);
	if (notify_fd(queue->producer_write_fd) < 0)
		return -(cnt + 1);
finish:
	return cnt;
}

int
xtm_queue_push_ptr(struct xtm_queue *queue, void *ptr)
{
	union xtm_msg xtm_msg;
	xtm_msg.ptr = ptr;
	if (queue->queue.put(&xtm_msg, 1) == 1)
		return 0;
	errno = ENOBUFS;
	if (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) {
		__atomic_store_n(&queue->is_notification_expected,
				 true, __ATOMIC_RELEASE);
	}
	return (queue->queue.put(&xtm_msg, 1) == 1 ? 0 : -1);
}

int
xtm_queue_pop_ptrs(struct xtm_queue *queue, void **ptr_array,
		   unsigned ptr_array_size)
{
	struct xtm_scsp_queue_read_iterator<xtm_msg> iter;
	const union xtm_msg *xtm_msg;
	void **ptr_array_begin = ptr_array;
	void **ptr_array_end = ptr_array + ptr_array_size;

	iter.begin(&queue->queue);
	while(ptr_array < ptr_array_end &&
	      (xtm_msg = iter.read()) != nullptr) {
		*ptr_array = xtm_msg->ptr;
		++ptr_array;
	}
	iter.end();
	if (! (queue->flags & XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS) ||
	    ! __atomic_load_n(&queue->is_notification_expected,
			      __ATOMIC_ACQUIRE))
		goto finish;

	__atomic_store_n(&queue->is_notification_expected, false,
			 __ATOMIC_RELEASE);
	if (notify_fd(queue->producer_write_fd) < 0)
		return -(ptr_array - ptr_array_begin + 1);

finish:
	return ptr_array - ptr_array_begin;
}

int
xtm_queue_consume(int fd)
{
	unsigned char tmp[XTM_PIPE_SIZE];
	ssize_t read_bytes;

	while ((read_bytes = read(fd, tmp, sizeof(tmp))) < 0 && errno == EINTR)
		;
	if (read_bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
		return -1;
	return 0;
}
