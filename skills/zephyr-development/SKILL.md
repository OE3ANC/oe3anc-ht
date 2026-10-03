---
name: zephyr-development
description: Implement, port, build, and debug Zephyr applications, out-of-tree boards, Kconfig/devicetree integration, and native_sim targets. Use for Zephyr-specific development and ListenAI Lisa integration, rather than generic C/C++ review.
---

# Zephyr Development

## Establish the actual build baseline

Read repository instructions, manifests, target configurations, and relevant existing code before editing. Determine the effective Zephyr checkout, board qualifier, toolchain, Python environment, and modules used by the build. Record commit IDs; the manifest can disagree with an active vendor environment.

Use headers, bindings, Kconfig definitions, and documentation from the pinned checkout as the API source of truth. Consult official version-matched documentation when local evidence is insufficient. Do not assume current upstream board layouts, LVGL APIs, or Kconfig symbols exist in an older vendor fork.

For ListenAI Lisa, inspect the installed environment instead of guessing paths. Prefer the established `lisa zep exec` environment for C62 work. Separate the application workspace from SDK and reference checkouts, and leave those checkouts unchanged unless the task explicitly includes modifying them.

## Implement through Zephyr boundaries

- Describe hardware in devicetree and bindings; describe optional capabilities and dependencies in Kconfig. Keep product logic out of board definitions.
- Use standard Zephyr driver APIs where available. Keep vendor HAL, cache, DSP RPC, and host OS details inside drivers or target backends.
- Choose APIs and initialization priorities against the pinned version. Check device readiness and propagate initialization/configuration errors to the owner of application state.
- Use explicit context and lifetime contracts for threads, work items, queues, callbacks, and buffer ownership. Check ISR legality, cancellation, lock ordering, and queue-full behavior at each boundary.
- Keep control messages separate from bulk audio/sample buffers. A dropped event must not prevent a required stop/release action.
- Use out-of-tree board/module integration supported by the actual baseline; do not copy OpenRTX's generated board-root architecture simply because it exists.
- Keep native_sim host facilities separate from simulated peripherals. Use wall-clock pacing for interactive operation and deterministic simulated time for tests where appropriate.
- Keep LVGL operations in their designated context and budget widgets, fonts, heap, and draw buffers explicitly.

## Verify what changed

Start with the affected target, then build other targets that share changed interfaces. Use separate build directories and a pristine configuration when changing boards, manifests, or Kconfig inputs. Inspect generated `.config`, `zephyr.dts`, and linker output to confirm the selected hardware and memory configuration.

Use ztest/Twister cases for meaningful state, error, concurrency, and driver contracts. Select runnable platforms supported by the pinned checkout rather than assuming modern board names. Native tests do not establish physical RF, DMA, cache, or peripheral timing correctness.

Report exact commands, target/environment, build results, and remaining hardware checks. Do not describe a successful compile as a successful hardware bring-up.

## OE3ANC HT project context

When working on this repository, read `AGENTS.md`, the target configuration and `tools/build.py` from the repository root for the current architecture and build contracts. Do not apply its product choices to unrelated projects.

Both implemented targets run Zephyr: C62 and native_sim emulator. LinHT is future work. Preserve the controller's exclusive radio ownership, the C62 firmware/host pairing, DSP memory reservations, and the separation between reusable Linux facilities and emulator-only behavior. The emulator has no live audio or two-instance link in V1. Preserve imported attribution and license headers.
