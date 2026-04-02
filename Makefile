UNAME := $(shell uname -s)

# Platform-specific library paths
ifeq ($(UNAME), Darwin)
    # macOS: use Homebrew paths
    RL_PREFIX ?= /opt/homebrew/opt/readline
    SQ_PREFIX ?= /opt/homebrew/opt/sqlite
    LDFLAGS    = -L$(RL_PREFIX)/lib -lreadline -L$(SQ_PREFIX)/lib -lsqlite3
else ifeq ($(OS), Windows_NT)
    # MSYS2/MinGW: libraries are in the MinGW sysroot, no prefix needed
    RL_PREFIX ?= /mingw64
    SQ_PREFIX ?= /mingw64
    LDFLAGS    = -L$(RL_PREFIX)/lib -lreadline -L$(SQ_PREFIX)/lib -lsqlite3
else
    # Linux: use system packages (apt: libreadline-dev libsqlite3-dev)
    RL_PREFIX ?= /usr
    SQ_PREFIX ?= /usr
    LDFLAGS    = -lreadline -lsqlite3
endif

CC      = cc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g \
          -I$(RL_PREFIX)/include -I$(SQ_PREFIX)/include

SRCS = main.c lexer.c parser.c script.c \
       executor.c jobs.c builtins.c \
       value.c render.c query.c \
       history.c env_config.c \
       config.c prompt.c complete.c line_editor.c error.c
OBJS = $(SRCS:%.c=obj/%.o)

.PHONY: all clean

all: build/nsh

build/nsh: $(OBJS) | build
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

obj/%.o: %.c shell.h | obj
	$(CC) $(CFLAGS) -c -o $@ $<

obj build:
	mkdir -p $@

clean:
	rm -rf obj build
