# xtm - a library for cross-thread messaging with event loop intergation

The library provides the following facilities:

# xtm_queue

Opaque struct, that represent unidirectional, single-writer-single-reader queue
implementation with eventloop integration ability.

## xtm_queue_new

Allocation function, used to create instance of struct xtm_queue and return
its pointer or NULL in case of error. Accepts the queue size as input, which
must be a power of two, and flags, defining library behavior.

## xtm_queue_delete

Deallocation function, used to free queue and close its internal fds.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
close(2), since it implies close of fds)

## xtm_queue_notify_consumer

Function for queue consumer notification.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
write(2), since it implies write to internal fd)

## xtm_queue_notify_producer
Function for queue producer notification.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
write(2), since it implies write to internal fd)

## xtm_queue_probe

Returns 0 if queue has space. Otherwise -1 with errno set to ENOBUFS.

## xtm_queue_count

Function returns current message count in queue.

## xtm_queue_push_fun

Function push function and its argument to the queue. This function does not
notify the consumer thread, but only push to the queue. To notify the consumer
thread you must call xtm_queue_notify_consumer. The less often you notify, the
greater the performance, but the greater the latency.
Returns 0 if queue has space. Otherwise -1 with errno set to ENOBUFS.

## xtm_queue_consumer_fd

Returns file descriptor, that should be watched by consumer thread to
become readable. When it became readable, consumer should call one of
the consumer functions: xtm_queue_pop_ptrs or xtm_queue_invoke_funs_all.

## xtm_queue_producer_fd
Return file descriptor, that should be watched by producer thread to
become readable. When it became readable, producer may push to queue,
being sure that there is a place in it.

## xtm_queue_invoke_funs_all

Function calls all functions contained in the queue.
Return count of invoked functions on success. Otherwise -(count + 1) with errno
set appropriately (as in write(2), since it implies write to internal fd).

## xtm_queue_push_ptr

Function зush pointer to the queue. This function does not notify the consumer
thread, but only push to the queue. To notify the consumer thread you must call
xtm_queue_notify_consumer. The less often you notify, the greater the performance,
but the greater the latency.
Returns 0 if queue has space. Otherwise -1 with errno set to ENOBUFS.

## xtm_queue_pop_ptrs

Function gets up to count elements from the queue and save them in pointer array.
Return count of extracted messages on success. Otherwise -(count + 1) with errno
set appropriately (as in write(2), since it implies write to internal fd).

## xtm_queue_consume

Function flush queue pipe, according to file descriptor passed to this function.
Return 0 on success. Otherwise -1 with errno set appropriately (as in read(2),
since it implies read from internal fd).

Examples
--------

Full example you can see in test or perf repositories, here is only simple
example of using library.

** Allocate xtm queue with appropriate flags **

```c
unsigned flags = 0;
flags |= XTM_QUEUE_NEED_TO_CLOSE_READFD;
flags |= XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS;
xtm_queue = xtm_queue_new(XTM_QUEUE_SIZE, flags);
```

** Get internal fd on consumer side and pool it in the loop **

```c
int fd = xtm_queue_consumer_fd(xtm_queue);
fd_set readset;
FD_ZERO(&readset);
FD_SET(fd, &readset);
select(fd + 1, &readset, NULL, NULL, NULL);
```

** Invoke functions in case when producer thread push functions **

```c
xtm_queue_invoke_funs_all(xtm_queue);
```

** Pop pointers to data in case when producer thread push pointers **

```c
unsigned count = xtm_queue_count(xtm_queue);
unsigned cnt = 0;
bool is_need_to_notify_again = false;
while (cnt < count) {
	void *ptr_array[BATCH_COUNT_MAX];
	int rc = xtm_queue_pop_ptrs(xtm_queue, ptr_array,
				    BATCH_COUNT_MAX);
	if (rc < 0) {
			/*
			 * If an error occurs, function returns -(count of
			 * ptrs saved in ptr_array + 1). So we can calsulate
			 * count of ptrs saved in ptr_array as -(rc + 1).
			 */
			rc = -(rc + 1);
			is_need_to_notify_again = true;
	}
	for (int i = 0; i < rc; i++)
		// do something with ptr_array[i]
	cnt += rc;
}
/* Try to notify producer again, if it's fails - panic */
if (is_need_to_notify_again &&
    xtm_queue_notify_producer(xtm_queue) != 0)
	panic();
```

** Delete xtm queue, when it is no longer needed **

```c
xtm_queue_delete(xtm_queue);
```

** Get internal fd on producer side, push fun to consumer thread **

```c
int fd = xtm_queue_producer_fd(xtm_queue);
if (xtm_queue_push_fun(xtm_queue, function, pointer) < 0) {
try_next:
	fd_set readset;
	FD_ZERO(&readset);
	FD_SET(fd, &readset);
	// Wait fror consumer notification, that queue is not full
	select(fd + 1, &readset, NULL, NULL, NULL);
	xtm_queue_consume(fd);
	if (xtm_queue_push_fun(xtm_queue, function, pointer) < 0)
		goto try_next;
}
```

** Get internal fd on producer side, push ptr to consumer thread **

```c
if (xtm_queue_push_ptr(xtm_queue, pointer) < 0) {
try_next:
	fd_set readset;
	FD_ZERO(&readset);
	FD_SET(fd, &readset);
	// Wait fror consumer notification, that queue is not full
	select(fd + 1, &readset, NULL, NULL, NULL);
	xtm_queue_consume(fd);
	if (xtm_queue_push_ptr(xtm_queue, pointer) < 0)
		goto try_next;
}
```

** Wait in the loop until queue became not full **

```c
while (xtm_queue_probe(queue) != 0)
	;
```
