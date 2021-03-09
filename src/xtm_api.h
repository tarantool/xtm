#pragma once
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
#include <stdlib.h>
#include <stdbool.h>

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/**
 * Flags, that define library behavior
 */
enum {
	/**
	 * Flag indicates, that xtm_delete must close producer read
	 * file descriptor, otherwise it must be closed by user before
	 * calling xtm_delete.
	 */
	XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD = 1 << 0,
	/**
	 * Flag indicates, that xtm_delete must close producer read
	 * file descriptor, otherwise it must be closed by user before
	 * calling xtm_delete.
	 */
	XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD = 1 << 1,
	/**
	 * Flag indicates, that producer thread want to receive
	 * notifications when queue is not full.
	 */
	XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS = 1 << 2,
};

/**
 * Opaque struct, that represents unidirectional, single-writer-single-reader
 * queue implementation with event loop integration ability.
 */
struct xtm_queue;

/**
 * Typedef for often used function pointer, used in push_fun/invoke_funs_all
 * xtm used pattern.
 */
typedef void (*xtm_queue_fun_t)(void*);

/**
 * Create instance of struct xtm_queue.
 * @param[in] size  - queue size, must be power of two and greater then one.
 * @retval    pointer to new xtm_queue or NULL in case of error.
 */
struct xtm_queue *
xtm_queue_new(unsigned size);

/**
 * Free queue and close its internal fds. Which of the file descriptors will be
 * closed is determined by flags value.
 * @param[in] queue - xtm_queue to delete.
 * @param[in] flags - flags defining library behavior. acceptable values:
 *                    XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD,
 *                    XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD (see enum above).
 * @retval    0 on success. Otherwise -1 with errno set appropriately.
 *            (as in close(2), since it implies close of fds).
 */
int
xtm_queue_delete(struct xtm_queue *queue, unsigned flags);

/**
 * Notify queue consumer.
 * @param[in] queue - xtm_queue to notify.
 * @retval    0 on success. Otherwise -1 with errno set appropriately.
 *            (as in write(2), since it implies write to internal fd).
 */
int
xtm_queue_notify_consumer(struct xtm_queue *queue);

/**
 * Notify queue producer, when queue is not full.
 * @param[in] queue - xtm_queue to notify.
 * @retval    0 on success. Otherwise -1 with errno set appropriately.
 *            (as in write(2), since it implies write to internal fd).
 */
int
xtm_queue_notify_producer(struct xtm_queue *queue);

/**
 * Check is there are free space in queue.
 * @param[in] queue - xtm_queue to ckeck free space.
 * @retval    0 if queue has space. Otherwise -1 with errno set to ENOBUFS.
 */
int
xtm_queue_probe(struct xtm_queue *queue);

/**
 * Return current count of data in xtm queue.
 * @param[in] queue - xtm_queue to check current count of data.
 * @retval    count of data in queue.
 */
unsigned
xtm_queue_count(struct xtm_queue *queue);

/**
 * Puts message, which contains function and its argument to the queue.
 * This function does not notify the consumer thread, but only pushes to the queue.
 * To notify the consumer thread you must call xtm_queue_notify_consumer. The less
 * often you notify, the greater the performance, but the greater the latency.
 * @param[in] queue   - xtm_queue to push.
 * @param[in] fun     - function to push.
 * @param[in] fun_arg - function argument to push.
 * @param[in] flags   - flags defining function behavior. acceptable values:
 *                      XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS (see enum above).
 * @retval    0 if queue has space. Otherwise -1 with errno set to ENOBUFS.
 */
int
xtm_queue_push_fun(struct xtm_queue *queue, xtm_queue_fun_t fun,
		   void *fun_arg, unsigned flags);

/**
 * Return file descriptor, that should be watched by consumer thread to
 * become readable. When it became readable, consumer should call one
 * of the consumer functions: xtm_queue_pop_ptrs or xtm_queue_invoke_funs_all.
 * @param[in] queue - xtm_queue to get file descriptor.
 * @retval    xtm queue file descriptor for consumer thread.
 */
int
xtm_queue_consumer_fd(struct xtm_queue *queue);

/**
 * Return file descriptor, that should be watched by producer thread to
 * become readable. When it became readable, producer may push to queue,
 * being sure that there is a place in it (there is a little race between
 * consumer and producer thread, so there is a little chance, that this
 * descriptor become readable, but there is no free space in the queue. In
 * this case producer thread must poll this descriptor again.
 * @param[in] queue - xtm_queue to get file descriptor.
 * @retval    xtm queue file descriptor for producer thread.
 */
int
xtm_queue_producer_fd(struct xtm_queue *queue);

/**
 * Calls all functions contained in the queue.
 * If producer thread pushes functions with XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS
 * flag, user should retrieve and reset "producer failed to put an item in the
 * queue and expects notification" flag, using `xtm_queue_get_reset_was_full`.
 * If this flag was true, user should notify producer thread (see examples in
 * README.md).
 * @param[in] queue - xtm_queue.
 * @retval    count of invoked functions if success.
 */
unsigned
xtm_queue_invoke_funs_all(struct xtm_queue *queue);

/**
 * Puts message, which contains pointer to the queue. This function does not
 * notify the consumer thread, but only pushes to the queue. To notify the consumer
 * thread you must call xtm_queue_notify_consumer. The less often you notify, the
 * greater the performance, but the greater the latency.
 * @param[in] queue - xtm_queue to push.
 * @param[in] ptr   - pointer to push.
 * @param[in] flags - flags defining function behavior. acceptable values:
 *                    XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS (see enum above).
 * @retval    0 if queue has space. Otherwise -1 with errno set to ENOBUFS.
 */
int
xtm_queue_push_ptr(struct xtm_queue *queue, void *ptr, unsigned flags);

/**
 * Gets up to count elements from queue and saves them in pointer array.
 * If producer thread pushes pointers with XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS
 * flag, user should retrieve and reset "producer failed to put an item in the
 * queue and expects notification" flag, using `xtm_queue_get_reset_was_full`.
 * If this flag was true, user should notify producer thread (see examples in
 * README.md).
 * @param[in]  queue           - xtm_queue containing pointers.
 * @param[out] ptr_array       - pointer array to save pointers.
 * @param[in]  ptr_array_count - maximum count of pointers, that can
 *                               be extracted.
 * @retval     count of extracted elements.
 */
unsigned
xtm_queue_pop_ptrs(struct xtm_queue *queue, void **ptr_array,
                   unsigned ptr_array_count);

/**
 * Reads from internal queue pipe, according to file descriptor passed
 * to this function.
 * @param[in] fd - file descriptor to consume.
 * @retval    0 on success. Otherwise -1 with errno set appropriately
 *            (as in read(2), since it implies read from internal fd).
 */
int
xtm_queue_consume(int fd);

/**
 * @retval retrieves and resets "producer failed to put an item
 *         in the queue and expects notification" flag.
 */
bool
xtm_queue_get_reset_was_full(struct xtm_queue *queue);

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

