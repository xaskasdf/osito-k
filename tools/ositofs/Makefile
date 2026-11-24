# OsitoFS host tools — v2 and v3
#
# Usage:
#   make              Build all tools
#   make clean        Remove build artifacts

CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LDFLAGS =

TOOLS_V2 = mkfs.ositofs ositofs-write ositofs-ls ositofs-info ositofs-read \
           ositofs-delete ositofs-fsck ositofs-rename ositofs-defrag
TOOLS_V3 = mkfs.ositofs3 ositofs-write3 ositofs-read3 ositofs-ls3 \
           ositofs-fsck3

all: $(TOOLS_V2) $(TOOLS_V3)

check: all
	bash ./test-journal.sh

# ── v2 Tools ──────────────────────────────────────────────────

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

ositofs-delete: delete.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-fsck: fsck.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-rename: rename.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-defrag: defrag.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-resize: resize.o common.o
	$(CC) $(LDFLAGS) -o $@ $^

# ── v3 Tools ──────────────────────────────────────────────────

mkfs.ositofs3: mkfs3.o common_v3.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-write3: write3.o common_v3.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-read3: read3.o common_v3.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-ls3: ls3.o common_v3.o
	$(CC) $(LDFLAGS) -o $@ $^

ositofs-fsck3: fsck3.o common_v3.o
	$(CC) $(LDFLAGS) -o $@ $^

# ── Common rules ──────────────────────────────────────────────

%.o: %.c common.h gguf.h ../../include/common/ositofs2_format.h ../../include/common/ositofs3_format.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TOOLS_V2) $(TOOLS_V3)

.PHONY: all check clean
