#include "xtm/xtm_api.h"
#include "unit.h"

#include <pthread.h>

enum {
	XTM_MODULE_SIZE = 1024,
	XTM_MSG_MAX = 100,
	XTM_BATCH_MAX = 10,
};

enum {
	CONSUMER_THREAD_WORK,
	CONSUMER_THREAD_PENDING_STOPPED,
	CONSUMER_THREAD_STOPPED,
};

struct xtm_msg {
	pthread_t owner;
	int msgcnt;
};

struct xtm_queue *p2c;
struct xtm_queue *c2p;
pthread_t producer;
pthread_t consumer;
int work;

static void
init(void)
{
	p2c = c2p = NULL;
	work = CONSUMER_THREAD_WORK;
}

static int
create_and_dispatch_msg(struct xtm_queue *q, void (*func)(void *), int msgcnt)
{
	struct xtm_msg *msg = (struct xtm_msg *)malloc(sizeof(struct xtm_msg));
	if (msg == NULL)
		return -1;
	msg->owner = pthread_self();
	msg->msgcnt = msgcnt;
	while (xtm_msg_probe(q) != 0)
		;
	return xtm_fun_dispatch(q, func, msg, 0);
}

static int
create_and_send_msg(struct xtm_queue *q, MAYBE_UNUSED void (*func)(void *),
		    int msgcnt)
{
	struct xtm_msg *msg = (struct xtm_msg *)malloc(sizeof(struct xtm_msg));
	if (msg == NULL)
		return -1;
	msg->owner = pthread_self();
	msg->msgcnt = msgcnt;
	while (xtm_msg_probe(q) != 0)
		;
	return xtm_msg_send(q, msg, 0);
}

static int
xtm_fun_invoke_all(struct xtm_queue *queue)
{
	int rc = xtm_fun_invoke(queue, 1);
	while (rc >= 0 && xtm_msg_count(queue) > 0)
		rc = xtm_fun_invoke(queue, 1);
	return (rc >= 0 ? 0 : -1);
}

static void
producer_process_m(struct xtm_msg *msg)
{
	fail_unless(msg->owner == consumer && pthread_self() == producer);
	free(msg);
}

static void
consumer_process_m(void *arg)
{
	struct xtm_msg *msg = (struct xtm_msg *)arg;
	fail_unless(msg->owner == producer && pthread_self() == consumer);
	msg->owner = pthread_self();
	while (xtm_msg_probe(p2c) != 0)
		;
	if (msg->msgcnt == XTM_MSG_MAX - 1) {
		__atomic_store_n(&work, CONSUMER_THREAD_PENDING_STOPPED,
				 __ATOMIC_RELEASE);
	}
	fail_unless(xtm_msg_send(p2c, msg, 0) == 0);
}

static int
xtm_fun_recv_all(struct xtm_queue *queue)
{
	void *msg[XTM_BATCH_MAX];
	while (xtm_msg_count(queue) != 0) {
		int rc = xtm_msg_recv(queue, msg, XTM_BATCH_MAX, 1);
		if (rc < 0)
			return -1;
		for (int i = 0; i < rc; i++) {
			if (pthread_self() == producer)
				producer_process_m(msg[i]);
			else if (pthread_self() == consumer)
				consumer_process_m(msg[i]);
			else
				fail_unless(0);
		}
	}
	return 0;
}

static int
wait_for_fd(int fd, struct timeval *timeout)
{
	fd_set readset;
	FD_ZERO(&readset);
	FD_SET(fd, &readset);
	return select(fd + 1, &readset, NULL, NULL, timeout);
}

static void
producer_msg_f(void *arg)
{
	struct xtm_msg *msg = (struct xtm_msg *)arg;
	fail_unless(msg->owner == consumer && pthread_self() == producer);
	free(msg);
}

static void
consumer_msg_f(void *arg)
{
	struct xtm_msg *msg = (struct xtm_msg *)arg;
	fail_unless(msg->owner == producer && pthread_self() == consumer);
	msg->owner = pthread_self();
	while (xtm_msg_probe(p2c) != 0)
		;
	if (msg->msgcnt == XTM_MSG_MAX - 1) {
		__atomic_store_n(&work, CONSUMER_THREAD_PENDING_STOPPED,
				 __ATOMIC_RELEASE);
	}
	fail_unless(xtm_fun_dispatch(p2c, producer_msg_f, msg, 0) == 0);
}

#define CONSUMER_THREAD_F(send_or_dispatch, recv_or_invoke)     		\
static void *   								\
consumer_thread_##send_or_dispatch##_##recv_or_invoke(MAYBE_UNUSED void *arg)   \
{       									\
	fail_unless((c2p = xtm_queue_new(XTM_MODULE_SIZE)) != NULL);       	\
	int fd = xtm_fd(c2p);   						\
										\
	while (! __atomic_load_n(&p2c, __ATOMIC_SEQ_CST))       		\
		;       							\
										\
	while (__atomic_load_n(&work, __ATOMIC_ACQUIRE) ==      		\
	       CONSUMER_THREAD_WORK) {  					\
		int rc = wait_for_fd(fd, NULL); 				\
		fail_unless(rc > 0);   						\
		fail_unless(xtm_fun_##recv_or_invoke##_all(c2p) == 0);  	\
	}									\
										\
	fail_unless(xtm_fun_##recv_or_invoke##_all(c2p) == 0);  		\
	fail_unless(xtm_msg_count(c2p) == 0);   				\
	fail_unless(xtm_queue_delete(c2p) == 0);      				\
	__atomic_store_n(&work, CONSUMER_THREAD_STOPPED, __ATOMIC_RELEASE);     \
	return NULL;    							\
}
CONSUMER_THREAD_F(dispatch, invoke)
CONSUMER_THREAD_F(send, recv)

#define PRODUCER_THREAD_F(send_or_dispatch, recv_or_invoke)     		\
static void *   								\
producer_thread_##send_or_dispatch##_##recv_or_invoke(MAYBE_UNUSED void *arg)   \
{       									\
	fail_unless((p2c = xtm_queue_new(XTM_MODULE_SIZE)) != NULL);       	\
	int fd = xtm_fd(p2c);    						\
	int msgcnt = 0; 							\
										\
	while (! __atomic_load_n(&c2p, __ATOMIC_SEQ_CST))       		\
		;       							\
										\
	while (msgcnt < XTM_MSG_MAX) {  					\
		struct timeval timeout; 					\
		timeout.tv_sec = 0;     					\
		timeout.tv_usec = 5000; 					\
		int rc = wait_for_fd(fd, &timeout);     			\
		fail_unless(rc >= 0);   					\
		if (rc == 0) {  						\
			fail_unless(create_and_##send_or_dispatch##_msg(c2p,    \
					consumer_msg_f, msgcnt) == 0);  	\
			msgcnt++;        					\
			continue;       					\
		}       							\
		fail_unless(xtm_fun_##recv_or_invoke##_all(p2c) == 0);   	\
	}       								\
										\
	while (__atomic_load_n(&work, __ATOMIC_ACQUIRE) !=      		\
	       CONSUMER_THREAD_STOPPED) 					\
		;       							\
										\
	fail_unless(xtm_fun_##recv_or_invoke##_all(p2c) == 0);  		\
	fail_unless(xtm_msg_count(p2c) == 0);   				\
	fail_unless(xtm_queue_delete(p2c) == 0);      				\
	return NULL;    							\
}
PRODUCER_THREAD_F(dispatch, invoke)
PRODUCER_THREAD_F(send, recv)

static void
xtm_dispatch_invoke_test(void)
{
	header();
	plan(0);

	init();
	fail_unless(pthread_create(&producer, NULL,
				   producer_thread_dispatch_invoke,
				   NULL) == 0);
	fail_unless(pthread_create(&consumer, NULL,
				   consumer_thread_dispatch_invoke,
				   NULL) == 0);
	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);

	check_plan();
	footer();
}

static void
xtm_send_recv_test(void)
{
	header();
	plan(0);

	init();
	fail_unless(pthread_create(&producer, NULL,
				   producer_thread_send_recv,
				   NULL) == 0);
	fail_unless(pthread_create(&consumer, NULL,
				   consumer_thread_send_recv,
				   NULL) == 0);
	pthread_join(producer, NULL);
	pthread_join(consumer, NULL);

	check_plan();
	footer();
}

int main()
{
	header();
	plan(2);

	xtm_dispatch_invoke_test();
	xtm_send_recv_test();

	int rc = check_plan();
	footer();
	return rc;
}
