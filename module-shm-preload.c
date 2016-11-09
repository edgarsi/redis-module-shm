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

#include <dlfcn.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/epoll.h>


void (*ModuleSHM_BeforeSelect)();
void (*ModuleSHM_AfterSelect)();

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

