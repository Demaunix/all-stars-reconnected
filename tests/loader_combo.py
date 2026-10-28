"""This mod's built plan under the real ChaoGarage loader, with real game and mod data, without the game.

ChaoGarage's own stand-in game process (fixture_game.exe, from its test build) starts the loader in a scratch game folder that holds
a copy of the game's SumoToolResources.xpac, this mod's built plan and payloads, and Metal Sonic's real changes to that archive
(the installed Roster Pack layer's plan group and payloads). Mod runtimes are the loader's inert fixture_runtime.dll; no game code
runs. The stand-in then reads the archive back through the loader's redirection, as the game would, and every member is compared.
It also hands the loader every installed mod's allstars-mod.json, which loaders before 1.1.0 never read.

    python -I tests\\loader_combo.py <ChaoGarage out (1.2.0 + tests)> <ChaoGarage 1.1.0 out> <work folder>

Cases: (a) 1.2.0 with Metal Sonic: our variants for Metal's keys; (b) 1.2.0 alone: our own keys; (c) 1.2.0 with a Metal Sonic whose
navigator scene differs (a later update): that member gives way, the others still use our variants; (d) 1.1.0 with Metal Sonic:
"needs a newer ChaoGarage loader" for this mod, nothing started; (e) 1.2.0 with every installed mod's description: no refusal.
"""
import ctypes
import ctypes.wintypes as wt
import hashlib
import json
import os
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import xpac  # noqa: E402

MODS = xpac.GAME / "mods"
METAL = MODS / "Metal Sonic (Roster Pack)"
# The members Metal Sonic changes too, each with our variant: 7 navigator scenes (key pictures), 7 lobby scenes (M8: the yellow
# bar, 1.0.0: Egg Hangar's card), 5 dialogs text tables (VIEW STEAM PROFILE, UNMUTE) and 7 Settings popup scenes (1.0.0: Egg
# Hangar's name).
SHARED = 26
OURS = ROOT / "out" / "Steam Multiplayer"
DESKTOP = "SteamMultiplayerLoaderCombo"
failures: list[str] = []


def check(ok: bool, what: str) -> None:
    """Record one check."""
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


def sha_file(p: Path) -> str:
    """SHA-256 of a file."""
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [("cb", wt.DWORD), ("lpReserved", wt.LPWSTR), ("lpDesktop", wt.LPWSTR), ("lpTitle", wt.LPWSTR), ("dwX", wt.DWORD),
                ("dwY", wt.DWORD), ("dwXSize", wt.DWORD), ("dwYSize", wt.DWORD), ("dwXCountChars", wt.DWORD), ("dwYCountChars", wt.DWORD),
                ("dwFillAttribute", wt.DWORD), ("dwFlags", wt.DWORD), ("wShowWindow", wt.WORD), ("cbReserved2", wt.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wt.HANDLE), ("hStdOutput", wt.HANDLE), ("hStdError", wt.HANDLE)]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [("hProcess", wt.HANDLE), ("hThread", wt.HANDLE), ("dwProcessId", wt.DWORD), ("dwThreadId", wt.DWORD)]


def run_hidden(exe: Path, args: list[str], cwd: Path) -> int:
    """Run exe on a desktop of its own, so the loader's message boxes (pressed by the stand-in) never reach the screen."""
    user32, kernel32 = ctypes.WinDLL("user32", use_last_error=True), ctypes.WinDLL("kernel32", use_last_error=True)
    user32.CreateDesktopW.restype = wt.HANDLE
    desk = user32.CreateDesktopW(DESKTOP, None, None, 0, 0x10000000, None)
    assert desk, ctypes.get_last_error()
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(si)
    si.lpDesktop = "WinSta0\\" + DESKTOP
    pi = PROCESS_INFORMATION()
    line = ctypes.create_unicode_buffer(" ".join('"%s"' % a for a in [str(exe)] + args))
    ok = kernel32.CreateProcessW(None, line, None, None, False, 0x08000000, None, str(cwd), ctypes.byref(si), ctypes.byref(pi))
    assert ok, ctypes.get_last_error()
    waited = kernel32.WaitForSingleObject(pi.hProcess, 600000)
    code = wt.DWORD()
    kernel32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
    kernel32.CloseHandle(pi.hThread)
    kernel32.CloseHandle(pi.hProcess)
    user32.CloseDesktop(desk)
    assert waited == 0, "the stand-in game did not finish"
    return code.value


def write_mod(game: Path, folder: str, mod_id: str, plan: dict, payloads: Path | None, desc: bytes | None, loader: Path) -> None:
    """A mod folder with this plan (its payloads copied from `payloads`), the inert runtime and an optional description."""
    d = game / "mods" / folder
    (d / "runtime").mkdir(parents=True)
    shutil.copy2(loader / "fixture_runtime.dll", d / "runtime" / "fixture_runtime.dll")
    for g in plan.get("archive_groups", []):
        for op in g.get("operations", []):
            for p in [op["payload"]] + [v["payload"] for v in op.get("variants", [])]:
                if not (d / p).exists():
                    (d / p).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(payloads / p, d / p)
    text = json.dumps(plan).encode("ascii")
    (d / "plan.json").write_bytes(text)
    (d / "mod.ini").write_bytes(("[mod]\r\nschema=1\r\nid=%s\r\nversion=1\r\nplan=plan.json\r\nplan_sha256=%s\r\nruntime=runtime\\fixture_runtime.dll\r\n"
                                 "runtime_sha256=%s\r\n" % (mod_id, hashlib.sha256(text).hexdigest(), sha_file(d / "runtime" / "fixture_runtime.dll"))).encode("ascii"))
    if desc is not None:
        (d / "allstars-mod.json").write_bytes(desc)


def new_game(work: Path, name: str, loader: Path, archive: Path) -> Path:
    """A scratch game folder: the loader pair and a hard link to the scratch copy of the archive."""
    g = work / name
    if g.exists():
        shutil.rmtree(g)
    (g / "Resource").mkdir(parents=True)
    for f in ("ChaoGarage.dll", "dinput8.dll"):
        shutil.copy2(loader / f, g / f)
    os.link(archive, g / "Resource" / "SumoToolResources.xpac")
    (g / "read-back.txt").write_bytes(b"Resource\\SumoToolResources.xpac")
    return g


def start(g: Path, stand_in: Path, mode: str = "v2") -> dict:
    """Start the loader in the stand-in game; its result, plus the log."""
    code = run_hidden(stand_in, [str(g), mode, str(g / "result.json")], g)
    check(code == 0, f"{g.name}: the stand-in game ran")
    r = json.loads((g / "result.json").read_text(encoding="utf-8"))
    r["log"] = (g / "mods" / "SsasrMods.log").read_text(encoding="utf-8", errors="replace") if (g / "mods" / "SsasrMods.log").exists() else ""
    return r


def read_back(g: Path) -> tuple[dict, Path]:
    """The members of the archive the stand-in read through the loader."""
    p = g / "read-back.bin"
    _, rows = xpac.read_rows(p)
    return rows, p


def main() -> int:
    """Run the cases; 0 when every check passed."""
    loader, prev, work = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
    stand_in = loader / "fixture_game.exe"
    work.mkdir(parents=True, exist_ok=True)
    archive = work / "SumoToolResources.xpac"
    if not archive.exists() or sha_file(archive) != xpac.BASE_SHA256:
        shutil.copy2(xpac.GAME / xpac.ARCHIVE, archive)
    check(sha_file(archive) == xpac.BASE_SHA256, "the scratch copy of the game's archive is the original")
    _, base_rows = xpac.read_rows(archive)

    metal_plan = json.loads((METAL / "plan.json").read_text(encoding="utf-8"))
    metal_group = next(g for g in metal_plan["archive_groups"] if g["path"] == xpac.ARCHIVE)
    metal_only = {"schema": 1, "archive_groups": [metal_group]}
    ours_plan = json.loads((OURS / "plan.json").read_text(encoding="utf-8"))
    ours_desc = (OURS / "allstars-mod.json").read_bytes()
    ours_ops = ours_plan["archive_groups"][0]["operations"]
    variant_ops = {op["key"]: op for op in ours_ops if op.get("variants")}
    check(len(variant_ops) == SHARED and all(op.get("optional") is True for op in variant_ops.values()), f"our plan: {SHARED} optional operations with variants")
    metal_by_key = {op["key"]: op for op in metal_group["operations"]}
    check(all(op["variants"][0]["over_payload_sha256"] == metal_by_key[k]["payload_sha256"] for k, op in variant_ops.items()),
          "each variant is made for the installed Metal Sonic's payload of that member")

    def expect(*layers: tuple[dict, Path]) -> dict:
        """{key: bytes} of the changed members, later layers winning."""
        out = {}
        for ops, folder in layers:
            for op in ops:
                out[int(op["key"], 16)] = (folder / op["payload"]).read_bytes()
        return out

    def compare(g: Path, changed: dict, what: str) -> None:
        """Every member of the read-back archive against the original with `changed` applied."""
        rows, p = read_back(g)
        check(set(rows) == set(base_rows) | set(changed), f"{what}: the member list")
        bad = [k for k in rows if xpac.member(p, rows, k) != (changed[k] if k in changed else xpac.member(archive, base_rows, k))]
        check(not bad, f"{what}: every member's bytes ({len(rows)} members, {len(changed)} changed)" + (f"; differ: {[hex(k) for k in bad[:8]]}" if bad else ""))

    # (a) 1.2.0, Metal Sonic + this mod
    g = new_game(work, "a-metal-and-ours", loader, archive)
    write_mod(g, "Metal Sonic (Roster Pack)", "metal-sonic", metal_only, METAL, (METAL / "allstars-mod.json").read_bytes(), loader)
    write_mod(g, "Steam Multiplayer", "steam-multiplayer", ours_plan, OURS, ours_desc, loader)
    r = start(g, stand_in)
    check(r["hr"] == 0 and not r["dialogs"], f"(a) 1.2.0 with Metal Sonic starts without a message (hr {r['hr']:#x}, {len(r['dialogs'])} message(s))")
    want = expect((metal_group["operations"], g / "mods" / "Metal Sonic (Roster Pack)"), ([op for op in ours_ops if not op.get("variants")], OURS))
    for k, op in variant_ops.items():
        want[int(k, 16)] = (OURS / op["variants"][0]["payload"]).read_bytes()
    compare(g, want, "(a)")
    check(r["log"].count("steam-multiplayer's version made for metal-sonic's change is used") == SHARED, f"(a) the log names the {SHARED} combined members")

    # (b) 1.2.0, this mod alone
    g = new_game(work, "b-ours-alone", loader, archive)
    write_mod(g, "Steam Multiplayer", "steam-multiplayer", ours_plan, OURS, ours_desc, loader)
    r = start(g, stand_in)
    check(r["hr"] == 0 and not r["dialogs"], "(b) 1.2.0 alone starts without a message")
    compare(g, expect((ours_ops, OURS)), "(b)")

    # (c) 1.2.0, a Metal Sonic whose English navigator scene differs (as a later update might)
    g = new_game(work, "c-metal-update", loader, archive)
    changed_metal = json.loads(json.dumps(metal_only))
    mg = changed_metal["archive_groups"][0]
    write_mod(g, "Metal Sonic (Roster Pack)", "metal-sonic", changed_metal, METAL, None, loader)
    md = g / "mods" / "Metal Sonic (Roster Pack)"
    op = next(o for o in mg["operations"] if o["key"] == "0720C59E")
    new_bytes = (md / op["payload"]).read_bytes() + b"a later Metal Sonic"
    (md / op["payload"]).write_bytes(new_bytes)
    op.update(payload_sha256=hashlib.sha256(new_bytes).hexdigest(), stored_size=len(new_bytes))
    size, count, composed = xpac.compose(archive, base_rows, {int(o["key"], 16): (md / o["payload"]).read_bytes() for o in mg["operations"]})
    mg.update(expected_composed_sha256=composed, expected_composed_size=size, expected_member_count=count)
    text = json.dumps(changed_metal).encode("ascii")
    (md / "plan.json").write_bytes(text)
    ini = (md / "mod.ini").read_bytes().decode("ascii").split("\r\n")
    (md / "mod.ini").write_bytes("\r\n".join(("plan_sha256=" + hashlib.sha256(text).hexdigest()) if l.startswith("plan_sha256=") else l for l in ini).encode("ascii"))
    write_mod(g, "Steam Multiplayer", "steam-multiplayer", ours_plan, OURS, ours_desc, loader)
    r = start(g, stand_in)
    check(r["hr"] == 0 and not r["dialogs"], "(c) 1.2.0 with a changed Metal Sonic still starts every mod")
    want = expect((mg["operations"], md), ([o for o in ours_ops if not o.get("variants")], OURS))
    for k, o in variant_ops.items():
        if k != "0720C59E":
            want[int(k, 16)] = (OURS / o["variants"][0]["payload"]).read_bytes()
    compare(g, want, "(c)")
    check("member 0720C59E: steam-multiplayer's optional change is skipped; metal-sonic's is used" in r["log"], "(c) the log says which change gave way")

    # (d) 1.1.0, Metal Sonic + this mod: the player is told to update the loader
    g = new_game(work, "d-prev-release", prev, archive)
    write_mod(g, "Metal Sonic (Roster Pack)", "metal-sonic", metal_only, METAL, (METAL / "allstars-mod.json").read_bytes(), prev)
    write_mod(g, "Steam Multiplayer", "steam-multiplayer", ours_plan, OURS, ours_desc, prev)
    r = start(g, stand_in, "v2-host-release")
    text = r["dialogs"][0]["text"] if r["dialogs"] else ""
    check(len(r["dialogs"]) == 1 and "needs a newer ChaoGarage loader" in text and "xpac-member-variants" in text
          and "https://github.com/Demaunix/ChaoGarage/releases/latest" in text, "(d) 1.1.0 says: needs a newer ChaoGarage loader, with the download page")
    check("Metal Sonic" not in text.split("needs a newer")[0].rsplit("•", 1)[-1], "(d) the message is about this mod, not Metal Sonic")
    check(r["slots"] == ["original"] * 10 and not r["loaded_runtimes"], "(d) nothing started")

    # (f) 1.2.0, this PC's chain on the archive: Egg Hangar, then Metal Sonic, then this mod (folder order E < M < S)
    egg = MODS / "Egg Hangar"
    egg_group = next(g for g in json.loads((egg / "plan.json").read_text(encoding="utf-8"))["archive_groups"] if g["path"] == xpac.ARCHIVE)
    g = new_game(work, "f-egg-metal-ours", loader, archive)
    write_mod(g, "Egg Hangar", "egg-hangar", {"schema": 1, "archive_groups": [egg_group]}, egg, (egg / "allstars-mod.json").read_bytes(), loader)
    write_mod(g, "Metal Sonic (Roster Pack)", "metal-sonic", metal_only, METAL, (METAL / "allstars-mod.json").read_bytes(), loader)
    write_mod(g, "Steam Multiplayer", "steam-multiplayer", ours_plan, OURS, ours_desc, loader)
    r = start(g, stand_in)
    check(r["hr"] == 0 and not r["dialogs"], "(f) 1.2.0 with Egg Hangar and Metal Sonic starts without a message")
    want = expect((egg_group["operations"], g / "mods" / "Egg Hangar"), (metal_group["operations"], g / "mods" / "Metal Sonic (Roster Pack)"),
                  ([op for op in ours_ops if not op.get("variants")], OURS))
    for k, op in variant_ops.items():
        want[int(k, 16)] = (OURS / op["variants"][0]["payload"]).read_bytes()
    # Egg Hangar 1.2.0-menus.1 (installed 2026-10-23) carries variants of its own over Metal Sonic's members: where its variant
    # is made for Metal's payload, that variant is the member (Egg Hangar's change on top of Metal's).
    egg_variants = 0
    for op in egg_group["operations"]:
        m = metal_by_key.get(op["key"])
        for v in op.get("variants", []):
            if v.get("over_mod") == "metal-sonic" and m and v["over_payload_sha256"] == m["payload_sha256"]:
                want[int(op["key"], 16)] = (g / "mods" / "Egg Hangar" / v["payload"]).read_bytes()
                egg_variants += 1
    check(not set(variant_ops) & {op["key"] for op in egg_group["operations"]}, "(f) Egg Hangar and this mod share no member")
    compare(g, want, f"(f) ({egg_variants} of Egg Hangar's variants over Metal Sonic)")
    check(r["log"].count("steam-multiplayer's version made for metal-sonic's change is used") == SHARED, f"(f) the {SHARED} combined members")

    # (e) 1.2.0 reads every installed mod's description (loaders before 1.1.0 never did): none may stop the mods
    g = new_game(work, "e-installed-descriptions", loader, archive)
    names = []
    for d in sorted(p for p in MODS.iterdir() if (p / "mod.ini").is_file()):
        ini = dict(l.split("=", 1) for l in (d / "mod.ini").read_text(encoding="utf-8-sig").splitlines() if "=" in l)
        desc = (d / "allstars-mod.json").read_bytes() if (d / "allstars-mod.json").is_file() else None
        if d.name == "Steam Multiplayer":
            desc = ours_desc
        write_mod(g, d.name, ini["id"].strip(), {"schema": 1, "archive_groups": []}, None, desc, loader)
        names.append(d.name)
    r = start(g, stand_in)
    check(r["hr"] == 0 and not r["dialogs"], f"(e) the {len(names)} installed mods' descriptions pass the 1.2.0 loader's checks: {', '.join(names)}"
          + ("" if not r["dialogs"] else " -- message: " + r["dialogs"][0]["text"][:600]))

    print(f"{'PASS' if not failures else 'FAIL'}: {len(failures)} failed check(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
