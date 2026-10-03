# Repository development instructions

These instructions apply to every feature, bug fix, refactor and code review.
Read the relevant source, configuration and tests before changing behavior.
Companion work also follows the normative definition in `protocol/companion/`.
The user's current request defines the task scope.

## Simplicity and architecture

- Use the **ponytail** skill for every coding task. Read its instructions and
  apply the simplest solution that satisfies the actual requirements.
- Keep code and architecture simple, clean and maintainable. Do not over-engineer.
- Trace the affected flow and callers before editing. Fix the root cause at the
  shared boundary, rather than adding patches to individual callers.
- Reuse existing helpers, standard libraries and Zephyr facilities before adding
  code or dependencies. Do not add speculative features, generic frameworks,
  one-use abstractions or configuration without a concrete need.
- Keep changes focused. Avoid unrelated cleanup, mass formatting and public API
  churn unless the user explicitly requests a source cleanup. Preserve existing
  user changes and leave dependency checkouts unchanged.
- Optimize for clarity and correctness first. Change performance-sensitive code
  using measured or demonstrable constraints; do not sacrifice safety to shorten it.

## Ownership and hardware contracts

- Preserve the radio controller as the sole owner of RF state and transitions.
  UI presents state and submits commands; one UI thread owns LVGL.
- Keep hardware details in drivers/backends, hardware descriptions in devicetree,
  optional capabilities in Kconfig, and reusable logic in modules.
- Preserve independent PTT release, fault shutdown and power-off paths. A full
  ordinary queue must never prevent RF deactivation or leave TX latched.
- Make thread, interrupt, peripheral and buffer ownership explicit. Use Zephyr
  synchronization; `volatile` does not provide thread synchronization.
- Keep ISR work bounded and nonblocking. Check lock ordering, cancellation,
  deadlines, queue capacity and the lifetime of buffers used asynchronously.
- Check device readiness and propagate errors. Validate external inputs and
  persisted data before using them; never silently ignore a safety-relevant failure.
- Preserve the pinned host/DSP pairing, 48 kHz transport, memory reservations,
  calibration and known-good initialization sequences unless the task changes them.
- Use the pinned SDK's headers and register definitions. Preserve unrelated
  register fields and respect write-one-to-clear/read-to-clear semantics.
- Do not flash hardware unless authorized. Compilation and simulator tests do
  not establish electrical, RF, audio, DMA or timing correctness on the radio.

## Companion website and protocol

- Keep the website source, build configuration and assets in `companion/`.
  Produce a self-contained static build in `companion/dist/` for later Vercel
  or GitHub Pages deployment. Support domain-root and project-subpath hosting;
  do not require a backend service. Deployment requires a user request.
- Require an exact companion/firmware release match for live UI and connected
  CPS. On mismatch, explain the required version and link to its archived
  companion. Preserve immutable versioned website releases; keep bootloader
  tools and offline codeplug editing/import/export available independently.
- Maintain one normative, versioned, language-neutral application protocol in
  `protocol/companion/`. Both Zephyr and the web companion must follow it.
  Keep protocol versions independent of firmware, LVGL and codeplug versions.
  The public baseline is protocol 1.0 and version 1 of each project-owned file
  format; development-era formats and migration paths are unsupported.
- Every interface change must update the definition, Zephyr implementation,
  web implementation, version/capabilities, shared fixtures and compatibility
  documentation together in the same change. Additions, changed semantics,
  limits and errors all count. One-sided protocol changes are incomplete.
- Run paired conformance checks and affected firmware/web builds for protocol
  changes. Check generated files for freshness; a version bump alone does not
  prove compatible behavior. Reject incompatible sessions before mutation.
- Keep the radio authoritative for navigation, drafts and RF state. The browser
  LVGL instance renders coherent presentation snapshots and sends virtual key
  events to the radio; it must not independently apply radio actions.
- Physical and virtual keys operate one shared UI in arrival order. The web
  companion provides ordinary front-panel keys only, with no remote PTT control
  or browser audio streaming.
- CPS writes replace the complete validated codeplug. Reject stale writes,
  preserve browser drafts for export and require a fresh read before retrying.
- Preserve companion-mode pin ownership and local unplug/Done confirmation.
  Remote keys must not confirm disconnection or restore physical PTT. Remote
  PTT requires controller validation and a radio-enforced expiry/release path
  independent of browser cleanup, ordinary queues and bulk transfers.
- Keep CSK bootloader flashing/readback separate from the application protocol.
  Preserve matching application/DSP images and factory/settings regions.
  Build one release bundle with matching images, release identity, addresses
  and checksums. Complete flash backups include raw bytes and device metadata;
  full restoration is a separate verified operation restricted to the same
  radio. Normal updates preserve settings/calibration. Calibration inspection
  is read-only and has no sharing workflow; codeplug files may be shared.
- Show the firmware-tool disclaimer prominently: use is at the user's own
  risk, flashing/restoring may brick the device or lose data, and authors and
  contributors accept no responsibility for resulting damage or loss. Require
  explicit acknowledgement before firmware erase/write or full restoration;
  retain all technical validation checks.

## Code style

- Follow surrounding code and `.clang-format`: four-space indentation, attached
  braces and a 100-column limit for C/C++. Format only the code you change unless
  the task explicitly requests formatting whole files or directories.
- Write source for human readers. Put each statement on its own line and expand
  dense control flow and function bodies. Use blank lines between functions and
  logical phases, such as validation, state changes and cleanup; avoid excessive
  spacing or decorative comment banners.
- Follow `.prettierrc.json` for JavaScript modules: four-space indentation,
  single quotes and a 100-column target. Expand packed declarations and use
  braced control-flow bodies rather than compressed one-line chains.
- Use C for drivers/vendor integration and C++14 for application/protocol logic.
  Do not introduce C++ exceptions, RTTI or a general OS abstraction layer.
- Use descriptive names, small cohesive functions, early returns and explicit
  ownership. Comment code whose intent or constraints are hard to infer, including
  hardware sequences, binary layouts, ownership, timing and cancellation. Explain
  why it works that way rather than restating the statements. Remove redundant or
  outdated comments; preserve license/attribution and safety-relevant explanations.
- Keep readability changes separate from behavior changes. Preserve evaluation
  order, wire/file formats and public interfaces. Leave generated outputs alone;
  change their generators only when the task requires it.
- Prefer fixed-size/bounded storage in real-time paths. Avoid allocation in ISRs
  and time-critical loops. Check sizes, ranges, units and integer overflow.
- Keep public declarations in `include/ht/`; keep internal helpers private.
  Make interfaces only as broad as the current callers require.
- Preserve SPDX attribution/license headers. New files use the applicable
  project license; imported code retains its original attribution and license.
- For Python tooling, follow existing four-space style, use the standard library
  where practical, and check subprocess failures. Do not add tooling dependencies
  solely for formatting or a small test.

## Validation and mandatory subagent review

1. Implement the smallest complete change and add a focused regression check for
   meaningful new logic or bug behavior. Prefer existing test infrastructure.
2. Run the relevant checks and affected target builds using the pinned environment
   and `tools/build.py`. Inspect generated configuration/devicetree when changing
   Kconfig or pin mappings. Build both targets when changing shared interfaces.
3. **After implementation, ask a separate subagent to review the changes.** Give
   it the task, intended behavior, changed files, validation evidence and known
   limitations. It must inspect correctness, concurrency, error handling,
   regressions, simplicity, maintainability and unnecessary complexity. Use the
   C/C++ review skill when applicable. Review the actual diff and affected callers.
4. Resolve every actionable finding, simplifying or optimizing the implementation
   where the review identifies a real problem. Rerun affected checks and request
   a fresh review of the fixes. Repeat until no actionable findings remain.
   Optimization means fixing demonstrated problems, not adding speculative tuning.
5. Do not claim completion with unresolved actionable findings. If a finding
   cannot be resolved in scope, explain the concrete blocker and remaining work.
   A review unavailable because of tool limits must be reported, not invented.
6. Before delivery, check the diff for accidental changes and whitespace errors.
   Update relevant documentation when behavior, interfaces or build steps change.
7. Once validation passes and independent review finds no remaining problems,
   create one or more focused, easily reviewable commits. Use clear commit
   messages describing what changed. Keep each commit cohesive and include only
   changes belonging to the task; do not commit unrelated user work.
8. Report what changed, commits created, tests/builds run, review outcome and
   remaining bench work.

Keep review delegation focused on the current change. Do not create user-visible
chats for subagent review or expand a small task into a broad repository audit.
