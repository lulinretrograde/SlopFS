CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wpedantic
LDFLAGS ?=

CORE_SRC = src/codec.c src/blkdev.c src/journal.c src/alloc.c \
           src/inode.c src/dir.c src/fs.c src/migrate.c src/check.c \
           src/stats.c src/replay.c
CORE_OBJ = $(CORE_SRC:.c=.o)

all: slopfs slopfs-view slopfs-fsck

slopfs: $(CORE_OBJ) src/shell.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

slopfs-view: $(CORE_OBJ) src/view.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lncurses

slopfs-fsck: $(CORE_OBJ) src/fsck.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c src/format.h src/fs.h src/blkdev.h src/codec.h
	$(CC) $(CFLAGS) -c -o $@ $<

test: slopfs slopfs-fsck
	./tests/run_tests.sh

example: slopfs
	rm -f example.img
	./slopfs create example.img 16MB
	printf 'mkdir /docs\nmkdir /demo\nwrite /demo/hello.txt "hello from slopfs"\nimport docs/slopfs_engineering_log.md /docs/slopfs_engineering_log.md\n' \
		| ./slopfs shell example.img

clean:
	rm -f slopfs slopfs-view slopfs-fsck src/*.o example.img

.PHONY: all clean test example
