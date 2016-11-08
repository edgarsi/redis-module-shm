/*
 * module-shm.c
 *
 *  Created on: Oct 27, 2016
 *      Author: Edgars
 */


#include "redismodule.h"

/* Redis modules are not supposed to use redis server files,
 * so some macros attempt get redefined. */
#undef _DEFAULT_SOURCE
#include "server.h"

#include <sys/mman.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <pthread.h>

#include "lockless-char-fifo/charfifo.h"


#define X(...)
//#define X printf



#define SHARED_MEMORY_BUF_SIZE 1000

typedef CHARFIFO(SHARED_MEMORY_BUF_SIZE) sharedMemoryBuffer;

typedef volatile struct sharedMemory {
    sharedMemoryBuffer to_server;
    sharedMemoryBuffer to_client;
} sharedMemory;

typedef struct threadCtx {
    RedisModuleCtx *module_ctx;
    sharedMemory *mem;
    client *client;
    pthread_t thread;
} threadCtx;

static pthread_t main_thread;
static int thread_signalled = 0;
static list *threads;

/*TODO: sharedMemory buffer split out and symlinked between projects */

/*inline void
clflush(volatile void *p)
{
    asm volatile ("clflush (%0)" :: "r"(p));
}*/

//static size_t bufFreeSpace(sharedMemoryBuffer *target) {
//    size_t read_idx = target->read_idx;
//    ssize_t free = (ssize_t)read_idx - target->write_idx - 1;
//    if (read_idx <= target->write_idx) {
//        free = free + sizeof(target->buf);
//    }
//    return free;
//}
//
//static void bufWrite(sharedMemoryBuffer *target, char *buf, size_t btw) {
//    X("%lld bufWrite(read_ptr=%d write_ptr=%d btw=%d\n", ustime(), target->read_idx, target->write_idx, btw);
//    size_t write_idx = target->write_idx;
//    __sync_synchronize();
//    char* target_buf = (char*)target->buf; /* casting away volatile because buf can't change in the range being used. */
//    if (write_idx >= target->read_idx) {
//        size_t bytes_to_eob = sizeof(target->buf) - write_idx;
//        if (bytes_to_eob <= btw) {
//            /* Write needs to rotate target->write_idx to the buffer beginning */
//            memcpy(target_buf + write_idx, buf, bytes_to_eob);
//            write_idx = 0;
//            btw -= bytes_to_eob;
//            buf += bytes_to_eob;
//        }
//    }
//    memcpy(target_buf + write_idx, buf, btw);
//    write_idx += btw;
//    __sync_synchronize();
//    target->write_idx = write_idx;
//    /* Need to push out of L1 cache, for other CPUs to see the update now,
//     * but the CPU seems to do a good enough job of that. There is no way to
//     * push out to L2, only purge of all cache hierarchy, which is slow. */
////    asm volatile ("mfence" ::: "memory");
////    clflush(&target->write_idx);
//    X("%lld bufWrite fin (read_ptr=%d write_ptr=%d btw=%d\n", ustime(), target->read_idx, target->write_idx, btw);
//}
//
///* TODO: Less duplication, maybe? */
//static size_t bufUsedSpace(sharedMemoryBuffer *source) {
//    size_t write_idx = source->write_idx;
//    ssize_t used = write_idx - source->read_idx;
//    if (write_idx < source->read_idx) {
//        used = used + sizeof(source->buf);
//    }
//    return used;
//}
//
///* TODO: Less duplication, maybe? */
//static void bufRead(sharedMemoryBuffer *source, char *buf, size_t btr) {
//    X("%lld bufRead(read_ptr=%d write_ptr=%d btr=%d\n", ustime(), source->read_idx, source->write_idx, btr);
//    size_t read_idx = source->read_idx;
//    __sync_synchronize();
//    char* source_buf = (char*)source->buf; /* casting away volatile because buf can't change in the range being used. */
//    if (read_idx > source->write_idx) {
//        size_t bytes_to_eob = sizeof(source->buf) - read_idx;
//        if (bytes_to_eob <= btr) {
//            /* Read needs to rotate source->read_idx to the buffer beginning */
//            memcpy(buf, source_buf + read_idx, bytes_to_eob);
//            read_idx = 0;
//            btr -= bytes_to_eob;
//            buf += bytes_to_eob;
//        }
//    }
//    memcpy(buf, source_buf + read_idx, btr);
//    read_idx += btr;
//    __sync_synchronize();
//    source->read_idx = read_idx;
//    /* Need to push out of L1 cache, for other CPUs to see the update now. */
////    asm volatile ("mfence" ::: "memory");
////    clflush(&source->read_idx);
//    X("%lld bufRead fin (read_ptr=%d write_ptr=%d btr=%d\n", ustime(), source->read_idx, source->write_idx, btr);
//}

static void ProcessPendingInput();

/*TODO: Search "pthread_" to see how redis handles threading, restrictions and such. */
static void* RunThread(void* arg)
{
    threadCtx *thread_ctx = arg;
    /*TODO: Test replication */
    for (;;) { /*TODO: Needs to exit when the context closes */
        if (thread_signalled) {
            continue;
        }
        size_t btr = CharFifo_UsedSpace(&thread_ctx->mem->to_server);
        if (btr > 0) {
            /* Can't process now because of data races. Sending a signal is not
             * beautiful and probably isn't ultra fast, depending on the kernel.
             * But I'd be pretty surprised if a pipe is faster. All of this 
             * shared memory magic is done to avoid using pipes...
             * There is no good solution unless the redis server code is changed.
             */
//            X("%lld alarm \n", ustime());
//            thread_signalled = 1;
            //pthread_kill(main_thread, SIGUSR2);
            
            //TODO: signalling is too slow. Need to override the whole redis main loop.
            // After the 'select', a mutex.
            
            // Doing the pending input directly here, while testing, because
            // there's just one connection.
            X("%lld processing \n", ustime());
            ProcessPendingInput();
        }
    }
    /*TODO: unsafe to do any of this stuff here. need to set a flag and delete in ProcessPendingInput */
    /*TODO: Can I really call these in parallel to main thread?*/
    freeClient(thread_ctx->client); /*TODO: err*/
    RedisModule_Free(thread_ctx); /*TODO: err*/
    listDelNode(threads, listSearchKey(threads, thread_ctx)); /*TODO: err*/ 
    
    return NULL;
}

/* Read the input from shared memory into redis client input buffer. 
 * Returns the number of bytes moved. */
/* Most of the code is just lifted from redis networking.c */
static size_t ReadInput(threadCtx *thread_ctx)
{
    size_t btr = CharFifo_UsedSpace(&thread_ctx->mem->to_server);
    if (btr == 0) {
        return 0;
    }
    X("%lld reading \n", ustime());
    if (btr > 1024) {
        btr = 1024;
    }
    char tmp[1024]; /*TODO: Read directly into sds */
    CharFifo_Read(&thread_ctx->mem->to_server, tmp, btr);
    tmp[btr] = '\0';
//            RedisModule_Log(thread_ctx->redis_ctx, "warning", "%s", tmp);
    X("%s", tmp);
    
    //TODO: Other stuff from readQueryFromClient, I'm blatantly ignoring here.
    //TODO: Split readQueryFromClient in parts, to avoid duplication.
    //TODO: Am I really safe modifying client structure in parallel to the main thread?
    client *c = thread_ctx->client;
    int readlen = btr;
    size_t qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    memcpy(c->querybuf+qblen, tmp, btr);
    int nread = btr;

    sdsIncrLen(c->querybuf,nread);
    c->lastinteraction = server.unixtime;
    if (c->flags & CLIENT_MASTER) c->reploff += nread;
    server.stat_net_input_bytes += nread;
    
    return btr;
}

/* Write the output redis client output buffer to shared memory. */
/* Most of the code is just lifted from redis networking.c */
static void WriteOutput(threadCtx *thread_ctx)
{
    ssize_t nwritten = 0, totwritten = 0;
    size_t objlen;
    sds o;
    
    sharedMemoryBuffer *target = &thread_ctx->mem->to_client;
    client *c = thread_ctx->client;

    while(clientHasPendingReplies(c)) {
        if (c->bufpos > 0) {
            
            
            /* TODO: Clean this mess up */
            
            
            X("%lld writing type1\n", ustime());
            X("%.*s", (int)objlen - c->sentlen, o + c->sentlen);
            size_t free = 0;
            do {
                free = CharFifo_FreeSpace(target);
            } while (free == 0); /* Should this always be blocking? */
            if (free == 0) break;
            if (free >= c->bufpos - c->sentlen) {
                nwritten = c->bufpos - c->sentlen;
            } else {
                nwritten = free;
            }
            CharFifo_Write(target, c->buf + c->sentlen, nwritten);
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            if ((int)c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else {
            X("%lld writing type2\n", ustime());
            o = listNodeValue(listFirst(c->reply));
            objlen = sdslen(o);

            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                continue;
            }

            X("%.*s", (int)objlen - c->sentlen, o + c->sentlen);
            
            size_t free = 0;
            do {
                free = CharFifo_FreeSpace(target);
            } while (free == 0);
            if (free == 0) break; /* TODO: Write all, not just a bit. See shm.c; (and analogically where needed) */
            if (free >= objlen - c->sentlen) {
                nwritten = objlen - c->sentlen;
            } else {
                nwritten = free;
            }
            CharFifo_Write(target, o + c->sentlen, nwritten);
//            nwritten = objlen - c->sentlen;
            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If we fully sent the object on head go to the next one */
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objlen;
            }
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & CLIENT_MASTER)) c->lastinteraction = server.unixtime; /*TODO: Think about this */
    }
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;

        /* Close connection after entire reply has been sent. */
//        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
//            freeClient(c);
//            return C_ERR;
//        }
        //^ WHAT? A client is created for each command?
    }
    X("%lld replied bytes=%d\n", ustime(), totwritten);
}

static void ProcessPendingInput()
{
    X("blah %lld \n", ustime());
    listNode* it = listFirst(threads);
    while (it != NULL) {
        threadCtx *thread_ctx = listNodeValue(it);
        
        if (ReadInput(thread_ctx)) {
            processInputBuffer(thread_ctx->client);
            WriteOutput(thread_ctx);
            X("%lld fin processing thread \n", ustime());
        }
        
        it = listNextNode(it);
    }
    thread_signalled = 0; /*TODO: explain*/
}

static int old_beforeSleep_set = 0;
static aeBeforeSleepProc* old_beforeSleep;

static void BeforeSleep_PlusShm(struct aeEventLoop *eventLoop)
{
    /* Latency is king. Housekeeping must wait. */
    ProcessPendingInput();
    
    if (old_beforeSleep) {
        old_beforeSleep(eventLoop); /* server.c beforeSleep, and maybe other modules misbehave as well. */
    }
}

/* Does the server end of establishing the shared memory connection. */
static int Command_Open(RedisModuleCtx *ctx, RedisModuleString **argv, int argc)
{
    X("%lld Command_Open \n", ustime());
    if (argc != 3) {
        return RedisModule_WrongArity(ctx);
    }
    long long version;
    if (RedisModule_StringToLongLong(argv[1], &version) == REDISMODULE_ERR) {
        return RedisModule_ReplyWithError(ctx, "Could not parse version");
    }
    if (version >= 100) {
        return RedisModule_ReplyWithError(ctx, "Client shm connector version "
                                               "is too high, not supported.");
    }
    
    size_t len;
    const char* shm_name = RedisModule_StringPtrLen(argv[2], &len);
    if (len > 37) {
        return RedisModule_ReplyWithError(ctx, "Shared memory file length too long");
    }
    char shm_name_cpy[38];
    memmove(shm_name_cpy, shm_name, len);
    shm_name_cpy[len] = '\0';
    
    int fd = shm_open(shm_name_cpy, O_RDWR, 0);
    if (fd < 0) {
        return RedisModule_ReplyWithError(ctx, "Can't find the shared memory "
                                               "file on this host"); /*TODO: err*/
    }
    sharedMemory *mem = mmap(NULL, sizeof(sharedMemory), (PROT_READ|PROT_WRITE), 
                         MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) {
        return RedisModule_ReplyWithError(ctx, "Found the shared memory file but "
                                               "unable to mmap it"); /*TODO: err*/
    }
    
    close(fd); /*TODO: error handling */
    
    /* Create a client for replaying the input to */
    client *c = createClient(-1);
    c->flags |= CLIENT_MODULE;
    
    /* Set up a call processing mechanism. To avoid data races with the main thread,
     * we get the main thread to do the processing at a good time.
     */
//    if (!old_beforeSleep_set) { /* Still NULL at RedisModule_OnLoad */
//        old_beforeSleep = server.el->beforesleep;
//        aeSetBeforeSleepProc(server.el, BeforeSleep_PlusShm);
//        old_beforeSleep_set = 1;
//    }
    
    threadCtx *thread_ctx = RedisModule_Alloc(sizeof(threadCtx));
    thread_ctx->module_ctx = ctx;
    thread_ctx->mem = mem;
    thread_ctx->client = c;
    
    listAddNodeHead(threads, thread_ctx); /*TODO: err*/
    
    printf("%lld creating thread \n", ustime());
    int err = pthread_create(&thread_ctx->thread, NULL, RunThread, thread_ctx);
    if (err != 0) {
        RedisModule_Free(thread_ctx);
        return RedisModule_ReplyWithError(ctx, "Can't create a thread to listen "
                                               "to the changes in shared memory."); /*TODO: err*/
    }
    
    return RedisModule_ReplyWithLongLong(ctx, 1);
}

/* Registering the module */
int RedisModule_OnLoad (RedisModuleCtx *ctx)
{
    if (RedisModule_Init(ctx, "SHM", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    RedisModule_Log(ctx, "warning", "!!!!!!!!!!  Shared memory module is very, very dangerous!  !!!!!!!!!!"); 
    
    /* https://github.com/RedisLabs/RedisModulesSDK/blob/master/FUNCTIONS.md */
    const char *flags = "readonly random deny-oom no-monitor fast"; /* TODO: allow-loading, and look into the used flags in detail */
    if (RedisModule_CreateCommand(ctx, "SHM.OPEN", Command_Open, 
                                  flags, 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    /* We will keep a list of threads, each listening to changes in the shared
     * memory, */
    main_thread = pthread_self();
    threads = listCreate();
    /* and whenever there are changes, we create a signal to force the main thread
     * to process them by issuing SIGUSR2 to stop 'select'. */
    signal(SIGUSR2, SIG_IGN);
    
//    X("hello\n");
    
    return 0;
}
