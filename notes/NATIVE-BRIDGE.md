# Script native bridge: Roster Pack collision, call ABI, text buffers

Static analysis, 2026-10-20. Sources: `re\bin\SSASR-unpacked.exe` (base 0x400000), read with `tools\disasm.py` and with Ghidra 12.1.4 headless (a read-only `Decomp.java` run on `re\ghidra-loader\asrloader`), plus capstone on private copies of the installed runtime DLLs. Raw outputs stayed in the session scratchpad. Nothing was run. **C** = CONFIRMED in bytes or decompile. **I** = INFERRED. DLL addresses are at their preferred base, 0x10000000.

## 1. All-Stars Roster Pack vs 0x54bb00 / 0x6f69f0

- **What it does (C).** `AllStarsRosterPack.dll` (SHA-256 `27781fb7…ae1f`) does not detour the dispatcher, does not call it, and adds no group. Both dwords belong to a **build fingerprint**. Function `0x10002a30` compares 12 game ranges against expected bytes using `0x10002620(gameVA, expected, len)`:
  - `0x4d4c14`/0x2c, `0x4ba870`/0x2e, `0x5b09a0`/0x18, `0x4d3d10`/0xd, `0x4d3ba0`/0xb2, `0x4d4ea0`/0x49
  - **`0x54bb00`/0x26**: the whole dispatcher. The push is at `0x10002ae2` (`68 00 bb 54 00`). The expected bytes are at `0x10010c5c`, and the `0x6f69f0` hit at `0x10010c66` falls inside that blob (`8b 14 d5 f0 69 6f 00`).
  - **`0x4900bd`/0x108**: the VM's native-call opcode.
  - `0x5fe690`/0x10.
  - **`0x6f6a88`/8**: the group 0x13 cell.
  - `0x6f6620`/4: the group 0x13 count, 0x36.
  - `0x6f661c`/4.

  All 12 ranges match the stock exe.
- **What it replaces (C).** One native: **0x1335**. Slot `0x6f661c` holds stock `0x5fe690`, which is a bare `ret`. The pack replaces it with `0x10002b80` (a racer's startup native: int argument 0/1/0x10..0x13, int return). The patch record is prepared at `0x100038ea` (`68 1c 66 6f 00`) and committed by `0x100031d0`. `SsasrModRollback` (`0x10003ab0` → `0x10002cf0`) undoes it. Its other patches are 5-byte call/jmp writes at `0x5fe7be`, `0x547661` and `0x4d4c35`.
- **When (C).** Only inside `SsasrModInitialize` (`0x10003290`). The fingerprint is checked at `0x10003646` (before the patches are prepared) and at `0x10003916` (just before commit). It is not run from DllMain, the DLL imports no CreateThread, and nothing checks again later. On a mismatch it logs "…native 0x1335 differs from the expected game layout…; initialization refused." and returns `0x80004005`.
- **Load order today (C, observed).** In `mods\SsasrMods.log` (15:32:13), the pack's final init line comes before "Steam Multiplayer: background running", so our init runs after both of its checks. How ChaoGarage orders inits in general is I.
- **Consequences.**
  - Any code byte we write in `0x54bb00..0x54bb25` or `0x4900bd..0x4901c4` is safe only if it is written after the pack's init. If ours ever ran first, the pack refuses, and per MODS-OVERVIEW the whole set fails.
  - Never touch group 0x13 (its cell, count or array).
  - No other installed runtime references `0x54bb00`, `0x6b467c`, the cell table, any group array or count, or the dispatcher byte signature. Checked: Egg Hangar, the 4 Metal DLLs, RosterLayer (one shared file), RosterLayerMetal and the other installed racer runtimes (C, byte scan; the only hits were string bytes).
  - Codex's unshipped `frontend-bridge-abi` pinned `0x6b467c` and cell 0x1b (`0x6f6ac8`). Never load Codex's MP mods together with ours (I).
- **A site nobody fingerprints (C).** The dispatcher is reached only through slot 3 of the script-VM vtable `0x6b4670`, at **`0x6b467c`** (`00 bb 54 00`, the same bytes on disk).
  - That slot is the image's only reference to `0x54bb00`. There are no direct call/jmp references.
  - The interpreter calls `[[vm]+0xc]` with ECX = VM (`0x49014b`, `0x490172`).
  - The vtable is installed only by the VM constructor `0x54bac0`, whose callers are `0x531c15`, `0x677103` and `0x677195`.

## 2. Descriptor, VM fields, return value (C unless marked)

The descriptor is built on the interpreter's stack at `0x490126`–`0x490158` for opcodes 0x12 and 0x32 (handler `0x4900bd`; 0x32 also carries two s16 debug words). Codex's layout is confirmed:

| Off | Field |
|---|---|
| +0x00 | native id: u16 from the bytecode, zero-extended. Group = bits 8..15. |
| +0x04 | byte: 1 = normal call; 0 = VM+0x24 was 2 at the opcode (re-entry after a yield; the arg base is then popped via `0x48d150`) |
| +0x08 | `u8*` into the VM type stack at the first argument (VM+0x10 + base) |
| +0x0C | `u32*` into the VM value stack at the first argument (VM+0xc + base*4) |
| +0x10 | argc (u8 from the bytecode) |
| +0x14 / +0x18 | return value / return type, both zeroed before the call |

- **Arguments.** The arg pointers point into the live VM stacks; they are not copies. A handler must not push onto the VM, because `0x48b5c0` can reallocate the stacks (I).
- **Calling convention.** The dispatcher is thiscall (ECX = VM, id, desc) with `ret 8`. A handler is `cdecl(vm, desc)`, and its EAX is ignored.
- **Before and after the call.** The interpreter sets VM+0x24 = 4 before the call (`0x490167`). Afterwards:
  - **4:** pop argc values (`0x492480`), push +0x14/+0x18 (`0x48c330`), then release the descriptor's return value (`0x4928d0`).
  - **2 or 3:** leave the interpreter (`0x490a13`).
  - **Anything else:** `int3` at `0x49018a`.

  Never write VM+0x24.
- **VM fields.**
  - +0: vtable.
  - +4: PC.
  - +8: module.
  - +0xc: value stack (`u32*`).
  - +0x10: type stack (`u8*`).
  - **+0x14: u16 capacity. +0x16: u16 count/top.** Codex's single "+0x14 count/capacity" field is really these two u16s.
  - +0x18: u16, zeroed by the constructor `0x48b1e0`; purpose unknown.
  - +0x24: state.
- **Types** (from the push helpers):
  - 0: none.
  - 1: float (`0x48c1a0`).
  - 2: int (`0x48c1e0`).
  - 3: hash (`0x48c220`).
  - 5: heap string. A `char*` with a u16 refcount at ptr-2 (`0x48c2a0`, `0x48c7f0`).
  - 6: literal string. A `char*` with no refcount (`0x48c260`).
  - 7: raw object pointer.
  - 8: sarray.
  - 9: the second built-in object type.
  - Types 0..7 have no type object. The `0xc28920[0..7]` entries are never written: registration starts at count 0x10 (`0x6eb95c` = 0x10 on disk). Type objects 8 and 9 are stored directly into their own entries. So type 7 has no addref or release (I, strong).
- **`0x48c520` (int return setter).** thiscall: ECX = value slot (desc+0x14), one stack int, `ret 4`.
  - It first releases the old slot content. Type 5: refcount−1, freed at 0 via `0x4bd7d0`. A type with a type object: that object's vtbl+0x10. A type < 0 or ≥ count: `int3`.
  - It then stores {value, 2}.
  - Real use: `0x5fd3af` (`lea ecx,[esi+0x14]`).
  - Because the slot is {0,0} on entry, writing `+0x14 = v; +0x18 = 2` directly is equivalent. The Roster Pack's handler does exactly this, guarded by `if (+0x18 == 0)`.
- **Returning "none".** Set nothing: the VM pushes {0, type 0}, and int conversion (`0x48d150`) turns types 0 and 7 into 0. An explicit 0 is {0,2}. A negative return type or one ≥ 0x22 hits `int3` in `0x48c330`.

## 3. Strings and text buffers

- **(a) String arguments (C).**
  - Opcode 0x04 (`0x48fc46` → `0x48c260`) pushes the NUL-terminated bytes inline after the opcode as **type 6, dword = `char*` into module code**.
  - Opcode 0x05 does the same with an s16 PC-relative pointer.
  - Constants (opcodes 0x09/0x58) copy the raw (value, type) pair from the constant table, so they arrive as type 5 or 6. Variables hold 5 or 6.

  In every case the argument dword is a `const char*` to UTF-8 text. Accept types 5 and 6. Never write to it or free it.
- **(b) Text buffer object (C).**
  - **Layout.** Native 0x1610 (`0x5fdc50`) allocates `new(0x10)`:
    - +0: vtable `0x6a71f8`.
    - +4: 0.
    - **+8: capacity in bytes, including the NUL** (the script's int arg0).
    - **+0xc: `char*`** allocated with `new[](capacity)` (`0x4044a0`); `buf[0]` = 0.

    It returns {obj, 7} via `0x48c440`.
  - **Deletion.** 0x1611 (`0x5fd1e0`) deletes it: vtbl[1](1) = `0x485220` frees the buffer and the object.
  - **0x1612 and 0x1613.** `0x5fd200` and `0x5fd250` are byte-identical: `sprintf(obj->buf, "%s", arg1)` through the CRT `sprintf` at `0x6478b4` (cdecl, varargs). They do **no length check**, so do not call them; copy in our own code.
- **Encoding (C).** `char`, UTF-8. Each draw converts the text with `0x482c90` (UTF-8 → UTF-16). That decoder handles only 1–3-byte sequences and validates nothing: a 4-byte sequence or a truncated tail is mis-decoded and can read past the NUL. Whether the fonts have glyphs beyond Latin is unknown (I).
- **Maximum length (C).** The capacity is obj+8. The draw path copies through vtbl[0] = `0x485200` (`sprintf(dst,"%s",buf)`) into a **4096-byte stack buffer** (`0x47ef0c`–`0x47ef15`), so keep every string under 4096 bytes. A Steam name fits in 128 bytes.
- **0x021e is a live binding.** 0x021e = `0x4a6f40` → `0x47f370(hash, obj)`. It looks up the TEXT element (`0x483860`, tag 'TEXT'). `0x4853c0` then stores obj at element+0x108 **without copying or owning it**, and frees the element's previously owned source first.
  - The TEXT draw method `0x485520` (vtable `0x6a7200`, slot at `0x6a7228`) calls `0x47ece0` at `0x4855a0` with element+0x108 on **every draw**. `0x47ece0` calls source vtbl[0] at `0x47ef15`.
  - So **rewriting obj->buf changes the text on the next frame the element is drawn, with no re-bind** (C; that the element draws every frame while visible is I).
  - By contrast, 0x0248 (`0x4a6f60` → `0x485410`) stores an owned snapshot copy and needs a re-bind for each update.
  - Freeing a buffer with 0x1611 while 0x021e still binds it leaves the element dangling. Unbind first with `0x021e(hash, 0)`: `0x4853c0` stores the 0, so the element falls back to its static text (I).
- **Safe writer native (proposal).**
  - Require: argc ≥ 1; `types[0] == 7`; `obj = values[0]` non-null; `*(u32*)obj == 0x6a71f8`; `cap = *(i32*)(obj+8)` within 1..4096; `buf = *(char**)(obj+0xc)` non-null.
  - Copy our UTF-8 string one whole code point at a time while `used + len + 1 <= cap`. Replace invalid sequences and anything above U+FFFF with `?`. Always NUL-terminate.
  - Return {1,2} on success and {0,2} otherwise.
  - Run only inside the native call. Never keep obj between calls; the script owns it.

## 4. Thread

- **Main-thread paths (C).**
  - `0x497450` loop → `0x4967c0` (`0x497492`) → `0x4bb0a0` (`0x49685d`) → `0x54b630` (`0x4bb0c0`) → `0x54bbd0` (`0x54b661`) → `0x490d50` → `jmp 0x48f990`.
  - Also `0x497450` → `0x496240`/`0x496da0` (`0x4974a2`/`0x4974a9`) → `0x4bb9b0`/`0x4bba90` → `0x54bb30` → `0x490ee0` → `0x48f990`.

  Natives run synchronously inside the interpreter `0x48f990`.
- **Other threads (I, not exhaustive).** The game's other threads are middleware: CreateThread at `0x40ee5e` (CRI file reader), and `_beginthreadex` called from `0x40f793`, `0x40fe19`, `0x45390f`, `0x467717` and `0x661a51`. No path from them to `0x48f990` was found.
- **Cheap guard.** Record the thread id at the first `0x4baf90` call (main thread; SCRIPT-LOADER §5). Make our natives refuse, and log once, on any other thread.

## 5. Design

**Recommended: replace the vtable slot `0x6b467c`. No code bytes, no stolen instructions, no asm.** MSVC `__fastcall` with two stack args uses ECX and EDX and the callee pops 8 bytes. That is the same frame as the stock thiscall `ret 8`. EDX is scratch for `0x54bb00` (it overwrites EDX at `0x54bb04`).

```cpp
struct NativeCall { uint32_t id; uint8_t first; uint8_t* types; uint32_t* values; uint32_t argc; uint32_t ret, retType; };
static_assert(offsetof(NativeCall, types) == 8 && offsetof(NativeCall, ret) == 0x14, "layout");
using Dispatch = void (__fastcall*)(void* vm, void* edx, uint32_t id, NativeCall* d);
static Dispatch g_next;                                   // value read from 0x6b467c
void __fastcall OurDispatch(void* vm, void* edx, uint32_t id, NativeCall* d) {
  if (((id >> 8) & 0xFF) == 0x7A) { OurNative(vm, d); return; }   // cdecl handler(vm, desc)
  g_next(vm, edx, id, d);                                  // stock 0x54bb00, or a later mod's wrapper
}
```

- **Install.** Do this in `SsasrModInitialize`; the slot is static `.rdata`, so it does not depend on the pack's order.
  1. Require `*(u32*)0x6b467c == 0x54bb00`. Otherwise log and skip.
  2. Call `VirtualProtect(0x6b467c, 4, PAGE_READWRITE)`.
  3. `InterlockedExchange` the slot to `OurDispatch`.
  4. Restore the old protection. `FlushInstructionCache` is not needed.
- **Rollback.** Write `g_next` back only if the slot still holds `OurDispatch`. Never unload the DLL.
- **Gate.** Substitute our script modules (SCRIPT-LOADER §4b) only if the install succeeded. Without the bridge, a 0x7Axx call reads cell `0x6f6bc0` = {`0x3f000000`, …} and faults. Group 0x7A is free: no stock id can use it, since the stock dispatcher would fault the same way (I).
- **Fallback: 7-byte code detour.** Replace `8b 44 24 04 0f b6 d4` at `0x54bb00` with `e9 rel32 90 90` (no relative operands are stolen).
  - Stub: `mov eax,[esp+4]; movzx edx,ah; cmp edx,0x7A; jne → jmp 0x54bb07` (EAX = id and EDX = group are live there).
  - Our branch: `push dword [esp+8]; push ecx; call handler; add esp,8; ret 8`.
  - Write it only after the pack's init and only when all 0x26 bytes are stock; otherwise the pack refuses.
- **Alternative: data redirect.** Point group 0x21's array pointer (cell `0x6f6af8`, now `0x6f68f0`) at our own copy: the 0x26 stock entries plus ours from 0x2126. It has zero overhead. However, it silently bypasses any later in-place slot patch in group 0x21, and an ungated 0x2126+ call would run group 0x20's natives.

| VA | What | Bytes (unpacked exe unless noted) | Tag |
|---|---|---|---|
| **0x6b467c** | VM vtable 0x6b4670 slot 3 → dispatcher (only ref to 0x54bb00) | `00 bb 54 00` (same on disk) | C |
| 0x54bb00 | dispatcher, thiscall(id, desc) ret 8; fingerprinted by the Roster Pack | `8b 44 24 04 0f b6 d4 8b 14 d5 f0 69 6f 00` | C |
| 0x6f69f0 | group cells {array, &count}, 0x00..0x21; read only by 0x54bb0a | `b0 5b 6f 00 ac 5b 6f 00` | C |
| 0x6f6bc0 | "cell" 0x7A = float data | `00 00 00 3f 35 fa 8e 3c` | C |
| 0x490167 / 0x490172 | VM+0x24 = 4; native call through vtbl+0xc | `c7 46 24 04 00 00 00` / `ff d2` | C |
| 0x48c520 | set int return, thiscall ECX = slot, ret 4 | `56 8b f1 8b 46 04 83 f8 05` | C |
| 0x48c330 / 0x4928d0 | push return value / release descriptor return | — | C |
| 0x5fdc50 | native 0x1610: create text buffer (type 7) | `83 ec 08 56 57 6a 10 e8` | C |
| 0x5fd250 (= 0x5fd200) | native 0x1613 (0x1612): unbounded `sprintf "%s"` | `8b 44 24 08 8b 40 0c 8b 08 8b 40 04` | C |
| 0x6a71f8 | text buffer vtable: [0] 0x485200 get-text, [1] 0x485220 delete | `00 52 48 00 20 52 48 00` | C |
| 0x4a6f40 | native 0x021e → 0x47f370 → 0x4853c0 (live pointer at element+0x108) | `8b 44 24 08 8b 40 0c 8b 48 04` | C |
| 0x485520 / 0x4855a0 | TEXT draw (slot at 0x6a7228) / call to 0x47ece0 | `56` / `e8 3b 97 ff ff` | C |
| 0x47ef15 | draw reads source vtbl[0] into a 4096-byte buffer | `ff d0` | C |
| 0x482c90 | UTF-8 → UTF-16 (no 4-byte, no validation) | `8b 4c 24 08` | C |
| DLL 0x10002ae2 | Roster Pack: fingerprint 0x54bb00, 0x26 bytes | `68 00 bb 54 00` | C |
| DLL 0x100038ea | Roster Pack: patch native 0x1335 slot 0x6f661c → 0x10002b80 | `68 1c 66 6f 00` | C |
