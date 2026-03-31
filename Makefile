CC      = cc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -g

# Homebrew readline (macOS ships libedit, not GNU readline)
RL_PREFIX  = /opt/homebrew/opt/readline
CFLAGS    += -I$(RL_PREFIX)/include
LDFLAGS    = -L$(RL_PREFIX)/lib -lreadline

# Homebrew SQLite (system SQLite lacks headers on macOS)
SQ_PREFIX  = /opt/homebrew/opt/sqlite
CFLAGS    += -I$(SQ_PREFIX)/include
LDFLAGS   += -L$(SQ_PREFIX)/lib -lsqlite3

BUILDDIR = build
TARGET   = $(BUILDDIR)/nsh
OBJDIR   = obj
SRCS    = main.c lexer.c parser.c builtins.c executor.c jobs.c \
          value.c render.c query.c history.c env_config.c \
          config.c prompt.c complete.c error.c predict.c
OBJS    = $(addprefix $(OBJDIR)/, $(SRCS:.c=.o))

.PHONY: all clean

all: $(OBJDIR) $(BUILDDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: %.c shell.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(BUILDDIR)
