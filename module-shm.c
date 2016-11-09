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


/* Choose?: pthread.h vs threads.h
 * threads.h has nicer API but GCC hasn't yet implemented it. */ 
#include <pthread.h>
#include <c11threads.h>


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

/* Only let shared memory thread process requests while the main Redis thread
 * is sleeping, and only let the maim Redis thread process process requests
 * when the shared memory thread is waiting. */ 
static mtx_t processing_requests;

extern void (*ModuleSHM_BeforeSelect)();
void ModuleSHM_BeforeSelect_Impl()
{
    mtx_unlock(&processing_requests);
}

extern void (*ModuleSHM_AfterSelect)();
void ModuleSHM_AfterSelect_Impl()
{
    /* Block until shared memory processing finishes work. It's negligible because
     * the main thread just called a slow syscall anyway. */
    mtx_lock(&processing_requests);
}

static void ProcessPendingInput();

/*TODO: Search "pthread_" to see how redis handles threading, restrictions and such. */
/*TODO: Try this: http://lxr.free-electrons.com/source/include/linux/hw_breakpoint.h */
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
            /* Spinning because avoids slow context switching. */
            while (mtx_trylock(&processing_requests) != thrd_success) {};
            
            // Doing the pending input directly here, while testing, because
            // there's just one connection.
            X("%lld processing \n", ustime());
            ProcessPendingInput();
            
            mtx_unlock(&processing_requests);
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
    
    ModuleSHM_BeforeSelect = ModuleSHM_BeforeSelect_Impl;
    ModuleSHM_AfterSelect = ModuleSHM_AfterSelect_Impl;
    mtx_init(&processing_requests, mtx_plain);
    mtx_lock(&processing_requests);
    
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
