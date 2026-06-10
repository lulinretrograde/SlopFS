# SlopFS On-Disk Format Specification (version 2)

All multi-byte integers are **little-endian**. The block size is fixed
at **4096 bytes**. All structures are written via explicit per-field
serialization; compiler struct layout never reaches the disk.

Version 2 replaces per-block pointer mapping with **extents** and linear
directories with a **hash-chained directory index**. Version 1 images
are still readable: a read-write mount migrates them in place (see
"Migration" below); a read-only mount of a v1 image is refused.

## Region layout

```
block 0                : SUPERBLOCK
bitmap_start ..        : BLOCK BITMAP   (1 bit per block on the disk)
ibitmap_start ..       : INODE BITMAP   (1 bit per inode)
inode_start ..         : INODE TABLE    (32 inodes of 128 B per block)
journal_start ..       : JOURNAL        (1 header + image blocks)
data_start .. total-1  : DATA BLOCKS
```

Region positions/sizes are recorded in the superblock; readers must use
those fields, not recompute them. The region layout is identical in v1
and v2 — only the version field, inode mapping format, and directory
data format differ.

At format time: 1 inode is provisioned per 8 KiB of disk (min 128,
rounded up to a whole inode block); the journal is 260 blocks
(1 header + 256 max transaction images + slack).

## Superblock (block 0)

| offset | size | field |
|-------:|-----:|-------|
| 0   | 8 | magic `"SLOPFS\0\0"` |
| 8   | 4 | version (1 or 2) |
| 12  | 4 | block_size (=4096) |
| 16  | 8 | total_blocks |
| 24  | 4 | inode_count |
| 28  | 4 | root_ino (=1) |
| 32  | 8 | bitmap_start |
| 40  | 8 | bitmap_blocks |
| 48  | 8 | ibitmap_start |
| 56  | 8 | ibitmap_blocks |
| 64  | 8 | inode_start |
| 72  | 8 | inode_blocks |
| 80  | 8 | journal_start |
| 88  | 8 | journal_blocks |
| 96  | 8 | data_start |
| 104 | 8 | free_blocks |
| 112 | 8 | free_inodes |
| 120 | 8 | generation (incremented by every committed transaction) |
| 128 | 4 | crc32 of bytes [0,128) |

Rest of the block is zero. A superblock whose magic or CRC does not
verify must be rejected. A version other than 1 or 2 must be rejected.

## Bitmaps

1 bit per block (resp. inode), bit i of byte `i/8` is `i%8`, set = used.
Bits beyond `total_blocks` / `inode_count` in the final bitmap block are
permanently set. Block bits `[0, data_start)` are set at format time.
Inode 0 is reserved and always marked used.

The block bitmap remains the **authoritative** allocation state. The
in-memory free-extent run list (sorted, coalesced runs of free blocks)
is derived from the bitmap at mount and maintained incrementally; it is
never stored on disk and can always be rebuilt.

## Extent

The unit of file mapping in v2:

```
struct extent {            /* 8 bytes on disk */
    uint32_t start_block;  /* offset 0 */
    uint32_t block_count;  /* offset 4 */
};
```

An extent with `block_count == 0` is invalid. Extents of one file never
overlap each other or any other allocated block. Logical file offsets
map onto the concatenation of the inode's extents in order; v2 files
have **no holes** (every logical block below `ceil(size/4096)` is
mapped).

## Inode (128 bytes, 32 per block)

| offset | size | field |
|-------:|-----:|-------|
| 0  | 2  | type: 0=free, 1=file, 2=directory |
| 2  | 2  | nlinks (reference count) |
| 4  | 4  | flags: bit 0 = inode uses the v2 extent format |
| 8  | 8  | size in bytes |
| 16 | 8  | ctime (unix seconds) |
| 24 | 8  | mtime |
| 32 | 8  | atime |
| 40 | 4  | nextents (number of valid extents, 0..522) |
| 44 | 4  | ext_block: overflow extent block (0 = none) |
| 48 | 80 | ext[10]: inline extents (8 bytes each) |

Extents 0..9 live inline; extents 10..521 live in the overflow block
`ext_block`, which holds 512 extents of 8 bytes (allocated on demand,
freed when the file shrinks back to ≤10 extents). Maximum extents per
file: **522**. Unused inline extent slots and overflow entries are zero.

Inode i lives at block `inode_start + i/32`, byte offset `(i%32)*128`.

In a **v1** inode, flags bit 0 is clear and bytes 40..91 hold the old
mapping instead: `direct[12]` u32 block pointers at offset 40 and one
`indirect` u32 pointer at offset 88 (the indirect block contains 1024
u32 pointers). The migration rewrites this area in the v2 layout and
sets flags bit 0.

## Directories (hash-chained index)

Directory contents are ordinary file data of a type-2 inode, but with a
fixed internal structure:

**Logical block 0 — bucket block**

| offset | size | field |
|-------:|-----:|-------|
| 0    | 4*1023 | hash buckets: slot index of chain head, `0xFFFFFFFF` = empty |
| 4092 | 4      | free-slot search hint (lowest slot that may be free) |

**Logical blocks 1.. — entry slots** (512-byte dirents, 8 per block).
Slot k lives at logical block `1 + k/8`, offset `(k%8)*512`.

| offset | size | field |
|-------:|-----:|-------|
| 0   | 4   | inode id (0 = empty slot) |
| 4   | 2   | name length (1..255) |
| 8   | 256 | name, NUL-padded |
| 264 | 4   | next: slot index of next entry in this hash chain, `0xFFFFFFFF` = end |

A name hashes with **FNV-1a (32-bit)** over its bytes; its bucket is
`hash % 1023`. Lookup walks one chain (amortized O(1)). Insert prepends
to the chain and reuses the lowest free slot (via the hint), extending
the directory by one block at a time when full — preferring physically
contiguous extension. Iteration in slot order gives a stable `ls`
order. `.` and `..` are not stored; path resolution normalizes them
lexically.

In **v1**, directory data was a flat array of the same 512-byte dirents
(no bucket block, `next` unused/zero) searched linearly.

## Journal

Block `journal_start` is the header; image blocks follow.

Header block:

| offset | size | field |
|-------:|-----:|-------|
| 0    | 8   | magic `"SLOPJRNL"` |
| 8    | 8   | seq |
| 16   | 4   | state: 0=CLEAN, 1=COMMITTED |
| 20   | 4   | nblocks |
| 24   | 4   | crc32 over the nblocks image blocks, in order |
| 32   | 8*n | target block numbers |
| 4092 | 4   | crc32 over header bytes [0,4092) |

In the CLEAN state `nblocks` retains the size of the **last committed
transaction** (reported by `slopfs stats` as journal utilization);
recovery ignores it when state=CLEAN.

### Transactions

A transaction stages full images of every metadata block it touches
(at most 256). Operations compose: nested begin/commit pairs join the
enclosing transaction, so a multi-op sequence (e.g. migration of one
inode, or create + write) commits as **one** atomic group. Blocks freed
inside a transaction are quarantined until commit — they are never
reallocated within the same transaction, because the last committed
metadata may still reference their old contents.

### Commit protocol

1. write the new content of every block touched by the transaction
   into journal image blocks 1..n, **fsync**
2. write header with state=COMMITTED, **fsync**  (commit point)
3. write the images to their target blocks, **fsync**
4. write header with state=CLEAN, **fsync**

### Recovery (on mount, read-write only)

- header magic/CRC invalid, or state=CLEAN: nothing to do; reset the
  header to CLEAN.
- state=COMMITTED and image CRC verifies: rewrite every target block
  from its journal image (idempotent), fsync, then mark CLEAN.

A transaction that died before step 2 leaves all target blocks
untouched (rollback is implicit). File data written to blocks that were
free in the last committed bitmap is not journaled; such blocks are
unreachable until the commit record lands, so a crash cannot expose
partial data. The same rule covers freshly allocated directory blocks
and extent overflow blocks.

## Migration (v1 → v2)

A read-write mount of a version-1 image migrates it in place before any
operation runs:

1. **Per inode, one transaction each.** Files: the v1 block list is
   adopted in place as coalesced extents (no data movement) when it
   fits in 522 extents, else the file is rewritten compactly; the
   indirect block is freed. Directories: live entries are collected,
   old blocks freed, and the directory is rebuilt as a hash index;
   mtime/atime are preserved. The migrated inode gets flags bit 0 set.
2. **Finalize, one transaction:** a migration entry is appended to
   `/docs/slopfs_engineering_log.md` and the superblock version is set
   to 2 in the same commit.

Because each step is one journaled transaction and migrated inodes are
marked by the flag, a crash mid-migration is harmless: the next mount
skips already-migrated inodes and continues. File contents are
preserved byte-for-byte (verified by checksum in the test suite).

## Integrity invariants (checked by `slopfs-fsck` / debug mode)

1. No two extents overlap, and no extent overlaps metadata regions or
   exceeds `total_blocks`.
2. Every extent (and overflow block) belongs to exactly one valid inode.
3. A file's size lies within the byte range covered by its extents;
   a directory's size is exactly its block count * 4096.
4. Every directory entry references a valid, allocated inode; link
   counts match the number of referencing entries (root has none).
5. Each dirent slot is on exactly the chain of its name's hash bucket;
   chains are cycle-free and contain no empty slots.
6. The block bitmap equals the set of blocks referenced by metadata
   regions + extents + overflow blocks; `free_blocks`/`free_inodes`
   match the bitmaps.
7. The journal header is well-formed and CLEAN after recovery.

With `SLOPFS_DEBUG_INVARIANTS=1`, the full deep check runs after every
committed transaction and aborts the process on the first violation.
