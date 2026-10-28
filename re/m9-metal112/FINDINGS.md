# The standalone Metal Sonic 1.1.2 online (reverse-engineer pass, 2026-10-24, session "M9 · Metal Times Two")

Saved from the main session (subagents can't write report files). The user's request: our online features with the public
standalone Metal Sonic 1.1.2 (its public release zip); Beta 3 dropped (the
user). The Roster Pack's Metal Sonic (1.1.0-beta.3.rp2) already works online. Scratch (not in the repo): the session scratchpad
`metal112\` (scripts, `out\cmp.txt`, `out\metal112-patches.txt`, `out\metalRP-patches.txt`, extracted modules `data\mods\`).

## Finding
Before 1.1.0-mods.7 our online features switched themselves off with 1.1.2: its `fe_network_game` (and alias
`fe_network_error_handler`) is stock + one Metal row, and the substitution is all-or-nothing. Everything else already fits.

| # | Item | 1.1.2 | Our status | Label |
|---|---|---|---|---|
| 1 | Script delivery | whole-file `FrontEndScripts.sbn` 96319de4… (stock c1f5ac48…), SetupObjects 70df115b…, Behaviours aecf21e0… | in-memory substitution works on any parsed package | CONFIRMED |
| 2 | `fe_network_game` + alias | 04ad1a0c… (8725 B) = stock + the Pack's Metal row at 0xbcb, count 0x2a -> 0x2c | mods.7: accepted (`kModNetMetal`, `fe_network_game_metal`, build_menus asserts the bytes) | CONFIRMED |
| 3 | `fe_mainmenu`, `dialogs`, `results` | same as stock | accepted | CONFIRMED |
| 4 | `fe_game_bridge` a147849e… | inserts at 0x275 (+17) and 0x7da (+22); later code +0x27. Difficulty `2003(3)` PC 0x20af, racer count `2003(1)` 0x2128, tips `1d02` 0xa0d | mods.7: the three sites (`kBridge*PcMetal`, `kTipsPcMetal`; bytes checked against the module) | CONFIRMED |
| 5 | Sites in pause_menu, hud, front_main, game, results; hooks 0905/0900/090a/0912/0913/0916/1b01/1304 | modules byte-identical to stock | unchanged | CONFIRMED |
| 6 | fe_charselscreen, fe_trackselect, fe_leaderboards, fe_license_records | Metal entries added | no sites of ours | CONFIRMED |
| 7 | Racer list | 22 = stock 21 + `mechasonic` (0x9cabac57) last (charselscreen 0x25c5: stock order then 21) | mods.7: `kRacersMetal[22]`, `RacerListKind`; RacerOn / AI pool test the name | CONFIRMED |
| 8 | Our 26 optional variant members (navigator 7, lobby 7, dialogs text 5, Settings popup 7) | each `over_payload_sha256` = 1.1.2's payload (26/26); `reference\navigator-bases\metal-sonic-1.1.0-beta.3` equals 1.1.2's payloads (old folder name) | key pictures, icons, yellow bar, UNMUTE apply | CONFIRMED offline; loader not run |
| 9 | Egg Hangar 1.2.0-sc.1 | its 22 shared members have variants for 1.1.2 | three-mod combination should start | CONFIRMED offline |
| 10 | Roster Pack | both replace `FrontEndScripts.sbn` and Base AA924689 / FECF5CFC, no variants | can't be installed together | CONFIRMED |
| 11 | 1.1.2 vs the Pack Metal | all other groups identical | 1.1.2 = Pack Metal + Metal-only versions of the Pack's 10 groups | CONFIRMED |
| 12 | Metal off in a lobby | RacerParams +MechaSonic rows; Shopping +Item93; SetupObjects/Behaviours additions only | additive | CONFIRMED |
| 13 | Code patches | 30 (0x4ffea0, 0x54c577/5b4/59f, 0x54d56a/58f/5a9, 0x5fe7c3, 0x54765a, 0x585fe2, 0x5863ea, 0x4f4f20, 0x59e6b0, 0x59e9d0, 0x59f1c0, 0x535710, 0x4e7831, 0x624338, 0x61026c, data 0x6e9070-87, 0x5080b0, 0x508110, 0x5081a0, 0x5081c0, 0x5a8616); identical to the installed Pack Metal | none touches a slot we hook; our slot-8 hook calls 0x4f4f20 (chains into Metal's), `kIconHash` 0x4ffea0 returns Metal's icon | CONFIRMED |
| 14 | Metal's integrity check | SHA-256 of 0x400000+0x400, .text, 0x67d000+0x6b200 (holds 0x6b467c and our vtable slots), 0x6f0c70+0x298, 0x6f1100+0x4038, only at its start-up | ChaoGarage 1.2.0 starts `metal-sonic` first (host.cpp v1.2.0 144/170): runs before our patches, as with the Pack | CONFIRMED in source; not tried in game |
| 15 | Metal's All-Star online | class in `MetalMaximumOverdrive.dll` (0x585fe2/0x5863ea), not one of our 14 mirrored classes | copies' aim / fire vs the owner: UNKNOWN (same for the Pack Metal) | UNKNOWN |
| 16 | Mod check / mixed lobby | listed `metal-sonic|1.1.2|27aa1134…|db9ce65b…` | Pack Metal vs 1.1.2: refused (the dash + other mods); lobby racer 21 would be a different racer vs Metal anyway | CONFIRMED (code) |
| 17 | Metal-off lobby, 1.1.2 host + stock guest | allowed (Metal toggleable); racers 0-20 the same; the AI pool skips Metal | should work | INFERRED |
| 18 | Commentary, results, HUD scenes | identical to the Pack Metal | as today | INFERRED |
| 19 | Save / profile | patches 0x54765a, 0x5fe7c3 (profile code); our rules record 0xc390e0 untouched | back the save up | UNKNOWN |
| 20 | Metal off: its patches stay active | hash checks 0x535710 / 0x5a8616, controller setters 0x5080b0..0x5081c0 | host runs AI, owners run items: no desync expected | UNKNOWN |

## Build plan
1. Third lobby variant (done, mods.7). 2. The three bridge sites (done, mods.7). 3. `tests\loader_combo.py` case with 1.1.2
(no `allstars-mod.json`) + ours + Egg Hangar: 26 variants used, no message. 4. A reverse-engineer pass on Metal's All-Star copies
(core b5c6062d…), mirroring only if copies differ (also covers the Pack Metal). 5. Optional: the 30 patches with Metal off.
6. README / GameBanana: "Metal Sonic 1.1.2 supported".

## In-game tests (each install needs the user's OK)
Setup: back up `ssr_save.bin`, `settings.ini`, the mods listing in each game; add an entry to the deploy log (the
game folder is shared with other projects). Move the Pack family aside (All-Stars Roster Pack, Metal Sonic (Roster Pack) and its other racers;
keep Egg Hangar and ours; copy `mods\Metal Sonic` from the zip. T1 (one game, `[Debug] TestPlayers`): "Metal
Sonic runtime ready (1.1.2; original stats)", the menus-on line naming the Metal variant, no loader message; pick Metal; an AI OFF
race; the difficulty log line. T2 (both games alike): a Metal-on race with Metal's All-Star; then Metal parked in one game for a
Metal-off join. Restore both games and verify the hashes.
