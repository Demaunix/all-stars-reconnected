"""Build the script modules our runtime substitutes into FrontEndScripts.sbn at load time.

usage: python -I tools/build_menus.py <project root>
Inputs (menus/base/, pinned by SHA-256):
  stock-fe_network_game.smosbin
                           the stock PC network stub (from the game's Scripts/FrontEndScripts.sbn).
  preview5-*.smosbin       Codex's Preview5 front-end modules (offline Group menu, Game Search,
                           Create, racer popup, local lobby, Settings). fe_mainmenu and dialogs are
                           used as is; fe_network_game is only the regression reference for
                           tools/netmenu, which rebuilds it from the stock stub.
  rosterpack-fe_network_game.smosbin
                           the All-Stars Roster Pack's network stub: stock plus 4 racer rows
                           (ROSTER_EXTRA) in the network racer table.
  metal112-fe_network_game.smosbin
                           the standalone Metal Sonic 1.1.2's network stub: stock plus Metal's row
                           (the same bytes as the Pack's Metal row; re/m9-metal112/FINDINGS.md).
Output: menus/out/<module>.smosbin, one file per substituted module (fe_network_game also serves
its alias fe_network_error_handler; fe_network_game_stock replaces it in games without the Roster
Pack, fe_network_game_metal in games with the standalone Metal Sonic 1.1.2), and build/menus_embed.h
for the runtime.
"""
import hashlib, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smos_reloc  # noqa: E402
from netmenu import build as netmenu  # noqa: E402
from netmenu.pkg import name_hash, read_package  # noqa: E402

PINS = {
    "stock-fe_network_game.smosbin": "39009356754f623d7a03e0ee9f21e413d7eb91c59c2d725f28d1a652b8115663",
    "preview5-fe_network_game.smosbin": "7d98393ca05df241231b03a89d9b578b527c5138cc1f886818c2d2c422a68711",
    "preview5-fe_mainmenu.smosbin": "8f7a1fe62751d8c5ad587424e05cc609fbc4559b7627c5e3786d1bb5fa9fabf6",
    "preview5-dialogs.smosbin": "d0aacdabbaf264c82931ea96580b844ad59b150defafc0b5d19d89a13f4a6c79",
    "rosterpack-fe_network_game.smosbin": "dda10019b222845c33e0918a6c407a99ad683c9a4741a42de01b4320ca00d36d",
    # The standalone Metal Sonic 1.1.2's (its whole-file FrontEndScripts.sbn 96319de4...; fe_network_error_handler is the alias).
    "metal112-fe_network_game.smosbin": "04ad1a0c7a603b725001ffa78ddf29f89abe22ec5e47235cfdb80cd992c1c854",
    # The game's own `results` module (the same bytes in the stock and the Roster Pack's FrontEndScripts.sbn).
    "stock-results.smosbin": "d582a4aaaf3ba5af94ada225b22f08188bd6cb42f47899a8966d68d596b252e3",
}

# The Roster Pack's racers beyond Codex's 21, in the order of its network-table rows (draft 21..24).
ROSTER_EXTRA = [("BANJO & KAZOOIE", "banjo"), ("AVATAR", "avatar"), ("MII", "mii"), ("METAL SONIC", "mechasonic")]
# The Roster Pack's fe_charselscreen racer list (25 entries at 0x274a), as draft numbers. Codex's stock
# order with the first three ROSTER_EXTRA racers after Opa-Opa, and Metal last.
ROSTER_ORDER = [0, 6, 8, 7, 10, 9, 11, 17, 19, 20, 18, 16, 15, 12, 13, 21, 22, 23, 14, 5, 4, 2, 3, 1, 24]
# The standalone Metal Sonic 1.1.2: the stock 21 and Metal (draft 21), last in its fe_charselscreen list (0x25c5; checked
# 2026-10-24: the stock order then 21).
METAL_ROWS = [("METAL SONIC", "mechasonic")]
METAL_ORDER = netmenu.STOCK_ORDER + [21]
METAL_SBN = Path(__file__).resolve().parent.parent / "reference" / "metal-sonic-1.1.2" / "FrontEndScripts.sbn"
METAL_CHARSEL_LIST = 0x25c5
ROSTER_SBN = r"A:\SteamLibrary\steamapps\common\Sonic and SEGA All Stars Racing\mods\All-Stars Roster Pack\payloads\Scripts\FrontEndScripts.sbn"
CHARSEL_LIST = 0x274a

# The network racer table: 21 stock rows of 5 hashes (`02 <hash>` x5, then `22 08 00 04 00 00 00`),
# closed by `22 09 00 <2 x rows> 00 00 00` at code offset 3019 in both the stock stub and Preview5.
TABLE_END = 3019
STOCK_CLOSE = bytes.fromhex("2209002a000000")  # 42 = 2 x 21 rows
ROW_LEN = 32


def load(base, name):
    if not (base / name).exists():  # (not in the public source tree: game-derived files can't be shared)
        raise SystemExit(f"menus/base/{name} is missing: these script modules come from the game and other mods and can't be "
                         f"shared; see README.md (Building)")
    data = (base / name).read_bytes()
    got = hashlib.sha256(data).hexdigest()
    if got != PINS[name]:
        raise SystemExit(f"{name}: sha256 {got}, expected {PINS[name]}")
    return data


def pack_racer_rows(pack):
    """The rows the Roster Pack inserted before the table's closing instruction."""
    close = pack.index(b"\x22\x09\x00", TABLE_END)
    rows = pack[TABLE_END:close]
    assert len(rows) % ROW_LEN == 0 and len(rows) // ROW_LEN == 4, len(rows)
    for i in range(0, len(rows), ROW_LEN):
        r = rows[i:i + ROW_LEN]
        assert all(r[k * 5] == 0x02 for k in range(5)) and r[25:] == bytes.fromhex("22080004000000"), r.hex()
    count = struct.unpack_from("<I", pack, close + 3)[0]
    assert count == 42 + 2 * len(rows) // ROW_LEN, count
    return rows


def check_metal_order(rows):
    """METAL_ORDER must be Metal Sonic 1.1.2's racer-select order (its package kept in reference/)."""
    chars = next(r["data"] for r in read_package(METAL_SBN.read_bytes()) if r["name"] == "fe_charselscreen")
    ids = [struct.unpack_from("<I", chars, METAL_CHARSEL_LIST + 5 * i + 1)[0] for i in range(len(rows))]
    assert all(chars[METAL_CHARSEL_LIST + 5 * i] == 0x02 for i in range(len(rows)))
    by_hash = {name_hash(identity): i for i, (_, identity) in enumerate(rows)}
    assert [by_hash.get(h) for h in ids] == METAL_ORDER, ids


def check_roster_order(rows):
    """ROSTER_ORDER must be the installed Roster Pack's racer-select order. Skipped if it isn't installed."""
    sbn = Path(ROSTER_SBN)
    if not sbn.exists():
        print("note: Roster Pack not found; racer order not re-checked against its fe_charselscreen")
        return
    chars = next(r["data"] for r in read_package(sbn.read_bytes()) if r["name"] == "fe_charselscreen")
    ids = [struct.unpack_from("<I", chars, CHARSEL_LIST + 5 * i + 1)[0] for i in range(len(rows))]
    assert all(chars[CHARSEL_LIST + 5 * i] == 0x02 for i in range(len(rows)))
    by_hash = {name_hash(identity): i for i, (_, identity) in enumerate(rows)}
    assert [by_hash.get(h) for h in ids] == ROSTER_ORDER, ids


# Steam Join Game / invites from the main menu (re\m6-join\FINDINGS.md 2, 4). Preview5's fe_mainmenu runs its
# menu inputs from 0x1c07 only when idle (the gate just before it: no transition, no dialog); g16 is the
# current menu, 0x6cb83a85 the license select (left only once a license is picked or made). There, with a
# Steam join waiting (native 7a1c), it does what the Play with Friends item does (export 786fbb8e: tell
# fe_network_game "friends", then the goto helper at 0x1317, which also clears the idle flag g3), and skips
# this frame's inputs (to the save check at 0x1ec2). fe_network_game's search screen then takes the join.
MAINMENU_INPUTS, MAINMENU_GOTO, MAINMENU_INPUTS_DONE = 0x1c07, 0x1317, 0x1ec2
LICENSE_SELECT, NETWORK_GAME, NETWORK_MODE = 0x6cb83a85, 0x8398d0bc, 0xd1223911
NAT_JOIN_POLL = 0x7a1c


def mainmenu_with_steam_join(data):
    """Preview5's fe_mainmenu plus the Steam-join check before its menu inputs."""
    assert data[MAINMENU_INPUTS - 3:MAINMENU_INPUTS] == bytes.fromhex("14ce02")  # the idle gate's JF
    assert data[MAINMENU_INPUTS:MAINMENU_INPUTS + 8] == bytes.fromhex("3200190401005a04")  # NAT 0419
    assert data[MAINMENU_INPUTS_DONE:MAINMENU_INPUTS_DONE + 5] == bytes.fromhex("5713140e00")  # GETG 13; JF
    c = netmenu.steam.Code()
    c.get(0x16).ident(LICENSE_SELECT).raw(b"\x4b").branch("skip")  # NE
    c.native(NAT_JOIN_POLL, 0, drop=False).branch("skip")
    c.ident(NETWORK_GAME).ident(NETWORK_MODE).num(1).array(1).native(0x1607, 3)
    c.raw(b"\x17").ident(NETWORK_GAME)
    call_at = len(c.data)
    c.raw(b"\x34\x01" + bytes(4) + struct.pack("<HH", 1, 0x7ffe) + b"\x18")  # CALL argc 1, DROP
    jump_at = len(c.data)
    c.raw(b"\x15\x00\x00")  # JMP
    c.label("skip")
    chunk = bytearray(c.finish())
    struct.pack_into("<i", chunk, call_at + 2, MAINMENU_GOTO - (MAINMENU_INPUTS + call_at + 10))
    struct.pack_into("<h", chunk, jump_at + 1, (MAINMENU_INPUTS_DONE + len(chunk)) - (MAINMENU_INPUTS + jump_at + 3))
    built, _ = smos_reloc.insert(data, {MAINMENU_INPUTS: bytes(chunk)})
    ops = {o["offset"]: o for o in smos_reloc.instructions(built)}
    assert ops[MAINMENU_INPUTS + call_at]["target"] == MAINMENU_GOTO
    assert ops[MAINMENU_INPUTS + jump_at]["target"] == MAINMENU_INPUTS_DONE + len(chunk)
    assert built[MAINMENU_INPUTS_DONE + len(chunk):MAINMENU_INPUTS_DONE + len(chunk) + 5] == bytes.fromhex("5713140e00")
    assert built[MAINMENU_INPUTS + len(chunk):MAINMENU_INPUTS + len(chunk) + 8] == bytes.fromhex("3200190401005a04")
    return built


# VIEW PROFILE / GAMER CARD on the results (re\m7-profile\FINDINGS.md). The PC `results` module kept the console's row
# highlight (020e on "RACERESULTS\BAR0n" between the markers 0d09208a / d279b4c1, at 0x45ec for g33 == 6) but lost the
# Up/Down/F1 handler of its list state (the Mac's 0x2685..0x27d7). Each non-busy list frame (right after the busy-dialog
# JF at 0x25de) now calls a subroutine after the module's code: native 7a20 (C++, race.cpp RaceProfileNative: the cursor
# over the rows whose car is a real player's; F1 opens that player's Steam profile in the overlay) says which row to
# un-highlight and highlight and whether the footer should read CONTINUE + VIEW PROFILE (F1) or CONTINUE only, through the
# module's own SetupButtonBar (0x3fa7, 4 label arguments; the third draws the F1 glyph; "VIEW_PROFILE" is the PC string
# table's "VIEW PROFILE", key a34c8e57 in D8A395BC / FD0EE89D, the label the PS3's results used; "VIEW_GAMERCARD" = "GAMER
# CARD" is the 360's). The 16-bit jumps of the module are untouched: the chunk at 0x25e1 is a 12-byte call.
RESULTS_LIST_TICK, RESULTS_BUTTON_BAR = 0x25e1, 0x3fa7
NAT_PROFILE, KEY_UP, KEY_DOWN, KEY_F1 = 0x7a20, 0x0421, 0x0422, 0x041d
BAR_ON, BAR_OFF = 0x0d09208a, 0xd279b4c1  # the row animation's markers: highlight = (BAR_ON -> BAR_OFF), off = the reverse
PROFILE_STEP, PROFILE_ROW_OFF, PROFILE_ROW_ON, PROFILE_FOOTER = 0, 1, 2, 3  # kProfile* in race.cpp
FOOTER_CARD, FOOTER_PLAIN = 1, 2


def results_with_profile(data):
    """The stock `results` module plus the profile cursor's frame call (every list frame) and its subroutine."""
    code_end = smos_reloc.u32(data, 12) + smos_reloc.u32(data, 16)
    assert code_end == 0x655f, hex(code_end)
    assert data[0x25de:0x25f1] == bytes.fromhex("14a40057365101422736003200160900009d04")  # JF -> 0x2685; g36++; NAT 0916
    assert data[0x25f1:0x25f9] == bytes.fromhex("51054a1408005709")  # INT8 5; EQ; JF; GETG 9 (the list's time limit: g9 with type 5)
    assert data[0x2685:0x2687] == b"\x17\x1b"
    assert data[0x3fa7:0x3fa9] == b"\x04\x2d" and data[0x4097:0x40a1] == bytes.fromhex("340406ffffff00006d09")  # SetupButtonBar, a 4-arg call of it
    assert data[0x45ec:0x460a] == bytes.fromhex("0507fc571451014242" "3c028a20090d02c1b479d2510032040e020000f409")  # the stock highlight of BAR01
    c = netmenu.steam.Code()
    calls = []

    def profile(field):  # 7a20(field, up, down, f1): the cursor moves on field 0; the fields after answer for this frame
        c.num(field).native(KEY_UP, 0, drop=False).native(KEY_DOWN, 0, drop=False).native(KEY_F1, 0, drop=False)
        c.native(NAT_PROFILE, 4, drop=False)

    def bar_anim(field, first, second):  # "RACERESULTS\BAR0" + (row + 1): 020e(node, first, second, 0), as the Mac's 0x26e3
        c.string("RACERESULTS\\BAR0")
        profile(field)
        c.num(1).raw(b"\x42\x42\x3c").ident(first).ident(second).num(0).native(0x020e, 4)

    def button_bar(third):  # SetupButtonBar("CONTINUE", NONE, third, NONE): FRAME; args; CALL argc 4; DROP
        c.raw(b"\x17").string("CONTINUE").raw(b"\x31")
        if third:
            c.string(third)
        else:
            c.raw(b"\x31")
        c.raw(b"\x31")
        calls.append(len(c.data))
        c.raw(b"\x34\x04" + bytes(4) + struct.pack("<HH", 1, 0x7ffe) + b"\x18")

    profile(PROFILE_STEP)
    c.raw(b"\x18")
    profile(PROFILE_ROW_OFF)
    c.num(0).raw(b"\x49").branch("no_off")  # GE
    bar_anim(PROFILE_ROW_OFF, BAR_OFF, BAR_ON)
    c.label("no_off")
    profile(PROFILE_ROW_ON)
    c.num(0).raw(b"\x49").branch("no_on")
    bar_anim(PROFILE_ROW_ON, BAR_ON, BAR_OFF)
    c.label("no_on")
    profile(PROFILE_FOOTER)
    c.num(FOOTER_CARD).raw(b"\x4a").branch("no_card")  # EQ
    button_bar("VIEW_PROFILE")
    c.label("no_card")
    profile(PROFILE_FOOTER)
    c.num(FOOTER_PLAIN).raw(b"\x4a").branch("no_plain")
    button_bar(None)
    c.label("no_plain")
    c.raw(b"\x17\x1b")
    sub = bytearray(c.finish())
    hook = bytearray(b"\x17\x34\x00" + bytes(4) + struct.pack("<HH", 1, 0x7ffe) + b"\x18")  # FRAME; CALL argc 0 -> sub; DROP
    sub_at = code_end + len(hook)
    for at in calls:
        struct.pack_into("<i", sub, at + 2, (RESULTS_BUTTON_BAR + len(hook)) - (sub_at + at + 10))
    struct.pack_into("<i", hook, 1 + 2, sub_at - (RESULTS_LIST_TICK + 1 + 10))
    built, _ = smos_reloc.insert(data, {RESULTS_LIST_TICK: bytes(hook), code_end: bytes(sub)})
    ops = {o["offset"]: o for o in smos_reloc.instructions(built)}
    assert ops[RESULTS_LIST_TICK + 1]["target"] == sub_at
    for at in calls:
        assert ops[sub_at + at]["target"] == RESULTS_BUTTON_BAR + len(hook)
    assert ops[0x25de]["target"] == 0x2685 + len(hook)  # the busy-dialog JF still skips the frame's work
    assert built[RESULTS_LIST_TICK + len(hook):RESULTS_LIST_TICK + len(hook) + 16] == data[RESULTS_LIST_TICK:RESULTS_LIST_TICK + 16]
    assert built[sub_at:sub_at + len(sub)] == bytes(sub) and built[sub_at + len(sub) - 2:sub_at + len(sub)] == b"\x17\x1b"
    natives = {struct.unpack_from("<H", sub, o["offset"] + 2)[0] for o in smos_reloc.instructions(bytes(sub), 0, len(sub), "<") if o["opcode"] == 0x32}
    assert natives == {NAT_PROFILE, KEY_UP, KEY_DOWN, KEY_F1, 0x020e}, sorted(hex(n) for n in natives)
    assert len(hook) == 12  # race.cpp kResultsListPc counts on it
    return built


def main():
    root = Path(sys.argv[1]).resolve()
    base, out = root / "menus" / "base", root / "menus" / "out"
    out.mkdir(parents=True, exist_ok=True)
    stock = load(base, "stock-fe_network_game.smosbin")
    stock_ids = {struct.unpack_from("<I", stock, at + 1)[0] for at in range(0x92b, TABLE_END, ROW_LEN)}
    assert stock_ids == {name_hash(identity) for _, identity in netmenu.STOCK_ROWS}
    # Regression: Codex's offline chain with its 21 racers must rebuild the module the user tested
    # as Preview5. The shipped modules add our Steam stage (tools/netmenu/steam.py) on top.
    assert netmenu.build(stock, netmenu.STOCK_ROWS, netmenu.STOCK_ORDER) == load(base, "preview5-fe_network_game.smosbin")
    net_stock = netmenu.build(stock, netmenu.STOCK_ROWS, netmenu.STOCK_ORDER, online=True)
    rows = pack_racer_rows(load(base, "rosterpack-fe_network_game.smosbin"))
    assert [struct.unpack_from("<I", rows, ROW_LEN * k + 1)[0] for k in range(len(ROSTER_EXTRA))] == \
        [name_hash(identity) for _, identity in ROSTER_EXTRA]
    roster = netmenu.STOCK_ROWS + ROSTER_EXTRA
    check_roster_order(roster)
    net = netmenu.build(stock, roster, ROSTER_ORDER, online=True)
    end = TABLE_END + sum(len(v) for v in netmenu.settings.egg_insertions(stock).values())  # (Egg's entries come before it)
    assert net[end:end + 7] == STOCK_CLOSE, net[end:end + 7].hex()
    built, info = smos_reloc.insert(net, {end: rows})
    built = bytearray(built)
    at = end + len(rows)
    assert built[at:at + 7] == STOCK_CLOSE
    struct.pack_into("<I", built, at + 3, 42 + 2 * len(rows) // ROW_LEN)
    smos_reloc.validate(bytes(built))
    # The standalone Metal Sonic 1.1.2: its module is exactly the stock stub with the Pack's Metal row inserted at the table's end
    # and the count 2 x 22; our build is the stock chain with that row, as the Roster Pack's.
    metal112 = load(base, "metal112-fe_network_game.smosbin")
    metal_row = metal112[TABLE_END:TABLE_END + ROW_LEN]
    assert metal_row == rows[-ROW_LEN:], "Metal 1.1.2's row is the Pack's Metal row"
    expect, _ = smos_reloc.insert(stock, {TABLE_END: metal_row})
    expect = bytearray(expect)
    struct.pack_into("<I", expect, TABLE_END + ROW_LEN + 3, 44)
    n = struct.unpack_from("<I", metal112, 8)[0]  # (its file isn't padded to 4 like our base copies)
    assert n == len(metal112) and bytes(expect[:n]) == metal112 and not any(expect[n:]), "Metal 1.1.2's network stub is stock + its row"
    metal = netmenu.STOCK_ROWS + METAL_ROWS
    check_metal_order(metal)
    net_metal = netmenu.build(stock, metal, METAL_ORDER, online=True)
    assert net_metal[end:end + 7] == STOCK_CLOSE
    built_metal, _ = smos_reloc.insert(net_metal, {end: metal_row})
    built_metal = bytearray(built_metal)
    assert built_metal[end + ROW_LEN:end + ROW_LEN + 7] == STOCK_CLOSE
    struct.pack_into("<I", built_metal, end + ROW_LEN + 3, 44)
    smos_reloc.validate(bytes(built_metal))
    outputs = {
        "fe_network_game": bytes(built),
        "fe_network_game_stock": net_stock,
        "fe_network_game_metal": bytes(built_metal),
        "fe_mainmenu": mainmenu_with_steam_join(load(base, "preview5-fe_mainmenu.smosbin")),
        "dialogs": dialogs_with_mute(dialogs_with_session_messages(load(base, "preview5-dialogs.smosbin"))),
        "results": results_with_profile(load(base, "stock-results.smosbin")),
    }
    egg = struct.pack("<I", netmenu.settings.EGG_COURSE)
    for name in ("fe_network_game", "fe_network_game_stock", "fe_network_game_metal"):  # Egg Hangar is the online lobby's 25th course
        assert outputs[name].count(b"\x02" + egg + bytes.fromhex("22080019000000271200")) == 1, name
        assert outputs[name].count(b"\x02" + egg + b"\x02" + struct.pack("<II", netmenu.settings.EGG_ART, 0)[:4]) == 1, name
    for name, data in outputs.items():
        smos_reloc.validate(data)
        (out / f"{name}.smosbin").write_bytes(data)
        print(f"{hashlib.sha256(data).hexdigest()} {name}.smosbin {len(data)} bytes")
    print(f"fe_network_game: +{info['inserted_bytes']} bytes of racer rows, {len(info['relative_fixes'])} relative fixes")
    write_embed(root / "build" / "menus_embed.h", outputs)
    write_race_tables(root / "build" / "race_tables.h", roster, metal)


# The console's session messages (M7, re\m7-messages\FINDINGS.md Q1): the dialogs module's definitions are typed
# constants [title key, body key, button label keys...]. Three dwords change in place (same size, no relocation):
# CB5D217E ("There are no other players in the match. Returning to the lobby.", OK) becomes the console's "The
# connection to the other player(s) has been lost. The match cannot continue." with a RETURN TO LOBBY button, and
# 8374A07B's body becomes "The connection to the host has been lost. The game session is no longer available."
# Offsets are in the shipped (Preview5) module, 0x1c past the stock module's.
DIALOG_EDITS = (
    (0x1844, 0x08d1c242, 0xcda264d3),  # CB5D217E body
    (0x184c, 0x0a9f2dcc, 0x21a53ccf),  # CB5D217E button: OK -> RETURN TO LOBBY
    (0x143c, 0xc4c2f0a1, 0x07b1de19),  # 8374A07B body
    # The console's lobby player popups (re\m7-flows\FINDINGS.md A; PS3/360 definitions 39A06F99 and 1B433955 are not in the PC
    # module): D4879E10 (the demo's exit question, 2 buttons; no PC script posts it) is given the guest's VIEW GAMER CARD / RETURN TO
    # LOBBY ("VIEW GAMER CARD" reads "VIEW STEAM PROFILE" through our text table, tools\text_tables.py). The host's popup used
    # 77B81411 until M8, which is NOT unused (the memory-card autosave's SaveDataTimeError posts it, re\m8-yellow\FINDINGS.md): it
    # keeps its own buttons now, and the host's popups are appended definitions (dialogs_with_mute).
    (0x189c, 0x1f733628, 0xbfb811cd),  # D4879E10 button 1: VIEW GAMER CARD
    (0x18a4, 0x97e89505, 0x21a53ccf),  # D4879E10 button 2: RETURN TO LOBBY
    # 1.1.1, the auto-update's notices (re\update-dialogs\FINDINGS.md): two definitions nothing on PC posts, each [title, body, OK],
    # get bodies nothing else uses (Wii-only keys whose text tools\text_tables.py rewrites); posted by src\race.cpp on the main menu.
    (0x163c, 0xeb89a815, 0x484f1e46),  # 9DC5CA23 body -> "A new version ... is ready. Restart the game to use it."
    (0x13dc, 0xc47a8040, 0x74df3842),  # 75CF8F27 body -> "Steam Multiplayer was updated to a new version."
)
DIALOG_KEPT = ((0xfcc, 0x65a73f51),)  # 168056C7 body ("The host has ended the match. Returning to the lobby.")


def dialogs_with_session_messages(data):
    out = bytearray(data)
    for at, old, new in DIALOG_EDITS:
        assert struct.unpack_from("<I", out, at)[0] == old, f"dialogs+{at:#x}: {struct.unpack_from('<I', out, at)[0]:08x}"
        assert out[at - 4] == 0x83 or out[at + 4] == 0x83, "a typed-constant hash entry"
        struct.pack_into("<I", out, at, new)
    for at, key in DIALOG_KEPT:
        assert struct.unpack_from("<I", out, at)[0] == key
    return bytes(out)


# MUTE in the lobby player popups (M8, re\m8-mute\FINDINGS.md): the PC popup scene has a 4th button (and SMALL_04), the dialogs
# script handles 4, but no PC definition has 4 buttons and smos_reloc can't grow a module with typed constants. So two new
# definition arrays are appended after the module and the two popups' map entries repointed to them (self-relative offsets, as
# the game's loader resolves them: 0x48bdd0 / 0x48bc90); no code byte changes. Host VIEW GAMER CARD / MUTE / KICK / RETURN TO
# LOBBY, guest VIEW GAMER CARD / MUTE / RETURN TO LOBBY (title and body as before). The old arrays stay, unreferenced.
# M8 (re\m8-yellow\FINDINGS.md 4): a muted player's popups say UNMUTE (the text key NOTHING, which our text tables make read UNMUTE;
# tools\text_tables.py), on two more definitions; and the host's popups use definitions nothing else posts (77B81411 is the
# memory-card autosave's and keeps its own buttons): 940FE7B4 (Wii "disconnected from Nintendo Wi-Fi Connection") the host's MUTE
# popup, 504ACB9A (a controller-config prompt) the host's UNMUTE popup, D4879E10 the guest's MUTE popup as before, 18C51485 (360
# "no privileges to view gamer cards") the guest's UNMUTE popup. None appears anywhere but its map entry (no PC script, the exe,
# other mods' DLLs, the memory-card scripts; re\m8-yellow\out\defuse_unused.txt). Title and body: the m8e popups' (77B81411's,
# D4879E10's).
VIEW_GAMERCARD, MUTE, KICK, RETURN_TO_LOBBY, UNMUTE = 0xbfb811cd, 0x51577208, 0x476d4a25, 0x21a53ccf, 0x72259212
POPUP_HOST, POPUP_HOST_UNMUTE, POPUP_GUEST, POPUP_GUEST_UNMUTE = 0x940fe7b4, 0x504acb9a, 0xd4879e10, 0x18c51485


def dialog_definitions(d):
    """{key: (value field offset, array offset, [element hashes])}, read the way the game's loader resolves them."""
    tc = struct.unpack_from("<I", d, 0x18)[0]
    assert struct.unpack_from("<3I", d, tc) == (1, struct.unpack_from("<I", d, tc + 4)[0], 0x89)
    mp = tc + 4 + struct.unpack_from("<I", d, tc + 4)[0]
    n, _, elems, rc = struct.unpack_from("<4I", d, mp)
    assert elems == 9 and rc == 0xffff
    out = {}
    for i in range(n):
        e = mp + 16 + 16 * i
        key, kt, val, vt = struct.unpack_from("<4I", d, e)
        assert kt == 0x83 and vt == 0x88
        arr = e + 8 + val
        cnt, cap, ael, arc = struct.unpack_from("<4I", d, arr)
        assert cap == cnt and ael == 9 and arc == 0xffff and arr % 2 == 0
        items = [struct.unpack_from("<II", d, arr + 16 + 8 * k) for k in range(cnt)]
        assert all(t == 0x83 for _, t in items)
        out[key] = (e + 8, arr, [h for h, _ in items])
    assert list(out) == sorted(out)
    return out


def dialogs_with_mute(data):
    defs = dialog_definitions(data)
    assert len(data) == struct.unpack_from("<I", data, 8)[0] == 0x1c29
    title = defs[0x77b81411][2][:2]  # (the m8e host popup's title and body)
    guest = defs[POPUP_GUEST][2]
    assert defs[0x77b81411][2] == title + [0x8b65323d, 0x8124a2c1, 0x30507f88] and guest[2:] == [VIEW_GAMERCARD, RETURN_TO_LOBBY], "77B81411 / D4879E10"
    def array(hashes):
        return struct.pack("<4I", len(hashes), len(hashes), 9, 0xffff) + b"".join(struct.pack("<II", h, 0x83) for h in hashes)
    out = bytearray(data) + bytes((-len(data)) % 4)
    for key, items in ((POPUP_HOST, title + [VIEW_GAMERCARD, MUTE, KICK, RETURN_TO_LOBBY]),
                       (POPUP_GUEST, guest[:2] + [VIEW_GAMERCARD, MUTE, RETURN_TO_LOBBY]),
                       (POPUP_HOST_UNMUTE, title + [VIEW_GAMERCARD, UNMUTE, KICK, RETURN_TO_LOBBY]),
                       (POPUP_GUEST_UNMUTE, guest[:2] + [VIEW_GAMERCARD, UNMUTE, RETURN_TO_LOBBY])):
        val = defs[key][0]
        struct.pack_into("<I", out, val, len(out) - val)
        out += array(items)
    struct.pack_into("<I", out, 8, len(out))
    after = dialog_definitions(bytes(out))
    assert {k for k in defs if defs[k][2] != after[k][2]} == {POPUP_HOST, POPUP_GUEST, POPUP_HOST_UNMUTE, POPUP_GUEST_UNMUTE}
    assert after[POPUP_HOST][2][2:] == [VIEW_GAMERCARD, MUTE, KICK, RETURN_TO_LOBBY] and after[POPUP_HOST_UNMUTE][2][3] == UNMUTE
    assert after[POPUP_GUEST][2][2:] == [VIEW_GAMERCARD, MUTE, RETURN_TO_LOBBY] and after[POPUP_GUEST_UNMUTE][2][3] == UNMUTE
    assert after[0x77b81411][2] == defs[0x77b81411][2] and len(out) == 0x1d1c
    return bytes(out)


# Course names in the network lobby's course order (stock fe_network_game global 0x12, built at
# COURSE_TABLE_AT; notes/RACE-BRIDGE.md). The race bridge sends the name: the game loads tracks by name.
COURSES = ["seasidehill_easy", "billyhatcher_easy", "casinopark_medium", "samba_easy", "jetsetradio_easy",
           "houseofthedead_easy", "finalfortress_easy", "smb_easy", "billyhatcher_medium", "finalfortress_medium",
           "samba_medium", "casinopark_easy", "houseofthedead_medium", "seasidehill_hard", "smb_medium",
           "jetsetradio_medium", "samba_hard", "jetsetradio_hard", "houseofthedead_hard", "seasidehill_medium",
           "casinopark_hard", "billyhatcher_hard", "smb_hard", "finalfortress_hard",
           "doomeggzone_dlc"]  # Egg Hangar: the console's 25th (title update 2; netmenu/settings.py egg_insertions)
STOCK_COURSES = 24
COURSE_TABLE_AT = 0x8a9  # file offset in stock-fe_network_game.smosbin (24 pushes, then ARRAY 24)
# Racers never picked as AI: they need per-player appearance data or a mod.
NO_AI = {"mii", "avatar", "ryoforklift"}


def write_race_tables(path, roster, metal):
    """C header for src/race.cpp: lobby racer index -> character hash (both menu variants), courses."""
    stock = load(Path(__file__).resolve().parent.parent / "menus" / "base", "stock-fe_network_game.smosbin")
    table = [struct.unpack_from("<I", stock, COURSE_TABLE_AT + 5 * i + 1)[0] for i in range(STOCK_COURSES)]
    assert all(stock[COURSE_TABLE_AT + 5 * i] == 0x02 for i in range(STOCK_COURSES))
    assert table == [name_hash(n) for n in COURSES[:STOCK_COURSES]], [hex(h) for h in table]
    assert name_hash(COURSES[STOCK_COURSES]) == netmenu.settings.EGG_COURSE and len(COURSES) == netmenu.settings.ONLINE_COURSES
    lines = ["// Generated by tools/build_menus.py. Do not edit.", "#pragma once", "#include <stdint.h>",
             "struct RacerEntry { uint32_t hash; bool ai; const char* name; const char* display; };  // display: the racer's menu name",
             "struct CourseEntry { uint32_t hash; const char* name; };"]
    for var, rows in (("kRacersRoster", roster), ("kRacersStock", netmenu.STOCK_ROWS), ("kRacersMetal", metal)):
        lines.append(f"static const RacerEntry {var}[{len(rows)}] = {{")
        for display, identity in rows:
            assert display.isascii() and '"' not in display and "\\" not in display, display
            lines.append(f"    {{0x{name_hash(identity):08x}, {'false' if identity in NO_AI else 'true'}, \"{identity}\", \"{display}\"}},")
        lines.append("};")
    lines.append(f"static const CourseEntry kCourses[{len(COURSES)}] = {{")
    for n in COURSES:
        lines.append(f"    {{0x{name_hash(n):08x}, \"{n}\"}},")
    lines.append("};")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def write_embed(path, outputs):
    """C header with the module bytes, compiled into the runtime DLL (src/script_subst.cpp)."""
    lines = ["// Generated by tools/build_menus.py. Do not edit.", "#pragma once", "#include <stdint.h>"]
    for name, data in outputs.items():
        lines.append(f"// {name}.smosbin sha256 {hashlib.sha256(data).hexdigest()}")
        lines.append(f"alignas(16) static const uint8_t kMenu_{name}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ",".join(f"0x{b:02x}" for b in data[i:i + 24]) + ",")
        lines.append("};")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
