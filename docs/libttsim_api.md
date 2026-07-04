# libttsim API and ABI

`libttsim.so` is the simulator packaged as a shared library. It exposes a small, flat C interface
that lets a host program present `ttsim` as a virtual Tenstorrent PCIe device: the host forwards
config-space and BAR accesses into the library, drives time forward, and supplies callbacks the
library uses to read and write host (guest-physical) memory for DMA.

This document specifies both the **API** (the entry points and their semantics) and the **ABI**
(the binary contract: symbols, dependencies, process behavior, and what an embedder may rely on),
along with the compatibility policy that governs how these evolve.

The API and ABI described here are identical between the public and private builds of `ttsim`; the
public/private binary-equivalence commitment in the README applies to this interface as well.

## Embedding model

The library is a process-wide singleton. Its state lives in file-scope globals, so there is exactly
one library instance per process and the entry points are not associated with a handle or context
object. A single library instance can present more than one chip: most builds simulate a single chip,
but a multi-chip build exposes several chips as distinct host-visible PCIe devices through the same
library (see [Config space](#config-space)). A typical embedding:

1. Load `libttsim.so` and resolve the entry points below (e.g. via `dlopen`/`dlsym`).
2. Optionally install DMA callbacks with `libttsim_set_pci_dma_mem_callbacks` (required before
   `libttsim_init` if the workload performs DMA).
3. Call `libttsim_init` exactly once.
4. Forward guest accesses with the `libttsim_pci_*` functions and advance time with `libttsim_clock`.
5. Call `libttsim_exit` at shutdown.

All entry points use the platform C calling convention and `extern "C"` linkage. The library is
**single-threaded and not reentrant**: the caller must serialize all calls, and the library assumes
a little-endian host.

## API reference

Unless noted otherwise, every function requires the library to be initialized
(`libttsim_init` has been called and `libttsim_exit` has not), and any precondition violation is a
fatal error (see "Error and exit behavior").

### Lifecycle

```c
void libttsim_init(void);
void libttsim_exit(void);
```

`libttsim_init` initializes the simulated device. It must be called exactly once per process;
calling it while the simulator is already running is a fatal error. `libttsim_exit` tears the
simulator down; it requires that the simulator is running.

### DMA callbacks

```c
void libttsim_set_pci_dma_mem_callbacks(
    void (*pci_dma_mem_rd)(uint64_t paddr, void       *dst, uint32_t size),
    void (*pci_dma_mem_wr)(uint64_t paddr, const void *src, uint32_t size));
```

Installs the callbacks the library invokes when the simulated device initiates DMA, i.e. when it
needs to read or write host (guest-physical) memory. `paddr` is the address in the host's memory
space; the callbacks must transfer `size` bytes. Installing them is optional, but if installed it
must be done before `libttsim_init`. If the workload performs DMA and no callbacks were installed,
the resulting DMA is a fatal error.

### Config space

```c
uint32_t libttsim_pci_config_rd32(uint32_t bus_device_function, uint32_t offset);
void     libttsim_pci_config_wr32(uint32_t bus_device_function, uint32_t offset, uint32_t data);
```

32-bit PCI configuration-space access. `bus_device_function` is the conventional PCI bus/device/function
tuple; the bus and function fields must be 0, and the device field selects which host-visible chip is
addressed. A single-chip build exposes only device 0. A multi-chip build exposes each host-visible chip
as a consecutive device number, and reading the config space of a device number beyond the last present
chip returns all-ones (`0xFFFFFFFF`) - the conventional "no device" response - so a host can enumerate
the chips by walking device numbers until it reads all-ones.

`offset` must be 4-byte aligned; other values are fatal errors. The config read at offset 0 returns
the vendor and device ID identifying the simulated chip; the BAR base-address registers are also
readable, and each device's BARs occupy a distinct physical-address window (so the `paddr` passed
to the memory-access functions identifies the target chip). `libttsim_pci_config_wr32` is reserved:
it is part of the ABI but config-space writes are not currently implemented and calling it is a
fatal error.

### Memory (BAR) access

```c
void libttsim_pci_mem_rd_bytes(uint64_t paddr, void       *dst, uint32_t size);
void libttsim_pci_mem_wr_bytes(uint64_t paddr, const void *src, uint32_t size);
```

Read and write the device's memory-mapped BAR windows. `paddr` is the simulator-internal physical
address of the access (the BAR base plus offset, as advertised through config space), not the
address at which the host happens to map the BAR. These are the primary data path for driving the
device.

### Time

```c
void libttsim_clock(uint32_t n_clocks);
```

Advances the simulation by `n_clocks` clock steps, running the RISC-V cores and Tensix units. The
host calls this to let the device make forward progress between or during register accesses. The
simulator does not advance time on its own; it only runs inside `libttsim_clock`.

### Tile-relative access (transitional)

```c
void libttsim_tile_rd_bytes(uint32_t x, uint32_t y, uint64_t addr, void       *dst, uint32_t size);
void libttsim_tile_wr_bytes(uint32_t x, uint32_t y, uint64_t addr, const void *src, uint32_t size);
```

Tile-relative memory access by `(x, y)` grid coordinate. These are **transitional** and exist only
because Quasar's PCIe/BAR access path is not yet in place: on Quasar they are currently the required
way to reach tile memory, while on Wormhole and Blackhole they are illegal and calling them fails as
`UnsupportedFunctionality` (use the BAR access functions there instead). They will be removed once
Quasar's PCIe path lands, at which point all chips use the BAR functions uniformly. Removal will
follow the staged policy below.

## Hidden API: host-environment surface

Beyond the exported functions, anything observable about how the library interacts with its host
process is part of the contract. The library is designed to be a well-behaved guest inside a host
process, with the explicit exception of semihosting (below).

- **Environment variables.** The library reads exactly one variable: `TTSIM_SEMIHOSTING`. If set,
  its value must be `1`, which enables semihosting (see below). No other environment variable
  affects `libttsim.so`. (Variables such as `TT_METAL_SIMULATOR` are read by tt-metal, not by this
  library.) Any environment variable the library consults is part of this contract.
- **Threads.** The library is single-threaded and creates no threads of its own. All work happens
  on the calling thread inside the call. Introducing a background thread would be a breaking change.
- **Memory.** The library performs no dynamic memory allocation after `libttsim_init` returns. All
  memory is either statically sized or allocated once during initialization; steady-state operation
  (`libttsim_clock`, the access functions) allocates nothing and frees nothing.
- **Signals.** The library installs no signal handlers and does not alter the host's signal
  disposition.
- **I/O.** In normal operation the library performs no file or network I/O. It writes diagnostic
  text to stdout/stderr; it does not open files, sockets, or devices.
- **Exceptions.** The library is compiled without C++ exception support and never throws. No
  exception will ever propagate out of an entry point; the only failure mode is the fatal-error
  path described below.
- **Dynamic loading.** The library never calls `dlopen` or otherwise loads code at runtime. It pulls
  in no plugins, backends, or auxiliary libraries behind the host's back, so its dependency set and
  audit surface are fixed and fully known at link time.
- **Code generation.** The library does not JIT-compile, generate, or self-modify executable code,
  and maps no executable memory at runtime. It is W^X-compliant and runs in environments that
  disallow runtime code generation at the kernel or runtime level (e.g. hardened or locked-down
  hosts).

### Semihosting (deliberate exception)

When `TTSIM_SEMIHOSTING=1`, a program running on the simulated RISC-V cores may make semihosting
calls that the library services on the host's behalf while inside `libttsim_clock`. This is an
intentional escape hatch for running bare-metal test programs, and the "well-behaved guest"
properties above are **relaxed by design** in this mode:

- A semihosting program-exit call terminates the host process (see below).
- Semihosting is expected to grow over time and may, in the future, perform real file I/O or other
  host operations on behalf of the simulated program.

Hosts that require the library to never affect their process or environment should not enable
semihosting.

### Error and exit behavior

The library treats contract violations and unsupported conditions as **fatal**: it prints a
structured error message and terminates the entire host process via `_Exit` (see
[Simulator Error Handling](sim_error_handling.md)). An embedder cannot catch or recover from these;
the simulator does not return error codes for them and does not throw. This is intentional and is
part of the contract: a host embedding `libttsim.so` must be prepared for any call to terminate the
process on a detected violation. Separately, when semihosting is enabled, a program-exit semihosting
call also exits the host process, with the program's exit status.

### Forking

A process that has loaded and initialized the library may `fork()`. This is a supported usage model,
and in fact a useful one. Because the simulator's memory is either statically sized or mapped
`MAP_PRIVATE`, a forked child inherits a faithful copy-on-write snapshot of the simulator state as of
the fork, isolated from the parent and from sibling children without re-initializing or re-allocating.
A natural pattern is to call `libttsim_init` once, run common initialization code (e.g. loading and
booting firmware), and then fork per test case, running many independent sub-simulations in parallel:
the initialization cost is paid once and copy-on-write shares all unmodified pages, so each child only
pays for the state it actually touches. Further, in this mode of operation, test cases that `_Exit`
do not affect other test cases or prevent them from running.

Fork at a quiescent point, i.e. when no `libttsim` call is in progress (which, for the single-threaded
library, simply means not from inside a DMA callback). Parent and child are then fully independent
simulators.

This should be avoided when semihosting is enabled. Copy-on-write isolates the simulator's memory, but
it does not isolate the host-side side effects semihosting performs on the program's behalf (process
exit today, potentially file I/O and other host operations in the future); parallel forked children
running semihosting would have those effects collide or duplicate. Use a non-fork strategy if you need
semihosting across parallel simulations.

## ABI

- **Linkage.** A flat C ABI (`extern "C"`). The library exports no C++ symbols, exposes no class or
  struct layouts, and relies on no name mangling, so the interface is stable across compilers and
  toolchains by construction.
- **Exported symbols.** Only the `libttsim_*` entry points documented above are exported; all other
  symbols are hidden by a linker version script, and the released library is stripped of local
  symbols. The exported set is the entire ABI surface.
- **Calling convention / endianness.** The platform's standard C calling convention; little-endian
  hosts only.
- **Dependencies.** The library links only against the system C library and the dynamic loader. It
  does not depend on the C++ runtime (`libstdc++`/`libgcc_s`). This minimal dependency surface is a
  deliberate property that supports deployment in constrained environments (hypervisors, formal-
  verification tooling, embedded contexts). The specific C-library functions the library calls are
  an implementation detail and not themselves part of the contract.

### Platform and binary compatibility

The binaries are built and tested in CI on GitHub-hosted `ubuntu-22.04` and `ubuntu-24.04`, on both
x86_64 and aarch64. Those are the platforms we stand behind. Release binaries are currently built on
`ubuntu-22.04`; we expect to move release builds to `ubuntu-24.04` soon (a newer compiler toolchain),
which is not expected to change which systems the binaries run on.

Linux/riscv64 and macOS/aarch64 are also supported build targets, and in general the library is
written to be portable and nearly any POSIX platform should work. However, only the CI platforms
above have continuous coverage; on everything else, including the other supported build targets,
occasional breakage is likely simply because nothing is exercising those configurations on every
change.

Running the binary on distributions or releases other than the CI platform is best-effort at this
stage: we make no guarantee of support for end-of-life distributions, and cross-distribution binary
compatibility is not something we currently verify. We do not promise a specific minimum C-library
version, because we do not check one in our own build and release scripts; tying the guarantee to
the CI platforms is what we can actually stand behind today. This may be tightened into a firmer
guarantee in the future.

## Compatibility policy

The interface above is intended to be stable, and we recognize that hosts embed it for the long
term. We do not promise that it will never change, but we commit to giving appropriate lead time
before any breaking change, and to staging removals so that consumers have a transition path rather
than a sudden break.

When an entry point is to be removed, it goes through the following stages, in order:

1. **Deprecation notice.** The function is documented as deprecated or transitional (as
   `libttsim_tile_*` are today), but continues to work where it is currently valid.
2. **Transition period.** A period during which both the deprecated function and its replacement
   are available, so consumers can migrate.
3. **`UnsupportedFunctionality`.** The entry point is changed to fail as `UnsupportedFunctionality`
   when called, making removal observable at runtime before the symbol disappears.
4. **Removal.** The symbol is removed from the library.

### What counts as a breaking change

The following require the staged lead-time treatment above:

- Removing or renaming an exported symbol, or changing the signature or semantics of one.
- Changing the calling, threading, or reentrancy model - including spawning a background thread,
  or making the library something other than a single-threaded, caller-serialized singleton.
- Changing the process model in a way a host must account for (e.g. the singleton or once-only
  `libttsim_init` semantics).
- Changing the host-process guarantees in normal (non-semihosting) operation - e.g. beginning to
  perform file or network I/O, installing signal handlers, allocating memory after `libttsim_init`
  returns, loading code via `dlopen`, or generating or self-modifying executable code (JIT).
- Changing the documented meaning of an environment variable the library reads.
- Adding a new mandatory runtime dependency, or dropping support for a currently supported CI
  platform.

### What is not a breaking change

These may happen at any time without notice:

- Accepting an input (e.g. a config-space offset or BAR address) that previously failed.
- Internal behavior, performance, or implementation changes that preserve the documented semantics
  and bit-exact results.
- Expansion of semihosting behavior (which is opt-in via `TTSIM_SEMIHOSTING`).

Adding a new exported symbol or a new optional environment variable is not a breaking change, but
as any new API is a long-term support commitment, the bar for adding one is high.
