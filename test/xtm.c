#include <xtm_api.h>

#include <pthread.h>
#include <time.h>
#include <sys/poll.h>
#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <string.h>

#include "unit.h"

enum {
	/** Count of messages sends from producer to consumer thread */
	XTM_MSG_MAX = 10000,
	/** Timeout waiting for thread completion */
	XTM_TEST_TIMEOUT = 2,
};

struct xtm_test_settings {
	/** Size of xtm queue */
	unsigned xtm_queue_size;
	/** Timeout between pushing to queue */
	unsigned xtm_push_timeout;
};

/** Message sent by the producer thread to consumer thread. */
struct xtm_msg {
	/**
	 * Thread, which create and send this message,
	 * used for test purpose.
	 */
	pthread_t owner;
};

/** Global pointer to xtm queue. */
static struct xtm_queue *xtm_queue;
/** Global producer thread id. */
static pthread_t producer;
/** Global consumer thread id. */
static pthread_t consumer;
/** Size of xtm queue, currently used for test */
static unsigned xtm_queue_size;
/** Timeout between pushing to queue, currently used for test */
static unsigned xtm_push_timeout;

static void
timer_handler(int signum)
{
	fail_unless(signum == SIGALRM);
	fail("timeout", "expired");
}

static void
start_test_timer(void)
{
	static struct sigaction sa;
	static struct itimerval timer;

	memset(&sa, 0, sizeof(sa));
	memset(&timer, 0, sizeof(timer));
	sa.sa_handler = &timer_handler;
	timer.it_value.tv_sec = XTM_TEST_TIMEOUT;
	timer.it_value.tv_usec = 0;
	timer.it_interval.tv_sec = 0;
	timer.it_interval.tv_usec = 1;
	fail_unless(sigaction(SIGALRM, &sa, NULL) == 0);
	fail_unless(setitimer(ITIMER_REAL, &timer, NULL) == 0);
}

static void
xtm_test_start(struct xtm_test_settings *settings)
{
	xtm_queue_size = settings->xtm_queue_size;
	xtm_push_timeout = settings->xtm_push_timeout;
	fail_unless((xtm_queue = xtm_queue_new(xtm_queue_size)) != NULL);
}

static void
xtm_test_finish(void)
{
	unsigned flags = 0;
	flags |= XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD |
		 XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD;
	fail_unless(xtm_queue_delete(xtm_queue, flags) == 0);
}

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

static int
sleep_for_n_microseconds(unsigned n_microseconds)
{
	struct timespec ts;
	ts.tv_sec = n_microseconds / 1000000;
	ts.tv_nsec = (n_microseconds % 1000000) * 1000;
	return nanosleep(&ts, NULL);
}

static int
create_and_push_fun(struct xtm_queue *queue, xtm_queue_fun_t fun)
{
	int fd = xtm_queue_producer_fd(xtm_queue);
	struct xtm_msg *msg =
		(struct xtm_msg *)malloc(sizeof(struct xtm_msg));
	if (msg == NULL)
		return -1;
	msg->owner = pthread_self();
	unsigned flags = XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS;
	while (xtm_queue_push_fun(queue, fun, msg, flags) != 0) {
		if (wait_for_fd(fd) <= 0)
			return -1;
		fail_unless(xtm_queue_consume(fd) == 0);
	}
	return xtm_queue_notify_consumer(queue);
}

static int
create_and_push_ptr(struct xtm_queue *queue)
{
	int fd = xtm_queue_producer_fd(xtm_queue);
	struct xtm_msg *msg =
		(struct xtm_msg *)malloc(sizeof(struct xtm_msg));
	if (msg == NULL)
		return -1;
	msg->owner = pthread_self();
	unsigned flags = XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS;
	while (xtm_queue_push_ptr(queue, msg, flags) != 0) {
		if (wait_for_fd(fd) <= 0)
			return -1;
		fail_unless(xtm_queue_consume(fd) == 0);
	}
	return xtm_queue_notify_consumer(queue);
}

static void
consumer_msg_f(void *arg)
{
	struct xtm_msg *msg = (struct xtm_msg *)arg;
	fail_unless(msg->owner == producer && pthread_self() == consumer);
	free(msg);
}

static void *
consumer_thread_push_and_pop_ptr(MAYBE_UNUSED void *arg)
{
	int fd = xtm_queue_consumer_fd(xtm_queue);
	unsigned received = 0;

	while (received < XTM_MSG_MAX) {
		fail_unless(wait_for_fd(fd) > 0);
		fail_unless(xtm_queue_consume(fd) == 0);
		void *ptr_array[XTM_MSG_MAX];
		unsigned rc = xtm_queue_pop_ptrs(xtm_queue, ptr_array, XTM_MSG_MAX);
		for (unsigned i = 0; i < rc; i++)
			consumer_msg_f(ptr_array[i]);
		received += rc;
		/* Try to notify producer again, if queue was full */
		if (xtm_queue_get_reset_was_full(xtm_queue))
			fail_unless(xtm_queue_notify_producer(xtm_queue) == 0);
	}

	fail_unless(xtm_queue_count(xtm_queue) == 0);
	return (void *)NULL;
}

static void *
consumer_thread_push_and_invoke_fun(MAYBE_UNUSED void *arg)
{
	int fd = xtm_queue_consumer_fd(xtm_queue);
	unsigned invoked = 0;

	while (invoked < XTM_MSG_MAX) {
		fail_unless(wait_for_fd(fd) > 0);
		fail_unless(xtm_queue_consume(fd) == 0);
		unsigned rc = xtm_queue_invoke_funs_all(xtm_queue);
		invoked += rc;
		/* Try to notify producer again, if queue was full */
		if (xtm_queue_get_reset_was_full(xtm_queue))
			fail_unless(xtm_queue_notify_producer(xtm_queue) == 0);
	}

	fail_unless(xtm_queue_count(xtm_queue) == 0);
	return (void *)NULL;
}

static void *
producer_thread_push_and_pop_ptr(MAYBE_UNUSED void *arg)
{
	for (unsigned msgcnt = 0; msgcnt < XTM_MSG_MAX; msgcnt++) {
		fail_unless(create_and_push_ptr(xtm_queue) == 0);
		fail_unless(sleep_for_n_microseconds(xtm_push_timeout) == 0);
	}

	return NULL;
}

static void *
producer_thread_push_and_invoke_fun(MAYBE_UNUSED void *arg)
{
	for (unsigned msgcnt = 0; msgcnt < XTM_MSG_MAX; msgcnt++) {
		fail_unless(create_and_push_fun(xtm_queue, consumer_msg_f) == 0);
		fail_unless(sleep_for_n_microseconds(xtm_push_timeout) == 0);
	}

	return NULL;
}

static void
xtm_push_and_invoke_fun_test(struct xtm_test_settings *settings)
{
	header();
	plan(0);

	xtm_test_start(settings);
	fail_unless(pthread_create(&producer, NULL,
				   producer_thread_push_and_invoke_fun,
				   NULL) == 0);
	fail_unless(pthread_create(&consumer, NULL,
				   consumer_thread_push_and_invoke_fun,
				   NULL) == 0);

	start_test_timer();
	fail_unless(pthread_join(producer, NULL) == 0);
	fail_unless(pthread_join(consumer, NULL) == 0);
	xtm_test_finish();

	check_plan();
	footer();
}

static void
xtm_push_and_pop_ptr_test(struct xtm_test_settings *settings)
{
	header();
	plan(0);

	xtm_test_start(settings);
	fail_unless(pthread_create(&producer, NULL,
				   producer_thread_push_and_pop_ptr,
				   NULL) == 0);
	fail_unless(pthread_create(&consumer, NULL,
				   consumer_thread_push_and_pop_ptr,
				   NULL) == 0);

	start_test_timer();
	fail_unless(pthread_join(producer, NULL) == 0);
	fail_unless(pthread_join(consumer, NULL) == 0);
	xtm_test_finish();

	check_plan();
	footer();
}

int main()
{
	header();
	plan(2 * 2 * 5);

	for (unsigned timeout = 0; timeout <= 1; timeout++) {
		for (unsigned size = 2; size <= 32; size *= 2) {
			struct xtm_test_settings settings;
			settings.xtm_push_timeout = timeout;
			settings.xtm_queue_size = size;
			xtm_push_and_invoke_fun_test(&settings);
			xtm_push_and_pop_ptr_test(&settings);
		}
	}

	int rc = check_plan();
	footer();
	return rc;
}
