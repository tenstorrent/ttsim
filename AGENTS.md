# AGENTS.md

This is a Tenstorrent hardware simulator and official ISA golden reference model, not idiomatic modern C++, and use cases include safety-critical markets. Before writing or editing code, read **`CODING_CONVENTIONS.md`** (language standards, prohibited features, error handling) and the **Simulator Behavior Contract in `README.md`**. The rules there are tightly enforced.

**Build:** `./make.py :build` from the repo root. Simulator libs are `src/_out/<release|debug>_<config>/libttsim.so`, one config per chip or system (`CONFIGS` in `src/rules.py`). Use `./make.py <path> [<path> ...]` to rebuild a specific list of targets, e.g., `./make.py src/_out/release_wh/libttsim.so`. Wormhole/Blackhole = `TT_ARCH_VERSION` 0/1. Anything under `_out/` is generated; never edit.

**Layout:** `src/` simulator (C++), `fw/` firmware, `data/{wh,bh}/` ISA/register tables and firmware images (generated; do not edit manually), `scripts/` tooling, `docs/` design docs.

**Style:** `./scripts/check_style.py` (CI-enforced: ASCII-only, no trailing whitespace, no extra blank lines, no tabs). Indent 4 spaces. Match surrounding code for everything else.

**Error handling:** Do not remove `UndefinedBehavior`/`NonContractualBehavior`/`UnpredictableValue` checks without a corresponding `tt-isa-documentation` spec change meeting the evidentiary conditions in the tt-isa-doc `Glossary.md`'s "UndefinedBehavior: Note on redefinition". Do not disable error checks or implement `UnsupportedFunctionality` (intentionally subsetted features) to chase "pass rate." A simulator that silently accepts UB/NCB/UV inputs is incorrect by design.

**Common pitfalls:** Runtime allocation, exceptions, `virtual`, RTTI, STL containers (`vector`/`string`/`map`), iostreams, threading, and environment variables are banned in simulator and on-device code. Errors use `TTSIM_ERROR`/`TTSIM_VERIFY` with a category, never `printf` or `assert`. Keep changes small and focused (1-50 lines is the sweet spot), one logical change per commit; larger refactors need a design proposal first. The simulator reflects silicon; don't add capabilities that don't exist in hardware.
