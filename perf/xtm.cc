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
#include <xtm_api.h>

#include <pthread.h>
#include <sys/poll.h>
#include <errno.h>
#include <benchmark/benchmark.h>

#define fail(expr, result) do {					\
	fprintf(stderr, "Test failed: %s is %s at %s:%d, "	\
			"in function '%s'\n", expr, result,	\
			__FILE__, __LINE__, __func__);		\
	exit(-1);						\
} while (0)
#define fail_unless(expr) if (!(expr)) fail(#expr, "false")

enum {
	/** Maximum message count in xtm queue. */
	XTM_TEST_QUEUE_SIZE = 64 * 1024,
	/**
	 * Maximum number of messages sent without notification.
	 * Upper bound for the test parameter.
	 */
	BATCH_COUNT_MAX = 1024,
	/** Count of messages in test */
	TEST_MSG_COUNT = 1024 * 1024,
};

struct xtm_msg {
	unsigned number;
};

/**
 * Global pointer to xtm queue.
 */
static struct xtm_queue *xtm_queue;
/** Consumer thread id */
static pthread_t consumer_thread;
/**
 * Global message preallocated pointers array, we used it,
 * because we want to test performance of xtm, not of malloc.
 */
static struct xtm_msg *xtm_msg_arr[TEST_MSG_COUNT];

static int
wait_for_fd(int fd)
{
	int rc;
	struct pollfd pfds[1];
	pfds[0].fd = fd;
	pfds[0].events = POLLIN;
	while ((rc = poll(pfds, 1, -1)) < 0 && errno == EINTR)
		;
	return (rc <= 0 ? rc : pfds[0].revents & POLLIN);
}

static void
consumer_msg_func(void *arg)
{
	struct xtm_msg *msg = (struct xtm_msg *)arg;
	xtm_msg_arr[msg->number] = NULL;
	free(msg);
}

static void *
consumer_thread_push_and_invoke_fun(void *arg)
{
	unsigned invoked = 0;
	int fd = xtm_queue_consumer_fd(xtm_queue);
	(void)arg;

	while (invoked < TEST_MSG_COUNT) {
		fail_unless(wait_for_fd(fd) > 0);
		fail_unless(xtm_queue_consume(fd) == 0);
		unsigned rc = xtm_queue_invoke_funs_all(xtm_queue);
		invoked += rc;
		/* Try to notify producer again, if queue was full */
		if (xtm_queue_get_reset_was_full(xtm_queue))
			fail_unless(xtm_queue_notify_producer(xtm_queue) == 0);
	}
	return NULL;
}

static void *
consumer_thread_push_and_pop_ptr(void *arg)
{
	unsigned received = 0;
	int fd = xtm_queue_consumer_fd(xtm_queue);
	(void)arg;

	while (received < TEST_MSG_COUNT) {
		fail_unless(wait_for_fd(fd) > 0);
		fail_unless(xtm_queue_consume(fd) == 0);
		unsigned count = xtm_queue_count(xtm_queue);
		unsigned cnt = 0;
		while (cnt < count) {
			void *ptr_array[BATCH_COUNT_MAX];
			unsigned rc = xtm_queue_pop_ptrs(xtm_queue, ptr_array,
							 BATCH_COUNT_MAX);
			for (unsigned i = 0; i < rc; i++)
				consumer_msg_func(ptr_array[i]);
			cnt += rc;
			/* Try to notify producer again, if queue was full */
			if (xtm_queue_get_reset_was_full(xtm_queue))
				fail_unless(xtm_queue_notify_producer(xtm_queue) == 0);
		}
		received += cnt;
	}
	return NULL;
}

/**
 * The function makes all the necessary preparation for performance testing.
 * Creates consumer thread, producer and consumer xtm queues, sets all global
 * variables and etc.
 * @param[in] state - benchmark state
 * @param[in] thread_func - consumer thread function
 * @retval return true if success, otherwise return false
 */
static bool
setup_xtm_perf_test(benchmark::State& state, void *(*thread_func)(void *))
{
	unsigned i;
	for (i = 0; i < TEST_MSG_COUNT; i++) {
		xtm_msg_arr[i] = (struct xtm_msg *)
			malloc(sizeof(xtm_msg));
		if (xtm_msg_arr[i] == NULL) {
			state.SkipWithError("Failed to allocate message");
			goto fail;
		}

	}
	xtm_queue = xtm_queue_new(XTM_TEST_QUEUE_SIZE);
	if (xtm_queue == NULL) {
		state.SkipWithError("Failed to create xtm queue");
		goto fail;
	}
	if (pthread_create(&consumer_thread, NULL, thread_func, NULL) < 0) {
		unsigned flags = 0;
		flags |= XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD |
			 XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD;
		xtm_queue_delete(xtm_queue, flags);
		state.SkipWithError("Failed to create consumer thread");
		goto fail;
	}
	return true;

fail:
	for (unsigned j = 0; j < i; j++) {
		free(xtm_msg_arr[j]);
		xtm_msg_arr[j] = NULL;
	}
	return false;
}

/**
 * Function cleanups all test resources and checks status
 * @param[in] state - benchmark state
 */
static void
teardown_xtm_perf_test(benchmark::State& state, unsigned number)
{
	unsigned flags = 0;
	flags |= XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD |
		 XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD;
	if (number < TEST_MSG_COUNT)
		pthread_cancel(consumer_thread);
	pthread_join(consumer_thread, NULL);
	if (xtm_queue_count(xtm_queue) != 0)
		state.SkipWithError("Xtm queue is not empty");
	if (xtm_queue_delete(xtm_queue, flags) != 0)
		state.SkipWithError("Failed to delete xtm queue");
	for (unsigned i = 0; i < TEST_MSG_COUNT; i++) {
		if (xtm_msg_arr[i] != NULL) {
			state.SkipWithError("Not all msg processed");
			free(xtm_msg_arr[i]);
			xtm_msg_arr[i] = NULL;
		}
	}
}

static void
create_test_arguments(benchmark::internal::Benchmark* b)
{
	for (unsigned batch = 1; batch <= BATCH_COUNT_MAX; batch *= 4)
		b->Args({batch});
}

static void
xtm_push_and_invoke_funs(benchmark::State& state)
{
	unsigned number = 0;
	unsigned batch = state.range(0);
	if (!setup_xtm_perf_test(state, consumer_thread_push_and_invoke_fun))
		return;
	int fd = xtm_queue_producer_fd(xtm_queue);

	for (auto _ : state) {
		/*
		 * Creates new message, notifed consumer
		 * thread only every batch message.
		 */
		xtm_msg_arr[number]->number = number;
		unsigned flags = XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS;
		while (xtm_queue_push_fun(xtm_queue, consumer_msg_func,
					  xtm_msg_arr[number], flags) != 0) {
			if (wait_for_fd(fd) <= 0) {
				state.SkipWithError("Failed to wait for fd");
				break;
			}
			if (xtm_queue_consume(fd) != 0) {
				state.SkipWithError("Failed to consumer fd");
				break;
			}
			/*
			 * Here is a little chance that we wake up because of
			 * false notification. So we try to push and if it's
			 * fails sleep again.
			 */
		}
		if (number % batch == 0 || number == TEST_MSG_COUNT - 1) {
			if (xtm_queue_notify_consumer(xtm_queue) != 0) {
				state.SkipWithError("Failed to notify "
						    "consumer thread");
				break;
			}
		}
		number++;
	}

	state.SetItemsProcessed(number);
	teardown_xtm_perf_test(state, number);
}
BENCHMARK(xtm_push_and_invoke_funs)
	->Iterations(TEST_MSG_COUNT)
	->Apply(create_test_arguments);

static void
xtm_push_and_pop_ptrs(benchmark::State& state)
{
	unsigned number = 0;
	unsigned batch = state.range(0);
	if (!setup_xtm_perf_test(state, consumer_thread_push_and_pop_ptr))
		return;
	int fd = xtm_queue_producer_fd(xtm_queue);

	for (auto _ : state) {
		/*
		 * Creates new message, notifed consumer
		 * thread only every batch message.
		 */
		xtm_msg_arr[number]->number = number;
		unsigned flags = XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS;
		while (xtm_queue_push_ptr(xtm_queue, xtm_msg_arr[number],
					  flags) != 0) {
			if (wait_for_fd(fd) <= 0) {
				state.SkipWithError("Failed to wait for fd");
				break;
			}
			if (xtm_queue_consume(fd) != 0) {
				state.SkipWithError("Failed to consumer fd");
				break;
			}
			/*
			 * Here is a little chance that we wake up because of
			 * false notification. So we try to push and if it's
			 * fails sleep again.
			 */
		}
		if (number % batch == 0 || number == TEST_MSG_COUNT - 1) {
			if (xtm_queue_notify_consumer(xtm_queue) != 0) {
				state.SkipWithError("Failed to notify "
						    "consumer thread");
				break;
			}
		}
		number++;
	}

	state.SetItemsProcessed(number);
	teardown_xtm_perf_test(state, number);
}
BENCHMARK(xtm_push_and_pop_ptrs)
	->Iterations(TEST_MSG_COUNT)
	->Apply(create_test_arguments);

BENCHMARK_MAIN();
