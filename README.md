# SlopFS

> [!WARNING]
> one shot claude fable 5 (high) attempt at a filesystem
> do not use this

A persistent, block-based filesystem in C11, stored in a single disk
image file, with write-ahead journaling, a CLI shell, and a real-time
ncurses inspector.

```
make                       # builds ./slopfs and ./slopfs-view
./slopfs create disk.img 100MB
./slopfs shell disk.img
slopfs> mkdir /docs
slopfs> write /docs/a.txt "hello"
slopfs> read /docs/a.txt
hello
```

In a second terminal:

```
./slopfs-view disk.img     # live block map + dir tree + inode inspector
```

Every mutation in the shell appears in the viewer within ~200 ms (the
viewer polls the superblock generation counter, which every committed
transaction bumps).

## Components

- `slopfs` — image creation + interactive shell
  (`mkdir touch write append read/cat ls rm stat import export df`)
- `slopfs-view` — read-only live inspector: colored block map
  (free/metadata/journal/data/fragmented), expandable directory tree,
  inode inspector (block pointers, size, fragmentation score,
  timestamps, refcount)
- `tests/run_tests.sh` — 16 checks incl. deterministic crash-recovery
  tests (`SLOPFS_CRASH=intent|commit`), kill -9 under load, a 3 MB
  indirect-block roundtrip, and 100,000 files on a 1 GB image

## Crash safety

All metadata mutations are wrapped in single write-ahead-logged
transactions (intent log -> fsync -> commit record -> fsync ->
checkpoint -> fsync -> clean). Mounting replays committed transactions
and discards incomplete ones. File data goes only to blocks that were
free in the last committed bitmap, so partially written data is never
reachable after a crash. See `docs/DISK_FORMAT.md` for the full spec.

## Engineering log

The build history lives *inside* the filesystem:

```
make example               # builds example.img
./slopfs shell example.img
slopfs> cat /docs/slopfs_engineering_log.md
```

(Source copy: `docs/slopfs_engineering_log.md`.)

## Targets

```
make            # build both binaries
make test       # run the test suite
make example    # create example.img with the engineering log inside
make clean
```
