#!/usr/bin/env bash
# SlopFS test suite: persistence, crash recovery, extents, migration,
# fsck/repair, deterministic replay, invariant mode, scale.
set -u
cd "$(dirname "$0")/.."
SLOPFS=./slopfs
FSCK=./slopfs-fsck
IMG=$(mktemp -u /tmp/slopfs_test_XXXX.img)
PASS=0
FAIL=0

ok()   { PASS=$((PASS+1)); echo "  PASS: $1"; }
bad()  { FAIL=$((FAIL+1)); echo "  FAIL: $1"; }
check(){ if [ "$1" = "$2" ]; then ok "$3"; else bad "$3 (got '$1', want '$2')"; fi; }

sh_run() { printf '%b' "$1" | $SLOPFS shell "$IMG" 2>&1; }

echo "== 1. format + basic ops =="
$SLOPFS create "$IMG" 64MB >/dev/null || bad "create"
out=$(sh_run 'mkdir /docs\ntouch /docs/a.txt\nwrite /docs/a.txt "hello"\nread /docs/a.txt\n')
check "$out" "hello" "write+read"

echo "== 2. persistence across restart =="
out=$(sh_run 'read /docs/a.txt\n')
check "$out" "hello" "content survives restart"
out=$(sh_run 'ls /\n' | grep -c docs)
check "$out" "1" "directory survives restart"

echo "== 3. overwrite + append =="
sh_run 'write /docs/a.txt "first"\nappend /docs/a.txt "+second"\n' >/dev/null
out=$(sh_run 'read /docs/a.txt\n')
check "$out" "first+second" "overwrite+append"

echo "== 4. delete + space reclaim =="
free_before=$(sh_run 'df\n' | awk '/free blocks/{print $3}')
sh_run 'write /big.bin "data"\n' >/dev/null
sh_run 'rm /big.bin\n' >/dev/null
free_after=$(sh_run 'df\n' | awk '/free blocks/{print $3}')
check "$free_after" "$free_before" "blocks reclaimed after rm"
out=$(sh_run 'read /big.bin\n')
check "$out" "read: /big.bin: No such file or directory" "file gone after rm"

echo "== 5. large file as extents (3 MB import/export) =="
dd if=/dev/urandom of=/tmp/sfs_big.bin bs=1M count=3 2>/dev/null
sh_run "import /tmp/sfs_big.bin /big3m\nexport /big3m /tmp/sfs_big_out.bin\n" >/dev/null
if cmp -s /tmp/sfs_big.bin /tmp/sfs_big_out.bin; then
    ok "3MB file roundtrip"
else
    bad "3MB file roundtrip"
fi
ext=$(sh_run 'stat /big3m\n' | awk '/extents:/{print $2}')
check "$ext" "1" "768-block sequential write coalesced to 1 extent"

echo "== 6. crash before commit record -> rollback =="
printf 'write /ghost.txt "never"\n' | SLOPFS_CRASH=intent $SLOPFS shell "$IMG" >/dev/null 2>&1
out=$(sh_run 'read /ghost.txt\n')
check "$out" "read: /ghost.txt: No such file or directory" "uncommitted txn rolled back"
out=$(sh_run 'read /docs/a.txt\n')
check "$out" "first+second" "existing data intact after rollback"

echo "== 7. crash after commit record -> replay =="
printf 'write /phoenix.txt "risen"\n' | SLOPFS_CRASH=commit $SLOPFS shell "$IMG" >/dev/null 2>&1
out=$(sh_run 'read /phoenix.txt\n')
check "$out" "risen" "committed txn replayed after crash"

echo "== 8. crash during multi-block extent append =="
printf 'append /big3m "XYZ"\n' | SLOPFS_CRASH=intent $SLOPFS shell "$IMG" >/dev/null 2>&1
sh_run 'export /big3m /tmp/sfs_big_out.bin\n' >/dev/null
if cmp -s /tmp/sfs_big.bin /tmp/sfs_big_out.bin; then
    ok "extent append rolled back, file byte-identical"
else
    bad "extent append rollback corrupted file"
fi
printf 'append /big3m "XYZ"\n' | SLOPFS_CRASH=commit $SLOPFS shell "$IMG" >/dev/null 2>&1
sh_run 'export /big3m /tmp/sfs_big_out.bin\n' >/dev/null
{ cat /tmp/sfs_big.bin; printf 'XYZ'; } > /tmp/sfs_big_exp.bin
if cmp -s /tmp/sfs_big_exp.bin /tmp/sfs_big_out.bin; then
    ok "extent append replayed after commit-point crash"
else
    bad "extent append replay wrong content"
fi

echo "== 9. random kill -9 under load =="
( i=0; while :; do echo "write /load_$i.txt \"payload $i\""; i=$((i+1)); done ) \
    | $SLOPFS shell "$IMG" >/dev/null 2>&1 &
LOADPID=$!
sleep 0.7
kill -9 $LOADPID 2>/dev/null
wait $LOADPID 2>/dev/null
out=$(sh_run 'df\n' | grep -c "SLOPFS v2")
check "$out" "1" "mountable after kill -9"
n=$(sh_run 'ls /\n' | grep -c load_)
last_ok=1
for f in $(sh_run 'ls /\n' | awk '/load_/{print $NF}'); do
    got=$(sh_run "read /$f\n")
    i=${f#load_}; i=${i%.txt}
    [ "$got" = "payload $i" ] || { last_ok=0; bad "content of $f corrupt"; }
done
[ $last_ok -eq 1 ] && ok "all $n surviving files readable and correct"

echo "== 10. free-count consistency + rename =="
free1=$(sh_run 'df\n' | awk '/free blocks/{print $3}')
sh_run 'write /tmpf "x"\nrm /tmpf\n' >/dev/null
free2=$(sh_run 'df\n' | awk '/free blocks/{print $3}')
check "$free2" "$free1" "alloc/free cycle is balanced"
sh_run 'mkdir /mvdir\nwrite /mvsrc.txt "moved"\nmv /mvsrc.txt /mvdir/mvdst.txt\n' >/dev/null
out=$(sh_run 'read /mvdir/mvdst.txt\n')
check "$out" "moved" "cross-directory rename"
out=$(sh_run 'read /mvsrc.txt\n')
check "$out" "read: /mvsrc.txt: No such file or directory" "rename source gone"

echo "== 11. deep fsck on the worked image =="
$FSCK --deep --quiet "$IMG" >/dev/null 2>&1
check "$?" "0" "deep fsck clean after all of the above"

echo "== 12. stats sanity =="
stats=$($SLOPFS stats "$IMG")
echo "$stats" | grep -q "fragmentation:" && ok "stats reports fragmentation" \
                                         || bad "stats missing fragmentation"
echo "$stats" | grep -q "avg extents/file:" && ok "stats reports avg extents/file" \
                                            || bad "stats missing avg extents/file"
frag_ok=$(echo "$stats" | awk '/fragmentation:/{print ($2>=0 && $2<=1) ? 1 : 0}')
check "$frag_ok" "1" "fragmentation ratio in [0,1]"
aef_ok=$(echo "$stats" | awk '/avg extents\/file:/{print ($3>=1) ? 1 : 0}')
check "$aef_ok" "1" "avg extents/file >= 1"
run_ok=$(echo "$stats" | awk '/largest free run:/{print ($4>0) ? 1 : 0}')
check "$run_ok" "1" "largest free run > 0"

echo "== 13. v1 -> v2 migration preserves contents =="
MIG=$(mktemp -u /tmp/slopfs_mig_XXXX.img)
gunzip -c tests/fixtures/v1.img.gz > "$MIG"
ver=$(printf 'df\n' | $SLOPFS shell "$MIG" 2>/dev/null | grep -c "SLOPFS v2")
check "$ver" "1" "v1 image migrated to v2 on mount"
mig_ok=1
while read -r path md5; do
    printf 'export %s /tmp/sfs_mig_out\n' "$path" \
        | $SLOPFS shell "$MIG" >/dev/null 2>&1
    got=$(md5sum /tmp/sfs_mig_out | awk '{print $1}')
    [ "$got" = "$md5" ] || { mig_ok=0; bad "checksum of $path after migration"; }
done < tests/fixtures/v1_md5.txt
[ $mig_ok -eq 1 ] && ok "all fixture checksums survive migration"
out=$(printf 'read /docs/slopfs_engineering_log.md\n' | $SLOPFS shell "$MIG" \
      | grep -c "v1 -> v2 migration")
check "$out" "1" "migration recorded in in-image engineering log"
$FSCK --deep --quiet "$MIG" >/dev/null 2>&1
check "$?" "0" "migrated image passes deep fsck"
rm -f "$MIG" /tmp/sfs_mig_out

echo "== 14. corruption detection + repair =="
COR=$(mktemp -u /tmp/slopfs_cor_XXXX.img)
$SLOPFS create "$COR" 64MB >/dev/null
printf 'mkdir /d\nwrite /d/x "victim"\n' | $SLOPFS shell "$COR" >/dev/null
# flip a block-bitmap byte (block bitmap lives in block 1): blocks
# 4000..4007 become spuriously allocated -> orphans + bad free count
printf '\xff' | dd of="$COR" bs=1 seek=$((4096 + 500)) conv=notrunc 2>/dev/null
$FSCK --deep --quiet "$COR" >/dev/null 2>&1
check "$?" "1" "fsck detects flipped bitmap bits"
$FSCK --deep --repair --quiet "$COR" >/dev/null 2>&1
check "$?" "0" "fsck --repair fixes the bitmap"
$FSCK --deep --quiet "$COR" >/dev/null 2>&1
check "$?" "0" "image clean after repair"
out=$(printf 'read /d/x\n' | $SLOPFS shell "$COR")
check "$out" "victim" "file content intact through repair"
rm -f "$COR"

echo "== 15. deterministic replay =="
R1=$(mktemp -u /tmp/slopfs_rp1_XXXX.img)
R2=$(mktemp -u /tmp/slopfs_rp2_XXXX.img)
$SLOPFS replay "$R1" 42 300 >/dev/null
$SLOPFS replay "$R2" 42 300 >/dev/null
if cmp -s "$R1" "$R2"; then ok "same seed -> byte-identical images"
else bad "same seed produced different images"; fi
$SLOPFS replay "$R2" 7 300 >/dev/null
if cmp -s "$R1" "$R2"; then bad "different seeds produced identical images"
else ok "different seed -> different image"; fi
$FSCK --deep --quiet "$R1" >/dev/null 2>&1
check "$?" "0" "replayed image passes deep fsck"
rm -f "$R1" "$R2"

echo "== 16. invariant mode under load =="
RINV=$(mktemp -u /tmp/slopfs_inv_XXXX.img)
SLOPFS_DEBUG_INVARIANTS=1 $SLOPFS replay "$RINV" 5 200 >/dev/null 2>&1
check "$?" "0" "200-op replay with per-commit deep checks (no violation)"
out=$(printf 'mkdir /iv\nimport /tmp/sfs_big.bin /iv/b\nappend /iv/b "tail"\nmv /iv/b /iv/c\nrm /iv/c\n' \
      | SLOPFS_DEBUG_INVARIANTS=1 $SLOPFS shell "$RINV" >/dev/null 2>&1; echo $?)
check "$out" "0" "shell ops with per-commit deep checks (no violation)"
rm -f "$RINV"

echo "== 17. scale: 100,000 files on 1GB image =="
BIG=$(mktemp -u /tmp/slopfs_scale_XXXX.img)
$SLOPFS create "$BIG" 1GB >/dev/null
start=$(date +%s)
( for d in $(seq 0 99); do
      echo "mkdir /d$d"
      for f in $(seq 0 999); do echo "touch /d$d/f$f"; done
  done ) | $SLOPFS shell "$BIG" >/dev/null 2>&1
end=$(date +%s)
total=0
for d in 0 50 99; do
    c=$(printf 'ls /d%s\n' "$d" | $SLOPFS shell "$BIG" | grep -c "^file")
    total=$((total+c))
done
check "$total" "3000" "spot-check 3 dirs x 1000 files"
used_inodes=$(printf 'df\n' | $SLOPFS shell "$BIG" | awk '/inodes:/{print $2-$4}')
check "$used_inodes" "100102" "100k files + 100 dirs + root + reserved allocated"
echo "  (created 100,100 nodes in $((end-start))s)"
$FSCK --quiet "$BIG" >/dev/null 2>&1
check "$?" "0" "1GB image passes fsck"
rm -f "$BIG"

echo
echo "================================"
echo "PASS: $PASS  FAIL: $FAIL"
rm -f "$IMG" /tmp/sfs_big.bin /tmp/sfs_big_out.bin /tmp/sfs_big_exp.bin
[ $FAIL -eq 0 ]
