# SlopFS Engineering Log

Formal build history and design record. This file lives inside every
SlopFS example image at `/docs/slopfs_engineering_log.md` and is readable
via `cat /docs/slopfs_engineering_log.md` in the slopfs shell.

## [STEP 1] - On-disk format and block device layer

### Goal
Define a versioned, explicitly serialized on-disk format and a reliable
block I/O layer over a single image file.

### Design decision
Fixed 4096-byte blocks. All on-disk integers are little-endian and are
encoded/decoded field by field (no raw struct dumping), so the format is
independent of compiler padding and host endianness. The superblock
carries a CRC32 so a torn superblock write is detectable. Layout order:
superblock, block bitmap, inode bitmap, inode table, journal, data.
A separate inode bitmap was added (beyond the required block bitmap) so
inode allocation does not require scanning the inode table.

### Implementation summary
- `src/format.h` + `src/codec.c`: constants, in-memory structs, and
  explicit serializers for superblock, inode (128 B), dirent (512 B).
- `src/blkdev.c`: pread/pwrite-based block I/O with EINTR handling and
  explicit fsync.

### Data structures affected
- superblock, inode table, both bitmaps, directories (formats defined)

### Disk layout impact
Initial layout: `[SB][block bitmap][inode bitmap][inode table][journal][data]`.
Superblock records the location and size of every region plus free
counts and a generation counter.

### Edge cases handled
- torn superblock write -> CRC mismatch -> mount refused
- short reads/writes and EINTR in the I/O path
- bitmap tail bits beyond `total_blocks` are pre-marked used so the
  allocator can never hand out an out-of-range block

### Result
Images can be formatted and reopened; all metadata round-trips through
explicit serialization.

## [STEP 2] - Write-ahead journal and transaction engine

### Goal
Atomic, crash-safe metadata updates spanning inode, bitmap, and
directory blocks.

### Design decision
Physical block journaling with a single in-flight transaction. A
transaction stages full block images in memory; commit writes them to
the journal (intent log), syncs, writes a commit record with CRCs over
header and images, syncs, checkpoints the blocks in place, syncs, and
finally marks the journal clean. Every transaction also rewrites the
superblock with an incremented generation counter, which doubles as the
change signal for the live inspector.

### Implementation summary
- `src/journal.c`: `sfs_txn_begin/commit/abort`, staged-aware
  `sfs_bread`/`sfs_bstage`, and `sfs_journal_recover`.
- Recovery: a committed record with valid CRCs is replayed
  (idempotent); anything else is rolled back by resetting the journal.

### Data structures affected
- journal (new on-disk region), superblock (generation, free counts)

### Disk layout impact
Journal = 1 header block + up to 256 image blocks (+ slack). Header
holds seq, state (CLEAN/COMMITTED), target block list, image CRC, and a
header CRC in the last 4 bytes of the block.

### Edge cases handled
- crash before commit record: intent discarded, no in-place data touched
- crash between commit record and checkpoint: replay on next mount
- torn commit-record write: header CRC fails -> treated as uncommitted
- replay is idempotent (full block images, not deltas)

### Result
Every metadata mutation is atomic across all touched blocks.

## [STEP 3] - Block and inode allocation

### Goal
O(1) allocation with no possibility of double allocation, recoverable
via journal replay.

### Design decision
Bitmaps are cached in memory with a next-fit hint (amortized O(1));
every bit flip stages the containing on-disk bitmap block into the
current transaction, so the on-disk allocation state changes only at
commit and is therefore always covered by the journal. Frees are
tracked in a per-transaction list and those blocks are never re-issued
within the same transaction: their old content may still be referenced
by the last committed metadata, so reusing them before commit could
corrupt a file if the process crashed mid-transaction.

### Implementation summary
- `src/alloc.c`: `sfs_balloc/bfree/ialloc/ifree` with double-free and
  double-alloc guards (bit state is asserted before every transition).

### Data structures affected
- block bitmap, inode bitmap, superblock free counters

### Disk layout impact
None (regions already defined in STEP 1).

### Edge cases handled
- double alloc/free rejected with EINVAL
- allocation hint wraps around; full-disk returns ENOSPC
- transaction abort reloads bitmap caches from disk, discarding
  speculative flips

### Result
Safe allocation/deallocation with stable free-count accounting
(verified by tests: alloc/free cycles are exactly balanced).

## [STEP 4] - Inodes, indirect mapping, directories

### Goal
Files and directories with 12 direct + 1 indirect block pointers,
timestamps, and reference counts; directory entries mapping names to
inode IDs.

### Design decision
128-byte inodes (32 per block). Directories are regular file data made
of fixed 512-byte entries (4-byte inode id, 2-byte name length, up to
255-char name); ino==0 marks an empty slot that inserts reuse, so
directories do not grow unboundedly under create/delete churn. Lookup
is a linear scan (O(n) in directory size, which satisfies the
"O(n log n) or better" requirement). `.`/`..` are not stored on disk;
they are resolved lexically during path walking.

### Implementation summary
- `src/inode.c`: inode table access, logical->physical mapping with
  on-demand allocation of data and indirect blocks, truncate.
- `src/dir.c`: iterate/lookup/add/remove/is_empty.

### Data structures affected
- inode table, directories, block bitmap (via allocation)

### Disk layout impact
Max file size = (12 + 1024) blocks = ~4.05 MiB at 4 KiB blocks.

### Edge cases handled
- names longer than 255 bytes rejected (ENAMETOOLONG)
- sparse files: unmapped logical blocks read back as zeroes
- indirect block freed together with data blocks on truncate

### Result
Full file/directory tree with timestamps and link counts.

## [STEP 5] - High-level operations and path resolution

### Goal
mkfs, mount (with recovery), and journaled namespace operations:
mkdir, create, write (replace/append), read, unlink, stat.

### Design decision
Every mutating operation is exactly one transaction. File data writes
take a hybrid path: blocks that are free in the *last committed* bitmap
are written directly (they are unreachable until the metadata commit,
so a crash cannot expose half-written data), while blocks already
referenced on disk (e.g. the partially filled last block of an append)
go through the journal. This keeps bulk writes at one I/O per data
block while preserving the no-corruption guarantee. A second in-memory
bitmap (`bmap_disk`, the committed state) makes this decision O(1).

### Implementation summary
- `src/fs.c`: mkfs layout computation (1 inode per 8 KiB), mount =
  superblock validate -> journal recover -> reload caches, absolute
  path resolution with `.`/`..` normalization, and all namespace ops.

### Data structures affected
- all of them: every op composes alloc + inode + dir mutations into one
  transaction

### Disk layout impact
mkfs marks the entire metadata region used in the block bitmap;
inode 0 is reserved, root is inode 1.

### Edge cases handled
- write-to-missing-path auto-creates the file inside the same txn
- overwrite first frees old blocks; the txn-freed list (STEP 3)
  guarantees the rewrite does not land on still-referenced blocks
- rm of a non-empty directory rejected (ENOTEMPTY)
- EFBIG on writes beyond the 12+1024 block mapping

### Result
Complete persistent filesystem API; survives restart between every
operation.

## [STEP 6] - slopfs CLI

### Goal
`slopfs create <img> <size>` and `slopfs shell <img>` with mkdir,
touch, write, append, read/cat, ls, rm, stat, import/export, df.

### Design decision
The shell reads commands from stdin (interactive prompt only on a TTY),
so the same binary is scriptable for tests. `import`/`export` move
files between host and image, which is also how the engineering log is
installed into images. `stat` reports the block list and a
fragmentation score (non-contiguous transitions / total transitions).

### Implementation summary
- `src/shell.c`: tokenizer with quoted-string support, size parser
  (100MB/1GB/...), all commands mapped to the fs API.

### Data structures affected
None (consumer of the API).

### Disk layout impact
None.

### Edge cases handled
- every operation flushes through the journal before the prompt returns,
  so a killed shell never leaves an inconsistent image

### Result
All required commands operate directly on the image.

## [STEP 7] - slopfs-view real-time inspector

### Goal
Live ncurses visualization: block map grid with color states, an
expandable directory tree, and an inode inspector.

### Design decision
The viewer mounts read-only and polls the superblock every 200 ms; the
generation counter (bumped by every committed transaction, STEP 2)
tells it exactly when to re-read bitmaps, re-categorize blocks, and
reload expanded tree nodes — so every filesystem mutation appears
within one poll tick. Block cells aggregate N blocks when the disk has
more blocks than screen cells; categories are prioritized
journal > fragmented > metadata > data > free. Blocks of a file whose
extents are non-contiguous are shown in the "fragmented" color.

### Implementation summary
- `src/view.c`: block categorizer (walks the inode table), block map
  renderer, lazy-loading directory tree with expansion preserved across
  reloads, inode inspector (pointers, size, frag score, timestamps,
  refcount).

### Data structures affected
None on disk (strictly read-only).

### Disk layout impact
None.

### Edge cases handled
- **Bug fix**: journal recovery originally ran on read-only mounts too,
  meaning the viewer could write to an image concurrently being written
  by the shell. Recovery is now skipped entirely when read-only.
- torn concurrent superblock reads are detected by CRC and simply
  retried next tick

### Result
Mutations from the shell appear in the viewer in real time (verified
with a pty-driven test).

## [STEP 8] - Crash injection and test suite

### Goal
Prove the recovery guarantees instead of asserting them.

### Design decision
Deterministic crash points inside the commit protocol, switched by the
`SLOPFS_CRASH` environment variable: `intent` kills the process after
the intent log but before the commit record (must roll back), `commit`
kills it after the commit record but before checkpoint (must replay).
Plus a non-deterministic kill -9 while a writer loops, then full
content verification of every surviving file.

### Implementation summary
- `crash_point()` in `src/journal.c`; `tests/run_tests.sh` with 16
  checks: basics, restart persistence, overwrite/append, space
  reclamation, 3 MB indirect-block roundtrip (cmp-verified), rollback,
  replay, kill -9 under load, alloc/free balance, and 100,000 files +
  100 dirs on a 1 GB image (completed in ~31 s; inode accounting
  verified exactly).

### Data structures affected
None (test-only hooks).

### Disk layout impact
None.

### Edge cases handled
- rollback leaves prior contents byte-identical
- replayed transaction is visible after remount
- random kill leaves every surviving file readable with correct content

### Result
16/16 tests pass; both journal recovery paths are exercised
deterministically.

## [STEP 9] - Format version 2: extent-based file mapping

### Goal
Replace the v1 direct/indirect block-pointer scheme with extents
(`{start_block, block_count}`) so contiguous data is described in O(1)
metadata instead of one pointer per block, and bump the on-disk format
version to 2 while keeping v1 images readable.

### Design decision
An inode holds 10 inline extents plus one on-demand overflow block of
512 more (522 max). Appends merge into the last extent when physically
contiguous, so a sequentially written file of any size stays at exactly
one extent. v2 files have no holes: every logical block under the file
size is mapped, which makes size-vs-extent-sum a checkable invariant
instead of an ambiguity. The version field in the superblock chooses
the decoder; per-inode flag bit 0 marks which mapping layout the inode
bytes 40..127 use, which is what makes in-place migration restartable.

### Implementation summary
- `src/format.h`/`src/codec.c`: v2 inode layout (nextents, ext_block,
  ext[10]) with the v1 decoder kept for migration.
- `src/inode.c`: `sfs_ext_get/append`, logical->physical lookup over
  extents, shrink-with-extent-split, truncate.

### Data structures affected
- inode table (bytes 40..127 reinterpreted), new overflow extent blocks

### Disk layout impact
Region layout unchanged. Superblock version=2. Max file mapping is now
522 extents — over 8 GB even at a pessimistic 16 blocks/extent, versus
the v1 hard cap of 4.05 MiB.

### EXTENT MODEL IMPACT
This step *is* the extent model: mapping cost becomes proportional to
fragmentation, not file size. A 200 MB sequential file costs 8 bytes of
map; the same file in v1 would have been impossible (pointer cap). The
overflow block is the only indirection left, and it is only paid by
files with >10 discontiguous extents.

### REQUIRED INVARIANTS
- extent.block_count > 0 for every stored extent
- extents of a file are disjoint from each other and from all other
  allocated blocks
- sum(block_count) covers exactly ceil(size/4096) logical blocks for
  files; equals size/4096 for directories
- nextents <= 522; nextents > 10 implies ext_block != 0
- ext_block, if set, is an allocated block owned by exactly this inode

### Edge cases handled
- shrinking a file that ends mid-extent splits the boundary extent and
  frees only the tail
- overflow block freed automatically when a shrink drops the file back
  to <= 10 extents
- EFBIG when an append would need a 523rd extent

### Result
All file mapping is extent-based; the format spec is updated to v2.

## [STEP 10] - Free-extent allocator

### Goal
Allocation that finds contiguous runs in O(log n) and keeps free space
coalesced, replacing the v1 next-fit bitmap scan.

### Design decision
The journaled block bitmap stays authoritative; a sorted, coalesced
in-memory run list `{start, len}` is derived from it at mount and
maintained incrementally. Allocation is best-fit (smallest run that
satisfies the request, lowest start as tie-break — fully deterministic,
which STEP 15 relies on), falling back to the largest run when nothing
fits, with the caller looping. `sfs_ext_alloc_at` extends a file's last
extent in place when the next physical block is free. Frees inserted by
binary search merge with both neighbors, so free space re-coalesces
continuously. Blocks freed inside an open transaction are quarantined
(`txn_freed`) and only join the run list at commit: their old contents
may still be referenced by the last committed metadata.

### Implementation summary
- `src/alloc.c`: run list (binary-search insert/merge), best-fit
  `sfs_ext_alloc`, in-place `sfs_ext_alloc_at`, deferred frees,
  `sfs_runs_rebuild` (mount/abort/repair), largest-run query for stats.

### Data structures affected
- block bitmap (unchanged on disk), new in-memory run list

### Disk layout impact
None — the run list is volatile and always rebuildable from the bitmap.

### EXTENT MODEL IMPACT
The allocator is what makes extents *stay* big: best-fit avoids
shredding large runs for small requests, alloc-at makes appends extend
extents instead of adding new ones, and free-side coalescing means
deleted files return contiguous space. Fragmentation ratio (1 -
largest_run/free) is now a first-class, measurable quantity.

### REQUIRED INVARIANTS
- run list is sorted by start, runs are non-empty, non-overlapping, and
  non-adjacent (adjacent runs must have been merged)
- every block in a run is free in the *committed* bitmap (this is what
  makes freshly allocated blocks safe to write without journaling)
- no block appears in both the run list and the txn-freed list
- bitmap popcount == sb.free_blocks at all times

### Edge cases handled
- allocation requests larger than any run return the largest run and
  the caller loops (file becomes multi-extent instead of failing)
- transaction abort rebuilds the run list from the on-disk bitmap,
  discarding speculative allocations
- double alloc/free still asserts on the bitmap bit state

### Result
Deterministic O(log n) extent allocation with continuous coalescing.

## [STEP 11] - Hash-indexed directories

### Goal
Amortized O(1) directory lookup at 100k+ entries (v1 was a linear
scan), efficient rename/move, and a stable `ls` order.

### Design decision
Directory logical block 0 becomes a bucket block: 1023 FNV-1a hash
buckets holding chain heads, plus a free-slot hint. Entries keep the
512-byte v1 slot shape (so migration is a re-link, not a re-encode) and
gain a `next` chain field. Insert prepends to the bucket chain and
reuses the lowest free slot; iteration is by slot order, independent of
hash order, giving stable `ls`. Rename within a directory is
remove+add in one transaction; cross-directory is add-then-remove in
one transaction, so a crash never loses the file. Directory blocks are
fresh-block writes wherever possible (STEP 12 hybrid rule), so even a
large directory growth journals only the bucket block and inode.

### Implementation summary
- `src/dir.c`: bucket init, chained lookup/add/remove, contiguous
  directory extension via alloc-at, slot-order iterate, emptiness check.
- `src/fs.c`: rename with lexical subtree guard (a directory cannot be
  moved into its own descendant).

### Data structures affected
- directory data format (bucket block + chained slots)

### Disk layout impact
Every directory costs one extra block (the bucket block). An empty
directory is 2 blocks: buckets + one slot block.

### EXTENT MODEL IMPACT
Directories are themselves extent-mapped files; growth prefers
extending the last extent, so even huge directories stay at a handful
of extents. Lookup cost is now independent of directory size, which
also bounds the per-entry work of fsck's directory pass.

### REQUIRED INVARIANTS
- every live slot is reachable from exactly one bucket chain, and that
  bucket is FNV1a(name) % 1023
- chains are cycle-free and never link an empty slot
- the slot count derived from directory size matches the mapped blocks
  (size = nblocks * 4096 exactly)
- every dirent references an allocated inode of a valid type; nlinks
  equals the number of referencing dirents (root: zero)

### Edge cases handled
- hash collisions (chains), bucket exhaustion impossible (chains grow)
- chain walk is bounded by total slot count -> a corrupted cycle is
  detected, not spun on
- free-slot hint is only a hint; stale hints fall back to scanning
- **Bug fix**: directories originally grew one block per extension, so
  a directory filled under interleaved allocation (file data taking the
  adjacent block every time) gained one extent per block and hit the
  522-extent cap at ~4,200 entries — create/mkdir in it then failed
  with EFBIG. Growth is now geometric (doubling, capped at 64 blocks
  per extension), keeping even an adversarially interleaved directory
  at ~nblocks/64 extents (measured: 8,000 entries = 21 extents)

### Result
O(1) amortized lookup verified against 100k files; `ls` order stable
across inserts/removes.

## [STEP 12] - Grouped transactions, hybrid writes, invariant hooks

### Goal
Multi-operation atomicity, bounded journal usage for arbitrarily large
operations, and machine-checked consistency after every commit.

### Design decision
Transactions nest by depth-counting: inner begin/commit pairs join the
outer transaction and the real commit happens at depth 0, so compound
operations (create+write, migrate-inode, fsck repairs) are one atomic
group. The journal stays bounded because of the hybrid write rule
(`sfs_bput`): a block that is free in the last *committed* bitmap is
written directly — it is unreachable until the commit record lands, so
a crash cannot expose it — while blocks already referenced by committed
metadata are staged through the journal. Data, directory slots, and
extent overflow blocks all use this rule. In-place overwrites that fit
the existing mapping (<= 64 blocks) go through the journal as full data
journaling; larger overwrites allocate fresh extents instead. With
`SLOPFS_DEBUG_INVARIANTS=1` every depth-0 commit runs the full deep
checker and aborts on the first violation, turning every test run into
a consistency proof.

### Implementation summary
- `src/journal.c`: txn depth, grouped commit, `sfs_bput`, post-commit
  invariant hook with re-entry guard.
- `src/fs.c`: in-place-overwrite vs reallocate decision, append RMW of
  the partial last block via the journal.

### Data structures affected
- journal (unchanged format; CLEAN header now retains last commit size
  for stats), all writers route through `sfs_bput`

### Disk layout impact
None.

### EXTENT MODEL IMPACT
Extent operations can touch many blocks (split, migrate, directory
rebuild); grouping + the hybrid rule keep the journaled footprint to
the metadata blocks only, so a transaction never exceeds 256 journal
images regardless of data volume.

### REQUIRED INVARIANTS
- at most one open transaction; commit only at depth 0; abort restores
  the exact pre-transaction in-memory state from disk
- every direct (non-journaled) write targets a block that is free in
  the committed bitmap
- after every commit: deep check passes (extents, directories, bitmap,
  counters, journal CLEAN)
- generation increases by exactly 1 per committed group

### Edge cases handled
- empty grouped commits (no staged blocks) are no-ops and do not bump
  the generation — callers that must commit something stage something
- abort inside a nested operation unwinds the entire group
- the invariant hook is suppressed while fsck itself commits repairs

### Result
Arbitrarily compound operations commit atomically with bounded journal
use; invariant mode runs the whole test suite without a violation.

## [STEP 13] - In-place v1 -> v2 migration

### Goal
Mounting a v1 image read-write upgrades it to v2 with contents
preserved byte-for-byte, safely under crash at any point.

### Design decision
Migration is one journaled transaction per inode, and the per-inode
extent flag records completion, so the process is restartable: a crash
mid-migration leaves a half-migrated image that the next mount simply
continues (already-flagged inodes are skipped). Files adopt their
existing blocks in place — the v1 block list is coalesced into extents
with zero data movement — unless they are so fragmented they exceed 522
extents, in which case they are rewritten compactly. Directories are
rebuilt into the hash-index format (the 512-byte slot shape is shared,
so live entries are collected and re-inserted). The final transaction
appends the migration record to the engineering log *and* flips the
superblock version in the same commit, so "migrated" becomes true
exactly once and atomically.

### Implementation summary
- `src/migrate.c`: v1 block-list walker, run coalescer, adopt-in-place
  and rewrite paths, directory rebuild, finalize txn.
- `src/fs.c`: mount dispatches on superblock version; v1 + read-only
  is refused (the viewer must not trigger migration).

### Data structures affected
- every inode, every directory, the superblock version field

### Disk layout impact
Indirect blocks are freed (they have no v2 meaning). Each directory
gains a bucket block. Data blocks do not move on the adopt path.

### EXTENT MODEL IMPACT
Migration is where v1 per-block pointers become extents: contiguous v1
files collapse to a single extent, and the run coalescer quantifies
exactly how fragmented the old image was. The 522-extent rewrite
fallback guarantees every v1 file is representable in v2.

### REQUIRED INVARIANTS
- file content is byte-identical before and after (checksum-verified)
- after each per-inode txn the image satisfies the full v2 invariants
  for migrated inodes while unmigrated inodes remain valid v1
- the version flip commits together with the log append (exactly-once)
- a crash at any point yields an image that mounts and completes
  migration on the next attempt

### Edge cases handled
- empty files, empty directories, nested directories
- v1 sparse files are rejected by the no-holes v2 rule on the rewrite
  path (fixture contains none; mkfs v1 never created holes via the shell)
- /docs is created if the v1 image lacked it, inside the finalize txn

### Result
The v1 fixture migrates with all md5 checksums intact and a clean deep
fsck; the migration is recorded in the in-image engineering log.

## [STEP 14] - slopfs-fsck: deep verification and repair

### Goal
An offline checker that proves the invariant list instead of trusting
it: extent overlap, ownership, directory index consistency, bitmap and
counter agreement, journal state — with optional repair.

### Design decision
One shared checker (`sfs_check`) serves three masters: the fsck binary,
the post-commit invariant hook, and the test suite. It builds a
reference bitmap by walking metadata regions and every inode's extents
(overlap = two owners claiming one block), then deep mode walks every
directory chain with a seen-set (cycles, wrong-bucket entries,
unreachable slots, dangling inode references) and cross-checks link
counts. Repair is deliberately conservative: remove dirents pointing at
invalid inodes, rebuild both bitmaps from the reference walk, and fix
the free counters — all in one journaled transaction, followed by a
re-check that reports what remains. Orphaned but well-formed metadata
is reclaimed via the bitmap rebuild rather than guessed back into the
namespace.

### Implementation summary
- `src/check.c`: reference-bitmap walk, per-directory chain
  verification, link-count pass, bitmap/counter comparison, journal
  header validation, repair transaction.
- `src/fsck.c`: CLI (`--deep`, `--repair`, `--quiet`), exit codes
  0/1/2, mounts read-write so journal recovery runs first.

### Data structures affected
- none on the check path; bitmaps + counters + offending dirents on the
  repair path

### Disk layout impact
None.

### EXTENT MODEL IMPACT
Extents make checking O(n_inodes + n_extents): the reference bitmap is
marked one *run* at a time instead of one pointer at a time, so a 1 GB
image checks in the time v1 took to walk its pointer tables. Overlap
detection is exact, not sampled.

### REQUIRED INVARIANTS
The checker *is* the invariant list (STEPs 9-12); additionally:
- repairs must themselves commit atomically and re-verify
- the checker never mutates state unless --repair is given
- exit code 0 strictly means "all listed invariants hold"

### Edge cases handled
- a flipped bitmap bit is reported as orphan/unreferenced and repaired
- repair re-runs the check so "repaired" is proven, not assumed
- the checker runs inside the post-commit hook without recursing

### Result
fsck detects injected bitmap corruption, repairs it, and re-verifies
clean; the same checker guards every commit in debug mode.

## [STEP 15] - Deterministic replay

### Goal
`slopfs replay disk.img <seed>`: reconstruct a byte-identical image
from a seed, so any reported state is reproducible from two integers
(seed, nops).

### Design decision
Determinism is achieved by removing every nondeterminism source rather
than recording a trace: timestamps come from a per-mount fake clock
(monotonic counter) and a fixed mkfs epoch; the allocator is already
fully deterministic (best-fit, lowest-start tie-break); the workload is
generated by xorshift64 from the seed. The op mix (35% create, 25%
append, 15% overwrite, 15% delete, 10% rename across 8 directories)
deliberately exercises extent split/merge/coalesce and directory churn.

### Implementation summary
- `src/replay.c`: seeded workload generator over the public fs API.
- `src/fs.c`: `fake_now` counter; `SLOPFS_FAKE_TIME` fixes mkfs times.

### Data structures affected
None on disk beyond what the workload itself writes.

### Disk layout impact
None.

### EXTENT MODEL IMPACT
Replay is the fragmentation stress rig: random create/delete/append
cycles fragment and re-coalesce free space, and identical seeds must
yield identical extent layouts — any hidden nondeterminism in the
allocator or write paths shows up as a byte diff.

### REQUIRED INVARIANTS
- same seed + nops => byte-identical image (cmp, not just fsck-equal)
- different seeds => different images (sanity)
- every replayed image passes deep fsck
- replay under SLOPFS_DEBUG_INVARIANTS completes without violation

### Edge cases handled
- ENOSPC during the workload is tolerated (op is skipped), keeping the
  generator deterministic even near disk-full
- rename collisions (-EEXIST) are tolerated the same way

### Result
Two runs of seed 42 produce bit-identical 64 MB images; seed 7 differs;
all replayed images deep-check clean.

## [STEP 16] - Stats engine and visualizer upgrade

### Goal
Quantify the extent model (`slopfs stats`, richer `df`/`stat`) and see
it: extent view, fragmentation heatmap, and free-space map in
slopfs-view.

### Design decision
Stats are computed from authoritative sources only: the run list for
largest-free-run and fragmentation ratio (1 - largest_run/free), one
inode-table sweep for file/dir/extent averages, and the journal header
for last-commit utilization. The viewer gains per-block owner and
extent-break maps built during its existing categorization sweep, and
four switchable render modes over the same cell grid: (1) category
block map, (2) extent view coloring each block by owning file, (3) a
heatmap coloring cells by extent-break density, (4) a free-space map
coloring free cells by the size of the run they belong to. Live
generation polling is unchanged, so all four views update in real time.

### Implementation summary
- `src/stats.c` + shell `stats` command and extended `df`/`stat`
  (extent list, frag score).
- `src/view.c`: per-mode cell renderers, per-mode legends, keys 1-4.

### Data structures affected
None on disk (read-only consumers).

### Disk layout impact
None.

### EXTENT MODEL IMPACT
The extent model becomes observable: avg extents/file measures how well
coalescing works, the heatmap shows *where* fragmentation lives, and
the free-space map shows whether deletes re-coalesce. These are the
metrics that validated STEPs 10-12 during development.

### REQUIRED INVARIANTS
- frag_ratio in [0,1]; largest_free_run <= free_blocks
- avg_extents_per_file >= 1 for any non-empty file population
- viewer renders every block exactly once per frame and never writes
  to the image (read-only mount, no recovery)

### Edge cases handled
- aggregated cells (blocks > screen cells) render the highest-priority
  state in the cell per mode
- torn concurrent superblock reads are CRC-detected and retried
- empty files (0 extents) report frag 0, not NaN

### Result
`slopfs stats` reports fragmentation/extent/inode/journal metrics; the
viewer shows extents, fragmentation hotspots, and free-space structure
live in four modes.
