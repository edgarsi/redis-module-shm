/*
 * module-shm-preload.c
 *
 *  Created on: Nov 9, 2016
 *      Author: Edgars
 */


/* Rewriting a few system functions to avoid duplicating a fluid Redis code.
 * I know this isn't pretty, but it's the next cleanest thing besides forking Redis.
 * Asking users to compile a custom Redis is not nice. The whole point of modules
 * is to avoid such weirdities.
 */

#include "config.h"

#include <dlfcn.h>
#include <stddef.h>

#ifdef HAVE_EVPORT
#include <port.h>
#endif

#ifdef HAVE_EPOLL
#include <sys/epoll.h>
#endif

#ifdef HAVE_KQUEUE
#include <sys/event.h>
#endif

#include <sys/select.h>


void (*ModuleSHM_BeforeSelect)();
void (*ModuleSHM_AfterSelect)();
ssize_t (*ModuleSHM_ReadUnusual)(int fd, void *buf, size_t count);
ssize_t (*ModuleSHM_WriteUnusual)(int fd, const void *buf, size_t count);

#ifdef HAVE_EVPORT
int port_getn(int port, port_event_t list[], uint_t max,
     uint_t *nget, const timespec_t *timeout)
{
    static int (*real_port_getn)(int epfd, struct epoll_event *events,
                                 int maxevents, int timeout) = NULL;
    if (real_port_getn == NULL) {
        real_port_getn = dlsym(RTLD_NEXT, "port_getn");
    }
    
    ModuleSHM_BeforeSelect();
    int res = real_port_getn(port, list, max, nget, timeout);
    ModuleSHM_AfterSelect();
    
    return res;
}
#endif

#ifdef HAVE_EPOLL
int epoll_wait(int epfd, struct epoll_event *events,
               int maxevents, int timeout)
{
    static int (*real_epoll_wait)(int epfd, struct epoll_event *events,
                                  int maxevents, int timeout) = NULL;
    if (real_epoll_wait == NULL) {
        real_epoll_wait = dlsym(RTLD_NEXT, "epoll_wait");
    }
    
    ModuleSHM_BeforeSelect();
    int res = real_epoll_wait(epfd, events, maxevents, timeout);
    ModuleSHM_AfterSelect();
    
    return res;
}
#endif

#ifdef HAVE_KQUEUE
int kevent(int kq, const struct kevent *changelist, int nchanges, 
           struct kevent *eventlist, int nevents, const struct timespec *timeout)
{
    static int (*real_kevent)(int kq, const struct kevent *changelist, 
                              int nchanges, struct kevent *eventlist, int nevents, 
                              const struct timespec *timeout) = NULL;
    if (real_kevent == NULL) {
        real_kevent = dlsym(RTLD_NEXT, "kevent");
    }
    
    ModuleSHM_BeforeSelect();
    int res = real_kevent(kq, changelist, nchanges, eventlist, nevents, timeout);
    ModuleSHM_AfterSelect();
    
    return res;
}
#endif

int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout)
{
    static int (*real_select)(int nfds, fd_set *readfds, fd_set *writefds,
                              fd_set *exceptfds, struct timeval *timeout) = NULL;
    if (real_select == NULL) {
        real_select = dlsym(RTLD_NEXT, "select");
    }
    
    ModuleSHM_BeforeSelect();
    int res = real_select(nfds, readfds, writefds, exceptfds, timeout);
    ModuleSHM_AfterSelect();
    
    return res;
}

ssize_t read(int fd, void *buf, size_t count)
{
    static int (*real_read)(int fd, void *buf, size_t count) = NULL;
    if (real_read == NULL) {
        real_read = dlsym(RTLD_NEXT, "read");
    }
    
    if (fd == -1) {
        return ModuleSHM_ReadUnusual(fd, buf, count);
    } else {
        return real_read(fd, buf, count);
    }
}

ssize_t write(int fd, const void *buf, size_t count)
{
    static int (*real_write)(int fd, const void *buf, size_t count) = NULL;
    if (real_write == NULL) {
        real_write = dlsym(RTLD_NEXT, "write");
    }
    
    if (fd == -1) {
        return ModuleSHM_WriteUnusual(fd, buf, count);
    } else {
        return real_write(fd, buf, count);
    }
}
