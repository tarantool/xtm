# xtm - a library for cross-thread communication with event loop intergation

The library provides the following facilities:

# xtm_queue

Opaque struct, that represent one-directional, one-reader-one-writer queue
implementation with eventloop integration ability.

## xtm_create

Allocation function, used to create instance of struct xtm_queue and return
its pointer or NULL with the following possible errno: ENOMEM, EINVAL, EMFILE,
ENFILE, ENODEV. Accepts the queue size as input, which must be a power of two.

## xtm_delete

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