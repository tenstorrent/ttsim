# Simulator Error Handling

`ttsim` is strict about errors. When it detects a problem, it prints a structured error message and
terminates the process via `_Exit(1)`. This document explains why, what alternatives have been
considered and rejected, and the supported paths for downstream consumers who hit simulator errors
during development.

## Categories of errors the simulator surfaces

The simulator distinguishes several error categories. The immediate handling is uniform: any of them
terminates the process. The categories exist primarily for diagnostic clarity and so that downstream
owners can route fixes to the right place.

- `UndefinedBehavior` (UB): software has triggered behavior that the ISA specification leaves
  undefined. The simulator catches this as a SW correctness issue and refuses to continue past the
  violation.
- `NonContractualBehavior` (NCB): software has depended on behavior the spec declares software
  must not depend on. Distinguishing this from UB is sometimes useful; the handling is the same.
- `UnpredictableValueUsed` (UV): software has read a value that was not deterministically set (for
  example, reading from a register without a prior write, in a context where the spec does not
  guarantee any specific value).
- `UnsupportedFunctionality`: a feature the simulator intentionally does not plan to implement
  without a strong justification for why the cost of implementation and testing is worthwhile.
- `UnimplementedFunctionality`: a feature that should eventually be implemented in the simulator
  but is not yet. Distinct from `UnsupportedFunctionality`, which is intentional.
- `UntestedFunctionality`: the simulator has reached a code path that is implemented but where the
  specific combination of inputs, configuration, or state has not been exercised by tests or
  cross-checked against silicon or RTL. The implementation may be correct, but there is no positive
  evidence that it is; the check exists to surface these cases conservatively until directed
  validation is added.
- `AssertionFailure`: an internal invariant of the simulator has been violated. This indicates a
  simulator bug.
- `MissingSpecification`: the simulator has reached a code path where the architectural
  specification is incomplete; behavior cannot be determined without spec work.

A surface complaint of the form *"the simulator exited and I don't know why"* applies regardless of
which category fired. The reasoning below applies uniformly to all of them.

## Why the simulator terminates the process on error

When a simulator error fires, the simulator's internal state has been established as inconsistent
with the architectural contract it is modeling. Continuing execution past that point produces one of:

1. Silent corruption of internal state, with downstream simulator output that appears valid but is
   not.
2. Cascading failures, where the simulator's downstream code assumes the violated invariant holds
   and produces secondary errors that obscure the primary cause.
3. Spurious downstream errors that look unrelated to the actual cause, making triage harder than it
   needs to be.

Process termination is the conservative response to invariant violation. It preserves the property
that *if the simulator produces output, that output is meaningful*. Any other choice trades that
property for downstream complexity.

The `noreturn` property of the error path is load-bearing. Compiler optimization, static analysis,
and simulator logic that depends on "we are still alive, therefore the invariant held" all assume
that the error path does not return. Compromising that property in any way breaks those assumptions
silently.

The simulator uses `_Exit(1)` rather than `exit(1)`:

- `exit(1)` runs `atexit` handlers and global destructors. In a `.so` context, these include cleanup
  routines for the host program and for other shared libraries loaded alongside the simulator. After
  a simulator invariant violation, the simulator's view of state is known-corrupt; running
  destructors over that state produces secondary crashes that obscure the original failure.
  Empirically, attempts to use `exit(1)` produced crashes in `atexit` handlers and destructors
  outside the simulator, exactly as the analysis predicts.
- `_Exit(1)` skips all of that and just terminates. It is the right primitive for a
  noreturn-on-invariant-violation pattern: it removes itself from execution cleanly without
  entangling with cleanup paths that may not be safe to run.

## Common counterproposals and why they don't work

Several variants of "make the error path softer" come up repeatedly. Each has specific technical or
structural problems.

### Throw a C++ exception instead of `_Exit(1)`

This proposal is appealing because exceptions look like a generic error-handling mechanism. In
practice it does not work for the simulator:

- **Cross-ABI exception unwinding is fragile.** The simulator ships as a `.so`. The host program
  loading it may have been built with a different C++ runtime version, a different unwind table
  format, or a different exception personality function. Throwing across that boundary is fragile
  and platform-dependent.
- **Exceptions interact poorly with threading.** The simulator may eventually be threaded (it is not
  currently). C++ exceptions and threading do not compose cleanly; `std::exception_ptr` is fragile
  across threads; async-safe throw does not exist.
- **Rust does not speak C++ exceptions.** A future port to Rust is under consideration. Building on
  C++ exceptions now would commit to an error path that does not transfer across languages.
- **The caller may catch and ignore.** Once the error is catchable, callers can drop it. After an
  invariant violation the simulator is in undefined state; resuming operations on that state
  produces wrong output. Avoiding this would require explicit "poison" state in the simulator (a
  corruption flag checked at every entry point), which adds significant complexity for an outcome
  that is operationally worse than termination.
- **The `noreturn` property would be broken.** Code that currently depends on the error path not
  returning would need to be rewritten, or would have to absorb the new "might return after
  exception is caught" semantics.

Every property the simulator currently has from `_Exit(1)`-on-error would have to be reconstructed
at substantial cost, and the result is operationally worse than termination.

### Downgrade some errors to warnings that do not terminate

This proposal usually takes the form "downgrade some checks to warnings; let the simulator continue
past them." It does not work for several distinct reasons.

**Post-error behavior must be designed for every check.** A check fires because an invariant was
violated. The next several lines of simulator code assume the invariant held. With the check
downgraded to a warning, the simulator must do *something* on the now-violated path. The options
are:

1. Continue with the literal violating value. Produces array-out-of-bounds accesses, silent
   corruption, or platform-specific crashes far from the original cause.
2. Clamp, modulo, or mask the violating value into a legal range. Produces an arbitrary fabricated
   behavior with no spec justification: different from silicon, different from what the SW expects,
   indistinguishable from a real result at the output.
3. Skip the operation entirely. Downstream simulator state assumes the operation completed.
   Subsequent code propagates NaN or zero values that look like real results.
4. Set a poison flag and bail on subsequent operations. Functionally equivalent to terminating, but
   with additional state, additional checks at every entry point, and a more confusing failure mode
   for users.

Whichever choice is made, it is wrong in some workloads. Designing the post-error behavior for every
check in the simulator is thousands of design decisions, each of them arbitrary. The resulting
simulator has thousands of "what happens after this error" paths, each of which is silently incorrect
for some workload.

Any attempt to analyze and define post-error behavior for each error at scale would be a massive
engineering effort with no clear value-delivery story, and would displace work the simulator team is
already prioritized to do - specification, new-chip support, performance, validation. It is not a
feasible undertaking even if the per-check design problem were solvable.

**Warnings at scale are ignored.** This is empirically the case across every large-scale software
project. Linux kernel build warnings, static-analysis warnings, test-harness warnings - once the
count is large, individual warnings are invisible. The first time a warning fires in CI it is
noticed; the thousandth time, it is background noise. The category "warnings that should have been
errors" grows monotonically.

The C++ `-Werror`/`-Wno-error` discipline is the canonical example. `-Werror` works because it
enforces uniformly. `-Wno-error=specific-warning` exists, is universally available, and is
universally an antipattern: once even one warning is downgraded the discipline is lost. The
simulator's error handling has the same shape. The discipline exists in its uniform strictness;
introducing per-error softening destroys it.

**The ratchet is one-way.** Once a soften-the-check path exists, two ratchets engage. First,
downstream users build dependencies on it: their CI uses it, their tests assume it, removing it
breaks their builds. Second, additional softening requests follow: if check A can be downgraded,
requesters argue that check B can too. The set of hard-fail checks shrinks monotonically. Within
months, the simulator has a "permissive mode" that effectively suppresses everything.

**The customer-facing artifact changes shape.** The simulator is evaluated by external consumers -
safety-critical certification reviewers, academic users, IP customers - *as an artifact*, not as
"this artifact with these specific configurations." A simulator with a permissive-execution-path is
materially different from a simulator without one, regardless of whether any specific use exercises
the permissive path. Once the path is shipped, the artifact's discipline story is materially weaker.
See "Why the discipline is load-bearing for safety-critical use" below.

**Performance cost, direct and structural.** The current noreturn-on-error pattern is heavily
optimized by the compiler. The failure path is `[[noreturn]] [[gnu::cold]]` and gets folded out of
the hot section; the success path has no branch overhead; downstream code benefits from the
compiler knowing the invariant holds. A permissive-mode pattern with CFG reconvergence loses all of
these: the violation-handling code must live in the hot path, branch prediction has real cost,
downstream optimizations lose the invariant, register pressure increases, and many loop and
inlining optimizations are foreclosed. The aggregate cost across thousands of checks is measurable
even for users who never invoke the permissive mode, because the code organization has to support
both.

Beyond the direct cost, certain simulator optimizations are *structurally foreclosed* once
invariants are no longer guaranteed downstream. A code path that maps cleanly onto AVX-512 SIMD
under the current invariants - aligned addresses, in-range indices, deterministic format states -
can become unnatural or inefficient if those invariants are no longer guaranteed. The optimization
is not just slower; it is no longer the right shape to write. Future performance work that would
have been possible under the current discipline is no longer available.

The largest cost may be the opportunity cost on the simulator team itself. Time spent designing,
implementing, testing, and maintaining permissive-mode infrastructure is time not spent making the
simulator faster at its primary job. The team is small and the backlog of useful performance work
is long; this trade-off is direct, not abstract.

**Configuration matrix explosion and ongoing maintenance.** A single permissive mode is bad enough;
a permissive *system* with independently-controllable suppressions is much worse. With N
suppressible checks the configuration space is 2^N; even with grouped suppressions the effective
configuration count grows quickly. Each is a distinct artifact whose behavior, performance, and bug
surface differ from the others. Bugs can be configuration-specific (manifesting only under a
specific combination of suppressions); customer-support questions require knowing which
configuration was used; bug triage requires distinguishing strict-mode behavior from
permissive-mode behavior. The current "one configuration, fully tested" property is a load-bearing
simplification that disappears as soon as suppressions become independently configurable.

Every supported configuration also becomes an ongoing engineering commitment. Issues filed against
permissive-mode behavior require triage, investigation, and either fixing or formally declining -
each of which is engineering work. The simulator team's capacity is finite; every commitment to
maintain an additional configuration competes against other work (specification, new-chip support,
performance, validation).

### "Just continue and do anything"

This is the casual form of the warning-mode proposal. It does not survive contact with a specific
check. As a concrete example:

Suppose `SrcA` row alignment fails with `src_a_row=61` (not a multiple of 8). The check fires. In
"just continue" mode, the simulator must execute the next line:
`value_a = p_tensix->src_a[bank][src_a_row + row][col]`.

`src_a_row + row` now ranges from 61 to 68, but the array is sized `SRC_ROWS` (64). There are four
choices: read past the end of the array (C++ UB, likely crash, possibly silent corruption);
modulo-reduce the index (silent fabricated behavior); skip the operation (propagating zero or NaN
downstream that looks like a real result); or abort (which is what currently happens).

Each of the first three produces wrong simulator output. The user sees the warning, ignores it
(warnings are ignored at scale), then sees the wrong output and concludes the simulator is broken.
The connection between the warning and the wrong output is invisible to them.

The "do anything" framing only sounds reasonable in the abstract. Walking through any specific check
shows what the choice actually means.

### Use `exit(1)` instead of `_Exit(1)`

Discussed in "Why the simulator terminates the process on error" above. `exit(1)` runs cleanup that
is not safe to run after invariant violation. Empirically it crashed during `atexit` handlers when
tried.

## Handling test-runner integration

The most operationally legitimate complaint about `_Exit(1)` is that it terminates the host process,
including any test runner (gtest, pytest, etc.) that was driving the simulator. Subsequent tests in
the same process do not run.

This is a real test-infrastructure pain with a known good solution. The supported approach is
process isolation in the test harness: each test (or each batch of related tests) runs in a forked
subprocess, and the parent test runner continues regardless of subprocess outcome. This is the
standard pattern for testing any code that can `_Exit`, `abort`, or otherwise catastrophically
terminate the process. It is used for kernel-adjacent code, hardware-interaction code,
sanitizer-instrumented code, and anywhere invariant violations are catastrophic.

### gtest

For C++ tests using Google Test:

- `EXPECT_DEATH(stmt, regex)`/`ASSERT_DEATH(stmt, regex)` - the canonical pattern for testing
  code that calls `_Exit` or `abort`. The statement runs in a forked subprocess; the parent expects
  it to die matching the regex. Other tests in the same test binary continue normally.
- `--gtest_death_test_style=threadsafe` - fork-based death tests. Recommended for the simulator's
  usage pattern.
- For tests that should not die but might (depending on the SW being exercised): structure them as
  separate gtest binaries per test group, invoked via CMake/CTest. Each subprocess invocation is
  independent; the harness aggregates results.

### pytest

For Python tests using pytest:

- `pytest-forked` - runs each test in a forked subprocess. Add `--forked` to the invocation. The
  parent process collects results regardless of subprocess outcome.
- `pytest-xdist` - runs tests across multiple worker processes. A worker that dies takes one test
  with it; the dispatcher continues sending tests to other workers. Use `--dist=loadfile` or
  `--dist=loadscope` to control which tests share workers.

### General subprocess-per-test

For test harnesses without native forked-test support, invoke the simulator code via
`subprocess.run()` (or equivalent) from the test, capture exit codes, and report per-test pass/fail
in the test itself. Higher overhead per test but full isolation. Useful for tests that exercise
sufficient simulator state to make in-process isolation impractical.

### Side benefits of process isolation

Adopting forked or subprocess-isolated tests is not just a workaround for the simulator's error
model. It has independent engineering benefits:

- Memory isolation between tests; no leakage of one test's state into the next.
- Cleaner per-test setup and teardown.
- Trivially parallelizable across cores.
- Robust to bugs in the test code itself, not just in code under test.
- Catches any catastrophic-termination case from any source, not just the simulator.

For tests that take more than a few hundred milliseconds anyway, the process-creation overhead is
negligible. For very fast tests, the overhead can be material; in those cases, batch related fast
tests into the same subprocess and accept losing all of them if any one terminates.

## What we will not do

The following remain non-options, even when individual requests are framed as narrow or temporary:

- **Suppression checks in `main`.** Even behind a flag, build option, or runtime configuration. The
  shipped main-branch simulator enforces its contract.
- **Suppression in public binary releases.** External consumers receive a simulator that has the
  discipline it claims.
- **Suppression in the OSS release.** Same reasoning. The public artifact represents the
  discipline.
- **Maintaining a suppression branch on behalf of any downstream consumer.** Maintenance of any
  local modification is the responsibility of the consumer that wants the modification. The
  simulator team declines to absorb that cost; doing so would create unbounded support commitments
  to ad-hoc suppression configurations. See "Local-only workarounds as a last resort" below for
  the narrow circumstance in which a consumer's own local modification is acceptable, and the
  constraints on it.
- **Runtime flags or environment variables to disable checks.** Shipping the capability is shipping
  permissive behavior.
- **Build-time flags to disable specific check categories.** Same. The shipped artifact does not
  include hidden permissive modes.

The constraint is uniform: *the shipped simulator artifact enforces its contract.* Downstream
consumers who need different behavior pursue one of the proper paths below.

## The proper paths for legitimate needs

Several legitimate needs come up regularly. Each has a documented path that is appropriate for the
need:

- **A required feature is currently `UnsupportedFunctionality`.** Use the documented promotion
  process for `UnsupportedFunctionality` features. Provide the workload, the benefit, the
  alternatives considered, and the specification status. The simulator team evaluates against
  documented criteria.
- **A UB/NCB/UV check turns out to be incorrect.** The ISA documentation has a redefinition
  process for these cases, documented in the `tt-isa-documentation` `Glossary.md` under
  "UndefinedBehavior: Note on redefinition". Provide RTL evidence, silicon evidence, and proposed
  spec language. The spec changes; the simulator follows.
- **The SW has a UB or NCB bug that the simulator is correctly flagging.** Fix the SW. This is the
  case in the substantial majority of complaints, and is almost always the right answer.
- **Tests don't continue after one fails.** Adopt process isolation in the test harness (see
  "Handling test-runner integration" above).
- **An internal `AssertionFailure` fires.** This is a simulator bug. File a bug report.

In each case, there is a documented, supported path. The combination of paths covers the legitimate
cases. Cases that do not fit any of them are typically requests for the simulator to accept
incorrect SW, which is the case the discipline exists to prevent.

## Local-only workarounds as a last resort

If - after working through the supported paths above - a downstream consumer has a genuinely urgent
local need to unblock specific development work, and none of the supported paths apply on the
available timeline, there is a last-resort option: maintain a local modification to the simulator
source. Specifically: check out the simulator source, comment out the specific check that is
blocking your local work, and build a custom `libttsim.so` from your modified tree for your local
development use.

This is **strongly discouraged as a general practice** and exists only as a short-term local unblock
while the proper path is being pursued. In nearly every case where this option is reached for, the
right thing to do is to fix the SW instead. The simulator caught a real problem; commenting out the
check hides the problem but does not solve it, and shipping the workaround would amount to shipping
the original problem with an extra step.

Specific constraints on this last-resort use:

- **Do not ship a modified simulator.** Whatever is built locally stays local. If a modified
  simulator ends up in a CI pipeline, a release artifact, a downstream deliverable, or any branch
  that other consumers may depend on, the SW quality of whatever product line is consuming it has
  been compromised - not just one team's local development unblocked. The correct response to the
  underlying need is to fix the SW or pursue the documented spec-change process, not to ship around
  the simulator.
- **Do not maintain a long-lived "suppressions branch" of the simulator source.** Branches like
  this immediately create their own problems: where does the branch live? If the consuming CI is
  on a public GitHub repository, the branch presumably has to be public too - which means shipping
  the modified simulator publicly, which is the failure mode above. There is no clean place for a
  long-lived suppression branch that does not also create the shipping problem. The correct
  response is not to find a clever home for the branch; the correct response is not to have the
  branch at all.
- **The simulator team will not maintain such a branch on a downstream consumer's behalf.**
  Maintenance of any local modification is the consumer's own responsibility (per "What we will
  not do" above).
- **Treat any local commenting-out as a debt to be repaid quickly.** The expected lifetime is days,
  not weeks. The bug in the SW (or the case for redefining the check) is still the actual problem;
  the local workaround is a coping mechanism while the actual problem is being addressed.

Local-workaround use should be rare and short-lived. If a consumer finds themselves repeatedly
reaching for this option, that is a strong signal that one of the proper paths above is the right
mechanism instead.

## Why the discipline is load-bearing for safety-critical use

The simulator's intended purposes include safety-critical use cases: ISO 26262 (automotive), DO-254
(avionics), IEC 62304/FDA (medical), and other regulated markets.

Safety certification evaluates tools against their documented properties. The relevant properties
for the simulator include strict enforcement of the architectural contract, absence of permissive
execution paths, traceability between simulator behavior and specification, and reproducibility.

A simulator with a suppression mode - even a mode that is rarely used, gated behind a flag, or
documented as "for development only" - has materially different properties from a simulator without
one. The certification narrative for the former is meaningfully weaker than for the latter.
Reviewers evaluate the tool as an artifact, not as a specific configuration. Pointing at "the
specific test runs we do not use the suppression mode for" does not change what the tool is.

Beyond safety-critical use specifically, the discipline is still valuable: it produces a simulator
that catches real SW bugs, that maintains correctness over silicon revisions, and that does not
accumulate "ignored warning" technical debt.

## A note on historical patterns

Simulator projects that have relaxed their error-handling discipline have a consistent track
record: the relaxation is structurally hard to reverse, downstream consumers come to depend on the
lax behavior, the maintenance burden compounds across silicon revisions and architectural variants,
and the original "temporary" justification turns out to be permanent.

This pattern is not specific to this simulator. It applies to any tool whose value depends on
enforcing a contract against the code that uses it: compilers (`-Werror`), static analyzers,
dynamic checkers, formal verification tools, type systems. Tools that maintain the discipline
retain their value over multi-year horizons. Tools that relax it become noise generators with
degraded ability to catch real problems.

The simulator's posture is consistent with this pattern. The discipline is intentional, the costs
of relaxation are understood, and the supported paths for legitimate needs are documented above.
