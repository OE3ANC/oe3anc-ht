---
name: embedded-development
description: Implement and debug MCU peripheral drivers, DMA/cache/shared-memory paths, DSP and audio integration, real-time execution, and firmware memory layouts. Use when correctness depends on embedded hardware behavior; not for generic desktop application changes.
---

# Embedded Development

## Ground hardware assumptions

Identify the exact chip, board revision, clocking, pin mappings, memory aliases, peripheral ownership, and firmware/toolchain versions relevant to the change. Prefer local working code plus primary datasheets, errata, vendor documentation, and register definitions. Distinguish physical chip capacity from the application linker budget and from memory owned by another core.

Preserve known-good hardware sequences while changing architecture. Keep signal-processing, clocking, calibration, and routing changes separate unless the task requires them. Retain source provenance and record which behavior was measured on hardware versus inferred.

## Review the hardware/software contract

- For registers, preserve unrelated fields and handle write-one-to-clear, read-to-clear, reserved bits, and reset requirements using actual hardware semantics. Avoid speculative read-modify-write operations.
- Use integer units at configuration boundaries and explicit conversion/range checks. Check multiplication overflow, fixed-point headroom, sample signedness, endianness, alignment, and packed structures before using data as samples or register values.
- For DMA and shared memory, document producer/consumer ownership and handover. Check cache-line alignment, clean/invalidate direction, memory barriers, address aliases, and accessible RAM regions. `volatile` is not thread synchronization or cache coherence.
- Treat linker, devicetree, MPU, heap, stacks, and coprocessor reservations as one memory layout. Prove non-overlap at build time where feasible. Do not place buffers in external RAM solely because it appears available.
- For interrupts, keep work bounded and use only APIs permitted in ISR context. Defer blocking operations and check interrupt priorities and shared peripheral ownership.
- For threads and streaming, account for worst-case blocking, queue capacity, stack usage, deadlines, underflow/overflow, cancellation, and shutdown. Cancellation must wake waiters without releasing memory still in use.
- Propagate peripheral/DSP errors to the controlling module. Provide an independent stop path for active outputs; do not make deactivation depend on an ordinary queue accepting another message.
- Keep calibration tables, DSP firmware, and host transport versions attributable and reproducible. Requested RF power is not measured RF power.

## Bring-up and verification

Use staged checks appropriate to the change: build/link layout, startup logs, peripheral checks, timing instrumentation, and then end-to-end hardware operation. Measure the actual target rather than deriving resource or timing claims from an emulator.

For audio/DSP changes, check rate, channel mapping, frame geometry, buffer lifetime, gain/polarity, start/stop ordering, and sustained operation while other tasks run. For RF changes, use the existing bench procedure and document frequency, modulation, supply, load, and measurement conditions.

Keep flash/update operations within the user's authorized scope. Before an authorized flash, identify the device and verify image hashes, offsets, reserved partitions, and the recovery path. Do not silently flash hardware as part of a code-only review.

Report build evidence, measured results, inferred assumptions, and remaining bench work separately. A reference implementation's hardware validation does not automatically validate a rewritten integration.

## OE3ANC HT project context

For this project, read the repository's `AGENTS.md`, relevant backend/driver code and devicetree before integration work. The working reference is the local OpenRTX `c62-pr-devel-wip` branch; use it as a hardware/feature reference without importing its overall architecture.

C62 uses a tested 48 kHz DSP transport. Preserve the pinned DSP image and channel/rate adaptation during initial integration. Preserve shared SRAM and PSRAM reservations; DSP heap ownership is not fully established. Radio control has one owner, diagnostics are exclusive, and DSP/audio faults disable TX until reboot. Simulator results cannot establish FM/M17 RF or hardware audio correctness.
