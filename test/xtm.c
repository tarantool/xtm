#include "xtm/xtm_api.h"
#include "unit.h"

#include <pthread.h>
#include <sys/poll.h>

enum {
	/**
	 * Size of xtm queue, specially small to simulate a
	 * situation when there is no space in the queue
	 */
	XTM_QUEUE_SIZE = 2,
	/** Count of messages sends from producer to consumer thread */
	XTM_MSG_MAX = 10000
};

/** Message sent by the producer thread to consumer thread. */
struct xtm_msg {
	/**
	 * Thread, which create and send this message,
	 * used for test purpose.
	 */
	pthread_t owner;
};

/**
 * Global pointer to xtm queue.
 */
static struct xtm_queue *xtm_queue;
/**
 * Global producer thread id.
 */
static pthread_t producer;
/**
 * Global consumer thread id.
 */
static pthread_t consumer;
/**
 * Array of pending messages, which was not pushed in qeueu,
 * because it's was full. Producer thread wait's for notification, and
 * send them after it.
 */
static struct xtm_msg *xtm_msg_pending[XTM_MSG_MAX];
/**
 * First valid position in xtm_msg_pending array.
 */
static unsigned pending_start_pos;
/**
 * Position after last valid position in xtm_msg_pending array.
 */
static unsigned pending_finish_pos;

static void
xtm_test_start(void)
{
	unsigned flags = 0;
	flags |= XTM_QUEUE_NEED_TO_CLOSE_READFD;
	flags |= XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS;
	fail_unless((xtm_queue = xtm_queue_new(XTM_QUEUE_SIZE, flags)) != NULL);
	pending_start_pos = pending_finish_pos = 0;
}

static void
xtm_test_finish(void)
{
	fail_unless(xtm_queue_delete(xtm_queue) == 0);
}

static int
create_and_push_fun(struct xtm_queue *queue, xtm_queue_fun_t fun)
{
	struct xtm_msg *msg =
		(struct xtm_msg *)malloc(sizeof(struct xtm_msg));
	if (msg == NULL)
		return -1;
	msg->owner = pthread_self();
	if (xtm_queue_push_fun(queue, fun, msg) != 0) {
		/*
		 * If there is no free space in queue, we save message
		 * in supporting array, and send it later, when consumer
		 * thread notify us about free space.
		 */
		xtm_msg_pending[pending_finish_pos++] = msg;
		return 0;
	}
	return xtm_queue_notify_consumer(queue);
}

static int
create_and_push_ptr(struct xtm_queue *queue)
{
	struct xtm_msg *msg =
		(struct xtm_msg *)malloc(sizeof(struct xtm_msg));
	if (msg == NULL)
		return -1;
	msg->owner = pthread_self();
	if (xtm_queue_push_ptr(queue, msg) != 0) {
		xtm_msg_pending[pending_finish_pos++] = msg;
		return 0;
	}
	return xtm_queue_notify_consumer(queue);
}

static int
wait_for_fd(int fd, int timeout)
{
	struct pollfd pfds[1];
	pfds[0].fd = fd;
	pfds[0].events = POLLIN;
	int rc = poll(pfds, 1, timeout);
	if (rc <= 0)
		return rc;
	fail_unless((pfds[0].revents & POLLIN) != 0);
	return rc;
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
	bool is_need_to_notify_again = false;

	while (received < XTM_MSG_MAX) {
		fail_unless(wait_for_fd(fd, -1) > 0);
		fail_unless(xtm_queue_consume(fd) == 0);
		void *ptr_array[XTM_MSG_MAX];
		int rc = xtm_queue_pop_ptrs(xtm_queue, ptr_array, XTM_MSG_MAX);
		if (rc < 0) {
			/*
			 * If an error occurs, function returns -(count of
			 * ptrs saved in ptr_array + 1). So we can calsulate
			 *  count of ptrs saved in ptr_array as -(rc + 1).
			 */
			rc = -(rc + 1);
			is_need_to_notify_again = true;
		}
		for (int i = 0; i < rc; i++)
			consumer_msg_f(ptr_array[i]);
		received += rc;
		/* Try to notify producer again, if it's fails - panic */
		if (is_need_to_notify_again) {
			fail_unless(xtm_queue_notify_producer(xtm_queue) == 0);
			is_need_to_notify_again = false;
		}
	}

	fail_unless(xtm_queue_count(xtm_queue) == 0);
	return (void *)NULL;
}

static void *
consumer_thread_push_and_invoke_fun(MAYBE_UNUSED void *arg)
{
	int fd = xtm_queue_consumer_fd(xtm_queue);
	unsigned invoked = 0;
	bool is_need_to_notify_again = false;

	while (invoked < XTM_MSG_MAX) {
		fail_unless(wait_for_fd(fd, -1) > 0);
		fail_unless(xtm_queue_consume(fd) == 0);
		int rc = xtm_queue_invoke_funs_all(xtm_queue);
		if (rc < 0) {
			/*
			 * If an error occurs, function returns -(count of
			 * invoked functions + 1). So we can calsulate count
			 * of invoked functions as -(rc + 1).
			 */
			rc = -(rc + 1);
			is_need_to_notify_again = true;
		}
		invoked += rc;
		/* Try to notify producer again, if it's fails - panic */
		if (is_need_to_notify_again) {
			fail_unless(xtm_queue_notify_producer(xtm_queue) == 0);
			is_need_to_notify_again = false;
		}
	}

	fail_unless(xtm_queue_count(xtm_queue) == 0);
	return (void *)NULL;
}

static void *
producer_thread_push_and_pop_ptr(MAYBE_UNUSED void *arg)
{
	/*
	 * Get fd to get notifications for free space in xtm_queue.
	 */
	int fd = xtm_queue_producer_fd(xtm_queue);
	int msgcnt = 0;

	while (msgcnt < XTM_MSG_MAX || pending_start_pos < pending_finish_pos) {
		/*
		 * In case msgcnt < XTM_MSG_MAX, we create message, otherwise we
		 * just wait for the notification, that the queue is not full and
		 * push the remaining messages in it.
		 */
		int timeout = (msgcnt < XTM_MSG_MAX ? 0 : -1);
		int rc = wait_for_fd(fd, timeout);
		fail_unless(rc >= 0);
		/*
		 * If rc == 0, than timeout expired and we creates
		 * and pushes new pointer.
		 */
		if (rc == 0) {
			rc = create_and_push_ptr(xtm_queue);
			fail_unless(rc == 0);
			msgcnt++;
			continue;
		}
		/*
		 * rc > 0, means that consumer thread notify producer
		 * thread about free space in queue. Try to send all
		 * pending messages.
		 */
		fail_unless(xtm_queue_consume(fd) == 0);
		while (pending_start_pos < pending_finish_pos) {
			unsigned pos = pending_start_pos;
			if (xtm_queue_push_ptr(xtm_queue,
					       xtm_msg_pending[pos]) < 0)
				break;
			fail_unless(xtm_queue_notify_consumer(xtm_queue) == 0);
			pending_start_pos++;
		}
	}

	return NULL;
}

static void *
producer_thread_push_and_invoke_fun(MAYBE_UNUSED void *arg)
{
	/*
	 * Get fd to get notifications for free space in xtm_queue.
	 */
	int fd = xtm_queue_producer_fd(xtm_queue);
	int msgcnt = 0;

	while (msgcnt < XTM_MSG_MAX || pending_start_pos < pending_finish_pos) {
		/*
		 * In case msgcnt < XTM_MSG_MAX, we create message, otherwise we
		 * just wait for the notification, that the queue is not full and
		 * push the remaining messages in it.
		 */
		int timeout = (msgcnt < XTM_MSG_MAX ? 0 : -1);
		int rc = wait_for_fd(fd, timeout);
		fail_unless(rc >= 0);
		/*
		 * If rc == 0, than timeout expired and we creates
		 * and pushes new function.
		 */
		if (rc == 0) {
			rc = create_and_push_fun(xtm_queue, consumer_msg_f);
			fail_unless(rc == 0);
			msgcnt++;
			continue;
		}
		/*
		 * rc > 0, means that consumer thread notify producer
		 * thread about free space in queue. Try to send all
		 * pending messages.
		 */
		fail_unless(xtm_queue_consume(fd) == 0);
		while (pending_start_pos < pending_finish_pos) {
			unsigned pos = pending_start_pos;
			if (xtm_queue_push_fun(xtm_queue, consumer_msg_f,
					       xtm_msg_pending[pos]) < 0)
				break;
			fail_unless(xtm_queue_notify_consumer(xtm_queue) == 0);
			pending_start_pos++;
		}
	}

	return NULL;
}

static void
xtm_push_and_invoke_fun_test(void)
{
	header();
	plan(0);

	xtm_test_start();
	fail_unless(pthread_create(&producer, NULL,
				   producer_thread_push_and_invoke_fun,
				   NULL) == 0);
	fail_unless(pthread_create(&consumer, NULL,
				   consumer_thread_push_and_invoke_fun,
				   NULL) == 0);
	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);
	xtm_test_finish();

	check_plan();
	footer();
}

static void
xtm_push_and_pop_ptr_test(void)
{
	header();
	plan(0);

	xtm_test_start();
	fail_unless(pthread_create(&producer, NULL,
				   producer_thread_push_and_pop_ptr,
				   NULL) == 0);
	fail_unless(pthread_create(&consumer, NULL,
				   consumer_thread_push_and_pop_ptr,
				   NULL) == 0);
	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);
	xtm_test_finish();

	check_plan();
	footer();
}

int main()
{
	header();
	plan(2);

	xtm_push_and_invoke_fun_test();
	xtm_push_and_pop_ptr_test();

	int rc = check_plan();
	footer();
	return rc;
}
