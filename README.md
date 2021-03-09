# xtm - a library for cross-thread messaging with event loop integration

The library provides the following facilities:

# xtm_queue

Opaque struct, that represents unidirectional, single-writer-single-reader queue
implementation with event loop integration ability.

## xtm_queue_new

Allocation function, used to create instance of `struct xtm_queue` and return
its pointer or `NULL` in case of error. Accepts the queue size as input, which
must be a power of two.

## xtm_queue_delete

Deallocation function, used to free queue and close its internal fds, in case
when appropriate flags (`XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD` and
`XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD`) are passed to this function.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
`close(2)`, since it implies close of fds)

## xtm_queue_notify_consumer

Function for queue consumer notification.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
`write(2)`, since it implies write to internal fd)

## xtm_queue_notify_producer

Function for queue producer notification.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
`write(2)`, since it implies write to internal fd)

## xtm_queue_probe

Returns 0 if queue has space. Otherwise -1 with errno set to `ENOBUFS`.

## xtm_queue_count

Function returns current message count in queue.

## xtm_queue_push_fun

Function puts message, which contains function and its argument to the queue.
This function does not notify the consumer thread, but only push to the queue.
To notify the consumer thread you must call `xtm_queue_notify_consumer`. The less
often you notify, the greater the performance, but the greater the latency.
Function accepts flags, which define it's behaviour:
`XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS` - if this flag is set, producer sets
special flag in queue internals, if push fails. Consumer can check this flag,
using `xtm_queue_get_reset_was_full` function and notify producer.
Returns 0 if queue has space. Otherwise -1 with errno set to `ENOBUFS`.

## xtm_queue_consumer_fd

Returns file descriptor, that should be watched by consumer thread to
become readable. When it became readable, consumer should call one of
the consumer functions: `xtm_queue_pop_ptrs` or `xtm_queue_invoke_funs_all`.

## xtm_queue_producer_fd

Return file descriptor, that should be watched by producer thread to become
readable in case when `xtm_queue_push_fun` or `xtm_queue_push_ptr` fails. When
it became readable, producer may try push to the queue again. Very rarely,
there may be a situation (because of race between producer and consumer
threads), when the descriptor became readable, but queue is still full. In this
case producer thread needs to poll this descriptor again.

## xtm_queue_invoke_funs_all

Function calls all functions contained in the queue at the time this function is
called. We do not call those functions, that the producer thread can add at the
moment, when this function is already called, in order to prevent an infinite loop
and hunger. Also, if the queue size is very large, then hunger is still possible,
since this function does not allow limiting the number of called functions.
If producer thread pushes functions with `XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS`
flag, user should retrieve and reset "producer failed to put an item
in the queue and expects notification" flag using `xtm_queue_get_reset_was_full`.
If this flag was true, user should notify producer thread (see simple example
further).
Return count of invoked functions.

## xtm_queue_push_ptr

Function puts message, which contains pointer to the queue. This function does not
notify the consumer thread, but only pushes to the queue. To notify the consumer thread
you must call `xtm_queue_notify_consumer`. The less often you notify, the greater the
performance, but the greater the latency.
Function accepts flags, which define it's behaviour:
`XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS` - if this flag is set, producer sets
special flag in queue internals, if push fails. Consumer can check this flag,
using `xtm_queue_get_reset_was_full` function and notify producer.
Returns 0 if queue has space. Otherwise -1 with errno set to `ENOBUFS`.

## xtm_queue_pop_ptrs

Function gets up to count elements from the queue and saves them in pointer array.
If producer thread pushes ptrs with `XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS`
flag, user should retrieve and reset "producer failed to put an item
in the queue and expects notification" flag using `xtm_queue_get_reset_was_full`.
If this flag was true, user should notify producer thread (see simple example further).
Return count of extracted messages.

## xtm_queue_consume

Function reads from internal queue pipe, according to file descriptor passed
to this function.
Return 0 on success. Otherwise -1 with errno set appropriately (as in `read(2)`,
since it implies read from internal fd).

## xtm_queue_get_reset_was_full

Function retrieves and resets "producer failed to put an item in the queue and
expects notification" flag.

Examples
--------

Full example you can see in test or perf folders, here is only simple
example of using library.

** Allocate xtm queue **

```c
xtm_queue = xtm_queue_new(XTM_QUEUE_SIZE);
```

** Get internal fd on consumer side and pool it in the loop **

```c
int fd = xtm_queue_consumer_fd(xtm_queue);
struct pollfd pfds[1];
pfds[0].fd = fd;
pfds[0].events = POLLIN;
int rc;
while ((rc = poll(pfds, 1, -1)) < 0 && errno == EINTR)
	;
if (rc <= 0 || (pfds[0].revents & POLLIN) == 0)
	//error handling
```

** Invoke functions in case when producer thread push functions **

```c
xtm_queue_invoke_funs_all(xtm_queue);
```

** Pop pointers to data in case when producer thread push pointers **

```c
unsigned count = xtm_queue_count(xtm_queue);
unsigned cnt = 0;
while (cnt < count) {
	void *ptr_array[BATCH_COUNT_MAX];
	unsigned rc = xtm_queue_pop_ptrs(xtm_queue, ptr_array,
					 BATCH_COUNT_MAX);
	for (unsigned i = 0; i < rc; i++)
		// do something with ptr_array[i]
	cnt += rc;
}
/* Try to notify producer again if queue was full, if it's fails - panic */
if (xtm_queue_get_reset_was_full(xtm_queue) &&
    xtm_queue_notify_producer(xtm_queue) != 0)
	panic();
```

** Delete xtm queue, when it is no longer needed, close all internal fds **

```c
unsigned flags = 0;
flags |= XTM_QUEUE_MUST_CLOSE_PRODUCER_READFD |
         XTM_QUEUE_MUST_CLOSE_CONSUMER_READFD;
xtm_queue_delete(xtm_queue, flags);
```

** Get internal fd on producer side, push fun to consumer thread **

```c
unsigned flags = XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS;
int fd = xtm_queue_producer_fd(xtm_queue);
while (xtm_queue_push_fun(xtm_queue, function, pointer, flags) != 0) {
	struct pollfd pfds[1];
	pfds[0].fd = fd;
	pfds[0].events = POLLIN;
	int rc;
	// Wait for consumer notification, that queue is not full
	while ((rc = poll(pfds, 1, -1)) < 0 && errno == EINTR)
		;
	if (rc <= 0 || (pfds[0].revents & POLLIN) == 0)
		//error handling
	xtm_queue_consume(fd);
}
```

** Get internal fd on producer side, push ptr to consumer thread **

```c
unsigned flags = XTM_QUEUE_PRODUCER_NEEDS_NOTIFICATIONS;
int fd = xtm_queue_producer_fd(xtm_queue);
while (xtm_queue_push_ptr(xtm_queue, pointer, flags) < 0) {
	struct pollfd pfds[1];
	pfds[0].fd = fd;
	pfds[0].events = POLLIN;
	int rc;
	// Wait for consumer notification, that queue is not full
	while ((rc = poll(pfds, 1, -1)) < 0 && errno == EINTR)
		;
	if (rc <= 0 || (pfds[0].revents & POLLIN) == 0)
		//error handling
	xtm_queue_consume(fd);
}
```

** Wait in the loop until queue became not full **

```c
while (xtm_queue_probe(queue) != 0)
	;
```
