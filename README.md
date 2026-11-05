# ttsim, a fast full-system simulator of Tenstorrent hardware

`ttsim` provides a virtual Wormhole, Blackhole, or Quasar device that can run on any Linux/x86_64
or Linux/aarch64 system (including Windows via WSL2 and macOS via UTM/QEMU), without Tenstorrent
silicon required. It is slower than silicon but still fast enough that you can run interesting
workloads with good productivity, allowing you to explore and experiment with Tenstorrent's hardware
and programming model before purchasing silicon.

Each simulator consists of a single `libttsim.so` file compiled for a specific chip architecture
(Wormhole, Blackhole, or Quasar). This library exports a [simple API](docs/libttsim_api.md) that
[TT-Metalium](https://github.com/tenstorrent/tt-metal) knows how to communicate with.

## Distribution
This repository contains the open-source `ttsim` source code for Wormhole and Blackhole, released
under the Apache License 2.0. Prebuilt binaries for Wormhole, Blackhole, and Quasar for both Linux/x86_64
and Linux/aarch64 are also provided via [GitHub Releases](https://github.com/tenstorrent/ttsim/releases/latest);
Quasar is pre-silicon and ships as a binary only.

## Chip Status
- **Wormhole/Blackhole**: Feature complete and production quality. Can run many tt-metal, ttnn,
  tt-forge, and LLK examples/tests, as well as entire LLMs and other AI models.

- **Wormhole/Blackhole multichip**: `wh_x2` (N300), `wh_x8` (T3000/LoudBox), `wh_x32` (WH Galaxy),
  `bh_x2` (P300), `bh_x4` (QuietBox 2: two P300 cards), and `bh_x32` (BH Galaxy) configs are available,
  each its own binary. Testing status is under [Known Issues](#known-issues).

- **Quasar**: Approaching feature complete and now alpha quality for slow dispatch mode. Some features
  remain, but the bulk of core Tensix and DM functionality is present and well tested. Can run many
  tt-metal, ttnn, and LLK examples/tests, with multiple AI model bringups actively underway.

## Getting Started

### Prerequisites
- [TT-Metalium](https://github.com/tenstorrent/tt-metal) installed and built
- Set `TT_METAL_HOME` environment variable (e.g., `export TT_METAL_HOME=~/tt-metal`)

### Installation
Download the simulator binary for your target chip from the [releases page](https://github.com/tenstorrent/ttsim/releases).
Replace `vX.Y` with the desired version number.

```bash
mkdir -p ~/sim
cd ~/sim

# Download simulators
wget https://github.com/tenstorrent/ttsim/releases/download/vX.Y/libttsim_wh.so
wget https://github.com/tenstorrent/ttsim/releases/download/vX.Y/libttsim_bh.so
wget https://github.com/tenstorrent/ttsim/releases/download/vX.Y/libttsim_qsr.so
```

Multichip binaries are named by config, e.g., `libttsim_wh_x2.so`. Linux/aarch64 binary releases are suffixed
with `_aarch64`, e.g., `libttsim_wh_aarch64.so`.

### Building from Source (Wormhole and Blackhole)
The included source can be built locally:

```bash
./make.py :build
```

Requires g++ with C++20 support and Python 3.9+. Builds release `.so` files at
`src/_out/release_wh/libttsim.so` and `src/_out/release_bh/libttsim.so`, functionally
equivalent to the corresponding GitHub Releases binaries (and byte-identical when built
on the same toolchain as our release CI: `ubuntu-24.04` with its default g++).

For the `libttsim.so` programmatic interface - the exported C entry points, the ABI, and the
compatibility policy that governs them - see [libttsim API and ABI](docs/libttsim_api.md).

### Running with TT-Metalium
Metal has simulator support out of the box: set the `TT_METAL_SIMULATOR` environment variable to the simulator
`.so` file, either a downloaded one or one you built (e.g., `src/_out/release_wh/libttsim.so`).

#### Wormhole and Blackhole
```bash
export TT_METAL_SIMULATOR=~/sim/libttsim_wh.so # or libttsim_bh.so, or a multichip binary

cd $TT_METAL_HOME
TT_METAL_SLOW_DISPATCH_MODE=1 ./build/programming_examples/metal_example_add_2_integers_in_riscv
```

Wormhole and Blackhole need no SoC descriptor as of tt-metal 5028883 (#62711, first in v0.83.0-dev20261105):
tt-metal reads the architecture from the PCI device ID the simulator reports, and on a multichip binary it finds the
chips and their Ethernet links the way it does on silicon. Older tt-metal, and LLK tests (tt-exalens still reads it),
need `wormhole_b0_80_arch.yaml` or `blackhole_140_arch.yaml` from `$TT_METAL_HOME/tt_metal/soc_descriptors/` copied
beside the `.so` as `soc_descriptor.yaml`. A `soc_descriptor.yaml` there always takes precedence, so delete any left
over from older instructions.

#### Quasar
Quasar still needs its SoC descriptor beside the `.so`, so give it a directory of its own:
```bash
mkdir -p ~/sim/qsr && mv ~/sim/libttsim_qsr.so ~/sim/qsr/
cp $TT_METAL_HOME/tt_metal/soc_descriptors/quasar_32_arch_ttsim.yaml ~/sim/qsr/soc_descriptor.yaml
export TT_METAL_SIMULATOR=~/sim/qsr/libttsim_qsr.so

cd $TT_METAL_HOME
TT_METAL_SLOW_DISPATCH_MODE=1 ./build/test/tt_metal/unit_tests_legacy --gtest_filter=QuasarMeshDeviceSingleCardFixture.SingleDmL1Write
```

## Running an LLM with tt-lab
[tt-lab](https://github.com/tenstorrent/tt-lab) runs gpt-oss-20b or gpt-oss-120b on a virtual
1-chip or 4-chip Blackhole system. It is a single C++ binary that `dlopen`s `libttsim.so` and
drives it through the [libttsim API](docs/libttsim_api.md) directly: no TT-Metalium, no compute
library, nothing between the model and the hardware. The transformer itself - attention, expert
matvecs, normalization, activations - runs as BRISC firmware inside the Tensix tiles, with all data
movement written out explicitly.

```bash
# Build a Blackhole simulator (or release_bh_x4 for the four-chip, 120b mapping):
./make.py src/_out/release_bh/libttsim.so

# Then, in tt-lab, once its GGUF weights have been requantized into a .ttq sidecar:
_out/tt-lab run -m ~/models/gpt-oss-20b-MXFP4.gguf \
    --sim ~/ttsim/src/_out/release_bh/libttsim.so \
    --ttq ~/models/gpt-oss-20b.ttq -p "Hi" --tiles 8 -n 1 --check
```

The whole path - GGUF loading, tokenizer, transformer, tile firmware, SFPU kernels - is roughly
12,000 lines of C++ with no framework underneath it. It is small enough to read end to end, and
complete enough that doing so leaves you with an entire accelerated inference stack, simple but
real, from the prompt down to the instructions issued on a Tensix tile.

A host-side bit-exact device proxy acts as the reference: `--check` requires every logit produced
under `--sim` to match it bit-for-bit, the same standard `ttsim` holds itself to
[against silicon](#numerical-accuracy-timing-and-ordering). Editing a kernel and re-checking a full 24-layer model
against an exact reference needs no silicon at all.

See tt-lab's README for model downloads, the requantization step, and its simulator compatibility
notes.

## Running ttsim as a QEMU PCI Device
[ttsim-qemu](https://github.com/tenstorrent/ttsim-qemu) adds a `ttsim` PCI device to `qemu-system-*`
that exposes `libttsim.so` to a guest VM over PCIe, letting [tt-kmd](https://github.com/tenstorrent/tt-kmd)
bind to it and surface `/dev/tenstorrent/0` inside the guest.

Build:
```bash
git clone git@github.com:tenstorrent/ttsim-qemu.git
cd ttsim-qemu
./configure --target-list=x86_64-softmmu && make -j$(nproc)
```

Run, with the BAR sizes for the chip:

| Chip | `bar0-size` | `bar4-size` |
| --- | --- | --- |
| Wormhole | `512M` (default) | `32M` |
| Blackhole | `512M` (default) | `32G` |
| Quasar | `4G` | `512G` |

```bash
qemu-system-x86_64 ... \
    -device ttsim,lib=/path/to/libttsim.so,bar0-size=512M,bar4-size=32M
```

On Quasar, libttsim implements the host MMIO path through its PCIe device:
```bash
qemu-system-x86_64 ... \
    -device ttsim,lib=/path/to/libttsim.so,bar0-size=4G,bar4-size=512G
```

A multi-chip `libttsim.so` (e.g. `wh_x32`/`bh_x32`, or the 4-MMIO `wh_x8`) presents several
host-visible chips through the one process-wide library. Add one `-device ttsim` per MMIO chip,
all pointing at the same library and using the BAR sizes for the chip (Wormhole shown); each
surfaces as its own PCI device and, in the guest, its own `/dev/tenstorrent/N`. Devices auto-assign
the lowest free chip number (or pass `index=N` to pin one):
```bash
qemu-system-x86_64 ... \
    -device ttsim,lib=/path/to/libttsim.so,bar0-size=512M,bar4-size=32M \
    -device ttsim,lib=/path/to/libttsim.so,bar0-size=512M,bar4-size=32M   # repeat NUM_MMIO_CHIPS times
```

Inside the guest, build and load `tt-kmd` to surface `/dev/tenstorrent/0`:
```bash
sudo apt install -y build-essential linux-headers-generic git
git clone https://github.com/tenstorrent/tt-kmd && cd tt-kmd && make
sudo insmod tenstorrent.ko
```

`qemu-system-aarch64` is also supported and will be faster on ARM-based host platforms.

## Full-System Simulation with ttsim-riscv64
Where the `libttsim.so` path models a Tenstorrent *chip*, **ttsim-riscv64** is the other face of
the simulator: a full-system simulator of an entire RISC-V computer that boots OpenSBI and the Linux
kernel (including up to 16 harts for SMP), all the way to an Ubuntu userspace shell, in approximately
1 minute. It is not a Tenstorrent chip: it is built with `-DTTSIM_RV64_SYSTEM=1` and leverages the
shared, tiny, efficient (>200 MIPS) RISC-V interpreter core already built into `ttsim`.

Build:
```bash
./make.py src/_out/release_riscv64/ttsim
```

The released ttsim-riscv64 binaries are built on Ubuntu 24.04 and need glibc 2.38 or newer (Ubuntu 24.04 or
later). On Ubuntu 22.04, build it from source as above.

Boot Linux (OpenSBI `fw_dynamic` firmware + a kernel `Image` + a flattened device tree), e.g. an 8-hart SMP
boot with a virtio-blk disk and an interactive console:
```bash
src/_out/release_riscv64/ttsim --harts 8 -i \
    --entry 0x80000000 --set-reg a0 0 --set-reg a1 0x88800000 \
    --fw-dynamic-next 0x80200000 \
    --load-bin fw_dynamic.bin 0x80000000 --load-bin Image 0x80200000 \
    --load-bin board.dtb 0x88800000 \
    --disk disk.img
```

A kernel with its disk driver and filesystem built in (like the one [docs/riscv64_image.md](docs/riscv64_image.md)
builds) mounts `root=/dev/vda` itself, so no initrd is needed. To boot your own initrd, add
`--load-bin initrd.img 0x84000000` and give the DTB's `/chosen` node `linux,initrd-start` and `linux,initrd-end`
properties for where it lies in memory.

`--fw-dynamic-next <addr>` gives OpenSBI's `fw_dynamic` the kernel's address and leaves the device tree where
`a1` points. `fw_jump.bin` boots too, without the option, but it copies the device tree to 0x82200000, which
overwrites part of a kernel image larger than 32 MiB (Ubuntu's riscv64 kernel is 43.7 MB).

`-i` attaches the simulated UART console to your terminal. Ctrl-C is hooked and passed into the
simulator, so use Ctrl-\ if you need to forcibly exit.

Guest writes to the `--disk` image go to a private copy and are discarded at exit; add `--disk-persist` to
write them to the image file itself.

`--stop-on-uart-text <text>` ends the run when the console output contains `<text>` (e.g. the shell prompt) and
prints the instruction count and wall time at that point, to measure boot time.

[docs/riscv64_image.md](docs/riscv64_image.md) shows how to build a bootable Ubuntu image (firmware, kernel and
disk image, no initrd) from scratch: no root or QEMU needed, and byte-for-byte reproducible. It needs ttsim
v1.16.0 or later.

Note that your .dtb must declare more harts in order for Linux to use them.

### Bringing up a Tenstorrent chip inside the guest
ttsim-riscv64 can host a Tenstorrent AI accelerator: pass `--tt-device <libttsim.so>` and it `dlopen`s
the chip model and presents it to the guest as a PCIe endpoint - an ECAM config region plus the chip's
BAR windows - just as [ttsim-qemu](#running-ttsim-as-a-qemu-pci-device) does for QEMU. The guest device
tree must declare a `pci-host-ecam-generic` bus at the ECAM base at `0x30000000`. Then
[tt-kmd](https://github.com/tenstorrent/tt-kmd) binds to it and exposes `/dev/tenstorrent/0`, providing
you with an entire simulated RISC-V computer with an AI hardware accelerator, running Linux - all in just
a few hundred KB of total simulator code, and with no external dependencies beyond libc.

```bash
# Build the Wormhole chip model (libttsim.so) and the ttsim-riscv64 host system:
./make.py src/_out/release_wh/libttsim.so src/_out/release_riscv64/ttsim

# Boot Linux with the Wormhole chip attached over (simulated) PCIe -- the DTB declares the ECAM bus:
src/_out/release_riscv64/ttsim -i \
    --entry 0x80000000 --set-reg a0 0 --set-reg a1 0x88800000 \
    --fw-dynamic-next 0x80200000 \
    --load-bin fw_dynamic.bin 0x80000000 --load-bin Image      0x80200000 \
    --load-bin board.dtb  0x88800000 \
    --tt-device src/_out/release_wh/libttsim.so --disk disk.img
```

Make sure your disk.img is already pre-populated with `build-essential linux-headers-generic` (the image
[docs/riscv64_image.md](docs/riscv64_image.md) builds has both) and the tt-kmd source. Inside the guest, build
and load `tt-kmd` exactly as for the QEMU path above:
```bash
cd tt-kmd && make
sudo insmod tenstorrent.ko # surfaces /dev/tenstorrent/0
```

## Known Issues
**Fast dispatch is not sufficiently tested**. It is believed to be fully functional, but run-to-run
determinism has not been adequately characterized, and simulations may take longer to run than under
slow dispatch. Unless debugging a fast-dispatch-specific issue, slow dispatch is still the recommended
mode of operation. Set `TT_METAL_SLOW_DISPATCH_MODE=1` to enable it.

SFPLOADMACRO is not supported in the SFPU. Set `TT_METAL_DISABLE_SFPLOADMACRO=1` to disable its usage.

Multichip testing is incomplete. Nearly every tt-metal C++ test runs in our CI on `wh_x2` and `bh_x2`, and
a Galaxy smoke set runs on `wh_x32` and `bh_x32` (in tt-metal's CI too); `wh_x8` and `bh_x4` are covered only by
our own Ethernet link tests. Fabric smoke tests pass on `bh_x2`; the larger fabric suites are too slow under the
simulator for now. The `bh_x32` binary is x86_64 only, since aarch64 can't link its extremely large BSS segment.

Not all hardware features are implemented, and the simulator is intentionally more restrictive than silicon
to help uncover potential issues. Simulator error messages are grouped into the following categories
to indicate whether the simulator or the software being simulated is at fault (see
[Simulator Error Handling](docs/sim_error_handling.md) for the full design rationale):
- **UndefinedBehavior, UnpredictableValue, NonContractualBehavior**: See
  [tt-isa-documentation glossary](https://github.com/tenstorrent/tt-isa-documentation/blob/main/Glossary.md)
- **UntestedFunctionality**: Feature is implemented but lacks sufficient test coverage to be enabled
- **UnimplementedFunctionality**: Feature not yet implemented but planned for future support
- **UnsupportedFunctionality**: Feature unlikely to be implemented without strong justification
- **SystemError/ConfigurationError**: OS errors or issues with command line options, environment variables,
  or configuration files
- **AssertionFailure**: Internal simulator bug

## Simulator Behavior Contract
`ttsim` serves both as a general-purpose pre-silicon AI accelerator simulator and as the official
golden reference implementation of the Tenstorrent ISA contract, designed to a level of rigor that
supports pre-silicon validation in safety-critical and regulated workflows (e.g. ISO 26262, DO-254,
IEC 62304/FDA software-as-medical-device guidance). The public source release, the public/private
binary-equivalence commitment, the strict spec/simulator coupling, and the explicit conformance
taxonomy are all aimed at customers who need evidence, not promises.

Beyond regulated industries specifically, the same discipline underpins the long-horizon
commitments enterprise deployments depend on: software portability and forwards/backwards
compatibility across silicon revisions, clear expectations about output stability for shipped AI
workloads across silicon updates, and defensible root-cause analysis when a customer investigates a
production incident or responds to an external audit. Each of these rests on the architecture being
precise about what is and isn't part of its contract - which is exactly what the
`UndefinedBehavior`/`NonContractualBehavior`/`UnsupportedFunctionality` taxonomy provides, and
exactly what `ttsim`'s strict enforcement protects.

The same discipline also serves academic and educational use of `ttsim` as a reference platform for
AI-accelerator microarchitecture, where legibility to an outside reader overlaps heavily with what
safety auditors need.

### Numerical Accuracy, Timing, and Ordering
`ttsim` is designed to provide **bit-exact** numerical results relative to silicon for all
computations, floating point and otherwise. The goal is to match all hardware computations
bit-for-bit across all instructions, opcodes, functional units, and special cases, including
the precise bit representation of NaNs produced by operations. While bugs are inevitable, since
testing can never be fully exhaustive, all known operations and code paths are verified and
believed to be fully bit-accurate in the Wormhole and Blackhole simulators.

Most code will achieve bit-exact results, but cases that can produce divergent results include:
- Computations with timing-dependent variation in operand order.
- Reads from hardware entropy sources or random number generators.
- Reads from performance counters, cycle counters, or timers.
- Missing synchronization, cache flushes, or memory fences.
- Execution of UndefinedBehavior or UnpredictableValue cases.
- Any other violations of ISA specification requirements.

`ttsim` is not cycle-accurate. Where timing is not fixed by the ISA and software synchronization,
it may complete operations, make writes visible, and order events in any way they permit, including
ways that are extremely unlikely on silicon; software that is correct only under silicon's usual
timing is outside this contract. To ensure an exact match with silicon, avoid algorithms where the
runtime order of operations affects the result. For example, floating-point reductions using
addition will diverge unless each addition is explicitly serialized in a deterministic order.

### Strictness vs. Silicon
The simulator may report `UndefinedBehavior` or `UnpredictableValue` for conditions that do not
produce visibly incorrect output on silicon under specific test workloads. This is by design.
UndefinedBehavior is not equivalent to "the device halts on this input"; it is the specification's
commitment that no behavior is guaranteed. Silicon executes some behavior on every input, and the
absence of a visible failure in a particular test does not constitute a defined contract. The
simulator's role is to flag the nonconformance at the point it occurs, before it propagates into a
non-portable correctness failure under a different workload, format combination, or silicon revision.
For `UnpredictableValue`, that point is where the value reaches architecturally visible state the
simulator doesn't track (in practice, the instruction that wrote it), whether or not anything reads it
later: past there, the simulator can no longer vouch for the run.

When the simulator reports such a condition, the resolution is to fix the software path that triggered
it. Suppressing or relaxing the simulator's check would convert a portable, locally detectable
correctness failure into a silent corruption risk that is non-portable across configurations and
revisions - strictly worse than the current state.

If a particular check appears to be incorrect, i.e., the underlying spec case should be defined rather
than UndefinedBehavior, then the correct resolution is to update the specification with the evidence
described in the [Glossary's UndefinedBehavior entry](https://github.com/tenstorrent/tt-isa-documentation/blob/main/Glossary.md#undefinedbehavior),
after which the simulator's check is updated to match. The simulator and the specification stay in
lockstep; neither is relaxed independently.

### UnsupportedFunctionality
`UnsupportedFunctionality` describes hardware features that exist in silicon but are deliberately
not implemented in `ttsim`. The category is distinct from `UnimplementedFunctionality` (planned
future work): an `UnsupportedFunctionality` error means the feature is intentionally absent. A
modern AI accelerator has on the order of several thousand distinct architectural features per
generation, the substantial majority of which `ttsim` implements for Wormhole and Blackhole; much
of the remainder is intentionally left as `UnsupportedFunctionality` and is not a backlog of work
to be eventually completed. Common reasons a feature is unsupported include: features intended for
silicon bring-up or factory test rather than production use; features with known hardware bugs
whose simulator replication would constrain future silicon revisions; features without a published
ISA specification; and features where the simulator offers superior alternatives (e.g. arbitrary
debug visibility via source-level modification is more useful than reproducing a silicon debug-bus
mechanism).

In most cases, software encountering an `UnsupportedFunctionality` error can achieve the desired
outcome by using a documented alternative path, restructuring the algorithm, or accepting the
feature's absence - this is the intended use of an architecture in which not every feature is
supported by every tool. Software with a genuine need for a feature currently marked
`UnsupportedFunctionality` may request its promotion to the planned-work backlog with sufficient
justification (specific real workload, concrete quantified benefit, alternatives evaluated,
acknowledgment of implementation cost, and confirmation that an ISA specification exists). The bar
for promotion tightens over time as `ttsim`'s workload coverage grows: features not exercised by
any real workload to date are increasingly unlikely to provide enough value to justify
implementation cost, and the prior on "genuinely needed but not yet hit" continues to shift
downward as more workloads run.

### Simulator-Specific Software Conditionals
In all of the above, it has been assumed that all host and device code follows the same logic paths
that would execute on real silicon.

Simulator-specific conditionals, detection flags, or alternate code paths break this equivalence.
Such behavior may cause the simulator to produce incorrect functional or performance results and
is therefore **unsupported**.

To ensure results are meaningful:
- Build and run identical host and device binaries for both simulator and silicon.
- Do not add compile-time or runtime checks that detect the simulator environment.
- Report issues only when they reproduce under the same code path that runs on hardware.

The simulator's value depends on its fidelity to hardware behavior. Divergent software paths
undermine that fidelity and are not part of the supported model.

## Contributing
We welcome bug reports and feature requests! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

**Note:** We do not accept pull requests. All development happens in an internal repository, and this
public repository contains filtered source and binary releases. Please file issues for bugs or suggestions.

## Support and Issues
If you encounter problems:
1. Check the [Known Issues](#known-issues) section above
2. Search [existing issues](https://github.com/tenstorrent/ttsim/issues) to see if it's already reported
3. [Open a new issue](https://github.com/tenstorrent/ttsim/issues/new/choose) with details about your problem

For security vulnerabilities, please follow our [Security Policy](SECURITY.md).

## License
This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.
Additional information is available in [LICENSE_understanding.txt](LICENSE_understanding.txt).

## Coda: Philosophical Inspirations

The principles `ttsim` operates under are not novel to this project. They draw on ideas
articulated and lived in several other communities, each in a different domain:

- **The Bell Labs Unix lineage** (V6/V7 Unix, Plan 9, Inferno) for orthogonality, simplicity, the
  principle that a system should be small enough for one engineer to read in a sitting, and the
  broader systems-research tradition of refusing complexity that isn't load-bearing. The 1970s V6
  Unix instance produced *Lions' Commentary on UNIX 6th Edition* - the first widely-circulated
  annotated source listing of a real operating system - and the recurring "wait, this is the
  kernel? the whole thing?" reaction from readers accustomed to industrial-scale opacity.
- **The reproducible-builds movement** (Debian, NixOS, Bazel, in-toto, SLSA) for the
  determinism-as-a-value posture applied to build pipelines and supply-chain integrity.
- **OpenBSD** for the "secure by default"/"strict by default" posture, the willingness to refuse
  features that compromise discipline, and the treatment of documentation as engineering work.
- **The Daniel J. Bernstein lineage of tools** (qmail, daemontools, djbdns, NaCl/libsodium
  ancestry) for the strict-input/small-codebase/hard-fail discipline, and for early
  articulation of timing-attack paranoia and constant-time implementation.
- **TigerBeetle's "TigerStyle"** as an unusually clear contemporary articulation of disciplined
  engineering principles, inherited in part from NASA's Power-of-Ten rules and applied to a
  modern database.

The novelty here is not the principles themselves but their application to chip simulator
infrastructure - a domain that has not historically required this discipline at the same level.
We think it should.
