---
name: c-cpp-review
description: Review C and C++ changes for actionable correctness, memory-lifetime, undefined-behavior, concurrency, error-handling, and ABI defects. Use for requested code reviews or focused audits, including embedded and Zephyr code; do not automatically turn ordinary implementation into a broad audit.
---

# C/C++ Code Review

## Establish review scope

Identify the requested diff, commit range, files, or subsystem. Read applicable repository instructions and build flags, including C/C++ dialect, exception/RTTI policy, optimization, libc, target architecture, and enabled configurations. Read callers and ownership contracts around changed code; a local snippet rarely proves lifetime or synchronization correctness.

Review existing bugs only when requested or when the change makes them relevant. Do not edit code during a review unless fixes are also requested. Preserve the author's architecture and style unless they cause a concrete defect.

## Trace consequential defects

Prioritize reachable failures with a concrete trigger and impact:

- **Lifetime and bounds:** escaping stack pointers, borrowed buffers retained by async work, dangling callbacks, use-after-free, double release, invalid iterator/reference use, allocation failure, count/byte confusion, and off-by-one access.
- **Language rules:** signed overflow, invalid shifts, narrowing, enum/range assumptions, uninitialized data, alignment, aliasing, object lifetime, packed access, and unsafe varargs formatting. Consider compiler flags before asserting behavior.
- **Concurrency:** mutable shared state, unsynchronized reads, atomic ordering, missed wakeups, lock inversion, reentrancy, ISR/thread context restrictions, queue saturation, and teardown racing with callbacks or DMA. `volatile` does not establish a happens-before relationship.
- **Errors and resources:** ignored return values, partial initialization, rollback, timeout/cancellation propagation, stale state after failure, and resource leaks across repeated operation.
- **C/C++ boundaries:** C linkage, struct layout, pointer ownership, ABI assumptions, exception escape, callback lifetime, and behavior under the project's no-exceptions/no-RTTI settings when applicable.
- **Embedded consequences:** register side effects, cache/barrier direction, aliased or reserved memory, RF/PA shutdown, audio buffer geometry, and worst-case blocking. Base hardware claims on the actual platform sources or primary documentation.

Check cross-target effects when shared interfaces change. Keep protocol wire compatibility, persistent data compatibility, and imported license/provenance changes in scope when touched.

## Validate findings proportionally

Use a minimal reproducer or existing targeted tests when practical. Sanitizers and host tests help with host-visible memory/UB defects but do not prove DMA coherency, MMIO semantics, interrupt timing, or target ABI correctness. Use cross-builds, static analysis, or linker/config inspection where those provide relevant evidence.

Do not invent a failing test or claim commands ran when they did not. Treat uncertain concerns as questions or limitations, not proven defects. Avoid findings based only on style, theoretical performance, or unsupported hypothetical inputs.

## Deliver an actionable review

Lead with findings, ordered by impact. For each, give a short title, verified file/line location, the triggering scenario, the observable consequence, and a concise correction direction. Use the repository's severity convention if present; otherwise use P0 for immediate critical breakage, P1 for urgent serious defects, P2 for ordinary correctness defects, and P3 for minor actionable defects.

Use tight inline code comments when the client supports them. Do not duplicate the same defect across callers. If there are no actionable findings, say so and summarize the material verification limits. Do not pad the review with praise or a rewrite plan.

## OE3ANC HT project context

When reviewing this project, read `AGENTS.md` from the repository root. Focus on controller ownership, loss-independent PTT release, no configuration changes during TX, exclusive diagnostics, fault-latched TX shutdown, settings isolation, bounded audio ownership, and C62 DSP/shared-memory contracts. Verify that Linux host reuse does not depend on emulator-only behavior. These are project invariants, not universal rules for unrelated C/C++ projects.
