#set environment variable RM_INCLUDE_DIR to the location of redismodule.h
ifndef RM_INCLUDE_DIR
	RM_INCLUDE_DIR=.
endif

#set environment variable REDIS_INCLUDE_DIR to the location of redis/src/*.h
ifndef REDIS_INCLUDE_DIR
	REDIS_INCLUDE_DIR=redis/src/
endif

# find the OS
uname_S := $(shell sh -c 'uname -s 2>/dev/null || echo not')

# Compile flags for linux / osx
ifeq ($(uname_S),Linux)
	SHOBJ_CFLAGS ?=  -fno-common -g -ggdb
	SHOBJ_LDFLAGS ?= -shared -Bsymbolic
else
	SHOBJ_CFLAGS ?= -dynamic -fno-common -g -ggdb
	SHOBJ_LDFLAGS ?= -bundle -undefined dynamic_lookup
endif
CFLAGS = -I$(RM_INCLUDE_DIR) -I$(REDIS_INCLUDE_DIR) -g -fPIC -O3 -std=gnu99 -Wall
CC=gcc

MODULE = module-shm

SRC = \
	module-shm.c \
	lockless-char-fifo/charfifo.c
OBJS = $(SRC:.c=.o)

all: $(MODULE)

$(MODULE): $(OBJS)
	$(LD) -o $@.so $^ $(SHOBJ_LDFLAGS) $(LIBS) -g -lrt

clean:
	rm -rf *.so *.o */*.o