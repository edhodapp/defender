# Defender — Testing Architecture

> "Sim effort for tests will be the major component in this project."
> "Get those requirements perfect then test from there."
> — Ed, 2026-05-18

This document captures the test-tooling layout, what each file is
responsible for, and how new tests get added when REQUIREMENTS.md
gains new entries. The companion document is `REQUIREMENTS.md` —
this file describes how we verify it.

## Hard rule: requirements drive tests, tests drive code

The order is:

1. Refine the requirement in `REQUIREMENTS.md` until it's one
   assertable claim.
2. Write a failing test that captures the claim.
3. Make the test pass — either by fixing the code or by documenting
   the deviation explicitly.

A bug that doesn't show up as a failing assertion is a missing
requirement, not just a missing test. Surface it as a refinement
PR against the requirements doc first.

## Test layers

| Layer | Tool | Purpose |
|-------|------|---------|
| Unit | `tests/test_*.c` | Per-requirement assertions; one or two scenarios per file. |
| Characterization | `tests/critter_physics.c` | Per-enemy motion fingerprints (CSV-ish output, no assertions — read by hand). |
| Gameplay profile | `tests/profile_gameplay.c` | Full-game simulation under a programmatic "test pilot"; metrics-only. |
| Integration | `tests/test_defender.c` | Cross-cutting boot → wave → ship → enemy → projectile flow on the simulator. |

Unit tests are the primary contract. The other layers exist to
catch system-level regressions (gameplay feel, balance) that the
unit tests aren't dense enough to surface.

## Test harness primitives (`sim_helpers.c`)

The harness wraps simavr with the project-specific glue. Anything
a test needs to do should land here:

- **`sim_boot(elf)` / `sim_boot_no_warmup(elf)`** — load firmware,
  initialize the AVR, set `skip_title_flag=1` in the warmup path
  so tests don't deadlock in `title_loop`.
- **`sim_run_frame(s)` / `sim_run_to_next_iter(s)`** — advance the
  simulator until the next `main_loop` entry. Times out at 4×
  frame budget with a clear error. Don't use during `title_loop` —
  it sits in a sleep wait and `main_loop` is never reached.
- **`sim_run_cycles(s, n)`** — advance N raw cycles. Use when the
  firmware is in `title_loop` or mid-`_reset`, or when measuring
  state at a specific cycle offset.
- **`sim_clear_state_minimal(s)`** — clear entities + projectiles +
  audio queue + spawn timer + game_state + lives. Disables the
  planet-destruction watcher by default. Each `test_*.c` file's
  scenario builder should call this before placing the entities
  the test cares about.
- **`sim_btn_press / sim_btn_release(s, irq)`** — drive button IRQs.
  Edge detection in `title_loop` and the game-over path means most
  tests will press → run frame → release → run frame for a clean edge.
- **`sim_mem_w / sim_mem_r(s, addr, val)`** — direct BSS access. All
  game state symbols (`lives`, `score_*`, `respawn_invuln`, etc.)
  are looked up at boot and exposed on the `sim_t` struct.

### Symbols exposed on `sim_t`

If your test references a BSS symbol, prefer the `sym_*` field
on `sim_t` over an inline `sim_lookup(s, "name")` call. Add new
ones as production code adds new BSS bytes. Current set in
`sim_helpers.h` includes the lifecycle bytes (`lives`, `game_state`,
`respawn_invuln`, `boot_warp_frames`, `planet_destroyed`,
`spawn_countdown`, etc.), the entity table base, the framebuffer
base, and the score bytes (looked up inline because they're
multi-byte).

### EEPROM access

simavr's m32u4 core attaches the EEPROM block by default. Use
`avr_ioctl(avr, AVR_IOCTL_EEPROM_GET, &desc)` and `AVR_IOCTL_EEPROM_SET`
to read/write `avr->eeprom` from outside the firmware — see
`tests/test_eeprom.c` and `tests/test_title_input.c` for the
pattern.

## When to add a test file vs. extend an existing one

Add a new file when:
- The requirement cluster is logically distinct (a new subsystem).
- The test needs a non-trivial setup helper that doesn't fit
  existing files (e.g., the EEPROM tests needed `reboot_to_main_loop`).
- The cycle budget of existing scenarios is already tight (some
  tests run multi-million cycles per case).

Extend an existing file when:
- You're adding 1–3 cases to an already-covered requirement (e.g.,
  a new edge case for the lives lifecycle).
- The setup is identical to existing cases.

## Test-design checklist (for new tests)

Before committing a new test file, verify:

- [ ] **Each requirement it pins is cited by ID in a header
      comment.** ("Pins R7.3 ...".)
- [ ] **Positive AND negative cases.** "Pod overlap deals damage"
      should be paired with "Pod off-screen does NOT deal damage".
- [ ] **Boundary cases.** Edge of overlap box, exact threshold
      crossings, off-by-one in counters.
- [ ] **No reliance on global test ordering.** Each test resets
      state via `scenario()` / `sim_clear_state_minimal`. The one
      exception (R15.5 / R12.3 post-boot checks) is documented
      explicitly in `test_score_events.c`.
- [ ] **The test would fail BEFORE the fix exists.** Run it against
      the broken code first to confirm it catches the bug. If it
      passes even before the fix, the test isn't actually pinning
      the requirement.
- [ ] **The `Makefile` is updated** in three places: the `all:`
      target list, the build rule, and the `test:` driver block.
- [ ] **REQUIREMENTS.md is updated** to point the matching IDs at
      the new test file.

## Common pitfalls

- **`main_loop` deadlock in `title_loop` or `_reset`.** Use
  `sim_run_cycles`, not `sim_run_frame`, when the firmware isn't
  going to return to `main_loop`. The 4×-frame safety timeout in
  `sim_run_to_next_iter` will fire and error out.
- **Slot recycling.** Several handlers (`try_spawn_enemy`,
  `spawn_swarmers`) scan from slot 0 looking for free slots, so a
  just-cleared slot is often reused. Assert by COUNTING or by
  inspecting the entity's TYPE, not by "slot N is inactive".
- **EEPROM write timing.** Each EEPROM byte write blocks ~3.4 ms
  (~54 K cycles). Tests that trigger writes need to advance enough
  cycles to let the writes drain — 500 K cycles for ~10 bytes,
  1.5 M cycles for the wipe path that touches 9 bytes.
- **`sim_clear_state_minimal` disables the planet watcher.** Tests
  that exercise R11.* must re-enable it via
  `sim_mem_w(s->sym_planet_check_disabled, 0)`.
- **Boot warp affects slot 0 only.** `sim_clear_state_minimal`
  already clears `boot_warp_frames` so tests that place a Lander
  at slot 0 see normal AI motion. If you need to test the warp
  itself, DON'T call `sim_clear_state_minimal` first.

## Test backlog tracking

`REQUIREMENTS.md` is the source of truth. Every row's Test column
is either a file path (covered) or "MISSING" / "STRUCTURAL" /
"FUTURE" (intentionally not covered, with reason).

When closing a backlog item:
1. Add the test case(s) that pin the requirement.
2. Run the full suite (`make -C tests test`) and confirm green.
3. Update REQUIREMENTS.md row to cite the new test file.
4. Commit with a message that names the requirement IDs covered.

The commit message convention is `tests: pin RX.Y-Z (subsystem)`.
This makes `git log --grep=R5.10` find every test commit that
touched a specific requirement.
