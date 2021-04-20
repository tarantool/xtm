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
#include "xtm/xtm_api.h"

#include <pthread.h>
#include <stdlib.h>

#include <benchmark/benchmark.h>

enum {
	/**
	 * Maximum message count in xtm queue.
	 */
	XTM_TEST_QUEUE_SIZE = 64 * 1024,
	/**
	 * Maximum number of messages sent without notification.
	 * Upper bound for the test parameter.
	 */
	BATCH_COUNT_MAX = 1024,
	/**
	 * Count of test iterations, also count of sending messages.
	 */
	XTM_ITERATION_COUNT = 500000,
};

struct msg {
	/**
	 * Flag, indicates whether to notify the thread, to
	 * which this message is intended.
	 */
	int delayed;
};

/**
 * Producer thread queue
 */
static struct xtm_queue *producer_queue;
/**
 * Consumer thread queue
 */
static struct xtm_queue *consumer_queue;
/**
 * Consumer thread id
 */
static pthread_t consumer_thread;
/**
 * Flag, if equal to zero, the consumer thread shuts down
 */
static int work;
/**
 * Message counter
 */
static unsigned counter;
/**
 * Status of consumer thread, if not equal to EXIT_SUCCESS
 * at the end of the test, then the test failed with an error.
 */
static unsigned status = EXIT_SUCCESS;

static inline int
xtm_fun_invoke_all(struct xtm_queue *queue)
{
	int rc = xtm_fun_invoke(queue, 1);
	while (rc >= 0 && xtm_msg_count(queue) > 0)
		rc = xtm_fun_invoke(queue, 0);
	return (rc >= 0 ? 0 : -1);
}

/**
 * Function called in the consumer thread to
 * process each received message in case of dispatch/invoke test.
 * @param[in] arg - pointer to received message
 */
static void
consumer_msg_func(void *arg)
{
	struct msg *msg = (struct msg *)arg;
	while(xtm_msg_probe(producer_queue) < 0)
		;
	if (xtm_fun_dispatch(producer_queue, free, msg, msg->delayed) != 0) {
		free(msg);
		status = EXIT_FAILURE;
	}
}

/**
 * Function called in the consumer thread to
 * process each received message in case of send/recv test.
 * @param[in] arg - pointer to received message
 */
static void
consumer_process_msg(void *arg)
{
	struct msg *msg = (struct msg *)arg;
	while(xtm_msg_probe(producer_queue) < 0)
		;
	if (xtm_msg_send(producer_queue, msg, msg->delayed) != 0) {
		free(msg);
		status = EXIT_FAILURE;
	}
}

/**
 * Helper function, called to process all messages in xtm queue.
 * @param[in] queue - xtm queue
 * @param[in] func - function called for each message in the queue,
 *                   message passed to the function as a parameter.
 * @retval 0 if success, otherwise return -1
 */
static int
process_all_msg(struct xtm_queue *queue, void (*func)(void *))
{
	void *msg[BATCH_COUNT_MAX];
	int rc = xtm_msg_recv(queue, msg, BATCH_COUNT_MAX, 1);
	if (rc < 0)
		return -1;
	for (int i = 0; i < rc; i++)
		func(msg[i]);
	while (xtm_msg_count(queue) != 0) {
		unsigned rc = xtm_msg_recv(queue, msg, BATCH_COUNT_MAX, 0);
		for (unsigned int i = 0; i < rc; i++)
			func(msg[i]);
	}
	return 0;
}

/**
 * Function creates new message
 * @param[in] delayed - flag, indicates whether to notify the thread, to
 *                      which this message is intended.
 * @retval return poiter to a new messageif success, otherwise return NULL.
 */
static struct msg *
create_new_msg(int delayed)
{
	struct msg *msg = (struct msg *) malloc(sizeof(struct msg));
	if (msg == NULL)
		return NULL;
	msg->delayed = delayed;
	return msg;
}

/**
 * Function waits on xtm queue file descriptor,
 * until notification from other thread.
 * @param[in] queue - xtm queue
 * @retval return > 0 if success.
 */
static bool
wait_for_event(struct xtm_queue *queue)
{
	fd_set readset;
	int fd = xtm_fd(queue);
	FD_ZERO(&readset);
	FD_SET(fd, &readset);
	if (select(fd + 1, &readset, NULL, NULL, NULL) < 0)
		return -1;
	return FD_ISSET(fd, &readset);
}

/**
 * Consumer thread function in sace of dispatch/invoke test
 * Set status to EXIT_FAILURE is case of some error.
 */
static void *
consumer_thread_dispatch_invoke(void *)
{
	while (__atomic_load_n(&work, __ATOMIC_ACQUIRE) == 1) {
		/*
		 * Wait for a new message
		 */
		if (wait_for_event(consumer_queue) <= 0) {
			status = EXIT_FAILURE;
			goto finish;
		}
		/*
		 * Invoke all message functions
		 */
		if (xtm_fun_invoke_all(consumer_queue) != 0) {
			status = EXIT_FAILURE;
			goto finish;
		}
	}
	/*
	 * Invoke all remaining message functions
	 */
	if (xtm_fun_invoke_all(consumer_queue) != 0) {
		status = EXIT_FAILURE;
		goto finish;
	}
	/*
	 * Check that queue is empty
	 */
	if (xtm_msg_count(consumer_queue) != 0)
		status = EXIT_FAILURE;
finish:
	return (void *)NULL;
}

/**
 * Consumer thread function in sace of send/recv test
 * Set status to EXIT_FAILURE is case of some error.
 */
static void *
consumer_thread_send_recv(void *)
{
	while (__atomic_load_n(&work, __ATOMIC_ACQUIRE) == 1) {
		/*
		 * Wait for a new message
		 */
		if (wait_for_event(consumer_queue) <= 0) {
			status = EXIT_FAILURE;
			goto finish;
		}
		/*
		 * Process all incoming messages
		 */
		if (process_all_msg(consumer_queue,
				    consumer_process_msg) < 0) {
			status = EXIT_FAILURE;
			goto finish;
		}
	}
	/*
	 * Process all remaning incoming messages
	 */
	if (process_all_msg(consumer_queue, consumer_process_msg) < 0) {
		status = EXIT_FAILURE;
		goto finish;
	}
finish:
	return (void *)NULL;
}

/**
 * The function makes all the necessary preparation for performance testing.
 * Creates consumer thread, producer and consumer xtm queues, sets all global
 * variables and etc.
 * @param[in] state - benchmark state
 * @param[in] thread_func - consumer thread function
 */
static void
setup_xtm_perf_test(benchmark::State& state, void *(*thread_func)(void *))
{
	counter = 0;
	status = EXIT_SUCCESS;
	producer_queue = xtm_queue_new(XTM_TEST_QUEUE_SIZE);
	if (producer_queue == NULL)
		state.SkipWithError("Failed to create xtm queue");
	consumer_queue = xtm_queue_new(XTM_TEST_QUEUE_SIZE);
	if (consumer_queue == NULL) {
		xtm_queue_delete(producer_queue);
		state.SkipWithError("Failed to create xtm queue");
	}

	__atomic_store_n(&work, 1, __ATOMIC_RELEASE);
	if (pthread_create(&consumer_thread, NULL, thread_func, NULL) < 0) {
		__atomic_store_n(&work, 0, __ATOMIC_RELEASE);
		xtm_queue_delete(consumer_queue);
		xtm_queue_delete(producer_queue);
		state.SkipWithError("Failed to create consumer thread");
	}
}

/**
 * Function cleanups all test resources and checks status
 * @param[in] state - benchmark state
 */
static void
teardown_xtm_perf_test(benchmark::State& state)
{
	if (xtm_msg_count(consumer_queue) != 0 ||
	    xtm_msg_count(producer_queue) != 0)
		state.SkipWithError("Xtm queue is not empty");
	if (xtm_queue_delete(consumer_queue) != 0)
		state.SkipWithError("Failed to delete xtm queue");
	if (xtm_queue_delete(producer_queue) != 0)
		state.SkipWithError("Failed to delete xtm queue");
	if (status != EXIT_SUCCESS)
		state.SkipWithError("Error in consumer thread");
	state.SetItemsProcessed(counter);
}

/**
 * Function for graceful teardown consumer thread
 * @param[in] state - benchmark state
 */
static void
teardown_consumer_thread(benchmark::State &state)
{
	__atomic_store_n(&work, 0, __ATOMIC_RELEASE);
	if (xtm_msg_notify(consumer_queue) != 0)
		state.SkipWithError("Failed to notify consumer queue");
	pthread_join(consumer_thread, NULL);
}

static void
batch_count(benchmark::internal::Benchmark* b)
{
	for (unsigned batch = 1; batch <= BATCH_COUNT_MAX; batch *= 4)
		b->Args({batch});
}

static void
xtm_dispatch_invoke(benchmark::State& state)
{
	unsigned batch = state.range(0);
	setup_xtm_perf_test(state, consumer_thread_dispatch_invoke);

	for (auto _ : state) {
		/*
		 * Creates new message, notifed consumer
		 * thread only every batch message.
		 */
		struct msg *msg = create_new_msg(counter % batch);
		if (msg == NULL) {
			state.SkipWithError("Failed to create message");
			teardown_consumer_thread(state);
			break;
		}
		while(xtm_msg_probe(consumer_queue) < 0)
			;
		/*
		 * Dispact message and function to process
		 * message into consumer thread.
		 */
		if (xtm_fun_dispatch(consumer_queue, consumer_msg_func,
				     msg, counter % batch) != 0) {
			state.SkipWithError("Failed to dispatch "
					    "consumer function");
			teardown_consumer_thread(state);
			break;
		}
		counter++;
		/*
		 * Checks that there is some messages in producer queue
		 * and invokes their functions.
		 */
		if (xtm_msg_count(producer_queue) &&
		    xtm_fun_invoke_all(producer_queue) != 0) {
			state.SkipWithError("Failed to invoke functions");
			teardown_consumer_thread(state);
			break;
		}
		/*
		 * Last iteration - teardown consumer thread and
		 * invokes all remaining messages.
		 */
		if (counter == XTM_ITERATION_COUNT) {
			teardown_consumer_thread(state);
			if (xtm_msg_count(producer_queue) &&
			    xtm_fun_invoke_all(producer_queue) != 0) {
				state.SkipWithError("Failed to invoke "
						    "functions");
				break;
			}
		}
	}

	teardown_xtm_perf_test(state);
}
BENCHMARK(xtm_dispatch_invoke)
	->Iterations(XTM_ITERATION_COUNT)->Apply(batch_count);

static void
xtm_send_recv(benchmark::State& state)
{
	unsigned batch = state.range(0);
	setup_xtm_perf_test(state, consumer_thread_send_recv);

	for (auto _ : state) {
		/*
		 * Creates new message, notifed consumer thread
		 * only every batch message
		 */
		struct msg *msg = create_new_msg(counter % batch);
		if (msg == NULL) {
			state.SkipWithError("Failed to create message");
			teardown_consumer_thread(state);
			break;
		}
		while(xtm_msg_probe(consumer_queue) < 0)
			;
		/*
		 * Sned message to consumer thread
		 */
		if (xtm_msg_send(consumer_queue, msg, counter % batch) != 0) {
			state.SkipWithError("Failed to send message to "
					    "consumer thread");
			teardown_consumer_thread(state);
			break;
		}
		counter++;
		/*
		 * Checks that there is some messages in producer queue
		 * and processes them.
		 */
		if (xtm_msg_count(producer_queue) &&
		    process_all_msg(producer_queue, free) < 0) {
			state.SkipWithError("Failed to process msg");
			teardown_consumer_thread(state);
			break;
		}
		/*
		 * Last iteration - teardown consumer thread and
		 * processes all remaining messages.
		 */
		if (counter == XTM_ITERATION_COUNT) {
			teardown_consumer_thread(state);
			if (xtm_msg_count(producer_queue) &&
			    process_all_msg(producer_queue, free) < 0) {
				state.SkipWithError("Failed to process msg");
				break;
			}
		}
	}

	teardown_xtm_perf_test(state);
}
BENCHMARK(xtm_send_recv)
	->Iterations(XTM_ITERATION_COUNT)->Apply(batch_count);

BENCHMARK_MAIN();
