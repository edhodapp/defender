# Defender — Requirements

Canonical source: **Williams Defender** (1981 arcade). Where the
arcade behavior is ambiguous or expensive to reproduce on a
128×64 mono Arduboy, this document records the chosen deviation and
its rationale (per the CLAUDE.md "every requirements deviation must
be tracked" rule).

Each requirement is one assertable claim. Status is one of:

- **IMPL** — implemented as specified.
- **PART** — implemented with documented deviation.
- **TODO** — agreed behavior, not yet implemented.
- **FUTURE** — planned subsystem, not in current scope.

Test field cites the test file that pins the requirement; **MISSING**
means a test should be written.

References:

- Williams Defender operator's manual (1981)
- MAME `defender.cpp` driver for cycle-exact behavior questions
- Ed (project owner) for game-design decisions specific to this port

---

## R1. Boot and title screen

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R1.1  | On cold boot the splash screen renders "DEFENDER" + difficulty rows (LOW / MED / HIGH / RESET). | Inferred | IMPL | MISSING (visual) |
| R1.2  | Title-screen cursor moves on UP / DOWN button edge (not level). | Inferred | IMPL | test_title_input.c |
| R1.3  | B-press edge on a difficulty row stores `difficulty` and exits the splash. | Inferred | IMPL | test_title_input.c |
| R1.4  | Holding B on the RESET row for ≥60 frames (1 s @ 60 Hz) zeroes the EEPROM high-score table. | Ed-specified | IMPL | test_title_input.c |
| R1.5  | The held-B that exits the splash MUST NOT immediately trigger SFX_FIRE in main_loop. | Ed-specified (bug reported) | IMPL | MISSING (`audio_play` priority) |
| R1.6  | After GAME OVER → B-press, the splash re-appears (no instant new game). | Ed-specified (bug reported) | IMPL | MISSING |
| R1.7  | High scores per difficulty render next to LOW / MED / HIGH rows. | Williams (HS tied to difficulty unique) | IMPL | MISSING (visual) |

---

## R2. HUD (page 0, top row)

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R2.1  | Wave number renders as a 1-digit glyph at cols 0–6. | Inferred | IMPL | MISSING (visual) |
| R2.2  | Reserve-ship count renders as 1–3 small ship glyphs at cols 8, 16, 24. | Williams (reserves visual) | IMPL | test_lives.c |
| R2.3  | Reserve display caps visual count at 3 even if `lives > 3` (e.g., from a bonus award). | Ed-specified | IMPL | test_lives.c |
| R2.4  | Radar bar occupies cols 32–95, page 0; renders one column per (world_x>>2). | Williams (scanner) | IMPL | test_defender.c |
| R2.5  | Score renders as five fixed-width digits, last digit's pixel col 4 lands on col 127 (right-aligned). | Ed-specified | IMPL | test_score_render.c |
| R2.6  | Score display clamps at 99999 (`render_score_value` saturates internally). | Inferred | IMPL | test_score_render.c |
| R2.7  | Score values 65536–99999 must render correctly (24-bit extractor). | Ed-reported bug, fixed | IMPL | test_score_render.c |

---

## R3. Ship — input and physics

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R3.1  | Ship's screen_x is locked at 60 (player scrolls the world, not the ship). | Williams | IMPL | test_defender.c |
| R3.2  | UP decreases `sprite_y` by 1 per held frame, clamped at 8 (just below HUD). | Inferred | IMPL | test_defender.c |
| R3.3  | DOWN increases `sprite_y` by 1 per held frame, clamped at 47 (just above mountains). | Inferred | IMPL | test_defender.c |
| R3.4  | LEFT decrements `scroll_offset` and sets `ship_facing = 1` (left). | Williams | IMPL | test_defender.c |
| R3.5  | RIGHT increments `scroll_offset` and sets `ship_facing = 0` (right). | Williams | IMPL | test_defender.c |
| R3.6  | B-press fires one beam per `FIRE_COOLDOWN_RELOAD + 1 = 6` frames (10 shots/s peak). | Williams | IMPL | test_proj_spawn.c |
| R3.7  | Beam moves +4 px/frame (right) or −4 px/frame (left) in screen-x; deactivates at edges. | Williams | IMPL | test_proj_motion.c |
| R3.8  | A 5-frame B-tap fires exactly 1 beam (cooldown gate). | Ed-specified (regression test) | IMPL | test_bug_repros.c |

---

## R4. Lives / death / game over

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R4.1  | Game starts with `lives = 3` reserve ships (HUD shows 3); active ship is implicit. Total = 4 chances. | Williams convention | IMPL | test_lives.c |
| R4.2  | A ship-enemy overlap with `lives > 0` decrements `lives` and respawns the ship at default position. | Williams | IMPL | test_lives.c |
| R4.3  | A ship-enemy overlap with `lives == 0` sets `game_state = 1` (GAME OVER). | Williams | IMPL | test_lives.c |
| R4.4  | Death sequence is 180 frames total (3.0 s @ 60 Hz): a 30-frame explosion phase (respawn_invuln in [151, 180]; no input, no collision, 8 fragments rendered from death_y), then a 150-frame respawn-blink phase (respawn_invuln in [1, 150]; input enabled, ship blinks every 8 frames, still no collision). At respawn_invuln==0, normal play resumes. | Williams arcade pacing | TODO | test_lives.c (will need update for new constants) |
| R4.5  | During the entire 180-frame death window, the ship is non-collidable. | Williams | TODO | test_lives.c (constant update) |
| R4.6  | During the 30-frame explosion sub-phase (respawn_invuln > 150), input is suppressed. During the 150-frame blink sub-phase, input is enabled. | Inferred | TODO | test_death_window.c (constant update) |
| R4.7  | After GAME OVER, B-press (edge-detected) soft-resets to splash. | Ed-specified | IMPL | test_death_window.c |
| R4.8  | A held-B carried over from the killing shot MUST NOT instantly soft-reset; edge detection required. | Ed-specified (bug) | IMPL | test_death_window.c |
| R4.9  | High score updates only on the GAME OVER transition (no per-frame EEPROM writes). | Ed-specified (EEPROM wear) | IMPL | test_eeprom.c |
| R4.10 | `lives` saturates at 255 — bonus-ship awards must not wrap to 0. | Ed-specified (bug, fixed) | IMPL | test_lives.c |

---

## R5. Enemies — Lander

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R5.1  | Each Lander update fires once per main_loop frame (60 Hz). | Inferred | IMPL | structural |
| R5.2  | Lander drift event fires every 4 frames (15 events/s). | Defenduino-equivalent | IMPL | test_lander_drift.c |
| R5.3  | Per drift: descend by 1 row toward y=40 if no humanoid alive OR y < 40; else freeze at current y. | Williams (descend to grab altitude) | IMPL | test_lander_drift.c |
| R5.4  | Per drift: seek 1 col toward nearest grounded humanoid (wrap-aware shorter direction). | Williams (Lander hunts humanoids) | IMPL | test_seek.c |
| R5.5  | When y==40 AND aligned (|dx| < 8) with a humanoid, pause counter arms to 7. | Williams (grab pause) | IMPL | test_abduction.c |
| R5.6  | Pause counter decrements 1 per drift event (every 4 frames). | Inferred | IMPL | test_abduction.c |
| R5.7  | Pause counter hitting 0 fires `ul_do_grab`: clear humanoid's slot, set Lander's carry bit, queue SFX_GRAB. | Williams (grab event) | IMPL | test_abduction.c |
| R5.8  | Carrying Lander ascends 1 row per drift toward y=9. | Williams (Mutant transformation) | IMPL | test_abduction.c |
| R5.9  | Carrying Lander reaching y=9 transforms to Mutant in place; humanoid is consumed. | Williams | IMPL | test_mutants.c |
| R5.10 | Beam hitting an empty Lander → Lander dies, +100 pts. | Williams (150 pts; we use 100 — DEVIATION) | IMPL | test_collision.c + test_score_events.c |
| R5.11 | Beam hitting carrying Lander upper half → Lander dies, humanoid becomes falling. | Williams (rescue) | IMPL | test_freeing.c |
| R5.12 | Beam hitting carrying Lander lower half → humanoid dies, Lander survives empty-handed. | Williams (kill cargo only) | IMPL | test_freeing.c |
| R5.13 | Landers are lethal on contact with ship. | Williams | IMPL | test_ship_collision.c |

---

## R6. Enemies — Mutant

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R6.1  | Mutant vertical chase: move 1 row toward ship's `sprite_y` every 2 frames. | Inferred | IMPL | test_mutants.c |
| R6.2  | Mutant horizontal chase: move 1 col toward ship's world_x every 4 frames (wrap-aware). | Inferred | IMPL | test_mutants.c |
| R6.3  | Mutants are lethal on contact with ship. | Williams | IMPL | test_mutants.c |
| R6.4  | Beam hitting a Mutant → Mutant dies, +100 pts. | Williams (150 — DEVIATION) | IMPL | test_collision.c + test_score_events.c |
| R6.5  | Mutants are immune to grab/seek logic (no humanoids to take). | Williams | IMPL | structural |

---

## R7. Enemies — Pod

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R7.1  | Pod horizontal drift: move 1 col toward ship's world_x every 8 frames. | Inferred | IMPL | critter_physics |
| R7.2  | Pod has no vertical motion; stays at spawn y (default 10). | Inferred | IMPL | critter_physics |
| R7.3  | Pod is lethal on contact with ship. | Williams | IMPL | test_critter_kills.c |
| R7.4  | Beam hitting a Pod spawns POD_SWARMER_COUNT (5) Swarmers at the Pod's position, then awards +100 pts for the Pod kill. | Williams (Pod splits into Swarmers) | IMPL | test_critter_kills.c |
| R7.5  | The 5 Swarmers get distinct initial directions (RNG-shuffled across 8 compass dirs) so they fan out visibly. | Williams (starburst spawn) | IMPL | test_critter_kills.c |

---

## R8. Enemies — Swarmer

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R8.1  | Swarmer moves 1 px per frame in its current 3-bit compass direction (8 directions). | Custom — Williams Swarmers had jittery motion | IMPL | critter_physics |
| R8.2  | Every 8 frames, an LFSR coin-flip decides whether to nudge direction ±1 step toward the ideal player bearing. | Custom (inertia + bias model) | IMPL | MISSING |
| R8.3  | Nudge probability is per-difficulty: LOW=25%, MED=50%, HIGH=75% (Williams-faithful aggression at HIGH). | Ed-specified | IMPL | MISSING |
| R8.4  | Swarmers are lethal on contact with ship. | Williams | IMPL | test_critter_kills.c |
| R8.5  | Beam hitting a Swarmer → Swarmer dies, +100 pts. | Williams (150 — DEVIATION) | IMPL | test_critter_kills.c |
| R8.6  | Swarmer trajectories must be reproducible given a fixed RNG seed. | Tooling requirement | IMPL | critter_physics |

---

## R9. Enemies — Bomber, Baiter (future)

| ID    | Requirement | Source | Status | Test |
|-------|-------------|--------|--------|------|
| R9.1  | Bomber follows a sine-wave horizontal path; drops mines that are stationary and lethal-on-contact. | Williams | FUTURE | — |
| R9.2  | Baiter appears ~25 s into a stuck wave; fast straight-line UFO. | Williams (anti-camping) | FUTURE | — |

---

## R10. Humanoids

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R10.1  | Boot spawns 8 humanoids at world_x ∈ {16, 48, 80, 112, 144, 176, 208, 240}, y=48. | Defenduino-equivalent (Williams used 10) | PART (8 not 10 — DEVIATION) | test_humanoids.c |
| R10.2  | A grounded humanoid is encoded as `byte0=0xF0`, byte1=world_x, byte2=48. | Inferred | IMPL | structural |
| R10.3  | A falling humanoid has byte0 bit 0 set; descends 1 row per 4 frames until y=48; on landing, falling bit clears. | Williams (rescue catch) | IMPL | test_humanoids.c |
| R10.4  | A falling humanoid that reaches y=48 becomes grounded again. | Inferred | IMPL | test_humanoids.c |
| R10.5  | Beam hitting a humanoid kills it; no score change. | Williams (penalty −100 — DEVIATION, we don't penalize) | PART | test_freeing.c + test_score_events.c |
| R10.6  | Catching a falling humanoid: on overlap, deactivate humanoid slot, award +500 pts, set `carried_humanoid` flag in BSS. Ship can carry at most one humanoid at a time; subsequent catches while carrying are ignored. | Williams | TODO | test_humanoid_catch.c (will need carry-state cases) |
| R10.7  | Returning a carried humanoid: while `carried_humanoid` is set, if the ship's sprite_y >= 40 (ground altitude band), the humanoid is released — `carried_humanoid` clears, a new TYPE_HUMANOID is spawned (grounded, byte 0 bit 0 == 0) at world_x = scroll_offset + 60 (ship's current world_x), y = 48, and +1000 pts is awarded. | Williams | TODO | MISSING |
| R10.8  | If the ship dies while carrying a humanoid, the carry is discarded silently (no penalty, no spawn). The carried humanoid is considered "lost with the ship". | Inferred (simplification) | TODO | MISSING |

---

## R11. Planet destruction (all-Mutant mode)

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R11.1  | When no humanoids and no carrying Landers remain alive, `planet_destroyed = 1`. | Williams | IMPL | test_planet_destruction.c |
| R11.2  | On destruction trip, every non-carrying enemy (TYPE_LANDER without carry, TYPE_POD, TYPE_SWARMER, future TYPE_BOMBER/TYPE_BAITER) transforms to TYPE_MUTANT in place — world_x and y preserved, state bits cleared. Carrying Landers are skipped so they finish their abduction trip and convert via the existing y==9 → ul_become_mutant path. Williams: skies are all-Mutant after planet loss. | Williams | TODO | test_planet_destruction.c (will need extension for Pod/Swarmer cases) |
| R11.3  | While `planet_destroyed`, the spawner installs only Mutants regardless of wave_pods_to_spawn. | Williams | IMPL | test_planet_destruction.c |
| R11.4  | At wave-end, 8 humanoids respawn and `planet_destroyed` clears. | Williams (new planet) | IMPL | test_planet_destruction.c |
| R11.5  | The destruction check is event-driven (fires at humanoid-removal sites: chh_kill, chh_drop_cargo, ul_become_mutant). | Custom (perf — per-frame scan unnecessary) | IMPL | test_planet_destruction.c |

---

## R12. Spawn cadence and waves

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R12.1  | `spawn_countdown` decrements once per frame; on 0, fires `try_spawn_enemy` and reloads from `spawn_interval_table[difficulty]`. | Custom | IMPL | test_lander_spawn.c |
| R12.2  | Spawn interval: LOW=96, MED=64, HIGH=32 frames. | Ed-specified (data-driven) | IMPL | test_difficulty_init.c |
| R12.3  | Wave 1 begins on splash dismiss; first spawn fires after one full interval. | Inferred | IMPL | test_score_events.c |
| R12.4  | A spawn picks the first inactive entity slot scanning 0..63. | Custom | IMPL | test_lander_spawn.c |
| R12.5  | New spawn position: world_x = (`spawn_pos_idx` * 32 + 16) mod 256, y = 10. `spawn_pos_idx` increments after each successful spawn. | Custom (Defenduino-equivalent) | IMPL | test_lander_spawn.c |
| R12.6  | Spawn type: Pod first if `wave_pods_to_spawn > 0` (decrementing the counter); else Lander. While `planet_destroyed`, always Mutant. | Custom | IMPL | test_planet_destruction.c |
| R12.7  | Wave size per (difficulty, wave) from `wave_sizes_table`. LOW: 5/7/8/9/10/12/12/12. MED: 10/13/14/16/18/20/20/20. HIGH: 15/20/20/.../20. | Custom | IMPL | test_waves.c |
| R12.8  | Pod count per (difficulty, wave) from `wave_pod_count_table`. | Custom | IMPL | test_score_events.c |
| R12.9  | Wave ends when `wave_to_spawn == 0` AND `enemies_alive == 0`. | Inferred | IMPL | test_waves.c |
| R12.10 | At wave-end, `wave_number++` (clamped at 8), `wave_to_spawn` and `wave_pods_to_spawn` re-derived, SFX_WAVE_CHANGE plays, kick-spawn fires. | Custom | IMPL | test_waves.c |
| R12.11 | Wave-end re-spawns humanoids and clears `planet_destroyed`. | Williams | IMPL | test_planet_destruction.c |

---

## R13. Scoring

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R13.1  | Score is 24-bit unsigned, stored as `score_lo / score_mid / score_hi`. | Inferred | IMPL | test_score_render.c |
| R13.2  | Per-kill scoring (Williams-faithful): Lander +150, Mutant +150, Pod +1000, Swarmer +150. Bomber +250 / Baiter +200 land when those enemies are implemented (FUTURE). | Williams | TODO | test_critter_kills.c + test_score_events.c (will need value updates) |
| R13.3  | Killing a humanoid awards −100 points (Williams penalty). Score is unsigned and clamped at 0 — it cannot go negative; any decrement that would underflow leaves score at 0. | Williams | TODO | test_score_events.c (will need new case) |
| R13.4  | Score increments are atomic 24-bit adds; no overflow risk in a single session (max ~16M, 99999 visible cap). | Inferred | IMPL | test_score_render.c |

---

## R14. Bonus ships

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R14.1  | Bonus ship awarded when score crosses `next_bonus` threshold (LOW=3000, MED=6000, HIGH=10000). | Custom (Williams: every 10000 — HIGH is faithful) | IMPL | test_lives.c |
| R14.2  | On award, `lives++` (capped at 255) and `next_bonus += per-difficulty increment`. | Custom | IMPL | test_lives.c |
| R14.3  | `next_bonus` is initialized from the per-difficulty table AFTER the splash sets `difficulty` (not from the MED default). | Ed-specified (bug, fixed) | IMPL | test_difficulty_init.c |

---

## R15. Audio

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R15.1  | SFX_FIRE plays on every successful beam spawn. | Williams | IMPL | test_audio.c |
| R15.2  | SFX_HIT plays on beam-enemy collision. | Williams | IMPL | test_audio.c |
| R15.3  | SFX_DEATH plays on ship-enemy collision. | Williams | IMPL | test_audio.c |
| R15.4  | SFX_GRAB plays when a Lander grabs a humanoid. | Williams | IMPL | test_abduction.c |
| R15.5  | SFX_START plays on game start (splash → main_loop transition). | Custom | IMPL | test_score_events.c |
| R15.6  | SFX_WAVE_CHANGE plays on wave-end. | Custom | IMPL | test_score_events.c |
| R15.7  | While SFX_START is playing, lower-priority FX (FIRE / HIT / GRAB) silently bow out so the start arpeggio finishes audibly. | Ed-specified (bug fix) | IMPL | test_audio_priority.c |
| R15.8  | SFX_DEATH and SFX_WAVE_CHANGE bypass the SFX_START priority guard (game events override the jingle). | Custom | IMPL | test_audio_priority.c |

---

## R16. EEPROM persistence

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R16.1  | EEPROM byte 0 is magic 0xDF marking initialized state. | Custom | IMPL | test_eeprom.c |
| R16.2  | EEPROM bytes 1..9 hold three 24-bit high scores: LOW, MED, HIGH. | Custom | IMPL | test_eeprom.c |
| R16.3  | On cold boot with magic mismatch (uninitialized 0xFF or unrelated firmware), the table is reset to zeros. | Custom | IMPL | test_eeprom.c |
| R16.4  | High score writes only on GAME OVER transition and only if the just-finished run beat the stored value. | Ed-specified | IMPL | test_eeprom.c |
| R16.5  | RESET row + hold-B-60-frames wipes the table (magic preserved, scores zeroed). | Ed-specified | IMPL | test_title_input.c |

---

## R17. Soft-reset semantics

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R17.1  | `_reset` entry zeroes SP to RAMEND and CLI before any other init (soft-reset path may land with IRQs enabled). | Ed-specified (bug, fixed) | IMPL | STRUCTURAL (code inspection) |
| R17.2  | The 30-frame `boot_warp_frames` grace period suppresses slot-0 Lander AI so the boot Lander doesn't start mid-descent during SFX_START. | Ed-specified (UX) | IMPL | test_boot_init.c |
| R17.3  | After GAME OVER → B-edge → `rjmp _reset`, the soft-reset magic byte forces the splash to show even with B still held. | Ed-specified (bug, fixed) | IMPL | test_boot_init.c (magic lifecycle) + test_death_window.c (B-edge path) |

---

## R18. Difficulty scaling (current)

Difficulty (0=LOW, 1=MED, 2=HIGH) currently varies these parameters:

| Parameter                    | LOW   | MED   | HIGH   | Table                          |
|------------------------------|-------|-------|--------|--------------------------------|
| Spawn interval (frames)      | 96    | 64    | 32     | `spawn_interval_table`         |
| Bonus-ship threshold (pts)   | 3000  | 6000  | 10000  | `lookup_bonus_increment`       |
| Wave size, wave 1            | 5     | 10    | 15     | `wave_sizes_table[diff][0]`    |
| Pod count, wave 1            | 0     | 0     | 1      | `wave_pod_count_table[diff][0]`|
| Swarmer nudge probability    | 25%   | 50%   | 75%    | `swarm_skip_thresh_table`      |

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R18.1  | All five tables above are indexed by `difficulty`; out-of-range values fall back to HIGH (table-end default). | Inferred | PART | MISSING |
| R18.2  | Switching difficulty between waves is not supported — `difficulty` is locked at splash dismissal. | Inferred | IMPL | MISSING |

---

## R19. Nukes / smart bombs (future)

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R19.1  | Player starts each game with 3 nukes. | Williams | FUTURE | — |
| R19.2  | A-button single-press triggers nuke if `nukes > 0`. | Ed-specified | FUTURE | — |
| R19.3  | Nuke deactivates every active enemy entity on screen and adds 0 pts (Williams awarded the per-kill score for each killed enemy — TBD). | Williams | FUTURE | — |
| R19.4  | Brief screen-flash visual effect on nuke fire. | Williams | FUTURE | — |

---

## R20. Hardware envelope

| ID     | Requirement | Source | Status | Test |
|--------|-------------|--------|--------|------|
| R20.1  | Target: ATmega32u4 @ 16 MHz, 32 KB flash, 2.5 KB SRAM, 1 KB EEPROM. | Arduboy hardware | IMPL | — |
| R20.2  | Display: 128×64 SSD1306 OLED, mono, page-aligned (8 pages of 128 bytes each). | Arduboy hardware | IMPL | test_oled.c |
| R20.3  | Main loop runs at 60 Hz via Timer1 CTC interrupt + sleep. | Inferred | IMPL | test_frame_rate.c |
| R20.4  | Total flash usage: < 28 KB (Caterina bootloader reserves ~4 KB). Currently ~6.5 KB. | Hardware budget | IMPL | structural |
| R20.5  | Total SRAM usage: < 2.0 KB (leaves stack + scratch). Currently ~1.3 KB (framebuffer dominates: 1024 B). | Hardware budget | IMPL | structural |

---

## Deviation summary (where we differ from Williams)

| Deviation | Williams | This port | Reason |
|-----------|----------|-----------|--------|
| Per-kill score | Lander 150 / Mutant 150 / Pod 1000 / Swarmer 150 / Bomber 250 / Baiter 200 | Same (Bomber/Baiter pending implementation) | Williams-faithful (refined 2026-05-18) |
| Humanoid count | 10 | 8 | Screen width / entity-slot budget |
| Humanoid kill penalty | −100 | −100, clamped at 0 (score can't go negative) | Williams-faithful (refined 2026-05-18) |
| Catch-falling-humanoid bonus | +500 (+1000 if returned to ground) | +500 catch + +1000 return-to-ground both implemented | Williams-faithful (refined 2026-05-18) |
| Death/respawn invuln | ~3 s (180 frames @ 60 Hz) | 180 frames = 30 explosion + 150 blink | Williams-faithful (refined 2026-05-18) |
| Smart bombs at boot | 3 | 0 (FUTURE) | Not yet implemented |
| Hyperspace | Random teleport button | Not present | Niche feature, low player value |

---

## Open coverage gaps (test work to do)

Counting "MISSING" entries above: **38 requirements** lack direct
test coverage. They cluster into:

- **Title-screen + soft-reset flow** (R1.*, R4.7-9, R17.*) — visual, input-driven; hardest to automate.
- **Audio priority + transitions** (R15.5-8) — needs an audio-state snapshot helper in `sim_helpers`.
- **Planet destruction + all-Mutant mode** (R11.*, R12.6, R12.11) — high-value, fully testable in the simulator.
- **Pod / Swarmer kill semantics** (R7.4-5, R8.2-5) — high-value, lethality + spawn-on-Pod-kill testable today.
- **Bonus-ship init flow** (R14.3) — small, single-scenario test.
- **EEPROM** (R16.*) — requires the simavr EEPROM model; checkable but slower.

Next pass (separate commit) will add tests for those, in priority order:
1. R7.3-5, R8.4-5 (lethality + Pod→Swarmer chain) — gameplay-critical.
2. R11.*, R12.6 / R12.11 (planet destruction) — gameplay-critical.
3. R4.6-8 (death-window input suppression + soft-reset edge) — bug-prone area.
4. R14.3, R17.* (init flow corner cases) — small, defensive.
5. R15.* (audio priority) — needs new helper.
6. R1.*, R16.* (visual + EEPROM) — hardest; last.
