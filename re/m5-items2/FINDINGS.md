# Items: event layouts and what injected events do (M5, 2026-10-21)

Two read-only reverse-engineer passes on `re\bin\SSASR-unpacked.exe` (scratch `a\` = layouts and ids,
`b\` = effects on puppets). Their reports, condensed by the main session (the agents could not write
files). C = confirmed in bytes/decompile, I = inferred. Main session re-checked: post 0x5c0820 (window
byte `[mgr+0x12] < 0x10`), factory 0x516760 (stdcall(kind), ret 4, null above 0x11), free 0x516900
(stdcall(ev) -> deleting destructor), box hash read `[[msg+0xc]+4]` at 0x4f4f34, 0x4c0fc0 = cdecl(type) ->
0x4c0e90(4, type), a linked-list search (safe for any value), the item table 0x6f0b70 (rows {key, gadget,
count}), the All-Star timer += [0xc2a030] (sim dt, seconds).

## Indices and ids (C)
- Event racer byte +8 = scene slot = 0905 call order = racer+0x9d8 = lap record index (0x49fa20..48).
  `cmdline_no_ai_racers` would shift slots (I). Racer ids +0x2b4 = (0x7ff<<5)|i.
- 0x49cb50(idx) (thiscall scene, ret 4): idx < 8 returns the slot with no count check (all 8 objects
  exist). Check `idx < [scene+0x1100]` yourself.
- 0x49d2e0(u16* id) (ret 4): owner = id & 0x1f; owner 0x1f searches pools; owner 8..0x1e can null-deref
  at 0x49d344. Not found -> 0.
- Projectile ids: FireGadget sets every spawned projectile's id from the serial in the event's launch
  frame (+4) and the racer slot (14 sites in 0x539df0): a mirrored fire makes the same ids on every PC.
  Spawns outside events (sub-projectiles via 0x51ef10(...,0), All-Star move objects, 0x4b7d80) use each
  PC's own counter [0xcdd048]: they can alias mirrored ids (I).

## Layouts (C)
Every event: +0 vtable, +4 kind (set by the factory: never copy).
- Kind 0 (0x14, only producer 0x539c10): +8 slot, +0xc gadget, +0x10 ammo adjust (0 full, -1 infinite).
  Executor: AddGadget 0x539680(+0xc, +0x10). No pointers.
- Kinds 1/2 (0xe0, ctor 0x5b7270; producer 0x538bf0, post 0x5390dd; kind 2 = aimed): +8 slot, +0xc gadget,
  +0x10 u16 parent id, +0x12/+0x13 FireGadget args, +0x14..+0x1f unused. Launch frame +0x20 (0x80 bytes):
  +0 projectile type, +4 u16 serial (the id), +8 target slot (bounds-checked), +0xc..+0x10 flags, +0x20 pos,
  +0x30 quat, +0x40 dir, +0x50 vel, +0x60/+0x70 vec4. Packed fields +0xa0..+0xdf with quantizer
  sub-objects {vtable, float} at +0xa8, +0xb8, +0xc4, +0xd0 (vtables only, no other pointers). Unpack
  0x5b7530 rewrites +0xdc in place, and FireGadget rewrites the frame: copy BEFORE the original post.
  No serializer exists (the console's wire form is gone).
- Kinds 3/4: never posted on PC (0x51ef10's only caller passes 0). Kind 7, 0xc, 0x10, 0x11: never posted.
  Kind 0xf: posted by respot, never runs on PC (needs 0x4e8910() == 0): never inject.
- Kind 5 (0x14, producer 0x4b6fd0): +8/+0xa u16 ids, +0xc/+0xd shape indexes, +0xe..+0x10 victim bytes
  (never read on PC), +0x11 random byte. Executor 0x49d400 null-checks ids and shapes.
- Kind 6 (0xc): +8 latching object id, +0xa racer id. Executor 0x49d380 and delayed 0x49d3c0: NO null check.
- Kinds 10/11 (0x10): +8 projectile id, +0xa target id. Executor null-checks the projectile.

## Who decides (the console's 0x4e8910 gates, C)
Kinds 0/1/2: the car's PC. Kind 5: the victim (racer vs projectile), else the lower owner index
(0x51cb30). Kind 6: the latched car. Kinds 10/11: the projectile owner.

## Re-entrancy (C unless noted)
- AddGadget: 0xf005c29d and 0xc01ccf24 fire at once (0x538bf0 -> kind 1); clear 0x537880 first.
- FireGadget: posts MSG 0x29 (message, not event); ammo 0 + stash +0xc18 -> 0x539d30 -> kind 0;
  0x9a67cb42 hands out 0xd045a8ca directly (no event); it does NOT need the held item.
- Kind 5 executor -> projectile/racer script handlers (I: may reach kinds 6/10/11, kind-0 stash restore).
- A nested post takes the next window slot (count incremented before the handler runs, 0x5c0862).

## Effects on a Network21 puppet (C)
- AddGadget refuses while +0x1d0c bit 0 (grabbed), during an All-Star, or while holding another item
  (+0xc1c != new, +0xc50 == 0). HUD calls are local-player only and bounds-checked. No crash path found
  for a car without a local player.
- Boosts only set physics fields; no item, All-Star or hit code pushes controller states. Network21's
  handler 0x5afd50 ignores autopilot/erratic/hit/gadget messages: items never take a puppet out of 0x15.
- Projectiles spawn from the event's frame. Exceptions read live local state: projectile +0x398 =
  clamp([[racer+0x9f0]+0xb8]) for 0x53f395, 0x7711877d, 0x11a9bc48, 0x9a67cb42.
- All-Star (racer+0x1ff0 = S): fire 0xb678e022 -> 0x4e8530 -> 0x585b10 sets S=2. Per tick 0x586980:
  2 -> 0x586160 (clears S+0x4b; 3 if transform time > 0, else length via 0x585de0), 3 transform,
  4 active: ends while timer S+0x20 >= length S+0x30 or byte S+0x4b != 0 (0x58680e), 5 detransform.
  Length (seconds) from race position (each PC's own view), computed only if all 4 lanes of S+0x30 are 0.
  End 0x586240 is valid only in 3/4: never call it from outside; set S+0x4b = 1 instead (not in phase 2).
  Autopilot messages are posted only for category-1 cars. All-Star hits act directly on every racer of
  the PC that runs it (no events): each PC applies them to its own cars.
- Injected kind 5 on a puppet: animations and 3.5 s invulnerability apply; position/velocity are
  overwritten by our poses (I).

## Hits and drops (C)
- Collision pass: a new pair in the 20-entry table posts one kind 5 (and sets projectile +0x5a4 bit 30);
  no re-post while the shapes overlap. A DROPPED kind 5 leaves bit 30 set: the pool allocator skips that
  projectile for the rest of the race (leak). Fix if ever dropped: clear bit 30 on resolved projectiles
  (+0x2b0 == 1).
- Kinds 10/11 dropped: re-posted every tick until the owner's arrives (allocations only).

## Event manager timing (C)
Frame 0x4967c0 (skipped while paused): messages 0x4d2a80 -> race phases 0x4d4270 (our Racing/WFF hooks)
-> scene update 0x49dca0 (racer slot 4 = gadget update + All-Star tick, projectiles, event-manager update
when 0x4cc120 && 0x4cc410 != 0x31 && 0x4cc130 != 5 && scene+0x1117 == 0) -> physics + collision pass.
Every accepted post increments [mgr+0x12]; at 15 manager updates the delayed pass runs and the count
resets. Injecting from the Racing hook before the stock update is safe; leave headroom.

## Item boxes (C)
Box message 0x06cd52a4 (racer slot 8, 0x4f4f20) returns 1 on every path; box hide/respawn is script
logic. 62 other message hashes go through that slot (hits, boost pads, respot, trophies): pass them.

## Credit (C)
Trophies and profile stats are gated to the category-1 racer whose pad matches the profile: a puppet is
never credited; the local player is credited only for its own actions. The game has no Steam
achievements (no Steamworks imports).
