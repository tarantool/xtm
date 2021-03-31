#pragma once
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
/**
 * Lock free single consumer, single producer queue,
 * based on ring buffer.
 * Template parameter can be of any type for which the
 * copying operation is carried out quickly and correctly.
 */
template<class T>
struct xtm_scsp_queue {
	/*
	 * Next position to be written
	 */
	unsigned write;
	/*
	 * Next position to be read
	 */
	unsigned read;
	/*
	 * Circular buffer length
	 */
	unsigned len;
	/*
	 * Buffer contains objects
	 */
	T buffer[];
};

/**
 * Function that initializes queue.
 * @param[out] queue - queue of elements with type T
 * @param[in] size - size of queue, must be power of 2
 * @retval return 0 if success otherwise return -1
 */
template <class T>
static inline int
xtm_scsp_queue_init(struct xtm_scsp_queue<T> *queue, unsigned size)
{
	/* Ensure size is power of 2 */
	if (size & (size - 1))
		return -1;

	queue->write = 0;
	queue->read = 0;
	queue->len = size;
	return 0;
}

/**
 * Function adds num elements into the queue.
 * @param[in] queue - queue of elements with type T
 * param[in] data - array of elements to add
 * param[in] num - count of elements to add
 * @retval return the number of elements actually written.
 */
template <class T>
static inline unsigned
xtm_scsp_queue_put(struct xtm_scsp_queue<T> *queue, T *data, unsigned num)
{
	unsigned i = 0;
	unsigned queue_write = queue->write;
	unsigned new_write = queue_write;
	unsigned queue_read =
		__atomic_load_n(&queue->read, __ATOMIC_ACQUIRE);

	for (i = 0; i < num; i++) {
		new_write = (new_write + 1) & (queue->len - 1);
		if (new_write == queue_read)
			break;
		queue->buffer[queue_write] = data[i];
		queue_write = new_write;
	}
	__atomic_store_n(&queue->write, queue_write, __ATOMIC_RELEASE);
	return i;
}

/**
 * Function gets up to num elements from the queue.
 * @param[in] queue - queue of elements with type T
 * @param[out] data - array to save elements
 * @param[in] num - count of elements to get
 * @retval return the number of elements actually read.
 */
template <class T>
static inline unsigned
xtm_scsp_queue_get(struct xtm_scsp_queue<T> *queue, T *data, unsigned num)
{
	unsigned i = 0;
	unsigned new_read = queue->read;
	unsigned queue_write =
		__atomic_load_n(&queue->write, __ATOMIC_ACQUIRE);

	for (i = 0; i < num; i++) {
		if (new_read == queue_write)
			break;
		data[i] = queue->buffer[new_read];
		new_read = (new_read + 1) & (queue->len - 1);
	}
	__atomic_store_n(&queue->read, new_read, __ATOMIC_RELEASE);
	return i;
}

/**
 * Function get the num of available elements in the queue
 * @param[in] queue - queue of elements with type T
 * @retval return num of available elements in the queue
 */
template <class T>
static inline unsigned
xtm_scsp_queue_free_count(struct xtm_scsp_queue<T> *queue)
{
	unsigned queue_write =
		__atomic_load_n(&queue->write, __ATOMIC_ACQUIRE);
	unsigned queue_read =
		__atomic_load_n(&queue->read, __ATOMIC_ACQUIRE);
	return (queue_read - queue_write - 1) & (queue->len - 1);
}

/**
 * Function get the num of elements in the queue
 * @param[in] queue - queue of elements with type T
 * @retval return num of elements in the queue
 */
template <class T>
static inline unsigned
xtm_scsp_queue_count(struct xtm_scsp_queue<T> *queue)
{
        unsigned queue_write =
		__atomic_load_n(&queue->write, __ATOMIC_ACQUIRE);
        unsigned queue_read =
		__atomic_load_n(&queue->read, __ATOMIC_ACQUIRE);
        return (queue->len + queue_write - queue_read) & (queue->len - 1);
}

