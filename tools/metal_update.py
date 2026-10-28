r"""A Metal Sonic update that carries the footer key picture (the A cap for the Steam Multiplayer mod's INVITE FRIENDS / VIEW
PROFILE) and the dialog text with VIEW STEAM PROFILE.

The loader allows one mod per archive member, and Metal Sonic replaces the seven navigator_info scenes (its icon art) and
the English dialogs text table, so those changes have to travel inside Metal Sonic for players who have it. This tool takes
the published package and replaces those eight payloads with the ones tools\footer_keys.py built on top of Metal's own
(build\keys\<base>\), then updates everything that pins them: the plan's operations and the archive group's composed hash,
mod.ini's plan hash and version, the root MANIFEST.sha256, VERSION.txt and the docs' version lines, and adds a changelog
entry.

    python tools\metal_update.py <repo root> "<Metal Sonic 1.1.0 Beta 3.zip>" <new version, e.g. 1.1.0-beta.4>

Writes out\metal-update\Metal Sonic <Version>\ (the package's layout) and out\metal-update\Metal Sonic <Version>.zip. Nothing is
installed; the game and the installed mods are not touched. The Roster Pack's Metal layer on this PC can NOT be updated this
way: AllStarsRosterPack.dll pins the layer's plan hash and refuses to start with another (found by the m7zz check).
"""
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from xpac import ARCHIVE, GAME, compose, read_rows  # noqa: E402

SCENES = (0x0720C59E, 0x08538F72, 0x0B11E779, 0x11C1615B, 0x5706E9CD, 0xA4877A54, 0xDFB2DA15,  # the 7 navigator_info scenes
          0x9421F31B)  # dialogs_EN: the lobby player menu's VIEW GAMER CARD reads VIEW STEAM PROFILE (tools\text_tables.py)
BASE = "metal-sonic-1.1.0-beta.3"  # build\keys\<BASE>\: the payloads built on Metal's navigator scenes
SEVEN_ZIP = r"C:\Program Files\7-Zip\7z.exe"
CHANGELOG_ENTRY = (
    "METAL SONIC {TITLE}\n\n"
    "- Added an \"A\" key picture to the menus' key font (the seven navigator_info\n"
    "  scenes, every language), so the Steam Multiplayer mod can show its INVITE\n"
    "  FRIENDS / VIEW PROFILE key in its footers; in the English dialog texts,\n"
    "  VIEW GAMER CARD reads VIEW STEAM PROFILE (that mod's lobby player menu).\n"
    "  Nothing else changed.\n\n"
)


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def composed(archive: Path, members: dict) -> tuple:
    """(size, count, sha256) of the loader's virtual archive: the game's archive with `members` {key: bytes} replacing or adding."""
    _, rows = read_rows(archive)
    return compose(archive, rows, members)


def update_mod_folder(mod: Path, keys: Path, version: str) -> list:
    """The seven payloads, plan.json, mod.ini (and allstars-mod.json / CHECKSUMS.sha256 when present) of a mod folder; the ops changed."""
    plan_path = mod / "plan.json"
    plan_text = plan_path.read_text(encoding="utf-8")
    plan = json.loads(plan_text)
    indent = 2 if "\n  \"" in plan_text[:200] else (4 if "\n    \"" in plan_text[:200] else None)
    group = next(g for g in plan["archive_groups"] if g.get("path", "").replace("\\", "/") == ARCHIVE)
    changed = []
    for op in group["operations"]:
        key = int(op["key"], 16)
        if key not in SCENES:
            continue
        payload = (keys / ("%08X.dat" % key)).read_bytes()
        target = mod / op["payload"]
        assert target.exists(), f"payload missing: {target}"
        target.write_bytes(payload)
        op["payload_sha256"] = sha(payload)
        for field in ("stored_size", "decoded_size", "entry_size2"):
            if field in op:
                op[field] = len(payload)
        changed.append(op["key"])
    assert len(changed) == len(SCENES), f"{len(changed)} of {len(SCENES)} operations found"
    members = {}
    for op in group["operations"]:
        members[int(op["key"], 16)] = (mod / op["payload"]).read_bytes()
        assert sha(members[int(op["key"], 16)]) == op["payload_sha256"], f"payload hash mismatch: {op['key']}"
    size, count, digest = composed(GAME / ARCHIVE, members)
    assert count == group["expected_member_count"], f"member count {count} vs the plan's {group['expected_member_count']}"
    group["expected_composed_sha256"], group["expected_composed_size"] = digest, size
    plan_path.write_bytes((json.dumps(plan, indent=indent) + ("\n" if plan_text.endswith("\n") else "")).encode("utf-8"))
    ini_path = mod / "mod.ini"
    ini = ini_path.read_bytes().decode("utf-8")
    ini = re.sub(r"(?m)^plan_sha256=[^\r\n]*", "plan_sha256=" + sha(plan_path.read_bytes()), ini)  # (keeps the line's \r)
    ini = re.sub(r"(?m)^version=[^\r\n]*", "version=" + version, ini)
    ini_path.write_bytes(ini.encode("utf-8"))
    sidecar = mod / "allstars-mod.json"
    if sidecar.exists():
        text = sidecar.read_text(encoding="utf-8")
        data = json.loads(text)
        if "version" in data:
            data["version"] = version
        sidecar.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    checksums = mod / "CHECKSUMS.sha256"
    if checksums.exists():
        write_manifest(checksums, mod)
    return changed


def write_manifest(path: Path, root: Path) -> None:
    """`<sha256>  <relative path>` for every file below root but the manifest itself, sorted, keeping the file's line ending."""
    old = path.read_bytes() if path.exists() else b""
    eol = "\r\n" if b"\r\n" in old else "\n"
    lines = []
    for p in sorted(root.rglob("*")):
        if p.is_file() and p != path:
            lines.append(f"{sha(p.read_bytes())}  {p.relative_to(root).as_posix()}")
    path.write_bytes((eol.join(lines) + eol).encode("utf-8"))


def retitle(path: Path, old_version: str, version: str) -> None:
    """Version words in a doc: "1.1.0 Beta 3" / "BETA 3" / "beta.3" -> the new ones."""
    if not path.exists():
        return
    raw = path.read_bytes()
    text = raw.decode("utf-8")
    o, n = old_version.split("-beta."), version.split("-beta.")
    if len(o) == 2 and len(n) == 2:
        text = text.replace(f"{o[0]} Beta {o[1]}", f"{n[0]} Beta {n[1]}").replace(f"{o[0]} BETA {o[1]}", f"{n[0]} BETA {n[1]}")
    text = text.replace(old_version, version)
    path.write_bytes(text.encode("utf-8"))


def main() -> None:
    root, source, version = Path(sys.argv[1]).resolve(), Path(sys.argv[2]), sys.argv[3]
    keys = root / "build" / "keys" / BASE
    assert all((keys / ("%08X.dat" % key)).exists() for key in SCENES), f"run tools\\footer_keys.py first ({keys})"
    out = root / "out" / "metal-update"
    out.mkdir(parents=True, exist_ok=True)
    src = out / "src"
    if src.exists():
        shutil.rmtree(src)
    subprocess.run([SEVEN_ZIP, "x", "-y", f"-o{src}", str(source)], check=True, stdout=subprocess.DEVNULL)
    mod = src / "mods" / "Metal Sonic"
    old_version = re.search(r"(?m)^version=([^\r\n]*)", (mod / "mod.ini").read_text()).group(1).strip()
    title = version.replace("-beta.", " Beta ")
    dst = out / f"Metal Sonic {title}"
    if dst.exists():
        shutil.rmtree(dst)
    shutil.copytree(src, dst)
    changed = update_mod_folder(dst / "mods" / "Metal Sonic", keys, version)
    for doc in ("README.txt", "INSTALL.txt", "VERSION.txt", "KNOWN ISSUES.txt", "Player Bug Report Template.txt"):
        retitle(dst / doc, old_version, version)
    log = dst / "CHANGELOG.txt"
    text = log.read_bytes().decode("utf-8")
    eol = "\r\n" if "\r\n" in text else "\n"
    log.write_bytes((CHANGELOG_ENTRY.replace("{TITLE}", title.upper()).replace("\n", eol) + text).encode("utf-8"))
    write_manifest(dst / "MANIFEST.sha256", dst)
    zip_path = out / f"Metal Sonic {title}.zip"
    if zip_path.exists():
        zip_path.unlink()
    subprocess.run([SEVEN_ZIP, "a", "-tzip", "-mx=5", str(zip_path), "*"], cwd=dst, check=True, stdout=subprocess.DEVNULL)
    print(f"{dst}\n{zip_path} ({zip_path.stat().st_size} bytes, sha256 {sha(zip_path.read_bytes())})\nops changed: {changed}")


if __name__ == "__main__":
    main()
