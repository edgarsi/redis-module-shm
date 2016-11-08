# Lockless char FIFO buffer.

* Fast - no malloc and other weird overhead.
* Concurrent read/write safe (but not multiple readers or multiple writers!) - atomics, fenced memory use.
* Shared memory safe - no pointers, volatile.

Requirements:
* GCC (of a not crazy old variety, at least 4.3.)
* x86 or x86-64
* Fixed memory sizes for buffers.
* Not interrupt safe! <-? depends on the atomics used... C11 stdatomics looks interrupt safe...
