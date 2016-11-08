/*
 * charfifo.c
 *
 *  Created on: Oct 27, 2016
 *      Author: Edgars
 */


#include "charfifo.h"

#include <sys/types.h>
#include <string.h>


#define X(...)
//#define X printf


#define EXTRACT_HEADER(p) \
    charfifo_header_t *header = (charfifo_header_t*)charfifo;

#define EXTRACT_FIFO_BUF(p) \
    volatile char* fifo_buf = (char*)p + sizeof(charfifo_header_t);

void CharFifo_Init(volatile void *charfifo, size_t size)
{
    EXTRACT_HEADER(charfifo);
    header->read_idx = 0;
    header->write_idx = 0;
    header->size = size;
}

// Given a circular buffer of size 'size', how many bytes can 'start_idx' move
// forward until 'stop_padding' bytes to reaching 'end_idx' are left.
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
    return WraparoundDiff(header->write_idx, header->read_idx, header->size, 1);
}

size_t CharFifo_UsedSpace(volatile void *charfifo)
{
    EXTRACT_HEADER(charfifo);
    return WraparoundDiff(header->read_idx, header->write_idx, header->size, 0);
}

/*inline void
clflush(volatile void *p)
{
    asm volatile ("clflush (%0)" :: "r"(p));
}*/

void CharFifo_Write(volatile void *charfifo, char *buf, size_t btw) {
    EXTRACT_HEADER(charfifo);
    EXTRACT_FIFO_BUF(charfifo);
    X("%lld bufWrite(read_ptr=%d write_ptr=%d btw=%d\n", ustime(), header->read_idx, header->write_idx, btw);
    size_t write_idx = header->write_idx;
    __sync_synchronize();
    char* target_buf = (char*)fifo_buf; /* casting away volatile because buf can't change in the range being used. */
    if (write_idx >= header->read_idx) {
        size_t bytes_to_eob = header->size - write_idx;
        if (bytes_to_eob <= btw) {
            /* Write needs to rotate target->write_idx to the buffer beginning */
            memcpy(target_buf + write_idx, buf, bytes_to_eob);
            write_idx = 0;
            btw -= bytes_to_eob;
            buf += bytes_to_eob;
        }
    }
    memcpy(target_buf + write_idx, buf, btw);
    write_idx += btw;
    __sync_synchronize();
    header->write_idx = write_idx;
    /* Need to push out of L1 cache, for other CPUs to see the update now,
     * but the CPU seems to do a good enough job of that. There is no way to
     * push out to L2, only purge of all cache hierarchy, which is slow. */
//    asm volatile ("mfence" ::: "memory");
//    clflush(&target->write_idx);
    X("%lld bufWrite fin (read_ptr=%d write_ptr=%d btw=%d\n", ustime(), header->read_idx, header->write_idx, btw);
}

/* TODO: Less duplication, maybe? */
void CharFifo_Read(volatile void *charfifo, char *buf, size_t btr) {
    EXTRACT_HEADER(charfifo);
    EXTRACT_FIFO_BUF(charfifo);
    X("%lld bufRead(read_ptr=%d write_ptr=%d btr=%d\n", ustime(), header->read_idx, header->write_idx, btr);
    size_t read_idx = header->read_idx;
    __sync_synchronize();
    char* source_buf = (char*)fifo_buf; /* casting away volatile because buf can't change in the range being used. */
    if (read_idx > header->write_idx) {
        size_t bytes_to_eob = header->size - read_idx;
        if (bytes_to_eob <= btr) {
            /* Read needs to rotate source->read_idx to the buffer beginning */
            memcpy(buf, source_buf + read_idx, bytes_to_eob);
            read_idx = 0;
            btr -= bytes_to_eob;
            buf += bytes_to_eob;
        }
    }
    memcpy(buf, source_buf + read_idx, btr);
    read_idx += btr;
    __sync_synchronize();
    header->read_idx = read_idx;
    /* Need to push out of L1 cache, for other CPUs to see the update now. */
//    asm volatile ("mfence" ::: "memory");
//    clflush(&source->read_idx);
    X("%lld bufRead fin (read_ptr=%d write_ptr=%d btr=%d\n", ustime(), header->read_idx, header->write_idx, btr);
}

