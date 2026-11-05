# How to build a ttsim-riscv64 bootable image

This recipe builds a bootable Ubuntu 24.04 (noble) boot set for `ttsim-riscv64` from scratch: OpenSBI firmware
(`fw_dynamic.bin`, `fw_dynamic.elf`, and `fw_jump.bin`, `fw_jump.elf`), a kernel `Image`, and
`ubuntu-riscv64-minimal.img`, a 4 GiB ext4 disk image with a minimal Ubuntu userspace. There is no initrd: the
kernel has virtio-blk and ext4 built in and mounts `root=/dev/vda` itself, which also makes the boot much faster.

The build needs no root, no loop mounts, and no QEMU. Every package's maintainer scripts run on a native riscv64
system, namely `ttsim-riscv64` itself. It is also deterministic: the same inputs give byte-identical outputs,
however many times you run it and wherever you run it.

The scripts are in [`scripts/riscv64_image/`](../scripts/riscv64_image/).

## What's in the image

The package set in `packages.txt` (328 packages: `systemd-sysv`, `sudo`, `build-essential`, `python3`, `vim-tiny`,
`strace` and the like, plus the kernel and its headers, so out-of-tree modules build in the guest with
`make -C /lib/modules/$(uname -r)/build M=$PWD modules`) and their dependencies, with Recommends off.
`guest/customize` then sets up:
- hostname `ubuntu`, a fixed `/etc/machine-id`, `/etc/fstab`, and the ext4 label `ttsim-root`;
- a user `ubuntu` with no password and passwordless `sudo`, logged in automatically on the serial console;
- `multi-user.target` as the default, with the timers, services and mounts a simulated boot doesn't need masked or
  disabled, for a faster, quieter boot;
- no apt sources: the simulated machine has no network device. To add a package, copy its .deb onto the disk and
  `sudo apt install ./<name>.deb`, or add it to `packages.txt` and build a new image.

The open login is on purpose: this image is for a simulator with no network, where the only way in is the
console you started. Don't boot it anywhere with a network.

## Prerequisites

To resolve (once, on a networked host):
- Ubuntu with `apt-get` and the Ubuntu archive keyring (`ubuntu-keyring`, which provides
  `/usr/share/keyrings/ubuntu-archive-keyring.gpg`);
- podman (rootless is fine);
- about 1 GB of downloads.

To build (offline, any x86_64 Linux host):
- podman (or docker);
- `ttsim-riscv64` v1.16.0 or later (`./make.py src/_out/release_riscv64/ttsim`, or a released binary), for
  `--fw-dynamic-next` and because earlier releases let guest code access memory outside the simulated machine
  through device accesses, and stage 2 runs every package's maintainer scripts as guest code;
- this tree's `data/riscv64/minimal.dts`;
- about 15 GB of free disk and 45 minutes or so. Stage 2 is most of it, and it runs on one core.

## Step 1: resolve the inputs

```bash
sh scripts/riscv64_image/resolve.sh 20260430T000000Z ~/riscv64-image
```

The argument is a timestamp on [snapshot.ubuntu.com](https://snapshot.ubuntu.com). It prints `resolve: DONE` or
`resolve: FAILED: <step>` and writes `~/riscv64-image/`:
- `riscv64-image-inputs-20260430T000000Z/`: `debs/` (every riscv64 package of the rootfs), `firmware/` (the
  `opensbi` package), `apt-lists/` (the signed `InRelease` files and `Packages` indices apt resolved against),
  `epoch` (the timestamp in seconds) and `MANIFEST` (the sha256 of every file);
- `image-flow-tools-20260430T000000Z.tar`: the tool container (amd64 `python3`, `e2fsprogs`, `zstd` and
  `device-tree-compiler`, installed into `ubuntu:noble` from the snapshot's packages with no network), as
  `podman export` writes it, plus its `.config.json`.

How it resolves: apt runs with a private configuration and an empty dpkg status, so nothing from your host leaks
in, and downloads the closure of `packages.txt` from noble, noble-updates and noble-security. It resolves no
initramfs generator: `initramfs-tools` is only a Recommends of the kernel package, and an apt pin makes resolving
fail if anything comes to depend on one. OpenSBI comes from the 26.04 (resolute) suite, as noble's is older than
the v1.8 `ttsim-riscv64` is tested with.

**The ports caveat.** snapshot.ubuntu.com only serves the main archive, not ubuntu-ports, which is where riscv64
lives. So the amd64 tools are pinned to the snapshot's timestamp, but the riscv64 packages are whatever
ports.ubuntu.com has the day you run `resolve.sh`. The downloaded inputs, checked by `MANIFEST`, are the pin: keep
the output directory for as long as you want to rebuild the same image. Running `resolve.sh` again later gives
newer packages and a different image, so give a new resolve its own directory. The timestamp still sets
`SOURCE_DATE_EPOCH` for the build; any file packaged after it gets its mtime clamped back to it.

## Step 2: build

Load the tool container, then run `build.sh` inside it, with no network:

```bash
podman import ~/riscv64-image/image-flow-tools-20260430T000000Z.tar image-flow-tools:20260430T000000Z
mkdir -p ~/riscv64-image/out
podman run --rm --network=none -e PATH=/usr/sbin:/usr/bin:/sbin:/bin \
    -v ~/ttsim:/ttsim:ro -v ~/riscv64-image:/work image-flow-tools:20260430T000000Z \
    sh /ttsim/scripts/riscv64_image/build.sh /work/riscv64-image-inputs-20260430T000000Z \
    /ttsim/src/_out/release_riscv64/ttsim /ttsim/data/riscv64/minimal.dts /work/out
```

(`~/ttsim` is this tree, with `ttsim-riscv64` built in it. With docker, use `docker import` and add
`--user "$(id -u):$(id -g)"` to `docker run`, so the outputs are yours.)

It prints `build: DONE` or `build: FAILED: <step>`, and writes to `out/`: `Image`, `fw_dynamic.bin`,
`fw_dynamic.elf`, `fw_jump.bin`, `fw_jump.elf`, `ubuntu-riscv64-minimal.img`, `rootfs.tar` (the image's tree),
`rootfs.list` (its canonical listing), `SHA256SUMS`, and `stage2.log` (the guest console of stage 2).

`build.sh` runs on any host with `python3`, `e2fsprogs` 1.47 or later, `zstd`, `dtc` and `fdtput`, but only the
tool container pins those tools' versions, and they affect the output's bytes (see "Determinism").

What it does:
1. **Stage 1** (`stage1.py`, on the host): checks every .deb against its sha256, and extracts all of their files
   into one merged-/usr tree. Nothing in a package is run. It adds `/stage2` (the .debs, the order to unpack them
   in, and `guest/*`), writes the tree as a canonical tarball, and `ext4tar.py` turns that into the stage-2 disk.
2. **Stage 2** (`guest/run`, on `ttsim-riscv64`): boots the kernel from stage 1 with `init=/stage2/run` and no
   initrd. As PID 1, it sets the guest clock to `SOURCE_DATE_EPOCH`, runs `dpkg --unpack` of every package
   (`base-passwd` first) and `dpkg --configure`, checks `dpkg --audit` is clean, runs `guest/customize`, deletes
   the logs and backups that record the run, tars `/` into `/stage2/rootfs.tar`, and powers off, which ends the
   simulation (`--disk-persist` keeps the disk's changes).
3. **Image** (`build.sh`): reads `rootfs.tar` back off the stage-2 disk with `debugfs`, and `ext4tar.py` builds
   the final ext4 image from it. `Image` is the kernel from the tree's `/boot`; the firmware comes from the
   opensbi package.

`ext4tar.py` builds ext4 images without root: `mke2fs -d` populates the image from a staging copy of the tarball
(every file owned by you), then one `debugfs` pass sets each inode's owner, mode, times and xattrs from the tar
headers, and creates the device nodes.

## Determinism

What pins each source of variance:
- **Inputs:** every .deb, the apt indices and the epoch, by `MANIFEST`, which `build.sh` checks first; the tools,
  by the tool container.
- **Time:** `SOURCE_DATE_EPOCH` is the snapshot's timestamp. Stage 1 clamps every mtime to it. The guest sets its
  clock to it before anything writes the root filesystem, so every file the guest writes is at or after it, and
  the guest's tar clamps them all back to it. ext4 uses it for every filesystem time (`E2FSPROGS_FAKE_TIME`), with
  each inode's atime, ctime and crtime equal to its mtime.
- **The guest:** `ttsim-riscv64` is deterministic (time is the instruction count; there is no host entropy), but
  the result mustn't depend on guest timing either, or any change to the simulator would move it. Hash seeds are
  fixed (`PYTHONHASHSEED`, `PERL_HASH_SEED`, `PERL_PERTURB_KEYS`), dbus's random `/var/lib/dbus/machine-id` is
  replaced with the fixed `/etc/machine-id`, and tar sorts by name, uses numeric owners, and drops atime and
  ctime.
- **ext4:** a pinned `mke2fs.conf`, a fixed UUID and directory hash seed. `mke2fs -d` (1.47) sorts directory
  entries and writes inode generation 0, so neither your filesystem's readdir order nor its type leaks in.

To check it yourself:
- Build twice into different output directories and compare the two `SHA256SUMS`: all eight files must match.
- Build once more with the guest's timing changed, by passing a wrapper as the `ttsim-riscv64` argument:

  ```bash
  printf '#!/bin/sh\nexec /ttsim/src/_out/release_riscv64/ttsim --timer-insns-per-tick 20 "$@"\n' > ~/riscv64-image/ttsim-slow
  chmod +x ~/riscv64-image/ttsim-slow
  ```

  and `/work/ttsim-slow` in place of `/ttsim/src/_out/release_riscv64/ttsim` in the build command. The guest
  clock and every timer interrupt move, and the outputs must still match.
- If two builds ever differ, `diff` their `rootfs.list` files: one line per path (type, mode, owner, mtime, size,
  sha256 or link target, xattrs) shows what moved. `python3 scripts/riscv64_image/ext4tar.py list --img <image>`
  makes the same listing from an image.

What isn't pinned: the `ttsim-riscv64` version (it can only matter through guest timing, which the check above
covers), and the archives themselves, which are trusted to serve what they signed (the `MANIFEST` catches any
later change).

For reference, inputs resolved on 2026-11-02 with snapshot `20260430T000000Z` (335 .debs, kernel
6.17.0-14-generic, OpenSBI 1.8.1) built twice, once with `--timer-insns-per-tick 20`, to byte-identical outputs,
with `ubuntu-riscv64-minimal.img` at sha256
`b331440162c552008f80235c8d5027ea7d4f6229744fff491f9a2de2e71fe106`. Your own resolve will have newer packages,
so your hashes will differ, but your builds must all agree with one another.

## Booting it

Compile the device tree, then boot:

```bash
dtc -q -I dts -O dtb -o minimal.dtb data/riscv64/minimal.dts
src/_out/release_riscv64/ttsim -i \
    --entry 0x80000000 --set-reg a0 0 --set-reg a1 0x88800000 \
    --fw-dynamic-next 0x80200000 --load-bin ~/riscv64-image/out/fw_dynamic.bin 0x80000000 \
    --load-bin minimal.dtb 0x88800000 \
    --load-bin ~/riscv64-image/out/Image 0x80200000 \
    --disk ~/riscv64-image/out/ubuntu-riscv64-minimal.img
```

Boot `fw_dynamic`, not `fw_jump`: `fw_jump` copies the device tree to 0x82200000, which is inside this kernel
(43.7 MB), so the kernel's BTF is overwritten and every module that carries BTF fails to load. `fw_dynamic` leaves
the device tree where `a1` points, and `--fw-dynamic-next` tells it where the kernel is.

You're logged in as `ubuntu` at the shell prompt in a minute or so. Guest writes to the disk are discarded at
exit unless you add `--disk-persist`.

To end the run, `sudo poweroff` in the guest: the kernel powers off through the device tree's `syscon-poweroff`
node, and `ttsim-riscv64` exits with status 0.

To measure the boot instead, replace `-i` with `--max 50000000000 --stop-on-uart-text 'ubuntu@ubuntu:~$'`. The run
ends at the prompt and prints `ttsim: stop text found after <N> instructions, <W> s`. The instruction count is
deterministic for a given `ttsim-riscv64` build and boot set; on v1.16.0, the reference build above reaches the
prompt in 7 523 970 377 instructions.

## Scripted runs

To run a workload without typing, give the console its input, and mark what you want measured with text it prints:

```bash
src/_out/release_riscv64/ttsim --max 100000000000 \
    --entry 0x80000000 --set-reg a0 0 --set-reg a1 0x88800000 \
    --fw-dynamic-next 0x80200000 --load-bin ~/riscv64-image/out/fw_dynamic.bin 0x80000000 \
    --load-bin minimal.dtb 0x88800000 \
    --load-bin ~/riscv64-image/out/Image 0x80200000 \
    --disk ~/riscv64-image/out/ubuntu-riscv64-minimal.img \
    --uart-inject-at 8000000000 --uart-inject-file commands.txt > run.log
```

`--uart-inject-at <N> --uart-inject-file <file>` (or `--uart-inject-text <text>`) types the file's bytes into the
console once the guest has run `<N>` instructions; pick `<N>` past the boot count above, so the shell is there
to read it. For example, `commands.txt`:

```
echo WORK-''BEGIN; python3 -c 'print(sum(range(10**6)))' && echo WORK-''PASS || echo WORK-''FAIL; echo WORK-''END
sudo poweroff
```

The run ends with status 0 at the power-off, and `run.log` holds the console, with `WORK-PASS` or `WORK-FAIL` in
it. The `''` keeps each marker out of the console's echo of the typed line, so only the command's output contains
it. To count the instructions between two markers, end the run at each with `--stop-on-uart-text`, which prints
the count there (`--stop-on-uart-text WORK-BEGIN`, then `--stop-on-uart-text WORK-END`), and subtract: the runs
are deterministic, so both execute the same instructions up to the first marker.
