# Lockless char FIFO buffer.

* Fast - no malloc and other weird overhead.
* Multithreading safe - atomics, fenced memory use.
* Shared memory safe - no pointers.

Requirements:
* GCC (of a not crazy old variety, at least 4.3.)
* x86 or x86-64.
* Fixed memory size.
* Not interrupt safe! <-? depends on the atomics used... C11 stdatomics looks interrupt safe...
