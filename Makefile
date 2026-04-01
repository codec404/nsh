RL_PREFIX = /opt/homebrew/opt/readline
SQ_PREFIX = /opt/homebrew/opt/sqlite

CC      = cc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g \
          -I$(RL_PREFIX)/include -I$(SQ_PREFIX)/include
LDFLAGS = -L$(RL_PREFIX)/lib -lreadline -L$(SQ_PREFIX)/lib -lsqlite3

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
