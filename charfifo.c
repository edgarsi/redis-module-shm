/*
 * charfifo.c
 *
 *  Created on: Oct 27, 2016
 *      Author: Edgars
 */


#include "charfifo.h"

#include <string.h>


#define X(...)
//#define X printf



#define SHARED_MEMORY_BUF_SIZE (1024-8-8-8)

typedef volatile struct sharedMemoryBuffer {
    char buf[SHARED_MEMORY_BUF_SIZE];
    size_t read_idx;
    size_t write_idx;
} sharedMemoryBuffer;

typedef volatile struct sharedMemory {
    sharedMemoryBuffer to_server;
    sharedMemoryBuffer to_client;
} sharedMemory;


/*inline void
clflush(volatile void *p)
{
    asm volatile ("clflush (%0)" :: "r"(p));
}*/

static size_t bufFreeSpace(sharedMemoryBuffer *target) {
    size_t read_idx = target->read_idx;
    ssize_t free = (ssize_t)read_idx - target->write_idx - 1;
    if (read_idx <= target->write_idx) {
        free = free + sizeof(target->buf);
    }
    return free;
}

static void bufWrite(sharedMemoryBuffer *target, char *buf, size_t btw) {
    X("%lld bufWrite(read_ptr=%d write_ptr=%d btw=%d\n", ustime(), target->read_idx, target->write_idx, btw);
    size_t write_idx = target->write_idx;
    __sync_synchronize();
    char* target_buf = (char*)target->buf; /* casting away volatile because buf can't change in the range being used. */
    if (write_idx >= target->read_idx) {
        size_t bytes_to_eob = sizeof(target->buf) - write_idx;
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
    target->write_idx = write_idx;
    /* Need to push out of L1 cache, for other CPUs to see the update now,
     * but the CPU seems to do a good enough job of that. There is no way to
     * push out to L2, only purge of all cache hierarchy, which is slow. */
//    asm volatile ("mfence" ::: "memory");
//    clflush(&target->write_idx);
    X("%lld bufWrite fin (read_ptr=%d write_ptr=%d btw=%d\n", ustime(), target->read_idx, target->write_idx, btw);
}

/* TODO: Less duplication, maybe? */
static size_t bufUsedSpace(sharedMemoryBuffer *source) {
    size_t write_idx = source->write_idx;
    ssize_t used = write_idx - source->read_idx;
    if (write_idx < source->read_idx) {
        used = used + sizeof(source->buf);
    }
    return used;
}

/* TODO: Less duplication, maybe? */
static void bufRead(sharedMemoryBuffer *source, char *buf, size_t btr) {
    X("%lld bufRead(read_ptr=%d write_ptr=%d btr=%d\n", ustime(), source->read_idx, source->write_idx, btr);
    size_t read_idx = source->read_idx;
    __sync_synchronize();
    char* source_buf = (char*)source->buf; /* casting away volatile because buf can't change in the range being used. */
    if (read_idx > source->write_idx) {
        size_t bytes_to_eob = sizeof(source->buf) - read_idx;
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
    source->read_idx = read_idx;
    /* Need to push out of L1 cache, for other CPUs to see the update now. */
//    asm volatile ("mfence" ::: "memory");
//    clflush(&source->read_idx);
    X("%lld bufRead fin (read_ptr=%d write_ptr=%d btr=%d\n", ustime(), source->read_idx, source->write_idx, btr);
}

