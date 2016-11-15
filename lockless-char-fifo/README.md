# Lockless char FIFO buffer.

* Fast - no malloc and other weird overhead.
* Concurrent read/write safe (but not multiple readers or multiple writers!) - atomics, fenced memory use.
* Shared memory safe - no pointers, volatile.
* Interrupt safe - stdatomic turns disables IRQs when on single CPU systems. It that's not OK with you, please request.

Requirements:
* GCC (of a not crazy old variety, at least 4.3.)
* -std=gnu11 or similar.
* x86 or x86-64 (although surely may work on some other systems)
* Fixed memory sizes for buffers.
