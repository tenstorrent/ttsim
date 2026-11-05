#!/bin/sh
# SPDX-FileCopyrightText: (c) 2026 Tenstorrent USA, Inc.
# SPDX-License-Identifier: Apache-2.0

# Resolve and fetch every input of the ttsim-riscv64 image build. The amd64 tool container is pinned to one snapshot
# of the Ubuntu archive; the riscv64 packages come from ports.ubuntu.com as it is at run time, because
# snapshot.ubuntu.com does not serve ubuntu-ports (401 for every ports URL, checked 2026-11-02). The kept signed
# InRelease files and the MANIFEST pin exactly what was fetched.
# Runs on a networked Ubuntu host with apt-get, the Ubuntu archive keyring and podman; no root. It only
# downloads: nothing it fetches is executed except inside the tool container, by dpkg.
#
#     resolve.sh <snapshot, e.g. 20260430T000000Z> <out-dir>
#
# Writes the following, and refuses to replace an earlier resolve's output (it is the only way to rebuild
# that image):
#   <out-dir>/riscv64-image-inputs-<snapshot>/
#       debs/          the rootfs packages (packages.txt and its dependencies, riscv64), with SHA256SUMS
#       firmware/      opensbi (fw_jump), from the 26.04 suite, with SHA256SUMS
#       apt-lists/     the signed InRelease files and Packages indices apt resolved against
#       epoch          the snapshot time in seconds (SOURCE_DATE_EPOCH for the build)
#       MANIFEST       sha256 of every file above
#   <out-dir>/image-flow-tools-<snapshot>.tar, .config.json
#       the build's tool container (amd64: python3, e2fsprogs, zstd, device-tree-compiler from the same
#       snapshot, installed into ubuntu:noble from the local .debs with no network), as `podman export`;
#       load it with `podman import`
# Prints "resolve: DONE" or "resolve: FAILED: <step>".

set -u
S="${1:?usage: resolve.sh <snapshot> <out-dir>}"
D="${2:?usage: resolve.sh <snapshot> <out-dir>}"
HERE="$(cd "$(dirname "$0")" && pwd)"
KEYRING=/usr/share/keyrings/ubuntu-archive-keyring.gpg
case "$S" in [0-9][0-9][0-9][0-9][0-1][0-9][0-3][0-9]T[0-9][0-9][0-9][0-9][0-9][0-9]Z) ;; *)
    echo "resolve: FAILED: snapshot must look like 20260430T000000Z"; exit 1;; esac
fail() { echo "resolve: FAILED: $1"; [ -f "${2:-}" ] && tail -20 "$2"; exit 1; }
[ -r "$KEYRING" ] || fail "no $KEYRING (install ubuntu-keyring)"

O="$D/riscv64-image-inputs-$S"
W="$D/.resolve-$S"
[ ! -e "$O" ] && [ ! -e "$D/image-flow-tools-$S.tar" ] ||
    fail "$D already has a resolve for $S (the only inputs that rebuild its image): use another <out-dir>"
rm -rf "$W"
mkdir -p "$O/debs" "$O/firmware" "$O/apt-lists" "$W" || fail "mkdir $O"

# apt <name> <arch> <archive URL> <suites> <components>: a private apt (no host config or state) for one archive
apt_env() {
    A="$W/$1"
    mkdir -p "$A/etc/apt.conf.d" "$A/etc/sources.list.d" "$A/etc/preferences.d" "$A/etc/trusted.gpg.d" \
        "$A/state/lists/partial" "$A/cache/archives/partial" "$A/log"
    : > "$A/state/status"
    for s in $4; do
        echo "deb [arch=$2 signed-by=$KEYRING] $3 $s $5"
    done > "$A/etc/sources.list"
    cat > "$A/apt.conf" <<EOF
Dir::Etc::Parts "$A/etc/apt.conf.d";
Dir::Etc::Main "/dev/null";
Dir::Etc::SourceList "$A/etc/sources.list";
Dir::Etc::SourceParts "$A/etc/sources.list.d";
Dir::Etc::Preferences "$A/etc/preferences";
Dir::Etc::PreferencesParts "$A/etc/preferences.d";
Dir::Etc::TrustedParts "$A/etc/trusted.gpg.d";
Dir::State "$A/state";
Dir::State::Status "$A/state/status";
Dir::Cache "$A/cache";
Dir::Log "$A/log";
APT::Architecture "$2";
APT::Architectures { "$2"; };
APT::Install-Recommends "false";
APT::Install-Suggests "false";
Acquire::Languages "none";
Acquire::Check-Valid-Until "false";
Debug::NoLocking "true";
EOF
    APT_CONFIG="$A/apt.conf" apt-get update > "$A/update.log" 2>&1 || fail "apt-get update ($1)" "$A/update.log"
}
aptg() { A="$W/$1"; shift; APT_CONFIG="$A/apt.conf" apt-get "$@"; }

# The rootfs packages and their dependencies, resolved against an empty system. No initrd (the kernel mounts
# root=/dev/vda itself): initramfs-tools is only a Recommends of the kernel, and the pin makes resolving fail
# if anything comes to depend on an initramfs generator.
PORTS=http://ports.ubuntu.com/ubuntu-ports
apt_env rootfs riscv64 "$PORTS" "noble noble-updates noble-security" "main universe multiverse restricted"
printf 'Package: *initramfs* dracut*\nPin: version *\nPin-Priority: -1\n' > "$W/rootfs/etc/preferences"
aptg rootfs -y --download-only install $(grep -v '^#' "$HERE/packages.txt") > "$W/rootfs.log" 2>&1 ||
    fail "resolve the rootfs packages" "$W/rootfs.log"
mv "$W"/rootfs/cache/archives/*.deb "$O/debs/" || fail "no rootfs .debs"
cp "$W"/rootfs/state/lists/*InRelease "$W"/rootfs/state/lists/*Packages "$O/apt-lists/" || fail "copy apt lists"

# OpenSBI from 26.04 (noble's 1.3 is older than the v1.8 ttsim-riscv64 is tested with); architecture all
apt_env firmware riscv64 "$PORTS" "resolute" "main universe"
(cd "$O/firmware" && aptg firmware download opensbi) > "$W/firmware.log" 2>&1 || fail "fetch opensbi" "$W/firmware.log"

# The tool container: the tools' packages from the same snapshot, installed into ubuntu:noble with no network
# (apt orders the local .debs by their dependencies). The closure was resolved against an empty status, so it
# includes base packages (libc6, dpkg, ...) at the snapshot's versions, older than today's ubuntu:noble: allow the
# downgrades, so the container's packages are exactly the snapshot's.
apt_env tools amd64 "https://snapshot.ubuntu.com/ubuntu/$S" "noble noble-updates noble-security" "main universe"
aptg tools -y --download-only install python3 e2fsprogs zstd device-tree-compiler > "$W/tools.log" 2>&1 ||
    fail "resolve the tool packages" "$W/tools.log"
T="image-flow-tools-$S"
podman pull -q docker.io/library/ubuntu:noble > "$W/pull.log" 2>&1 || fail "podman pull ubuntu:noble" "$W/pull.log"
BASE="$(podman image inspect --format '{{.Digest}}' docker.io/library/ubuntu:noble)"
podman rm -f "$T" > /dev/null 2>&1
podman run --name "$T" --network=none -v "$W/tools/cache/archives:/debs:ro" docker.io/library/ubuntu:noble \
    sh -c 'apt-get install -y --allow-downgrades --no-install-recommends /debs/*.deb && dpkg-query -W > /tool-packages' \
    > "$W/podman.log" 2>&1 ||
    fail "install the tools in the container" "$W/podman.log"
podman export -o "$D/$T.tar" "$T" || fail "podman export"
podman container inspect --format '{{json .Config}}' "$T" > "$D/$T.config.json" || fail "inspect"
podman rm "$T" > /dev/null
echo "$BASE" > "$O/tools-base-image"
sha256sum "$D/$T.tar" | sed "s#  $D/#  #" > "$O/tools-sha256"

(cd "$O/debs" && sha256sum *.deb > SHA256SUMS) && (cd "$O/firmware" && sha256sum *.deb > SHA256SUMS) &&
date -u -d "$(echo "$S" | sed 's/^\(....\)\(..\)\(..\)T\(..\)\(..\)\(..\)Z$/\1-\2-\3 \4:\5:\6/')" +%s > "$O/epoch" &&
(cd "$O" && find . -type f ! -name MANIFEST | LC_ALL=C sort | xargs sha256sum > MANIFEST) || fail "manifest"
rm -rf "$W"
echo "resolve: $(ls "$O/debs" | grep -c '\.deb$') rootfs .debs, $(ls "$O/firmware"), tools base $BASE"
echo "resolve: MANIFEST sha256 $(sha256sum "$O/MANIFEST" | cut -c1-64)"
echo "resolve: DONE"
