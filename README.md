# xtm - a library for cross-thread messaging with event loop intergation

The library provides the following facilities:

# xtm_queue

Opaque struct, that represent one-directional, one-reader-one-writer queue
implementation with eventloop integration ability.

## xtm_queue_new

Allocation function, used to create instance of struct xtm_queue and return
its pointer or NULL in case of error. Accepts the queue size as input, which
must be a power of two.

## xtm_queue_delete

Deallocation function, used to free queue and close its internal fds.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
close(2), since it implies close of fds)

## xtm_msg_notify

Function for queue consumer notification.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
write(2), since it implies write to internal fd)

## xtm_msg_probe

Returns 0 if queue has space. Otherwise -1 with errno set to ENOBUFS.

## xtm_msg_count

Function returns current message count in queue.

## xtm_fun_dispatch

Function puts message containing the function and its argument to the queue.
If delayed == 0 notifies consumer.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
write(2), since it implies write to internal fd + ENOBUFS in case there
is no space in queue)

## xtm_fd

Returns file descriptor, that should be watched to become readable. When it
became readable, consumer should call one of the consumer functions:
xtm_msg_recv or xtm_fun_invoke.

## xtm_fun_invoke

Function retrieves messages from the queue and calls functions contained
in them. If flushed == 1, flushing queue pipe. Return count of invoked
functions on success. Otherwise -1 with errno set appropriately (as in
read(2), since it implies read to internal fd)

## xtm_msg_send

Function puts message to the queue. If delayed == 0 notifies consumer.
Returns 0 on success. Otherwise -1 with errno set appropriately (as in
write(2), since it implies write to internal fd + ENOBUFS in case there
is no space in queue)

## xtm_msg_recv

Function gets up to count elements from the queue. If flushed == 1 flushing
queue pipe. Return count of extracted messages on success. Otherwise -1 with
errno set appropriately (as in read(2), since it implies read to internal fd)

Examples
--------

Full example you can see in test repository, here is only simple example of
using library.

**Create xtm queue**

queue = xtm_queue_new(XTM_QUEUE_SIZE);

**Get xtm internal file descriptor to poll**

fd = xtm_fd(queue);

**Poll it in loop**

fd_set readset;
FD_ZERO(&readset);
FD_SET(fd, &readset);
select(fd + 1, &readset, NULL, NULL, NULL);

**Process messages according invoke pattern**

xtm_fun_invoke(queue, 1);

**Process messages according receive pattern**

void* msg[XTM_BATCH_MAX];
int rc = xtm_msg_recv(queue, msg, XTM_BATCH_MAX, 1)

**Destroy xtm queue, when it is no longer needed**

xtm_queue_delete(queue);

**Send message to other thread, using queue created in other thread**

while (xtm_msg_probe(queue) != 0)
	;
xtm_msg_send(queue, msg, 0);

**Dispatch function and it's argument to other thread**

while (xtm_msg_probe(queue) != 0)
	;
xtm_fun_dispatch(queue, msg, func, arg);
