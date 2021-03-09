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

struct xtm_queue;

/**
 * Function create new struct xtm_queue.
 * @param[in] size - queue size, must be power of two
 * @retval return new xtm_queue or NULL in case of error,
           and sets the errno value
 */
struct xtm_queue *
xtm_create(unsigned size);

/**
 * Function destroys xtm_queue, and frees all resources, associated with it.
 * @param[in] queue - xtm_queue to delete
 * @return 0 if success, otherwise return -1 and sets the errno value
 */
int
xtm_delete(struct xtm_queue *queue);

/**
 * Function notifies queue consumer about new messages in queue
 * @param[in] queue - queue to notify
 * @return 0 if success, otherwise return -1 and sets the errno value
 */
int
xtm_msg_notify(struct xtm_queue *queue);

/**
 * Function checks is there are free space in queue
 * @param[in] queue - queue to ckeck free space
 * @retval return 0 in case there is a free space in queue,
 *         otherwise return -1 and sets the errno value to ENOBUFS
 */
int
xtm_msg_probe(struct xtm_queue *queue);

/**
 * Function returns current count of messages in xtm queue
 * @param[in] queue -xtm message queue
 * @retval return count of messages in queue
 */
unsigned
xtm_msg_count(struct xtm_queue *queue);

/**
 * Function puts a message containing the function and
 * its argument in the queue
 * @param[in] queue - message queue
 * @param[in] fun - message function
 * @param[in] fun_arg - message function argument
 * @param[in] delayed - flag indicating the consumer should be notified
 * @retval return 0 if success, otherwise return -1 and sets the errno value
 */
int
xtm_fun_dispatch(struct xtm_queue *queue, void (*fun)(void *),
		 void *fun_arg, int delayed);

/**
 * Function returns queue file descriptor
 * @param[in] queue - xtm queue
 * @retval xtm queue file descriptor
 */
int
xtm_fd(struct xtm_queue *queue);

/**
 * Function retrieves messages from the queue and calls
 * functions contained in them
 * @param[in] queue - message queue
 * @retval return count of remaining messages in the queue
 */
unsigned
xtm_fun_invoke(struct xtm_queue *queue);

/**
 * Function retrieves messages from the queue and calls
 * functions contained in them, also flushing queue pipe
 * @param[in] queue - message queue
 * @retval return count of remaining messages in the queue,
 *         if successful, otherwise return -1 and set errno value
 */
int
xtm_fun_invoke_with_pipe_flushing(struct xtm_queue *queue);

/**
 * Helper function, invoked all queue messages
 * @param[in] queue - message queue
 * @retval return 0 if successful, otherwise return -1 and set errno value
 */
static inline int
xtm_fun_invoke_all(struct xtm_queue *queue)
{
	int rc = xtm_fun_invoke_with_pipe_flushing(queue);
	while (rc > 0)
		rc = xtm_fun_invoke(queue);
	return rc;
}

/**
 * Function puts a message pointer in the queue.
 * @param[in] queue - message queue
 * @param[in] msg - message pointer
 * @param[in] delayed - flag indicating the consumer should be notified
 * @retval return 0 if successful, otherwise return -1 and sets the errno value
 */
static inline int
xtm_msg_send(struct xtm_queue *queue, void *msg, int delayed)
{
	return xtm_fun_dispatch(queue, NULL, msg, delayed);
}

/**
 * Function gets up to count elements from the queue
 * and save them in msg pointer array.
 * @param[in] queue - message queue
 * @param[out] msg - msg pointer array
 * @param[in] count - maximum count of messages, that can be extracted
 * @retval return count of extracted messages
 */
unsigned
xtm_msg_recv(struct xtm_queue *queue, void **msg, unsigned count);

/**
 * Function gets up to count elements from the queue
 * and save them in msg pointer array, also flushing queue pipe.
 * @param[in] queue - message queue
 * @param[out] msg - msg pointer array
 * @param[in] count - maximum count of messages, that can be extracted
 * @retval return count of extracted messages
 */
int
xtm_msg_recv_with_pipe_flushing(struct xtm_queue *queue, void **msg,
				unsigned count);

/** \endcond public */

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

