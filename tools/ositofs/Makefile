# OsitoFS v2 host tools — build with standard gcc
#
# Usage:
#   make              Build all tools
#   make clean        Remove build artifacts

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS =

TOOLS   = mkfs.ositofs ositofs-write ositofs-ls ositofs-info ositofs-read
COMMON  = common.o gguf.o

all: $(TOOLS)

mkfs.ositofs: mkfs.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-write: write.o common.o gguf.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-ls: ls.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-info: info.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-read: read.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

%.o: %.c common.h gguf.h ../../include/common/ositofs2_format.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TOOLS)

.PHONY: all clean
