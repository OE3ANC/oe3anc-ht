# Working on OE3ANC-HT

These instructions apply to every feature, bug fix, refactor and code review.
Read the relevant source, configuration and tests before changing behavior.
Companion work also follows the normative definition in `protocol/companion/`.
The user's current request defines the task scope.

## Preserve the current work state

For multi-step implementation, keep a concise `state.md` at the repository root
(next to this file). Update it at meaningful checkpoints, before handoff and
before anticipated context compaction. Record the objective, completed work,
commit IDs, dirty files, exact checks/review results and evidence paths, live
process/tool handles, blockers and the next action. Replace stale status; do not
include secrets, private messages or long command transcripts.

Read the checkpoint at the start of a turn and after compaction when resuming
work. Verify it against Git, files and live handles: a checkpoint is not proof
that a process is running or that a test still applies. Reconstruct missing or
stale state from authoritative evidence. Never restart a job merely because
observing it timed out, or repeat valid work just to populate a checkpoint.

After completing a milestone, finish checks/review and commit its intended
changes before starting the next milestone. Record the commit, evidence and next
action; keep any task board consistent. Save the checkpoint whether or not
compaction occurs. Do not claim to trigger compaction without a supported tool,
or block authorized work waiting for the user to compact. Keep bookkeeping
proportionate: a small documentation-only edit needs no new state file or board.

## Bounded planning and execution

- When asked to "grill me", remain in planning: challenge assumptions, ask small
  batches of consequential questions and recommend simple defaults. Record the
  smallest useful deliverable, observable acceptance criteria, checks, non-goals
  and handoff artifact in the existing plan. Do not implement until approved.
- Work on one approved milestone at a time. Distinguish concrete correctness,
  security, safety or acceptance failures (BLOCKER) from optional improvements
  and unrelated discoveries (FOLLOW-UP). Do not make follow-ups conditions of
  delivery or silently change the agreed scope or architecture.
- For multi-milestone work, reuse an existing execution board or create
  `TASKS.md` with stable IDs, dependencies, acceptance criteria, status and
  verification evidence. Use READY, ACTIVE, BLOCKED and DONE, with FOLLOW-UP
  separate. Keep one primary implementation task ACTIVE. Include final
  package/verify/handoff work; DONE requires evidence. `state.md` references
  the board rather than duplicating it. Do not create a competing plan.
- Keep delegation flat and bounded. When implementation is delegated, use one
  fresh-context worker per milestone and a separate independent reviewer. Give
  each the scope, invariants, baseline, file ownership, evidence and acceptance
  checks. Workers/reviewers must not recursively delegate or make commits.
  The coordinator owns integration, verification, commits and checkpoint files.
  Use inherited model settings unless the user authorizes a specific override;
  model permissions from another project do not transfer here.
- A worker handoff is a candidate, not acceptance. It must include changed paths,
  exact check results, evidence, remaining findings and the next action. Do not
  restart completed work just to adopt a different agent or workflow.
- Give concise progress updates with actual results, remaining checks/blockers
  and any needed decision. Time pressure triggers reporting or scope discussion,
  never skipped checks, fabricated success or waived safety requirements.

## Simplicity and architecture

- Use `skills/ponytail/SKILL.md` for coding tasks. Explicit requirements,
  readability and this review workflow take precedence over brevity suggestions.
  Apply the simplest solution that satisfies the actual requirements.
- Keep code and architecture simple, clean and maintainable. Do not over-engineer.
- Trace the affected flow and callers before editing. Fix the root cause at the
  shared boundary, rather than adding patches to individual callers.
- Reuse existing helpers, standard libraries and Zephyr facilities before adding
  code or dependencies. Do not add speculative features, generic frameworks,
  one-use abstractions or configuration without a concrete need.
- Keep README feature sections short and user-facing: include only essential
  capabilities and limitations that affect everyday use. Do not append register
  details, implementation notes, debugging procedures, test results or fix history.
  Put necessary technical detail in the relevant developer documentation or commit
  description; update existing feature bullets instead of growing a changelog.
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
  Keep real-time audio callbacks free of blocking operations, allocation, I/O
  and synchronous logging; keep DSP separate from device adapters and UI code.
- Check device readiness and propagate errors. Validate external inputs and
  persisted data before using them; never silently ignore a safety-relevant failure.
- Preserve the pinned host/DSP pairing, 48 kHz transport, memory reservations,
  calibration and known-good initialization sequences unless the task changes them.
- Use the pinned SDK's headers and register definitions. Preserve unrelated
  register fields and respect write-one-to-clear/read-to-clear semantics.
- Do not key a physical radio or flash hardware unless explicitly authorized.
  Never introduce transmission as a default startup action. The user performs
  bench tests; report required hardware verification as pending. Use automated
  tests, saved fixtures and emulator checks for agent validation. Compilation
  and simulator tests do not establish electrical, RF, audio, DMA or timing
  correctness on the radio.

## Companion website and protocol

- Keep the website source, build configuration and assets in `companion/`.
  Produce a self-contained static build in `companion/dist/` for later Vercel
  or GitHub Pages deployment. Support domain-root and project-subpath hosting;
  do not require a backend service. Push/tag/deployment actions require user
  authorization. An authorized push to `main` runs the existing automatic nightly
  firmware/companion workflow without another approval or manual trigger. Preserve
  replaceable nightly artifacts and build-date labeling; versioned releases are
  created from `v*` tags and their published companions remain immutable.
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
- Keep UART reception and physical PTT available without a companion-mode
  toggle. On C62, a continuous shared-pin low of 300 ms is physical PTT; valid
  UART bytes reset the timer. Physical PTT takes priority over remote intent.
  Remote PTT requires controller validation and a radio-enforced expiry/release
  path independent of browser cleanup, ordinary queues and bulk transfers.
  Remote cancellation must never release a physical hold.
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

## Validation, independent review and handoff

During development, run the smallest useful check set for the change and affected
callers/integration boundaries. Broaden it for shared behavior changes, failures
or concrete unresolved risks. An acceptance checklist is not an instruction to
rerun every check after each small edit. Test meaningful behavior and failures;
add focused regression coverage for meaningful new logic or bug behavior using
existing test infrastructure. Do not weaken assertions or discard failed fixtures
to obtain a pass.

Use the pinned toolchains in `config/ci-toolchains.json`, `tools/build.py` and
the local Docker environment when available. Preserve dependency/container and
incremental build caches. Do not delete build directories, disable caches or
force clean builds without a concrete cache/toolchain problem, an explicit
clean-build acceptance criterion or a user request. Limit invalidation to the
affected target. Inspect generated configuration/devicetree for Kconfig/pin
changes; build C62 and emulator when shared interfaces change.

Cached compilation is not a cached test result. Reuse a previous passing result
only when the relevant source and transitive dependencies, fixtures/generated
inputs, build scripts, toolchain, target/configuration and runtime environment
are unchanged, with recorded evidence. A binary's existence or an unchanged test
filename is insufficient. If applicability is uncertain, rerun using the build
cache. Report fresh and validly reused results separately, including any skipped
or unexecuted coverage. Do not build a new caching framework for this purpose.

Keep CI feedback fast and measure preparation as well as test runtime, including
dependency installation, builds, fixture generation and environment startup.
The pinned Zephyr/DSP/web release gate remains mandatory even when it exceeds
60 seconds. Do not remove or
move required coverage out of CI merely to meet a runtime target. Any proposed
CI budget or coverage change is a separate scoped decision, with cold-cache
measurements on the target runner and an explicit account of coverage gaps.

For explicitly requested diagnostic/smoketest builds, prioritize the hardware
handoff once the requested target builds and package/integrity checks pass.
Label the exact revision/hash and pending or failed checks; never call it
production-ready. Known safety/data-loss blockers and build/package failures
still block shipment. Preserve runtime RF/PTT gates. A hash-pinned working-tree
snapshot may be handed off before comprehensive checks, but finish required
checks/review before committing code. Routine features do not require a separate
smoketest kit or a wait for manual RF results at each step.

1. On the final code candidate, run applicable comprehensive checks. For shared
   firmware/companion or release work, use `tools/check_ci.py`: it covers paired
   C62/emulator/web builds, protocol conformance, required native/storage/UI tests
   and native/WASM rendering parity. Check generated contracts/fixtures for
   freshness. `--skip-tests` verifies builds only and must never be reported as
   a test pass. Guidance-only edits need content review and `git diff --check`,
   not another firmware build or full test run.
2. **Obtain an independent subagent review before committing.** Give the reviewer
   the task, intended behavior, diff, affected callers, requirements, validation
   evidence and limitations. Use `skills/c-cpp-review/SKILL.md` for C/C++ work.
   Inspect correctness, concurrency, error handling, regressions, simplicity and
   unnecessary complexity. Reviewers classify concrete failures as BLOCKER and
   optional improvements as FOLLOW-UP, with evidence. They do not delegate or
   commit. Checks and review may run concurrently on the same unchanged candidate.
3. Fix all actionable in-scope defects and re-review the fixes and affected
   behavior. Repeat broad review only if changes invalidate it. Record optional
   follow-ups without expanding delivery. If two fix/review rounds do not converge
   or severity is disputed, report the concrete issue and request a decision;
   never waive genuine blockers to meet that limit. Report unavailable review
   honestly rather than claiming it was clean.
4. Rerun affected checks after fixes; repeat the full gate only when changes
   invalidate prior results or required acceptance demands it. Once checks pass
   and review has no unresolved blockers, inspect the staged diff and run
   `git diff --check`. Update relevant documentation for behavior, interface or
   build changes. Create focused, reviewable commits. Exclude unrelated work,
   private captures, dependency checkouts and generated build output. Preserve
   required tracked generated protocol files. Do not push or tag unless requested.
5. Freeze the accepted candidate. Verify agreed artifacts and paired release
   identity against the final source/commit; rebuild metadata after committing
   where necessary. Hand off the exact paths/revision, actual checks and review
   results, limitations and pending bench checks. Update the checkpoint/board
   when applicable. Keep follow-ups separate; do not polish completed work or
   begin another milestone before handoff unless already authorized.

Keep review delegation focused on this change. Do not create user-visible chats
for review or turn a small task into a broad repository audit. Keep skills
project-local and preserve their upstream licenses and attribution.
