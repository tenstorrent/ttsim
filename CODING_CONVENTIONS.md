# Coding Conventions

This document defines coding style and technical conventions specific to this codebase.

## General Principles

- **Consistency over preference.** Follow the style of the surrounding code.
- **Copy existing patterns, not code.** If unsure, imitate the nearest example; extend an existing model rather than
  adding a parallel copy that needs every fix twice.
- **No dead code.** Nothing lands that nothing calls or tests; code arrives with its caller and its test.
- **Names mean their obvious reading.** A field, flag or register does what its name says; don't repurpose one.
- **Write for outside readers.** This codebase is read by safety auditors and by academic users
  adopting `ttsim` as a reference implementation. Code only its author can follow is a liability.

## Language & Standards

- Primary language: a constrained subset of **C++20** (with limited C/assembly where appropriate).
- Scripts/tools: **Python 3.x**, plus **JSON** for data interchange/serialization.
- Bash may be used for trivial scripts (a few lines). For anything with control flow or >10 lines, use Python.
- Code must build and run on: Linux/x86_64, Linux/aarch64, Linux/riscv64, and macOS/aarch64.
- Must build cleanly with `-Wall -Werror` on every supported compiler, the oldest being GCC 11 (Ubuntu 22.04).

## Style Rules

- Indentation: 4 spaces, no tabs.
- Line length: 120 characters max, no trailing whitespace.
  - Scripts enforce 170 character hard limit, 120 is soft maximum.
- ASCII text only, no Unicode or binary files.
- C/C++:
  - Brace style: match surrounding code (default: K&R + mandatory braces for all blocks).
  - One declaration/one statement per line.
  - Use `#pragma once` instead of include guards.
  - Use `nullptr` instead of `NULL`.
  - Prefer function-style casts `uint64_t(x)`, then C-style `(uint8_t *)x` if function-style not possible.
  - Unused variables: `[[maybe_unused]]`, not `(void)x`, which is only for discarding a return value.
- Python:
  - Prefer single quoted strings over double quoted strings.
  - One `import` per line.

## Prohibited Features

The following are **not allowed** unless a maintainer explicitly approves the need:

- **Dynamic memory allocation at runtime** (`new`, `delete`, `malloc`, `free`).
- **C++ exceptions** (`try`, `catch`, `throw`).
- **`virtual`** methods and inheritance.
- **Run-time type information (RTTI)** (`dynamic_cast`, `typeid`).
- **Standard library containers or types that allocate at runtime**, e.g. `std::string`, `std::vector`, `std::map`. Use fixed-size or stack-based alternatives instead.
- **C++ iostreams (e.g. `std::cout`, `std::fstream`)**: banned due to hidden allocations, locale dependence, and unpredictable formatting.
- **Console, file, and network I/O**: disallowed except for explicitly permitted subsystems (e.g. checkpointing).
- **Environment variables (e.g. `getenv`)**: disallowed except for extremely limited cases.
- **Threading/concurrency**: not yet supported (but the simulator will be multithreaded in the future for performance).
- **`goto`, `setjmp`/`longjmp`**: prohibited as they break structured control flow.

**Rationale:** these features break determinism, complicate verification, introduce hidden runtime costs, or
are explicitly forbidden by safety-cert customer requirements.

## Error Handling

- **Fail hard and early.** Print an error and terminate (`_Exit(1)`) rather than printing a warning and continuing or silently misbehaving.
- **Use `TTSIM_ERROR` and `TTSIM_VERIFY`:** These require every error to be tagged with one of a standardized set of
  error categories.
- **Gating unproven code:** Code paths lacking adequate tests must remain disabled as `UntestedFunctionality` until proven reliable.
- Always error on unsupported features, unexpected states, or conditions that are not expected in production;
  refuse rather than guess.
- **Pick the category that says who must act:** software broke the spec (`UndefinedBehavior` only where the spec gives
  no guarantees at all, e.g. hangs or nonlocal corruption; a deterministic case is `NonContractualBehavior` with an
  exact rule), a feature left out on purpose (`UnsupportedFunctionality`) or not done yet
  (`UnimplementedFunctionality`), or a simulator bug (`AssertionFailure`).
- A check depends on opcode, configuration and address, never on the data computed on or on cycle timing.
- When in doubt, make error checks too strict rather than too loose.

## Undefined Behavior

UB in deployed code is explicitly forbidden by safety-critical customer requirements. Be aware of
the difference between "undefined behavior" and "implementation-defined behavior":
- **Implementation-defined behavior:** The compiler chooses a documented result.
- **Undefined behavior:** No rules at all. The compiler can crash your program, corrupt data,
  leak secrets, or rewrite logic. It can even assume UB code paths are impossible and
  optimize away or alter upstream logic accordingly.

Avoid common C/C++ pitfalls that lead to undefined behavior, including but not limited to:

- Signed integer overflow (e.g., `int32_t a = x + y;` where overflow possible).
- Shifting by a count >= width (`x << 32` on a 32-bit type).
- Accessing out-of-bounds array elements.
- Use of uninitialized variables.
- Violating strict aliasing rules (type-punning via pointers). Use `std::bit_cast` if you need bitwise reinterprets and `mem_rd`/`mem_wr` for raw buffer access.
- Double-free or use-after-free.
- Modifying string literals.
- Relying on unspecified evaluation order to produce side-effects.
- Calling functions through mismatched function pointers or incompatible prototypes.
- Unsequenced modifications: e.g., `i = i++;` or mixing pre/post increment in the same expression.
- Returning references/pointers to stack variables.
- Null pointer dereference.
- Dangling references/iterators.

Tooling: compiler warnings at high levels and `-fsanitize=undefined` in testing where appropriate;
static analysis (clang-tidy, etc.) is FV-adoption substrate that safety-cert customers increasingly require.

Arithmetic right-shift of signed integers is permitted; all supported platforms implement it consistently.
However, avoid narrowing conversions of signed integers that are not value-preserving (implementation-defined
and confusing at best).

## Floating Point

Bit-identical results across x86, AArch64, and RISC-V hosts are required (also a safety-cert customer
expectation; numerical reproducibility is part of their safety case). Host IEEE `float` and `double` are allowed
for modeling the hardware's float ops, under these rules:

- Use only operations IEEE 754 defines exactly: `+`, `-`, `*`, compares, and conversions between `float`,
  `double` and integers. Move bits in and out with `std::bit_cast`.
- Wherever the hardware isn't plain IEEE, fix it up explicitly in integer code: denormal inputs and outputs
  flushed, NaNs made canonical, narrowing to the device's own formats (e.g. bf16) with its rounding, and anything
  else the hardware does differently. Never rely on what the host does with a NaN's payload or sign.
- If a result is rounded twice (e.g. computed in `double`, then narrowed to `float`), say in a comment why that
  equals one rounding (the wider format has at least 2p+2 bits of precision for p-bit operands).
- No implicit fused multiply-add: the build passes `-ffp-contract=off`, so the compiler never fuses a multiply and
  an add on its own. An explicit `__builtin_fma`/`__builtin_fmaf` is fine where the modeled operation really is a
  single-rounding fused multiply-add and you know exactly why (e.g. RISC-V FMADD in `rv64_fpu.cpp`). ttsim doesn't
  support hosts without hardware FMA (e.g. Ivy Bridge). No `-ffast-math` or anything like it.
- Do not use `<math.h>` or `<cmath>` functions: their results often differ by platform.
- Code runs under the default FP environment (round to nearest even, no FTZ/DAZ, exceptions masked). libttsim.so
  sets it at every exported entry point and restores the host's on return and around host callbacks; nothing else
  changes it.
- Intrinsics that perform floating-point arithmetic follow the same rules and are prohibited unless explicitly
  reviewed for determinism and NaN handling.
- Check host-float code against the RTL exhaustively where the input space allows.

## Serialization/Checkpointing

The program must be able to save its entire state to disk and restore it later.
Design data structures with serialization in mind:

- Prefer plain old data (POD) layouts.
- Minimize raw pointers; prefer indices or offsets into fixed structures.
- Assuming little endian is acceptable, but otherwise, keep serialization format deterministic across platforms.

## Performance-Critical Code

- Performance-sensitive paths may be reviewed at the assembly level.
- Be especially careful with changes to the RISC-V simulator, as even tiny changes can cause large performance regressions.
- Prefer intrinsics over inline assembly.
- Validate all performance assumptions with benchmarks.

## File Organization

- `.github/` - CI workflows
- `data/` - hardware specifications (do not edit manually)
- `docs/` - design docs
- `fw/` - firmware sources (built images are checked in under `data/`)
- `scripts/` - build/utility scripts
- `src/` - source code

## Documentation & Comments

- Comment *why*, not what.
- Document invariants, preconditions, and edge cases.
- Fix bad code rather than explaining it.
- The best code should be clear enough to need minimal comments.
