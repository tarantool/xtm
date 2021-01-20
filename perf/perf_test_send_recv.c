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
#include "../xtm_api.h"
#include "common.h"

#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

static struct xtm_queue *producer_queue;
static struct xtm_queue *consumer_queue;
static int is_running = 0;
static unsigned long long msg_count = 0;

static void
process_all_messages(struct xtm_queue * queue)
{
	unsigned count = 0;
	do {
		void *msg[MSG_BATCH_COUNT];
		int rc = xtm_msg_recv_with_pipe_flushing(queue,
							 msg,
							 MSG_BATCH_COUNT,
							 &count);
		xtm_fail_unless(rc >= 0);
	} while (count > 0);
}

static void
alarm_sig_handler(int signum)
{
	(void)signum;
	__atomic_store_n(&is_running, 0, __ATOMIC_RELEASE);
}

static void
enqueue_message(void)
{
	if (consumer_queue == NULL)
		return;

	struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
	xtm_fail_unless(msg != NULL);
	if (xtm_msg_probe(consumer_queue) == 0) {
		msg->counter = msg_count++;
		xtm_fail_unless(xtm_msg_send(consumer_queue, msg, 0) == 0);
	}
	else {
		free(msg);
	}
}

static void *
consumer_thread_func(void *arg)
{
	(void)arg;
	xtm_fail_unless((consumer_queue =
			 xtm_create(XTM_TEST_QUEUE_SIZE)) != NULL);
	int consumer_pipe_fd = xtm_fd(consumer_queue);

	while (__atomic_load_n(&is_running, __ATOMIC_ACQUIRE) == 1) {
		fd_set readset;
		FD_ZERO(&readset);
		FD_SET(consumer_pipe_fd, &readset);
		int rc = select(consumer_pipe_fd + 1, &readset, NULL,
				NULL, NULL);
		if (rc < 0 && errno == EINTR)
			break;
		xtm_fail_unless(rc > 0);

		if (FD_ISSET(consumer_pipe_fd, &readset))
			process_all_messages(consumer_queue);
	}
	/*
	 * Flush queue
	 */
	process_all_messages(consumer_queue);
	return (void *)NULL;
}

static void *
producer_thread_func(void *arg)
{
	(void)arg;
	xtm_fail_unless((producer_queue =
			 xtm_create(XTM_TEST_QUEUE_SIZE)) != NULL);
	int producer_pipe_fd = xtm_fd(producer_queue);
	__atomic_store_n(&is_running, 1, __ATOMIC_RELEASE);

	while (__atomic_load_n(&is_running, __ATOMIC_ACQUIRE) == 1) {
		fd_set readset;
		FD_ZERO(&readset);
		FD_SET(producer_pipe_fd, &readset);
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 1;
		int rc = select(producer_pipe_fd + 1, &readset, NULL,
				NULL, &timeout);
		if (rc < 0 && errno == EINTR) {
			enqueue_message();
			break;
		}
		xtm_fail_unless(rc >= 0);
		if (rc == 0) {
			enqueue_message();
			continue;
		}
		if (FD_ISSET(producer_pipe_fd, &readset))
			process_all_messages(producer_queue);

		if (__atomic_load_n(&is_running, __ATOMIC_ACQUIRE) == 0)
			enqueue_message();
	}
	/*
	 * Flush queue
	 */
	process_all_messages(producer_queue);
	return (void *)NULL;
}

int main()
{
	pthread_t thread_1, thread_2;
	xtm_fail_unless(pthread_create(&thread_1, NULL,
				       producer_thread_func, (void *)1) == 0);
	while (__atomic_load_n(&is_running, __ATOMIC_ACQUIRE) != 1)
		;
	xtm_fail_unless(pthread_create(&thread_2, NULL,
				       consumer_thread_func, (void *)2) == 0);
	alarm(XTM_TEST_TIME);
	signal(SIGALRM, alarm_sig_handler);
	pthread_join(thread_1, NULL);
	pthread_join(thread_2, NULL);
	fprintf(stderr, "Perf test msg count %llu\n", msg_count);
	return EXIT_SUCCESS;
}