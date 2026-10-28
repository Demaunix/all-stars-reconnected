# Esc on the modifications question (reverse-engineer pass, 2026-10-24, session "M9 · Running on Fumes")

Saved from the main session (subagents can't write report files). The user's request: no CANCEL button on "Would you like to
enable your modifications?"; Esc closes it (no lobby, search again). Scratch disassemblies (not in the repo): the session
scratchpad `esc\` (shipped_dialogs.txt, ps3/x360/mac_dialogs.txt, fng.txt); Ghidra copy `re\m9-esc\ghidra`.

## Why Esc never closes a PC dialog (CONFIRMED)
- Each pad keeps three button layers at pad+0xd4 / +0x102 / +0x130 (per button a byte: bit 0 held, bit 4 pressed this frame,
  bit 7 release-first). Only the active layer (`[0x6eca88]`) gets live input (`0x4aeac0` -> `0x4ae190`).
- Menus run in layer 0 (constructor `0x4a94e0` sets bit 0 of ctrl+0x971). The dialog's show handler calls `043f(1,0)` (shipped
  dialogs +0x261): layer 1 while the dialog is up. BACK_KEY `041a` (0x600d80) reads the primary pad's +0xf8 bit 4 (layer 0,
  button 0x24): nothing while a dialog is up.
- The dialog reads only 043c (layer 1 +0x125, Enter), 043d, 043e. No native handler reads layer 1's back byte (+0x126).
- PS3, 360, Mac, stock PC and Roster Pack `dialogs` all do the same: no console "back cancels" path. Esc closing this question
  is an ADDITION the user asked for.

## The route taken (b): a read-only native + the search frame's busy branch
- `kNatDialogBack` (0x7a2b): primary pad = `[ctrl+0xc + 4*[ctrl+0x960]]` (count `[ctrl+4]`), ctrl = `[[0xc2a1b4]+0x14]`; returns
  1 when `pad[0x126] & 0x10` and not `pad[0x125] & 0x10`, and the Steam overlay isn't open.
- steam.py search_active, while a dialog is up and MODS_STATE == MODS_ASKED: once the dialog scene bf85b602's opening is done
  (0213, as its own Enter waits), on the native: FE_BACK, then post the dialogs module's close `160c(DIALOGS, 3cb781ef, 0x31)`
  (byte for byte as front_main +0x707 / fe_startscreen +0x69d); nothing else that frame.
- Next frame the close handler (shipped +0x4f2..0x551, stock +0x4d6) calls the callback with -1 (callback_entry: MODS_STATE 9),
  destroys scene 424b961f, sets g2 = 0 and calls `043f(0,0)` (layer 0); the switch runs `0x4ae2e0(1)` on every pad, marking held
  keys release-first, so this Esc can't also fire the search screen's BACK. A double post is harmless (g2 = 0 skips the callback).
- Rejected: (a) patching the dialogs module (needs a native anyway; its only per-dialog field P[4] is the "any pad" mode; growing a
  module with typed constants is unsupported); (c) C++ only (a 043c hook at dialogs PC 0x6d3 / stock 0x6b7 plus a C++ post): more code.

## Not confirmed
- INFERRED: a pad's back is button 0x24 (as BACK_KEY); nothing rewrites ctrl+0x958 at run time; a foreign dialog shown while our
  question is posted but not shown would get the stock "replaced" -1 answer (its owner handles it).
- UNKNOWN: whether the Steam overlay passes Esc to the game (the overlay guard covers it); the in-game feel.
