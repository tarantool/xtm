# xtm - a library for cross-thread messaging with event loop intergation

The library provides the following facilities:

# xtm_queue

Opaque struct, that represent unidirectional, single-writer-single-reader queue
implementation with eventloop integration ability.

# xtm_queue_attr

Struct containing various flags for configuring the behavior of xtm library.

## xtm_queue_attr_create

Function, used to create xtm_queue_attr struct with default flags.
Return 0 on success, otherwise return -1.

## xtm_queue_attr_set
Function, used to set flag in xtm_queue_attr struct.

## xtm_queue_attr_destroy
Destoy, previously created xtm_queue_attr struct.

## xtm_queue_new

Allocation function, used to create instance of struct xtm_queue and return
its pointer or NULL in case of error. Accepts the queue size as input, which
must be a power of two, and previously created xtm_queue_attr struct, with
appropriate flags.

## xtm_queue_delete

Deallocation function, used to free queue and close its internal fds.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
close(2), since it implies close of fds)

## xtm_queue_notify_consumer

Function for queue consumer notification.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
write(2), since it implies write to internal fd)

## xtm_queue_probe

Returns 0 if queue has space. Otherwise -1 with errno set to ENOBUFS.

## xtm_queue_count

Function returns current message count in queue.

## xtm_queue_push_fun

Function push function and its argument to the queue.
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
Return count of invoked functions on success. Otherwise -1 with errno set
appropriately (as in write(2), since it implies write to internal fd).

## xtm_queue_push_ptr

Function зush pointer to the queue.
Returns 0 if queue has space. Otherwise -1 with errno set to ENOBUFS.

## xtm_queue_pop_ptrs

Function gets up to count elements from the queue and save them in pointer array.
Return count of extracted messages on success. Otherwise -1 with errno set
appropriately (as in write(2), since it implies write to internal fd).

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
struct xtm_queue_attr attr;
xtm_queue_attr_create(&attr);
xtm_queue_attr_set(&attr, XTM_QUEUE_NEED_TO_CLOSE_READFD);
xtm_queue_attr_set(&attr, XTM_QUEUE_PRODUCER_NEED_NOTIFICATIONS);
xtm_queue = xtm_queue_new(XTM_QUEUE_SIZE, &attr);
xtm_queue_attr_destroy(&attr);
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
while (cnt < count) {
	void *ptr_array[BATCH_COUNT_MAX];
	unsigned popcnt = (count - cnt >= BATCH_COUNT_MAX ?
		(unsigned)BATCH_COUNT_MAX : count - cnt);
	int rc = xtm_queue_pop_ptrs(xtm_queue, ptr_array,
				    popcnt);
	for (int i = 0; i < rc; i++)
		// do something with ptr_array[i]
	cnt += rc;
}
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
while (xtm_msg_probe(queue) != 0);
```
