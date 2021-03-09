# xtm - library that implements the API for inter-thread communication

The library provides the following facilities:

# xtm_queue

An opaque queue for sending messages to another thread.

## xtm_create

Function for creating a unidirectional message queue between threads.
Accepts the queue size as input, which must be a power of two.
If success return new xtm_queue struct, otherwise return NULL,
and sets errno value.

## xtm_delete

Function for  deleting xtm_queue. Frees all accosiated resources.
If success return 0, otherwise return -1, and sets errno value.

## xtm_msg_notify

Function for queue consumer notification.
If success return 0, otherwise return -1, and sets errno value.

## xtm_msg_probe

Function return 0 if there is free space in queue, otherwise return -1,
and set errno value to ENOBUFS.

## xtm_msg_count
Function returns current message count in xtm queue.

## xtm_fun_dispatch

Function puts message containing the function and its argument in the queue.
If delayed == 0 notifies consumer queue.
If success return 0, otherwise return -1 and sets the errno value.

## xtm_fd

Function return xtm_queue file descriptor. Consumer thread must poll it,
to receive a notification about messages in queue.

## xtm_fun_invoke

Function retrieves messages from the queue and calls functions contained in them.
Return count of remaining messages in the queue.

## xtm_fun_invoke_with_pipe_flushing

Same as the previous function, but also flushing queue pipe.
Return count of remaining messages in the queue, otherwise
return -1, and sets errno value.

## xtm_fun_invoke_all

Helper function, invoked all queue messages
Return 0 if success, otherwise return -1 and set errno value.

## xtm_msg_send

Function  puts message in the queue. If delayed == 0 notifies consumer queue.
If success return 0, otherwise return -1 and sets the errno value.

## xtm_msg_recv

Function gets up to count elements from the queue. Return count
of extracted messages

## xtm_msg_recv_with_pipe_flushing

Same as the previous function, but also flushing queue pipe.
Gets up to count elements from the queue. Return count
of extracted messages.
