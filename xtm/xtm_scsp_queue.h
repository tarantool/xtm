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

template <class T>
struct xtm_scsp_queue_read_iterator;

/**
 * Lock free single consumer, single producer queue,
 * based on ring buffer.
 * Template parameter can be of any type for which the
 * copying operation is carried out quickly and correctly.
 */
template<class T>
struct xtm_scsp_queue {
	friend class xtm_scsp_queue_read_iterator<T>;
	/**
	 * Init scsp queue struct.
	 * @param[in] size - size of queue ring buffer, must be power of two,
	 *                   for fast calculation of read/write position, using
	 *                   '& (size - 1' operation.
	 * @retval 0 if success, otherwise return -1.
	 */
	int
	create(unsigned size) {
		/* Ensure size is power of 2 */
		if (size & (size - 1))
			return -1;

		write = 0;
		read = 0;
		len = size;
		return 0;
	}
	/**
	 * Add num elements into queue
	 * @param[in] data - array of elements to add
	 * @param[in] num - count of elements to add
	 * @retval number of elements actually written.
	 */
	unsigned
	put(T *data, unsigned num) {
		unsigned i;
		unsigned queue_write = write;
		unsigned new_write = queue_write;
		unsigned len_minus_1 = len - 1;
		unsigned queue_read = __atomic_load_n(&read, __ATOMIC_ACQUIRE);

		for (i = 0; i < num; i++) {
			new_write = (new_write + 1) & len_minus_1;
			if (new_write == queue_read)
				break;
			buffer[queue_write] = data[i];
			queue_write = new_write;
		}
		__atomic_store_n(&write, queue_write, __ATOMIC_RELEASE);
		return i;
	}
	/**
 	 * Get num of available elements in the queue
	 * @retval num of available elements in the queue
	 */
	 unsigned
	free_count(void) {
		unsigned queue_write = __atomic_load_n(&write, __ATOMIC_ACQUIRE);
		unsigned queue_read = __atomic_load_n(&read, __ATOMIC_ACQUIRE);
		return (queue_read - queue_write - 1) & (len - 1);
	}
	/**
	 * Get num of elements in the queue
	 * @retval return num of elements in the queue
	 */
	unsigned
	count(void) {
		unsigned queue_write = __atomic_load_n(&write, __ATOMIC_ACQUIRE);
		unsigned queue_read = __atomic_load_n(&read, __ATOMIC_ACQUIRE);
		return (len + queue_write - queue_read) & (len - 1);
	}
private:
	/** Next position to be written */
	unsigned write;
	/** Next position to be read */
	unsigned read;
	/** Circular buffer length */
	unsigned len;
	/** Buffer contains objects */
	T buffer[];
};

/**
 * Read iterator for single consumer, single producer queue.
 * Template parameter must be same, as in queue to iterate.
 */
template <class T>
struct xtm_scsp_queue_read_iterator {
	/**
	 * Create new queue read iterator
	 * @param[in] queue - queue to iterate
	 */
	void
	begin(struct xtm_scsp_queue<T> *q) {
		queue = q;
		read_pos = queue->read;
		end_of_read = __atomic_load_n(&queue->write, __ATOMIC_ACQUIRE);
	}
	/**
	 * Read next element from queue.
	 * @retval next element from queue.
	 */
	const T*
	read(void) {
		if (read_pos != end_of_read) {
			const T *rc = &queue->buffer[read_pos];
			read_pos = (read_pos + 1) & (queue->len - 1);
			return rc;
		}
		return nullptr;
	}
	/**
	 * Store new read index to the queue.
	 */
	void
	end(void) {
		__atomic_store_n(&queue->read, read_pos, __ATOMIC_RELEASE);
	}
private:
	/**
	 * Current read position for this iterator.
	 */
	unsigned read_pos;
	/**
	 * Last position to be read.
	 */
	unsigned end_of_read;
	/**
	 * Single consumer, single producer queue to iterate.
	 */
	struct xtm_scsp_queue<T> *queue;
};

