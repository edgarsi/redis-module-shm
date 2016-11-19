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
#undef _GNU_SOURCE
#undef _LARGEFILE_SOURCE
#include "server.h"

#include <sys/mman.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "c11threads.h"
#include "lockless-char-fifo/charfifo.h"


#define X(...)
//#define X printf



/* redisBufferRead thinks 16k is best for a temporary buffer reading replies.
 * A good guess is this will do well with shared memory buffer size too. */
#define SHARED_MEMORY_BUF_SIZE (1024*16)

typedef CHARFIFO(SHARED_MEMORY_BUF_SIZE) sharedMemoryBuffer;

typedef volatile struct sharedMemory {
    sharedMemoryBuffer to_server;
    sharedMemoryBuffer to_client;
} sharedMemory;

typedef struct shmConnCtx {
    int fd;
    sharedMemory *mem;
    client *client;
} shmConnCtx;

static list *connections;
static mtx_t accessing_connections;

static thrd_t thread;

/* Only let shared memory thread process requests while the main Redis thread
 * is sleeping, and only let the main Redis thread process process requests
 * when the shared memory thread is waiting. */ 
static mtx_t processing_requests;
static shmConnCtx *conn_ctx_processing = NULL;


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

extern ssize_t (*ModuleSHM_ReadUnusual)(int fd, void *buf, size_t count);
ssize_t ModuleSHM_ReadUnusual_Impl(int fd, void *buf, size_t count)
{
    errno = 0;
    size_t btr = CharFifo_UsedSpace(&conn_ctx_processing->mem->to_server);
    if (btr == 0) {
        errno = EAGAIN;
        return -1;
    }
    CharFifo_Read(&conn_ctx_processing->mem->to_server, buf, btr);
    return btr;
}

extern ssize_t (*ModuleSHM_WriteUnusual)(int fd, const void *buf, size_t count);
ssize_t ModuleSHM_WriteUnusual_Impl(int fd, const void *buf, size_t count)
{
    errno = 0;
    size_t free = CharFifo_FreeSpace(&conn_ctx_processing->mem->to_client);
    ssize_t nwritten;
    if (free >= count) {
        nwritten = count;
    } else {
        nwritten = free;
    }
    CharFifo_Write(&conn_ctx_processing->mem->to_client, buf, nwritten);
    return nwritten;
}

/* Spinning because avoids slow context switching. */
static inline void mtx_lock_spinning(mtx_t *m)
{
    while (mtx_trylock(m) != thrd_success) {};
}

static inline int module_client_fd(RedisModuleCtx *module)
{
    /* I need to get to the module client fd some way :( */
    typedef struct RedisModuleCtx {
        void *getapifuncptr;            /* NOTE: Must be the first field. */
        struct RedisModule *module;     /* Module reference. */
        client *client;                 /* Client calling a command. */
    } RedisModuleCtx;
    return ((RedisModuleCtx*)module)->client->fd;
}

/*TODO: Try this: http://lxr.free-electrons.com/source/include/linux/hw_breakpoint.h */
static int RunThread(void* dummy __attribute__((unused)))
{
    for (;;) {
        mtx_lock_spinning(&processing_requests);
        mtx_lock_spinning(&accessing_connections);
        
        /* Check each connection for incoming data. */
        listNode* it = listFirst(connections);
        while (it != NULL) {
            shmConnCtx *conn_ctx = listNodeValue(it);
            
            if (CharFifo_UsedSpace(&conn_ctx->mem->to_server) != 0) {
                /* ...and process it. */
                conn_ctx_processing = conn_ctx;
                readQueryFromClient(server.el, -1, conn_ctx->client, AE_READABLE);
                conn_ctx_processing = NULL;
            }
            if (clientHasPendingReplies(conn_ctx->client)) {
                conn_ctx_processing = conn_ctx;
                sendReplyToClient(server.el, -1, conn_ctx->client, AE_WRITABLE);
                conn_ctx_processing = NULL;
            }
            
            listNode *next_it = listNextNode(it);
            
            /* Let's break down the connections when the socket closes */
            int error = 0;
            socklen_t len = sizeof(error);
            int retval = getsockopt(conn_ctx->fd, SOL_SOCKET, SO_ERROR, &error, &len);
            if (retval != 0 || error != 0) {
                X("%lld closing socket fd=%d retval=%d error=%d\n", ustime(), conn_ctx->fd, retval, error);
                freeClient(conn_ctx->client);
                munmap((void*)conn_ctx->mem, sizeof(sharedMemory));
                RedisModule_Free(conn_ctx);
                listDelNode(connections, it);
            }
            
            it = next_it;
        }
        
        /* No need to waste CPU when no shared memory connection established. */
        if (listLength(connections) == 0) {
            break;
        }
        
        mtx_unlock(&accessing_connections);
        mtx_unlock(&processing_requests);
    }
    mtx_unlock(&accessing_connections);
    mtx_unlock(&processing_requests);
    
    return thrd_success;
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
                                               "file on this host");
    }
    sharedMemory *mem = mmap(NULL, sizeof(sharedMemory), (PROT_READ|PROT_WRITE), 
                         MAP_SHARED, fd, 0);
    close(fd);
    if (mem == MAP_FAILED) {
        return RedisModule_ReplyWithError(ctx, "Found the shared memory file but "
                                               "unable to mmap it");
    }
    
    
    X("%lld creating shm connection \n", ustime());
    
    /* Create a client for replaying the input to */
    client *c = createClient(-1);
    c->flags |= CLIENT_MODULE;
    
    shmConnCtx *conn_ctx = RedisModule_Alloc(sizeof(shmConnCtx));
    conn_ctx->fd = module_client_fd(ctx);
    conn_ctx->mem = mem;
    conn_ctx->client = c;
    
    mtx_lock(&accessing_connections);
    
    listAddNodeHead(connections, conn_ctx);
    
    if (listLength(connections) == 1) {
        X("%lld creating thread \n", ustime());
        
        if (thrd_create(&thread, RunThread, NULL) != thrd_success) {
            
            mtx_unlock(&accessing_connections);
            munmap((void*)mem, sizeof(sharedMemory));
            return RedisModule_ReplyWithError(ctx, "Can't create a thread to listen "
                                                   "to the changes in shared memory.");
        }
    }
    
    mtx_unlock(&accessing_connections);
    
    return RedisModule_ReplyWithLongLong(ctx, 1);
}

/* Registering the module */
int RedisModule_OnLoad (RedisModuleCtx *ctx)
{
    if (RedisModule_Init(ctx, "SHM", 1, REDISMODULE_APIVER_1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    connections = listCreate();
    mtx_init(&accessing_connections, mtx_plain);
    
    ModuleSHM_BeforeSelect = ModuleSHM_BeforeSelect_Impl;
    ModuleSHM_AfterSelect = ModuleSHM_AfterSelect_Impl;
    ModuleSHM_ReadUnusual = ModuleSHM_ReadUnusual_Impl;
    ModuleSHM_WriteUnusual = ModuleSHM_WriteUnusual_Impl;
    mtx_init(&processing_requests, mtx_plain);
    mtx_lock(&processing_requests);
    
    /* https://github.com/RedisLabs/RedisModulesSDK/blob/master/FUNCTIONS.md */
    const char *flags = "readonly deny-oom allow-loading random allow-stale fast";
    if (RedisModule_CreateCommand(ctx, "SHM.OPEN", Command_Open, 
                                  flags, 1, 1, 1) == REDISMODULE_ERR) {
        return REDISMODULE_ERR;
    }
    
    return 0;
}
