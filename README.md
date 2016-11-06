# Shared memory support for Redis

![Average latency: TCP=0.226ms Unix_socket=0.170ms Shared_memory=0.013ms](docs/latency_barchart.png)

When your client is on the **same host as redis server**, you can get much better latency than going through the TCP stack. And surely you want the **best latency you can get**. Redis folk know this - that's why unix socket support exists. But you can do a lot better with shared memory! See the benchmark at [#Performance](#performance) section below.

# Summary and usage

Redis module for shared memory client-server communication.

**EARLY DEVELOPMENT!**

**NOT ready for use to ANYONE!**


### Compilation

### Usage

Server side: `redis-server --loadmodule module-shm.so`

Client side: It's all the same [hiredis](https://github.com/redis/hiredis) you're used to using. Latency has its tradeoffs - to achieve maximum latency, a single thread is working at 100% load, actively reading for changes in memory. So, you must first understand this, and confirm latency is important for you, by calling: `asdfasdf`
TODO

### How does it work?

This code all relies on Linux, GCC and x86. I'm not even trying to investigate other platforms or compilers, as each is like a unicorn in this field - amazingly unique, complex, entwined in mysterious behaviours. Linux & GCC & x86 is the most popular combo.
* Linux shared memory - makes the communication possible. There are 2 FIFOs - one for each direction of communication. They are circular buffers of chars, nothing more.
* GCC atomics - allows avoiding relatively heavy synchronization mechanisms. Forcing the compiler and CPU do things in order with [memory barriers](https://gcc.gnu.org/onlinedocs/gcc-4.4.0/gcc/Atomic-Builtins.html), and using TODO to avoid partial memory write/read nastiness.
* GCC volatile - avoids optimization dropping "unneeded" reads/writes. GCC sanely assumes no outsider is able to modify the memory the code accesses, but the shared memory does exactly that. Using `volatile` drops the GCC assumption.
* ~~mfence/clflush - avoids waiting for shared memory writes to move out of L1. (An alernative could be a [userspace DMA](https://github.com/ikwzm/udmabuf)?)~~ <- Nah, the default behaviour seems good enough and even slightly faster than clflush. Data moves out of L1 quickly. Calling clflush however forces data to be moved up to the main memory, a slow and often unnecessary task. 

Whenever a TCP or socket read()/write() would be called, a pop/push on the circular buffer is done instead.

A single dedicated thread on redis-server is actively reading the shared memory, waiting for input, or waiting for the buffer to free up. It never sleeps, and uses 100% of the CPU core, whenever at least one client is connected. Similarily, the redis client uses 100% of its CPU core during any redis API call. It's a chosen tradeof to avoid communicating through kernel, thus keeping latency extremely low. There is a gotcha though. When CPU load is high, the CPU scheduler gives bursts of CPU time, causing high latency during downtimes. As load gets higher, the duration and time between (=latency) the bursts gets higher. To avoid this latency, redis-server and client should be run with higher priority than other processes. Note that the same problem does not affect TCP or unix socket communication when redis requests have relatively large gaps between them. When a request occurs, the "waiting for sockets" redis server (and client, if it was also sleeping) is woken up almost immediately because it hasn't used up any recent CPU time yet. I may implement configurable alternatives later...

Now, the nasty part...
TODO

The TCP or Unix socket connection is still necessary. It is used to exchange the info about the shared memory. When a new shared memory connection is established, a `SHM.OPEN` redis module command is sent through the socket. No more information is exchanged through the socket after that, until the connection needs to be broken down.

### Performance

Well, the barchart above pretty much describes it. There are a few things to take attention for though:
* Your latency sensitive redis server and client need to run with high CPU scheduler priority. See section [#How does it work?](#how-does-it-work) for the explanation why it's needed.
* I made the test on a practically idle host. When the host is under load (`stress --cpu 50`), the latency of TCP and unix socket communication decreases... Yeah, I know. Under load, unix socket ping latency drops to 0.052ms. Several times higher than shared memory but an impressive drop nonetheless. But why it happens, I don't know. CPU power saving? Other folk have noticed this phenomena too and are [guessing it may be a scheduler thing](http://stackoverflow.com/questions/33950984/how-to-understand-redis-clis-result-vs-redis-benchmarks-result).

This is how the benchmark for the barchart is done:
<pre>

sudo nice -n -15 sudo -u edg ~/tmp/redis/src/redis-server --loadmodule ~/tmp/redis-module-shm/module-shm.so
03:08:54 edg@host ~/tmp/redis $ sudo taskset 0x01 nice -n -15 sudo -u edg ~/tmp/redis/src/redis-cli --latency
min: 0, max: 94, avg: 0.013 (878912 samples)^C

sudo nice -n -15 sudo -u edg ~/tmp/redis/src/redis-server
05:37:58 edg@host ~/tmp/redis $ sudo taskset 0x01 nice -n -15 sudo -u edg ~/tmp/redis/src/redis-cli --latency
min: 0, max: 70, avg: 0.226 (853519 samples)^C

sudo nice -n -15 sudo -u edg ~/tmp/redis/src/redis-server redis.conf # <- enabled unixsocket in conf
10:02:25 edg@host ~/tmp/redis $ sudo taskset 0x01 nice -n -15 sudo -u edg ~/tmp/redis/src/redis-cli -s redis.sock --latency
min: 0, max: 106, avg: 0.170 (1041387 samples)^C

</pre>

If you compile this module, please make the tests and send in your results (as a new issue or email), for a little less bias.
You need a redis-cli which uses the hiredis with shared memory. You can find it here, and compile as you would a normal redis: TODO 

(Note that redis-benchmark won't use shared memory. It implements its own low level communication with redis server, bypassing the hiredis code with shared memory support. But redis-cli doesn't do magic, so can be used for testing latency.)

