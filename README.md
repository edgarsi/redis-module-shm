# Shared memory support for Redis

![Average latency: TCP=0.24ms Unix_socket=0.18ms Shared_memory=0.01ms](docs/latency_barchart.png)

When your client is on the **same host as redis server**, you can get much better latency than going through the TCP stack. And surely you want the **best latency you can get**. Redis folk know this - that's why unix socket support exists. But you can do a lot better with shared memory!

<pre>
01:19:53 edg@host ~/tmp/redis $ ~/tmp/redis/src/redis-cli --latency
min: 0, max: 3, avg: 0.24 (66054 samples)^C

01:31:36 edg@host ~/tmp/redis $ ~/tmp/redis/src/redis-cli -s /tmp/redis.sock --latency
min: 0, max: 3, avg: 0.18 (73476 samples)^C

~/tmp/redis/src/redis-server --loadmodule ~/tmp/redis-module-shm/module-shm.so
00:58:44 edg@host ~/tmp/redis $ ~/tmp/redis/src/redis-cli --latency-history
min: 0, max: 1, avg: 0.02 (1476 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1476 samples) -- 15.01 seconds range
min: 0, max: 520, avg: 0.43 (1416 samples) -- 15.01 seconds range
min: 0, max: 5, avg: 0.02 (1475 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1475 samples) -- 15.01 seconds range
min: 0, max: 4, avg: 0.01 (1474 samples) -- 15.00 seconds range
min: 0, max: 40, avg: 0.28 (1436 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1476 samples) -- 15.00 seconds range
min: 0, max: 2, avg: 0.01 (1476 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.02 (1473 samples) -- 15.00 seconds range
min: 0, max: 30, avg: 0.23 (1441 samples) -- 15.00 seconds range
min: 0, max: 1, avg: 0.01 (1476 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1475 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1475 samples) -- 15.01 seconds range
min: 0, max: 50, avg: 0.23 (1445 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1476 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1476 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1474 samples) -- 15.01 seconds range
min: 0, max: 40, avg: 0.35 (1417 samples) -- 15.00 seconds range
min: 0, max: 2, avg: 0.02 (1474 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1474 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1474 samples) -- 15.00 seconds range
min: 0, max: 33, avg: 0.26 (1438 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1474 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1473 samples) -- 15.01 seconds range
min: 0, max: 1, avg: 0.01 (1474 samples) -- 15.01 seconds range
min: 0, max: 50, avg: 0.42 (1416 samples) -- 15.00 seconds range
min: 0, max: 1, avg: 0.01 (1354 samples)^C
</pre>
^ Way too sensitive to system load... Working on it...

# Summary and usage

Redis module for shared memory client-server communication.

**EARLY DEVELOPMENT!**

**NOT ready for use to ANYONE!**
