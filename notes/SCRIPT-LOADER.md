# Script package loader: how to substitute our own (larger) modules

Static analysis, 2026-10-20, of `re\bin\SSASR-unpacked.exe` (image base 0x400000) only. Tools: Ghidra 12.1.4 headless on a private copy of the analysed gzf (`re\ghidra-loader\asrloader`, scripts and raw outputs in `re\ghidra-loader\scripts` and `re\ghidra-loader\out`), plus `tools\disasm.py` for every byte quoted. Nothing was run, and no game file was read, so the package layout facts that come from the brief (magic, `32` at +8) are checked only against the code that consumes them.

Tags: **C** = CONFIRMED in the decompile or bytes at the given address. **I** = INFERRED.

## 1. Where and when the package is read and parsed

- **Load call (C).** Game init `0x4971f0` calls `0x4bb6b0("Scripts/FrontEndScripts.sbn", 1)` at `0x497424`. The string at 0x6a8ce0 has exactly one xref. `0x4bb6b0` → `0x499c00` → `0x499910(name, type 4, 0, 0)` queues a resource record (0x28 bytes: +0 type, +4 buffer, +8 size, +0xc name, +0x10 state, +0x14 refcount). `0x499630` then blocks in `0x499000`, which keeps calling the resource manager's pump (vtable `0x6a8ecc` slot 2 = `0x499080`).
- **Type 4 handlers (C).** The pump dispatches through the load table `0x6ec684[type]`. Type 4 is `0x497f00`, which reads the file through the loader's vfuncs +0x14/+0x24 and, on completion, calls `0x4baf90(record->buffer)` at `0x497f2b`. That is a cdecl thunk to `0x54b780` (ECX = `0xce7b98`, the global module tree). The unload table `0x6ec6bc[4]` is `0x497b30`.
- **Parse is in place, with no copy (C, `0x54b780`).** It turns `pkg+8` and `pkg+0x20` from self-relative offsets into absolute pointers (`add [edx],edx` at 0x54b7a0). For each 32-byte entry it does the same for +0x14 (name), +0x18 (back-pointer to the package base) and **+0x1c (module data)** (`8d 7e 1c 01 3f` at 0x54b7b9). It zeroes +0..+0xc and inserts **the entry itself** as a tree node keyed by +0x10 (`0x483ea0`; a duplicate hash is not inserted). It then calls `0x48b210(module)` and adds the count to tree+8. The tree nodes, names and module bytes all live inside the resource's single file buffer.
- **Lifetime (C/I).** The package is loaded once, synchronously, at startup. The only unloads are `0x499780(name)`, whose refcount reaches 0 → `0x497b30` → `0x4bb6e0` (kills script instances whose node+0x18 == pkg, then removes those nodes) → loader vfunc+0x2c(buffer), which frees the one buffer (I), and the resource-manager teardown flush `0x499250`. No exe code releases the FrontEndScripts name (C: single string xref). Script natives that release a resource by a script-supplied name exist (`0x601af0`, `0x606617`, ...), but stock scripts are not known to pass this name (I). It is not reloaded between front end and race (I: no other load site).

## 2. Lookup by hash and binding to a VM

- **Lookup `0x54b750` (C).** thiscall with ECX = `0xce7b98`, arg = hash. It walks the binary tree (node+0x10 = hash, +4 left, +8 right) and returns the **node**, not the module. Name hashing is `0x4947e0(name, len, 0x04c11db7)`, cdecl. Raw call scan (C): `0x4baf6a` (wrapper `0x4baf60`, used by behaviour scripts `0x531bb0`/`0x531be0`), `0x4bb6a0` (by-name helper `0x4bb670`, no callers), `0x4bb6fe` (unload), `0x4bb815`, `0x4bb83b` (`0x4bb830`, which starts a script instance by hash into the 48-slot table `0xce5498`).
- **Binding (C).** Every live bind goes through `0x54b590(node, flag)`. At 0x54b597 it reads **node+0x1c** and calls `0x491e20(module, 0)`. Its callers are `0x4bb898` (script start), `0x4c067e` (SetupObjects walks the raw entry array), and `0x531a97` (behaviour scripts).
- **What `0x491e20` really is (C).** It is not a loader. It is `VM::BindModule(module, unused)`, thiscall, `ret 8`. It frees the VM stacks, releases the old module's globals (`0x491800`), decrements old+0x4c, stores VM+8, increments new+0x4c, and asserts `"smosbin"` (int3 at 0x491ea6) and **+0x38 != 0** (int3 at 0x491eaf). It sets PC = module + [module+0xc] and registers the module's global-variable hashes in tree `0xc28b30` (`0x491630`).
- **+0x38 (C).** Written only by the relocator `0x48b210` (`c7 45 38 01 00 00 00` at 0x48b288). It means "relocated", not "loaded".
- **+0x4c (C).** This is the count of VMs bound to the module. It is incremented at 0x491e8b, and decremented at 0x491e7c and in clear `0x491f60` (0x491fbc).
- **Load-time fix-up (C, `0x48b210`).** The function is cdecl. If +0x38 == 0, it walks the constant table at module+[+0x18]: a count, then (value, type) pairs. For each pair whose type byte has bit 7 set, it clears the bit. Types 5 and 6 then get `value += &value` (slot-relative → absolute, at 0x48b256). Other types call the type object's vtable+0x5c: a no-op for the base type, while arrays (`0x48bc90`) and dicts (`0x48bdd0`) resolve nested slot-relative pointers. Finally it sets +0x38 = 1.
- **Pointers into module memory that outlive a bind (C).**
  - VM+4 is the PC.
  - VM+0x1c is set by opcode 0x33 (module + [module+0x2c][i], in `0x48f990`).
  - Type-5 values are pointers with a **16-bit refcount at ptr-2 inside the module**. Copies increment it; at 0 the pointer is passed to `0x4bd7d0` (free), see `0x492480` and `0x491a30`.
  - Values can sit in the shared global-variable tree and on VM stacks.
- **Module fields used by the VM (C).** Everything else the VM reads is **module-relative**:
  - +0xc: code offset.
  - +0x10: code length, the PC bounds check in `0x48f990`.
  - +0x14: export table, binary-searched by hash in `0x48b360`.
  - +0x18: constants.
  - +0x1c: global hashes.
  - +0x2c: a table read by opcode 0x33.
  - Return addresses are pushed as 32-bit offsets (`0x48c1e0`).
- **Dead code (C: no call/jmp or pointer to them anywhere in the image).** Codex's swap `0x491ed0` and setter `0x48b2c0`.

## 3. Package-buffer assumptions

- **No size field (C).** No size table and no package-relative bounds check were found. The entry has no module size, and the VM never compares against the package. A module is self-contained relative to its own base.
- **Node back-pointer (C).** node+0x18 (the package base) is used only on unload, to match nodes and scripts to a package (`0x4bb6e0`, `0x54b810`). Redirecting +0x1c leaves it intact.
- **Single allocation (I).** The whole file is one allocation, freed as one block on unload. Growing a module in place is therefore impossible: modules are packed next to each other in a buffer sized to the file.
- **Slot-relative constants (C).** Type 5/6/array/dict constants are relative **to the slot address**. A module copied elsewhere stays valid only if every such target lies inside the copy. A stock module that points at shared package data (for example whatever `pkg+0x20` addresses) would break when moved. This was not checked against a real file (I). Your builder must verify it.

## 4. Hook options

**(b) Redirect the entry's data pointer during parse. RECOMMENDED.** Detour the cdecl thunk `0x4baf90` (bytes `8b 44 24 04 50 b9 98 7b ce 00 e8 e1 07 09 00 c3`). Steal the first 5 bytes (`mov eax,[esp+4]; push eax`, no relative operands). The trampoline is those 5 bytes followed by `jmp 0x4baf95`. Before calling the original:

```
if pkg && memcmp(pkg,"smospkg2",8)==0 && *(u32*)(pkg+8)==32:   // still unparsed
  for i < *(i32*)(pkg+12): e = pkg+40+32*i
    name = e+0x14 + *(i32*)(e+0x14)                              // still relative
    if name in {fe_network_game, fe_network_error_handler, ...} (also check hash e+0x10):
      old = e+0x1c + *(i32*)(e+0x1c); require memcmp(old,"smosbin",7)==0
      *(u32*)(e+0x1c) = (u32)ours - (u32)(e+0x1c)               // 32-bit wrap; any address works
call original
```

The game then absolutises the field (`add [edi],edi` makes it `ours` exactly, with mod-2^32 arithmetic), relocates **our** module with its own type handlers (`0x48b210`), and sets +0x38. Both aliases can point at one buffer, because a second `0x48b210` call is a no-op once +0x38 == 1. Afterwards, check that `ours+0x38 == 1` and that `0x54b750(hash)->+0x1c == ours`, and log the result.

- **Single choke point instead (C).** `0x54b780` covers all three parse callers (`0x497f2b`, `0x54aa65`, and native `0x5f2330` → `0x4bafa0`). Its first 8 bytes are `83 ec 08 55 8b 6c 24 10`, it is thiscall with `ret 4`, and resume is at 0x54b788. FrontEndScripts always arrives via `0x4baf90`, so the thunk is sufficient.
- **Post-parse variant.** After the original returns: look up the node, check node+0x18 == pkg and old+0x4c == 0, call `0x48b210(ours)`, then set node+0x1c = ours. This is equally valid but calls one more game function.
- **Risks.** If the layout check fails, skip and log, and the game stays stock. Your module **replaces whatever the Roster Pack's sbn contains** for those names. Codex notes that Metal Sonic edits the network module at runtime. How it finds and patches the module is unknown here. If it uses node+0x1c or byte patterns, it could now hit our buffer or silently lose its edit. Check this before shipping.

**(a) Hook lookup `0x54b750`.** This returns a node, so you would have to fabricate a 32-byte node (hash, name, the real pkg in +0x18, ours in +0x1c). The stolen bytes `8b 01 85 c0 74 23` contain a `je rel8`, so the trampoline needs fix-ups. It runs on every start and lookup, and the unload path compares node fields. It gains nothing over (b). Not recommended.

**(c) Hook `0x491e20`.** Bytes `56 8b f1 0f b7 46 16 | 57 50 e8 52 06 00 00`. The 7-byte steal ends on an instruction boundary; resume is at 0x491e27. Substitute the arg when it equals the stock module pointer.
- Our module must already be relocated, otherwise the int3 at 0x491eaf fires. That means calling `0x48b210` ourselves after the CRT has registered the types.
- It sits on the hot path of every bind.
- `0x531ae0` compares node+0x1c with VM+8, so it would mismatch for any behaviour script swapped this way.
- Codex's mods hooked this same site, so there is a collision risk.

It is the only option that can also redirect already-running binds, which we do not need. Not recommended.

Not usable: in-place growth, for the reasons in section 3. Also not usable for existing names is the loose-file fallback `0x4bb790`. It loads `Scripts\<name>.sbn` only when a hash is **absent** from the tree, and only for files that `0x4bb480` enumerated with `FindFirstFileA("Scripts\*")` at startup.

**Requirements for our buffer (C unless marked).**
- Valid smosbin, with `"smosbin"` at +0.
- +0x38 = 0 and +0x4c = 0 on entry.
- Constant-table types have bit 7 set exactly as the stock compiler emits them.
- Exports sorted by hash, for the binary search.
- Every slot-relative target lies inside the buffer.
- String and array refcount headers are at least 1, as in stock output (I).
- **Writable:** the relocator, the refcounts and +0x4c all write to it.
- **Never freed or moved for the life of the process**, not even on `SsasrModRollback`: VMs, globals and string values keep absolute pointers into it.
- 16-byte aligned (I: x86 tolerates less, but this is free with VirtualAlloc/HeapAlloc).
- Load it from the mod folder during DLL init.
- Size: code jumps and return offsets are 32-bit, and the constant index is u8/u16, so 35-50 KB modules fit (I).

## 5. Thread and timing

All of this runs on the main thread (C, static call graph): `0x4012d0` (WinMain-like) → `0x497450` (run: init, loop, shutdown `0x4966e0`) → `0x4971f0` → load at 0x497424 → `0x499000` wait loop → pump `0x499080` → `0x497f00` → `0x4baf90` → `0x54b780`. The file read may be asynchronous (I), but the parse runs inside the pump.

Our DLL starts before the CRT, so before the CRT static initializers that register script types (`0x48c970`/`0x48cd10` via 0x676d35/0x676d55) and before WinMain. **The package cannot exist when we install the hook. The hook fires later, during game init.**
- At DllMain, only write the detour and read our files. Do not call game script functions there.
- The first script bind happens right after the parse (`ssr_wriststrap`, 0x497432). Front-end modules are bound much later.

## Address and byte table

| VA | What | Bytes (verified in unpacked exe) | Tag |
|---|---|---|---|
| 0x497424 | FrontEndScripts load call (push 0x6a8ce0 at 0x497413) | `e8 87 42 02 00` | C |
| 0x4bb6b0 | load resource type 4 (+ wait) | — | C |
| 0x497f00 | type-4 load handler (table 0x6ec694) | `56 8b 74 24 08 8b 46 1c` | C |
| 0x497f2b | call to parse thunk | `e8 60 30 02 00` | C |
| **0x4baf90** | **parse thunk, cdecl (recommended hook)** | `8b 44 24 04 50 b9 98 7b ce 00 e8 e1 07 09 00 c3` | C |
| 0x54b780 | ParsePackage, thiscall ECX=0xce7b98, ret 4 | `83 ec 08 55 8b 6c 24 10 57 8b f9` | C |
| 0x54b7b9 | entry+0x1c rel→abs | `8d 7e 1c 01 3f` | C |
| 0x54b7dc | call relocator per entry | `e8 2f fa f3 ff` | C |
| 0x48b210 | relocate module, cdecl | `55 8b 6c 24 08 6a 07 68 b0 81 6a 00` | C |
| 0x48b288 | +0x38 = 1 | `c7 45 38 01 00 00 00` | C |
| 0x54b750 | tree lookup by hash, ret node | `8b 01 85 c0 74 23 8b 54 24 04` | C |
| 0x54b590 | bind node (reads node+0x1c at 0x54b597) | `8b 44 24 04 56 8b f1 8b 48 1c 6a 00 51` | C |
| 0x491e20 | VM::BindModule(module, 0), ret 8 | `56 8b f1 0f b7 46 16 57 50 e8 52 06 00 00` | C |
| 0x491e8b / 0x491e7c | +0x4c inc / dec | `83 40 4c 01` / `83 40 4c ff` | C |
| 0x491eaa | assert +0x38 != 0 | `39 78 38 75 01 cc` | C |
| 0x491f60 | VM clear (dec +0x4c at 0x491fbc) | `56 8b f1 0f b7 46 16 57` | C |
| 0x497b30 | type-4 unload handler (table 0x6ec6cc) | `8b 44 24 04 56 57 8b 78 04` | C |
| 0x4947e0 | name hash, cdecl (name, len, 0x04c11db7) | `83 ec 08 8b 4c 24 14 53` | C |
| 0xce7b98 | module tree: +0 root, +8 node count | data | C |
| 0x491ed0, 0x48b2c0 | swap / setter: no references | — | C |
