/*
 * charfifo.c
 *
 *  Created on: Oct 27, 2016
 *      Author: Edgars
 */


#include "charfifo.h"

#include <sys/types.h>
#include <string.h>
#include <stdatomic.h>


#define X(...)
//#define X printf


#define EXTRACT_HEADER(p) \
    charfifo_header_t *header = (charfifo_header_t*)charfifo;

#define EXTRACT_FIFO_BUF(p) \
    volatile char* fifo_buf = (char*)p + sizeof(charfifo_header_t);

inline static size_t aget(size_t *p)
{
    return atomic_load_explicit(p, memory_order_relaxed);
}

void CharFifo_Init(volatile void *charfifo, size_t size)
{
    EXTRACT_HEADER(charfifo);
    header->read_idx = 0;
    header->write_idx = 0;
    header->size = size;
    __sync_synchronize();
}

// Given a circular buffer of size 'size', how many bytes can 'start_idx' move
// forward until exactly 'stop_padding' bytes to reaching 'end_idx' are left.
inline static size_t WraparoundDiff(size_t start_idx, size_t end_idx, size_t size, size_t stop_padding)
{
    size_t end_idx_cpy = end_idx;
    ssize_t diff = (ssize_t) end_idx_cpy - start_idx - stop_padding;
    if (end_idx_cpy < start_idx + stop_padding) {
        diff = diff + size;
    }
    return diff;
}

size_t CharFifo_FreeSpace(volatile void *charfifo)
{
    EXTRACT_HEADER(charfifo);
    return WraparoundDiff(aget(&header->write_idx), aget(&header->read_idx), header->size, 1);
}

size_t CharFifo_UsedSpace(volatile void *charfifo)
{
    EXTRACT_HEADER(charfifo);
    return WraparoundDiff(aget(&header->read_idx), aget(&header->write_idx), header->size, 0);
}

/*inline void
clflush(volatile void *p)
{
    asm volatile ("clflush (%0)" :: "r"(p));
}*/

typedef enum {
    BUF_TO_FIFO,
    FIFO_TO_BUF
} transfer_direction_t;

static inline void MemcpyInDirection(char* fifo, char* buf, size_t bytes, transfer_direction_t direction)
{
    if (direction == BUF_TO_FIFO) {
        memcpy(fifo, buf, bytes);
    } else {
        memcpy(buf, fifo, bytes);
    }
}

static inline void Transfer(volatile char *v_fifo_buf, char *buf, size_t bytes, 
        volatile size_t *p_start_idx, size_t end_idx, size_t size, transfer_direction_t direction)
{
    X("%lld Transfer(start_idx=%d end_idx=%d bytes=%d direction=%d\n", ustime(), *p_start_idx, end_idx, bytes, direction);
    size_t start_idx = atomic_load_explicit(p_start_idx, memory_order_acquire);
    char* fifo_buf = (char*)v_fifo_buf; /* casting away volatile because buf can't change in the range being used. */
    if (start_idx >= end_idx) {
        size_t bytes_to_eob = size - start_idx;
        if (bytes_to_eob <= bytes) {
            /* Write needs to rotate start_idx to the buffer beginning */
            MemcpyInDirection(fifo_buf + start_idx, buf, bytes_to_eob, direction);
            start_idx = 0;
            bytes -= bytes_to_eob;
            buf += bytes_to_eob;
        }
    }
    MemcpyInDirection(fifo_buf + start_idx, buf, bytes, direction);
    start_idx += bytes;
    atomic_store_explicit(p_start_idx, start_idx, memory_order_release);
    /* Need to push out of L1 cache, for other CPUs to see the update now,
     * but the CPU seems to do a good enough job of that. There is no way to
     * push out to L2, only purge of all cache hierarchy, which is slow. 
     * Also, I don't even know which hierarchy is high enough to be seen
     * for all cores on all physical CPUs, for the system used... */
//    asm volatile ("mfence" ::: "memory");
//    clflush(&target->write_idx);
    X("%lld Transfer fin (start_idx=%d end_idx=%d bytes=%d direction=%d\n", ustime(), *p_start_idx, end_idx, bytes, direction);
}

void CharFifo_Write(volatile void *charfifo, const char *buf, size_t btw) {
    EXTRACT_HEADER(charfifo);
    EXTRACT_FIFO_BUF(charfifo);
    Transfer(fifo_buf, buf, btw, 
            &header->write_idx, 
            aget(&header->read_idx), 
            header->size, 
            BUF_TO_FIFO);
}

void CharFifo_Read(volatile void *charfifo, char *buf, size_t btr) {
    EXTRACT_HEADER(charfifo);
    EXTRACT_FIFO_BUF(charfifo);
    Transfer(fifo_buf, buf, btr, 
            &header->read_idx, 
            aget(&header->write_idx), 
            header->size, 
            FIFO_TO_BUF);
}

