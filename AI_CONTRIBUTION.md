# AI-generated local input changes: disclosure and review notes

## Authorship / provenance

All newly proposed code changes in this contribution were written by AI (OpenAI Codex), following the submitting user's requirements and iterative manual feedback. The user did not manually author these implementation changes. Existing upstream code and other contributors' commits are **not** claimed as AI work. This is an experimental local customization offered for review, not a claim of production readiness or complete human line-by-line auditing.

Environment used for local application testing: **macOS 27.0.1 (build 26A434), Apple Silicon arm64; Xiaomi / Redmi K80 (24117RK2CC), Android 16**. Qt 6.9.3, CMake 3.31.6. This is not a Windows, Linux, Qt 5, Intel Mac, or other-device validation claim.

The original local changes were made against pinned commit `9e388b8`. This publication branch applies those changes on upstream `main` at `db8ccba`, preserving the upstream Ctrl/Shift virtual-finger pinch implementation. That existing upstream feature was not written by this contribution.

## Complete input change list

1. `InputConvertGame::wheelEvent`: while game capture is active, accumulate vertical wheel deltas to a 120-unit step and translate an opted-in wheel-up/down key into a key press/release pair. Normal mode / released-cursor mode continues through normal wheel conversion. This chooses configured weapon slots; it does not detect the currently selected weapon.
2. Ignore auto-repeat key events before mapping-mode and panel toggles, avoiding repeated on/off toggles while a switch key is held.
3. Add optional root JSON `cursorKey` to release/reacquire the game cursor. On toggle, release touches, reset panel tracking and update cursor visibility.
4. Add optional root JSON `wheelUpKey` / `wheelDownKey`.
5. Add optional click-node `closeAndroidKey`: pressing the tracked panel's opening key again can send Android Back rather than clicking a covered opening coordinate. Other Android keys can be specified; this is an explicit opt-in mapping.
6. Track the panel opening key locally and reset it when switching mapping mode or toggling cursor mode. There is no feedback from the game about actual panel visibility.
7. In released-cursor mode, route otherwise-unhandled keyboard events to normal input rather than combat mappings.
8. Replace shared static MOVE deduplication with independent per-touch position and active-state bookkeeping. One touch no longer suppresses a different touch's same-coordinate move.
9. DOWN and MOVE carry nonzero pressure; UP carries zero pressure.
10. Reject out-of-range touch IDs without asserting the process. The existing 10-pointer capacity is retained, not newly invented.
11. Reuse an already attached ID for the same key instead of allocating duplicates. Repeated key/mouse press guards avoid duplicate DOWN allocations.
12. Centralize `releaseAllTouches`: stop view-motion and steering/drag timers, clear queued interpolated positions/delays, reset directional and sprint flags, release all active pointers and clear the ID table, view-motion/small-eye state and wheel accumulator.
13. Call release-all when switching mapping modes, releasing the cursor or opening a cursor-enabled panel, reducing stuck touches and continued movement after mode changes.
14. Handle zero-length interpolation without dividing by zero; explicitly enqueue the final interpolation endpoint.
15. Add optional steering-node `sprintKey` and `sprintOffset`, with reverse-key lookup so either Shift/W press order can update the joystick.
16. Extend the forward joystick offset while sprint is held and forward is pressed without backward. Releasing sprint returns to walking offset without replacing the ongoing joystick touch.
17. Initialize steering touch at the center only when it has no existing touch ID, preserving continuity when additional direction keys are pressed.
18. Make `switchMap` optional for ordinary click validation; omitted values use existing false/default conversion. This fixes otherwise-valid Q/E nodes being silently omitted.
19. Add `IDevice::tapUnlockPosition`, the matching Device forwarder and Controller implementation: validate non-camera mode, non-empty frame and normalized position, clamp to pixel bounds, send one DOWN/UP pair using the reserved pointer identity. **This is an unused remnant of an abandoned desktop unlock keypad experiment.** No caller or keypad overlay remains in the main application. It performs no automatic password attempt, brute force, retry loop, security-surface capture or Android lock-screen bypass. It is included to disclose all local changes and is a candidate to remove before merge.
20. The extra pure virtual API changes source/ABI expectations for alternative IDevice implementations; maintainers should review or drop it rather than assume binary compatibility.

## Verification and limitations

The companion desktop app has an isolated protocol harness. Previous local runs checked every configured click node, DOWN/UP coordinates relative to its JSON, simultaneous independent Q/E touches, WASD directions/diagonals, a combined multi-touch sequence, 10-ID capacity and overflow, cleanup, wheel selection, cursor release, panel Android Back and both Shift/W press orders. These checks inspect serialized control messages without sending synthetic test taps to a physical phone. They do not prove a game accepts every action or that HUD coordinates match a current layout.

The user's real-game feedback was that the mapping still did not achieve the usability of the official emulator. Coordinates are fixed; there is no scene recognition or smart context-dependent F interaction, no verified scope/medicine selection, no guarantee manual panel dismissal synchronizes local state, and no anti-cheat/account-safety assurance. No macro, auto-fire, auto-aim, automatic looting or automated password attempts were added.

Review priorities: pointer cleanup on every transition; integer/key-ID assumptions; compatibility of optional JSON fields with existing mappings; zero-motion queue semantics; continuous joystick behavior; actual Android acceptance; interaction with upstream normal-input virtual fingers; cross-platform builds; the unused unlock API.
