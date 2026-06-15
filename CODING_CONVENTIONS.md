# Coding Conventions

This document defines coding style and technical conventions specific to this codebase.

## General Principles

- **Consistency over preference.** Follow the style of the surrounding code.
- **No style debates.** Time is better spent on correctness and performance.
- **Copy existing patterns.** If unsure, imitate the nearest example.
- **Write for outside readers.** This codebase is read by safety auditors and by academic users
  adopting `ttsim` as a reference implementation. Code only its author can follow is a liability.

## Language & Standards

- Primary language: a constrained subset of **gnu++20** (with limited C/assembly where appropriate).
- Scripts/tools: **Python 3.x**, plus **JSON** for data interchange/serialization.
- Bash may be used for trivial scripts (a few lines). For anything with control flow or >10 lines, use Python.
- Code must build and run on: Linux/x86_64, Linux/aarch64, Linux/riscv64, and macOS/aarch64.
- Must build cleanly with `-Wall -Werror`.

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
- Always error on unsupported features, unexpected states, or conditions that are not expected in production.
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
expectation; numerical reproducibility is part of their safety case). In service of this requirement:

- Avoid `float` and `double` entirely, as they open up too much flexibility for compiler misbehavior.
- Do not use `<math.h>` or `<cmath>` functions: their results often differ by platform.
- Intrinsics that perform floating-point arithmetic are prohibited unless explicitly reviewed for determinism and NaN handling.

If any FP use is later approved, results must be bitwise identical across all supported platforms.

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

## Debugging

- No ad-hoc debug prints (`printf`, etc.).
- Use only structured debugging systems defined in the project.
- Temporary debug aids must be removed before commit.

## File Organization

- `.github/` - CI workflows
- `data/` - hardware specifications (do not edit manually)
- `docs/` - design docs
- `scripts/` - build/utility scripts
- `src/` - source code

## Documentation & Comments

- Comment *why*, not what.
- Document invariants, preconditions, and edge cases.
- Fix bad code rather than explaining it.
- The best code should be clear enough to need minimal comments.
