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

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** \cond public */

/**
 * Opaque struct, that represent one-directional, one-reader-one-writer
 * queue implementation with eventloop integration ability.
 */
struct xtm_queue;

/**
 * Allocation function, used to create instance of struct xtm_queue.
 * @param[in] size - queue size, must be power of two
 * @retval returns pointer to new xtm_queue or NULL in case of error,
 *         with the following possible errno: ENOMEM, EINVAL, EMFILE,
 *         ENFILE, ENODEV.
 */
struct xtm_queue *
xtm_create(unsigned size);

/**
 * Deallocation function, used to free queue and close its internal fds.
 * @param[in] queue - xtm_queue to delete
 * @retval returns 0 on success. Otherwise -1 with errno set appropriately
 *         (as in close(2), since it implies close of fds).
 */
int
xtm_delete(struct xtm_queue *queue);

/**
 * Function for queue consumer notification.
 * @param[in] queue - xtm_queue to notify
 * @retval returns 0 on success. Otherwise -1 with errno set appropriately
 *         (as in write(2), since it implies write to internal fd).
 */
int
xtm_msg_notify(struct xtm_queue *queue);

/**
 * Function checks is there are free space in queue
 * @param[in] queue - xtm_queue to ckeck free space
 * @retval returns 0 if queue has space. Otherwise -1 with errno set
 *         to ENOBUFS.
 */
int
xtm_msg_probe(struct xtm_queue *queue);

/**
 * Function returns current count of messages in xtm queue
 * @param[in] queue - xtm_queue to check current count of messages
 * @retval returns count of messages in queue.
 */
unsigned
xtm_msg_count(struct xtm_queue *queue);

/**
 * Function puts a message containing the function and
 * its argument to the queue
 * @param[in] queue - xtm_queue to put message
 * @param[in] fun - message function
 * @param[in] fun_arg - message function argument
 * @param[in] delayed - flag indicating the consumer should be notified
 * @retval returns 0 on success. Otherwise -1 with errno set appropriately
 *         (as in write(2), since it implies write to internal fd + ENOBUFS
 *         in case there is no space in queue).
 */
int
xtm_fun_dispatch(struct xtm_queue *queue, void (*fun)(void *),
		 void *fun_arg, int delayed);

/**
 * Function returns file descriptor, that should be watched to become readable.
 * When it became readable, consumer should call one of the consumer functions:
 * xtm_msg_recv or xtm_fun_invoke.
 * @param[in] queue - xtm_queue to get file descriptor
 * @retval returns xtm queue file descriptor.
 */
int
xtm_fd(struct xtm_queue *queue);

/**
 * Function retrieves messages from the queue and calls
 * functions contained in them
 * @param[in] queue - xtm_queue containing messages
 * @param[in] flushed - flag indicating pipe should be flushed
 * @retval returns count of invoked functions on success. Otherwise -1
 *         with errno set appropriately (as in read(2), since it implies
 *         read to internal fd).
 */
int
xtm_fun_invoke(struct xtm_queue *queue, int flushed);

/**
 * Function puts a message pointer to the queue.
 * @param[in] queue - xtm_queue containing messages
 * @param[in] msg - message pointer
 * @param[in] delayed - flag indicating the consumer should be notified
 * @retval returns 0 on success. Otherwise -1 with errno set appropriately
 *         (as in write(2), since it implies write to internal fd + ENOBUFS
 *         in case there is no space in queue).
 */
int
xtm_msg_send(struct xtm_queue *queue, void *msg, int delayed);

/**
 * Function gets up to count elements from the queue
 * and save them in message pointer array.
 * @param[in] queue - xtm_queue containing messages
 * @param[out] msg - msg pointer array
 * @param[in] count - maximum count of messages, that can be extracted
 * @param[in] flushed - flag indicating pipe should be flushed
 * @retval returns count of extracted messages on success. Otherwise -1 with
 *         errno set appropriately (as in read(2), since it implies read to
 *         internal fd).
 */
int
xtm_msg_recv(struct xtm_queue *queue, void **msg, unsigned count, int flushed);

/** \endcond public */

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

