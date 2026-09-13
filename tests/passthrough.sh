#!/bin/sh

# Exercise both sides of the open-path decision: ordinary local files must use
# passthrough in force mode, while members supplied by an archive must continue
# through rar2fs extraction I/O.  Exit 77 when the host cannot run FUSE
# passthrough integration tests.

set -u

RAR2FS=${RAR2FS:-../src/rar2fs}
RAR=${RAR:-rar}

skip()
{
        echo "SKIP: $*"
        exit 77
}

command -v "$RAR" >/dev/null 2>&1 || skip "rar command is not installed"
command -v fusermount3 >/dev/null 2>&1 || skip "fusermount3 is not installed"
command -v mountpoint >/dev/null 2>&1 || skip "mountpoint is not installed"
test -c /dev/fuse || skip "/dev/fuse is not available"
test -x "$RAR2FS" || skip "rar2fs has not been built"

tmp=${TMPDIR:-/tmp}/rar2fs-passthrough.$$
src=$tmp/source
archive_src=$tmp/archive-source
mnt=$tmp/mount
log=$tmp/rar2fs.log
local_log=$tmp/local.log
create_log=$tmp/create.log
archive_log=$tmp/archive.log
pid=

cleanup()
{
        exec 3<&-
        if mountpoint -q "$mnt" 2>/dev/null; then
                fusermount3 -u "$mnt" >/dev/null 2>&1 || true
        fi
        if test -n "$pid" && kill -0 "$pid" 2>/dev/null; then
                kill "$pid" 2>/dev/null || true
        fi
        test -z "$pid" || wait "$pid" 2>/dev/null || true
        rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$src" "$archive_src" "$mnt"
dd if=/dev/urandom of="$src/local.bin" bs=1M count=2 >/dev/null 2>&1 || exit 1
printf '%s\n' archive-data >"$archive_src/archived.txt"
(cd "$archive_src" && "$RAR" a -idq "$src/content.rar" archived.txt) || exit 1

# Deliberately include direct_io: rar2fs must suppress the global libfuse flag
# after negotiating passthrough, because the kernel gives FOPEN_DIRECT_IO
# precedence over FOPEN_PASSTHROUGH for reads and writes.
"$RAR2FS" "$src" "$mnt" -f -d -o passthrough=force,direct_io >"$log" 2>&1 &
pid=$!

i=0
while ! mountpoint -q "$mnt" 2>/dev/null; do
        if ! kill -0 "$pid" 2>/dev/null; then
                if grep -Eqi 'not supported|requires libfuse3|operation not permitted|/dev/fuse' "$log"; then
                        skip "FUSE passthrough is not supported by this host"
                fi
                cat "$log" >&2
                exit 1
        fi
        i=$((i + 1))
        test "$i" -lt 100 || skip "mount did not become ready"
        sleep 0.1
done

# Read a local file through cp.  A registered passthrough handle must keep the
# READ request in the kernel instead of dispatching it to the rar2fs daemon.
log_offset=$(( $(wc -c <"$log") + 1 ))
# Keep one handle open while cp opens the same FUSE inode again.  Both opens
# must share one backing ID; registering a second backing object makes the
# kernel reject the overlapping open with EIO.
exec 3<"$mnt/local.bin" || exit 1
cp "$mnt/local.bin" "$tmp/local-copy.bin" || exit 1
cmp "$src/local.bin" "$tmp/local-copy.bin" || exit 1
exec 3<&-
sleep 1
tail -c +"$log_offset" "$log" >"$local_log"
if ! grep -Eq 'passthrough backing id [1-9][0-9]* for .*local\.bin' \
                "$local_log"; then
        echo "local file was not registered as a passthrough backing file" >&2
        cat "$local_log" >&2
        exit 1
fi
backing_ids=$(sed -n 's/.*passthrough backing id \([1-9][0-9]*\) for .*local\.bin.*/\1/p' \
                      "$local_log" | sort -u | wc -l)
if test "$backing_ids" -ne 1; then
        echo "overlapping opens did not share one passthrough backing ID" >&2
        cat "$local_log" >&2
        exit 1
fi
if grep -q 'opcode: READ ' "$local_log"; then
        echo "local-file copy reached the FUSE read callback" >&2
        cat "$local_log" >&2
        exit 1
fi

# A newly created local file must return its backing ID as part of the CREATE
# reply.  Its payload writes must likewise stay out of the rar2fs daemon.
log_offset=$(( $(wc -c <"$log") + 1 ))
cp "$src/local.bin" "$mnt/created.bin" || exit 1
cmp "$src/local.bin" "$src/created.bin" || exit 1
sleep 1
tail -c +"$log_offset" "$log" >"$create_log"
if ! grep -Eq 'passthrough backing id [1-9][0-9]* for .*created\.bin' \
                "$create_log"; then
        echo "created file was not registered as a passthrough backing file" >&2
        cat "$create_log" >&2
        exit 1
fi
if grep -q 'opcode: WRITE ' "$create_log"; then
        echo "created-file copy reached the FUSE write callback" >&2
        cat "$create_log" >&2
        exit 1
fi

# There is no local archived.txt in the source tree.  A successful read here
# therefore verifies that force mode does not try to register archive handles
# as passthrough backing files.
log_offset=$(( $(wc -c <"$log") + 1 ))
printf '%s\n' archive-data | cmp - "$mnt/archived.txt" || exit 1
sleep 1
tail -c +"$log_offset" "$log" >"$archive_log"
if ! grep -q 'opcode: READ ' "$archive_log"; then
        echo "archive read unexpectedly bypassed the FUSE read callback" >&2
        cat "$archive_log" >&2
        exit 1
fi

exit 0
