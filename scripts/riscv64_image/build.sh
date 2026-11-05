#!/bin/sh
# SPDX-FileCopyrightText: (c) 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Offline, rootless build of the ttsim-riscv64 boot set from resolve.sh's inputs. No network, no root, no
# emulator: the packages are configured on ttsim-riscv64 itself.
#
#     build.sh <inputs-dir> <ttsim-riscv64> <minimal.dts> <out-dir>
#
# <inputs-dir> is resolve.sh's riscv64-image-inputs-<snapshot>. <ttsim-riscv64> must be v1.16.0 or later (it needs
# --disk-persist, the SiFive test device and --fw-dynamic-next, and earlier releases let guest code access host
# memory through device accesses), and <minimal.dts> needs its syscon-poweroff node (ttsim's own
# data/riscv64/minimal.dts has it).
# Needs python3, e2fsprogs >= 1.47 (mke2fs -d sorts directories), zstd, dtc and fdtput.
#
# Writes <out-dir>/: Image, fw_dynamic.bin, fw_dynamic.elf, fw_jump.bin, fw_jump.elf,
# ubuntu-riscv64-minimal.img (4 GiB ext4), rootfs.tar (the image's tree), rootfs.list (its canonical listing),
# SHA256SUMS, and stage2.log (the guest console). There is no initrd: the kernel mounts root=/dev/vda itself.
# Prints "build: DONE" or "build: FAILED: <step>". About 45 minutes on a loaded 32-core machine, most of it
# stage 2 (dpkg in the guest).

set -eu
I="$(realpath "${1:?usage: build.sh <inputs-dir> <ttsim> <minimal.dts> <out-dir>}")"
T="$(realpath "$2")"
DTS="$(realpath "$3")"
O="$4"
H="$(cd "$(dirname "$0")" && pwd)"
fail() { echo "build: FAILED: $1"; [ -f "${2:-}" ] && tail -c 3000 "$2"; exit 1; }

E="$(cat "$I/epoch")"
export SOURCE_DATE_EPOCH="$E" TZ=UTC LC_ALL=C
(cd "$I" && sha256sum --quiet -c MANIFEST) || fail "inputs do not match their MANIFEST"
KVER="$(ls "$I/debs" | sed -n 's/^linux-image-\([^_]*\)_.*/\1/p')"
[ -n "$KVER" ] || fail "no linux-image .deb in the inputs"
mkdir -p "$O/tmp"
cd "$O"
export TMPDIR="$PWD/tmp"

# Stage 1, on the host: every package's files in one tree, plus the .debs and guest scripts for stage 2
echo "build: stage 1 (kernel $KVER, SOURCE_DATE_EPOCH $E)"
python3 -I "$H/stage1.py" "$I/debs" "$I/debs/SHA256SUMS" "$H/guest" "$E" stage1.tar || fail "stage1.py"
python3 -I "$H/ext4tar.py" build stage1.tar stage2.img --size $((8 << 30)) --epoch "$E" || fail "stage-2 disk"
python3 -I "$H/ext4tar.py" get stage1.tar "boot/vmlinuz-$KVER" stage2-Image || fail "kernel from stage 1"
OPENSBI="$(ls "$I"/firmware/opensbi_*.deb)"
for f in fw_dynamic.bin fw_dynamic.elf fw_jump.bin fw_jump.elf; do
    python3 -I "$H/stage1.py" --file "$OPENSBI" "usr/lib/riscv64-linux-gnu/opensbi/generic/$f" "$f" ||
        fail "$f from $OPENSBI"
done

# Stage 2, on ttsim-riscv64: the same kernel, no initrd, init=/stage2/run; the guest powers off when done.
# fw_dynamic, not fw_jump: fw_jump copies the FDT to 0x82200000, inside a kernel image larger than 32 MiB
dtc -q -I dts -O dtb -o stage2.dtb "$DTS"
fdtput -t s stage2.dtb /chosen bootargs "earlycon=sbi console=ttyS0 root=/dev/vda rw init=/stage2/run"
echo "build: stage 2 on $T"
timeout 12h "$T" --max 100000000000000 --entry 0x80000000 --set-reg a0 0 --set-reg a1 0x88800000 \
    --fw-dynamic-next 0x80200000 --load-bin fw_dynamic.bin 0x80000000 --load-bin stage2.dtb 0x88800000 \
    --load-bin stage2-Image 0x80200000 \
    --disk stage2.img --disk-persist > stage2.log 2>&1 < /dev/null || true
grep -aq '^stage2: DONE' stage2.log || fail "stage 2 (see $O/stage2.log)" stage2.log
debugfs -R 'dump /stage2/rootfs.tar rootfs.tar' stage2.img > /dev/null 2>&1 && [ -s rootfs.tar ] ||
    fail "rootfs.tar from the stage-2 disk"

# The boot set
echo "build: image"
python3 -I "$H/ext4tar.py" build rootfs.tar ubuntu-riscv64-minimal.img --size $((4 << 30)) --epoch "$E" ||
    fail "ext4 image"
python3 -I "$H/ext4tar.py" get rootfs.tar "boot/vmlinuz-$KVER" Image || fail "Image"
python3 -I "$H/ext4tar.py" list --tar rootfs.tar > rootfs.list || fail "listing"
rm -f stage1.tar stage2.img stage2-Image stage2.dtb
rmdir tmp
sha256sum Image fw_dynamic.bin fw_dynamic.elf fw_jump.bin fw_jump.elf ubuntu-riscv64-minimal.img rootfs.tar \
    rootfs.list > SHA256SUMS
cat SHA256SUMS
echo "build: DONE"
