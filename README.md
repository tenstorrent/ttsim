# ttsim, a fast full-system simulator of Tenstorrent hardware

`ttsim` provides a virtual Wormhole or Blackhole device that can run on any Linux/x86_64
system, even without Tenstorrent silicon present. It is slower than silicon but still fast
enough that you can run interesting workloads with good productivity, allowing you to explore
and experiment with Tenstorrent's hardware and programming model before purchasing silicon.

Each simulator consists of a single `libttsim.so` file compiled for a specific chip architecture
(Wormhole or Blackhole). This library exports a simple API that [TT-Metalium](https://github.com/tenstorrent/tt-metal)
knows how to communicate with.

## Distribution
We currently provide binary releases for Linux/x86_64 only, with plans to release source
code in the future under the Apache License. Visit the [latest release page](https://github.com/tenstorrent/ttsim/releases/latest)
to download the latest version.

## Chip Status
- **Wormhole/Blackhole**: Can run many tt-metal and ttnn examples/tests in slow dispatch mode. 
  Can also run numerous LLK tests. Overall, Wormhole is in more mature shape than Blackhole.

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
```

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

## Known Issues
**Fast dispatch is not working.** You must set `TT_METAL_SLOW_DISPATCH_MODE=1`.

There are a variety of unimplemented features in the simulator at present. We are working to fill in the
gaps, but this will take time. Error messages will include one of the following categories:
- **UndefinedBehavior, UnpredictableValueUsed, NonContractualBehavior**: See
  [tt-isa-documentation glossary](https://github.com/tenstorrent/tt-isa-documentation/blob/main/Glossary.md)
- **UntestedFunctionality**: Feature is implemented but lacks sufficient test coverage to be enabled
- **UnimplementedFunctionality**: Feature not yet implemented but planned for future support
- **UnsupportedFunctionality**: Feature unlikely to be implemented without strong justification
- **MissingSpecification**: Feature requires additional internal specification work before implementation can proceed
- **SystemError/ConfigurationError**: OS errors or issues with command line options, environment variables,
  or configuration files
- **AssertionFailure**: Internal simulator bug

### Numerical Accuracy
`ttsim` is designed to provide **bit-exact** numerical results relative to silicon for all
computations, floating point and otherwise. The goal is to match all hardware computations
bit-for-bit across all instructions, opcodes, functional units, and special cases, including
the precise bit representation of NaNs produced by operations. While bugs are inevitable and
some code paths are not yet bit-exact, this fidelity is the intended target.

While most code will achieve bit-exact results, cases that can produce divergent results include:
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

Current implementation status within the simulated Tensix:
- SFPU: believed to be fully bit-accurate.
- Unpacker, FPU, and packer: not yet bit-accurate, but planned to be fixed to the extent possible.

## Contributing
We welcome bug reports and feature requests! Please see [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

**Note:** We do not accept pull requests. All development happens in an internal repository, and this 
public repository contains filtered binary releases. Please file issues for bugs or suggestions.

## Support and Issues
If you encounter problems:
1. Check the [Known Issues](#known-issues) section above
2. Search [existing issues](https://github.com/tenstorrent/ttsim/issues) to see if it's already reported
3. [Open a new issue](https://github.com/tenstorrent/ttsim/issues/new/choose) with details about your problem

For security vulnerabilities, please follow our [Security Policy](SECURITY.md).

## License
This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.
Additional information is available in [LICENSE_understanding.txt](LICENSE_understanding.txt).
