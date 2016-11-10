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

typedef struct shmConnCtx {
    RedisModuleCtx *module_ctx;
    sharedMemory *mem;
    client *client;
} shmConnCtx;

static list *connections;
static mtx_t accessing_connections;

static pthread_t thread;

/* Only let shared memory thread process requests while the main Redis thread
 * is sleeping, and only let the main Redis thread process process requests
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

/* Read the input from shared memory into redis client input buffer. 
 * Returns the number of bytes moved. */
/* Most of the code is just lifted from redis networking.c */
static size_t ReadInput(shmConnCtx *conn_ctx)
{
    size_t btr = CharFifo_UsedSpace(&conn_ctx->mem->to_server);
    if (btr == 0) {
        return 0;
    }
    X("%lld reading \n", ustime());
    if (btr > 1024) {
        btr = 1024;
    }
    char tmp[1024]; /*TODO: Read directly into sds */
    CharFifo_Read(&conn_ctx->mem->to_server, tmp, btr);
    tmp[btr] = '\0';
//            RedisModule_Log(thread_ctx->redis_ctx, "warning", "%s", tmp);
    X("%s", tmp);
    
    //TODO: Other stuff from readQueryFromClient, I'm blatantly ignoring here.
    //TODO: Split readQueryFromClient in parts, to avoid duplication.
    client *c = conn_ctx->client;
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
static void WriteOutput(shmConnCtx *conn_ctx)
{
    ssize_t nwritten = 0, totwritten = 0;
    size_t objlen;
    sds o;
    
    sharedMemoryBuffer *target = &conn_ctx->mem->to_client;
    client *c = conn_ctx->client;

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


/* Spinning because avoids slow context switching. */
static inline void mtx_lock_spinning(mtx_t *m)
{
    while (mtx_trylock(m) != thrd_success) {};
}

/*TODO: Search "pthread_" to see how redis handles threading, restrictions and such. */
/*TODO: Try this: http://lxr.free-electrons.com/source/include/linux/hw_breakpoint.h */
static void* RunThread(void* dummy __attribute__((unused)))
{
    /*TODO: Test replication */
    for (;;) {
        mtx_lock_spinning(&accessing_connections);
        
        /* Check each connection for incoming data. */
        listNode* it = listFirst(connections);
        while (it != NULL) {
            shmConnCtx *conn_ctx = listNodeValue(it);
            
            if (ReadInput(conn_ctx)) {
                /* ...and process them. */
                X("%lld processing \n", ustime());
                mtx_lock_spinning(&processing_requests);
                processInputBuffer(conn_ctx->client);
                WriteOutput(conn_ctx);
                mtx_unlock(&processing_requests);
                X("%lld fin processing client connection \n", ustime());
            }
            
            it = listNextNode(it);
        }
        
        if (listLength(connections) == 0) {
            /* No need to waste CPU when no shared memory 
             * connection established. */
            break;
        }
        
        mtx_unlock(&accessing_connections);
    }
    mtx_unlock(&accessing_connections);
    
    return NULL;
}


//mtx_lock_spinning(&processing_requests);
//RedisModule_Free(conn_ctx); /*TODO: err*/
//freeClient(thread_ctx->client); /*TODO: err*/
//mtx_unlock(&processing_requests);
//mtx_lock_spinning(&accessing_connections);
//listDelNode(threads, listSearchKey(threads, thread_ctx)); /*TODO: err*/
//mtx_unlock(&accessing_connections);




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
    
    shmConnCtx *conn_ctx = RedisModule_Alloc(sizeof(shmConnCtx));
    conn_ctx->module_ctx = ctx;
    conn_ctx->mem = mem;
    conn_ctx->client = c;
    
    mtx_lock_spinning(&accessing_connections);
    listAddNodeHead(connections, conn_ctx); /*TODO: err*/
    mtx_unlock(&accessing_connections);
    
    if (listLength(connections) == 1) {
        printf("%lld creating thread \n", ustime());
        int err = pthread_create(&thread, NULL, RunThread, NULL);
        if (err != 0) {
            return RedisModule_ReplyWithError(ctx, "Can't create a thread to listen "
                                                   "to the changes in shared memory."); /*TODO: err*/
        }
    }
    
    return RedisModule_ReplyWithLongLong(ctx, 1);
}

/* Registering the module */
int RedisModule_OnLoad (RedisModuleCtx *ctx)
{
    if (RedisModule_Init(ctx, "SHM", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    RedisModule_Log(ctx, "warning", "!!!!!!!!!!  Shared memory module can get dangerous!  !!!!!!!!!!");
    
    connections = listCreate();
    mtx_init(&accessing_connections, mtx_plain);
    
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
    
    return 0;
}
