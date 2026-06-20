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
- **Wormhole/Blackhole**: Nearing feature complete, with a small number of remaining features and
  bugs under active debug. Can run many tt-metal, ttnn, tt-forge, and LLK examples/tests.

- **Wormhole/Blackhole multichip**: *Experimental* `wh_x2` (N300), `wh_x8` (T3000/LoudBox), `wh_x32`
  (WH Galaxy), `bh_x2` (P300), and `bh_x32` (BH Galaxy) configs are available for preliminary
  testing, with significant numbers of Ethernet, multidevice, and fabric tests passing.

- **Quasar**: DM cores and TRISCs can be taken out of reset. RV32/64 code and simple NOC transfers work.
  More tests and features are under active debug and bringup.

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

Linux/aarch64 binary releases are suffixed with `_aarch64`, e.g., `libttsim_wh_aarch64.so`.

### Building from Source (Wormhole and Blackhole)
The included source can be built locally:

```bash
./make.py :build
```

Requires g++ with C++20 support and Python 3.8+. Builds release `.so` files at
`src/_out/release_wh/libttsim.so` and `src/_out/release_bh/libttsim.so`, functionally
equivalent to the corresponding GitHub Releases binaries (and byte-identical when built
on the same toolchain as our release CI: `ubuntu-22.04` with its default g++).

For the `libttsim.so` programmatic interface - the exported C entry points, the ABI, and the
compatibility policy that governs them - see [libttsim API and ABI](docs/libttsim_api.md).

### Running with TT-Metalium
Metal has simulator support out of the box, enabled by setting the `TT_METAL_SIMULATOR`
environment variable to point to the simulator .so file. The `libttsim.so` must live in
a directory also containing an SOC descriptor YAML file.

#### Wormhole
```bash
export TT_METAL_SIMULATOR=~/sim/libttsim_wh.so
cp $TT_METAL_HOME/tt_metal/soc_descriptors/wormhole_b0_80_arch.yaml ~/sim/soc_descriptor.yaml

cd $TT_METAL_HOME
TT_METAL_SLOW_DISPATCH_MODE=1 ./build/programming_examples/metal_example_add_2_integers_in_riscv
```

#### Blackhole
```bash
export TT_METAL_SIMULATOR=~/sim/libttsim_bh.so
cp $TT_METAL_HOME/tt_metal/soc_descriptors/blackhole_140_arch.yaml ~/sim/soc_descriptor.yaml

cd $TT_METAL_HOME
TT_METAL_SLOW_DISPATCH_MODE=1 ./build/programming_examples/metal_example_add_2_integers_in_riscv
```

#### Quasar
```bash
export TT_METAL_SIMULATOR=~/sim/libttsim_qsr.so
cp $TT_METAL_HOME/tt_metal/soc_descriptors/quasar_32_arch.yaml ~/sim/soc_descriptor.yaml

cd $TT_METAL_HOME
TT_METAL_SLOW_DISPATCH_MODE=1 ./build/test/tt_metal/unit_tests_legacy --gtest_filter=QuasarMeshDeviceSingleCardFixture.SingleDmL1Write
```

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

Run, with the appropriate `bar4-size` for the chip (Wormhole: `32M`, Blackhole: `32G`):
```bash
qemu-system-x86_64 ... \
    -device ttsim,lib=/path/to/libttsim.so,bar4-size=32M
```

Inside the guest, build and load `tt-kmd` to surface `/dev/tenstorrent/0`:
```bash
sudo apt install -y build-essential linux-headers-generic git
git clone https://github.com/tenstorrent/tt-kmd && cd tt-kmd && make
sudo insmod tenstorrent.ko
```

`qemu-system-aarch64` is also supported and will be faster on ARM-based host platforms.

Quasar's host MMIO path is not implemented in libttsim and will not work end-to-end.

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

Boot Linux (OpenSBI `fw_jump` firmware + a kernel `Image` + initramfs + a flattened device tree), e.g.
an 8-hart SMP boot with a virtio-blk disk and an interactive console:
```bash
src/_out/release_riscv64/ttsim --harts 8 -i \
    --entry 0x80000000 --set-reg a0 0 --set-reg a1 0x88800000 \
    --load-bin fw_jump.bin 0x80000000 --load-bin Image      0x80200000 \
    --load-bin initrd.img  0x84000000 --load-bin board.dtb  0x88800000 \
    --disk disk.img
```

`-i` attaches the simulated UART console to your terminal. Ctrl-C is hooked and passed into the
simulator, so use Ctrl-\ if you need to forcibly exit.

Note that your .dtb must declare more harts in order for Linux to use them.

### Bringing up a Tenstorrent chip inside the guest
ttsim-riscv64 can host a Tenstorrent AI accelerator: pass `--tt-device <libttsim.so>` and it `dlopen`s
the chip model and presents it to the guest as a PCIe endpoint - an ECAM config region plus the chip's
BAR windows - just as [ttsim-qemu](#running-ttsim-as-a-qemu-pci-device) does for QEMU. The guest device
tree must declare a `pci-host-ecam-generic` bus at the ECAM base at `0x30000000`. Then
[tt-kmd](https://github.com/tenstorrent/tt-kmd) binds to it and exposes `/dev/tenstorrent/0`, providing
you with a entire simulated RISC-V computer with an AI hardware accelerator, running Linux - all in just
a few hundred KB of total simulator code, and with no external dependencies beyond libc.

```bash
# Build the Wormhole chip model (libttsim.so) and the ttsim-riscv64 host system:
./make.py src/_out/release_wh/libttsim.so src/_out/release_riscv64/ttsim

# Boot Linux with the Wormhole chip attached over (simulated) PCIe -- the DTB declares the ECAM bus:
src/_out/release_riscv64/ttsim -i \
    --entry 0x80000000 --set-reg a0 0 --set-reg a1 0x88800000 \
    --load-bin fw_jump.bin 0x80000000 --load-bin Image      0x80200000 \
    --load-bin initrd.img  0x84000000 --load-bin board.dtb  0x88800000 \
    --tt-device src/_out/release_wh/libttsim.so --disk disk.img
```

Make sure your disk.img is already pre-populated with `build-essential linux-headers-generic` and
the tt-kmd source. Inside the guest, build and load `tt-kmd` exactly as for the QEMU path above:
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

Multichip support is in early stages and testing is incomplete. Use the `mcraighead/mc-p300` branch of
`tt-metal` for the (not yet merged) software changes required to interface with the `wh_x2`, `wh_x8`,
`wh_x32`, `bh_x2`, and `bh_x32` simulator builds. Set the `TT_METAL_MOCK_CLUSTER_DESC_PATH` environment
variable to point to a valid cluster .yaml (e.g.
`tt_metal/third_party/umd/tests/cluster_descriptor_examples/wormhole_N300.yaml` or
`tt_metal/third_party/umd/tests/cluster_descriptor_examples/blackhole_P300_both_mmio.yaml`)
when running tt-metal tests. An example of a test that has been validated to run on N300, P300, T3000,
WH Galaxy, and BH Galaxy and not only pass but also dispatch work to all chips in both SD and FD mode is
`tests/ttnn/unit_tests/base_functionality/test_multi_device.py::test_multi_device_single_op_binary`.
The `bh_x32` binary is x86_64 only at present due to aarch64 linker limitations with the extremely large
BSS segment this requires.

Quasar support is in early stages and testing is incomplete. The unit_tests_legacy test listed above was
validated against tt-metal commit `ad73db77a3e135d225f9efe333e45ef9810387b0`, with `#define NOC_API_V2`
commented out in `tt_metal/hw/inc/internal/tt-2xx/quasar/noc_nonblocking_api.h`. Most QSR tests, including
this one, currently require disabling `NOC_API_V2`.

Not all hardware features are implemented, and the simulator is intentionally more restrictive than silicon
to help uncover potential issues. Simulator error messages are grouped into the following categories
to indicate whether the simulator or the software being simulated is at fault (see
[Simulator Error Handling](docs/sim_error_handling.md) for the full design rationale):
- **UndefinedBehavior, UnpredictableValueUsed, NonContractualBehavior**: See
  [tt-isa-documentation glossary](https://github.com/tenstorrent/tt-isa-documentation/blob/main/Glossary.md)
- **UntestedFunctionality**: Feature is implemented but lacks sufficient test coverage to be enabled
- **UnimplementedFunctionality**: Feature not yet implemented but planned for future support
- **UnsupportedFunctionality**: Feature unlikely to be implemented without strong justification
- **MissingSpecification**: Feature requires additional internal specification work before implementation can proceed
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

### Numerical Accuracy
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

For timing-dependent computations, `ttsim` may evaluate operations in any order permitted by
software synchronization. This may include operation orders that are extremely unlikely on silicon.
To ensure an exact match with silicon, avoid algorithms where the runtime order of operations
affects the result. For example, floating-point reductions using addition will diverge unless each
addition is explicitly serialized in a deterministic order.

### Strictness vs. Silicon
The simulator may report `UndefinedBehavior` or `UnpredictableValueUsed` for conditions that do not
produce visibly incorrect output on silicon under specific test workloads. This is by design.
UndefinedBehavior is not equivalent to "the device halts on this input"; it is the specification's
commitment that no behavior is guaranteed. Silicon executes some behavior on every input, and the
absence of a visible failure in a particular test does not constitute a defined contract. The
simulator's role is to flag the nonconformance at the point it occurs, before it propagates into a
non-portable correctness failure under a different workload, format combination, or silicon revision.

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
