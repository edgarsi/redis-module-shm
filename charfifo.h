/*
 * charfifo.h
 *
 *  Created on: Nov 8, 2016
 *      Author: Edgars
 */

#ifndef LOCKLESS_CHAR_FIFO_CHARFIFO_H_
#define LOCKLESS_CHAR_FIFO_CHARFIFO_H_

#include <stdlib.h>

typedef struct {
    size_t read_idx;
    size_t write_idx;
    size_t size;
} charfifo_header_t;

// Outputs the type of a struct having 'size' many chars in buffer.
// Example: typedef CHARFIFO(2000) t_my_buffer;
#define CHARFIFO(size)                                                         \
    volatile struct { /* volatile needed iff shared memory used */             \
        charfifo_header_t header;                                              \
        char buf[size + 1];                                                    \
    } __attribute__((aligned(8))) /* aligned for atomic instructions */

void CharFifo_Init(volatile void *charfifo, size_t size);

size_t CharFifo_FreeSpace(volatile void *charfifo);
void CharFifo_Write(volatile void *charfifo, const char *buf, size_t btw); // does not check free space!

size_t CharFifo_UsedSpace(volatile void *charfifo);
void CharFifo_Read(volatile void *charfifo, char *buf, size_t btr); // does not check used space!


#endif /* LOCKLESS_CHAR_FIFO_CHARFIFO_H_ */
