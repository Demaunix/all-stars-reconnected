"""Assemble out/Steam Multiplayer/ with mod.ini hashes that match the files."""
import hashlib, json, re, shutil, subprocess, sys
from pathlib import Path

root = Path(sys.argv[1]).resolve()
mod = root / "out" / "Steam Multiplayer"
if mod.exists():
    shutil.rmtree(mod)
(mod / "runtime").mkdir(parents=True)
shutil.copy2(root / "build" / "SteamMultiplayer.dll", mod / "runtime" / "SteamMultiplayer.dll")
shutil.copy2(root / "third_party/steamworks/sdk/redistributable_bin/steam_api.dll", mod / "runtime" / "steam_api.dll")
shutil.copy2(root / "package" / "config.ini", mod / "config.ini")

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

# The Multiplayer menu's tiles say STEAM: tools/steam_tiles.py builds the scene from this PC's game archive
# into build/tiles/ (run without -I: it needs numpy, Pillow and OpenCV), again only when the tool changed.
tiles = root / "build" / "tiles"
def tiles_stamp():
    try:
        s = json.loads((tiles / "group.json").read_text(encoding="utf-8"))
        ok = s["tool_sha256"] == sha(root / "tools" / "steam_tiles.py") and all(
            sha(tiles / Path(op["payload"]).name) == op["payload_sha256"] for op in s["group"]["operations"])
        return s if ok else None
    except (OSError, ValueError, KeyError):
        return None
if not tiles_stamp():
    subprocess.run([sys.executable, str(root / "tools" / "steam_tiles.py"), str(root)], check=True, stdout=subprocess.DEVNULL)
group = tiles_stamp()["group"]
for op in group["operations"]:
    payload = mod / op["payload"]
    payload.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(tiles / payload.name, payload)

# The footer's key pictures with the A cap (INVITE FRIENDS / VIEW PROFILE) and the dialogs text table with the popup's VIEW STEAM
# PROFILE and UNMUTE (4 more languages' tables since M8), and the 7 lobby scenes with a muted player's yellow bar
# (tools/footer_keys.py, run without -I), in this mod's own plan since ChaoGarage 1.2.0. Metal Sonic replaces these very
# members, so each operation also carries a variant made on Metal Sonic 1.1.0 Beta 3's version (the same payloads in the standalone mod
# and the Roster Pack layer), used when that is the installed content, and is optional: a Metal Sonic whose member differs (a later
# update) keeps its own and only the A picture and the wording are missing, instead of the loader starting no mod.
keys = root / "build" / "keys"
# Metal Sonic's versions of these members, each a base under reference\navigator-bases (file suffix for its variant payloads).
# 1.1.4 (2026-10-27) adds "METAL SONIC" to its 7 navigator_info scenes; its other 19 members are 1.1.3's (= Beta 3's), so they
# add no second variant (the loader refuses two variants over the same payload).
METALS = {"metal-sonic-1.1.0-beta.3": ".metal-sonic.dat", "metal-sonic-1.1.4": ".metal-sonic-114.dat"}
def keys_stamp():
    try:
        s = json.loads((keys / "keys.json").read_text(encoding="utf-8"))
        ok = s["tool_sha256"] == sha(root / "tools" / "footer_keys.py") and all(
            s["inputs"][name] == sha(root / "tools" / name) for name in ("steam_tiles.py", "text_tables.py", "dialog_icons.py", "scene_container.py", "lobby_bars.py",
                                                                        "egg_lobby.py", "mod_icons.py", "lobby_drift_row.py", "codexui/build_ui.py", "codexui/decode_ui_texture.py", "codexui/probe.py")) and all(
            sha(keys / base / Path(op["payload"]).name) == op["payload_sha256"] for base, g in s["bases"].items() for op in g["operations"]) and all(
            base in s["bases"] for base in METALS)
        return s if ok else None
    except (OSError, ValueError, KeyError):
        return None
if not keys_stamp():
    subprocess.run([sys.executable, str(root / "tools" / "footer_keys.py"), str(root)], check=True, stdout=subprocess.DEVNULL)
stamp = keys_stamp()
metals = {base: {op["key"]: op for op in stamp["bases"][base]["operations"]} for base in METALS}
tiles_keys = {op["key"] for op in group["operations"]}
for op in stamp["bases"]["stock"]["operations"]:
    assert op["key"] not in tiles_keys and all(op["key"] in m for m in metals.values()), op["key"]
    payload = mod / op["payload"]
    payload.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(keys / "stock" / payload.name, payload)
    entry = {k: v for k, v in op.items() if k != "facts"}
    entry["optional"] = True
    entry["variants"] = []
    for base, suffix in METALS.items():
        m = metals[base][op["key"]]
        if any(v["over_payload_sha256"] == m["base_stored_sha256"] for v in entry["variants"]):
            continue  # (this Metal version's member is an earlier one's: that variant serves both)
        variant = op["payload"].replace(".dat", suffix)
        shutil.copy2(keys / base / Path(m["payload"]).name, mod / variant)
        entry["variants"].append({"over_mod": "metal-sonic", "over_payload_sha256": m["base_stored_sha256"], "payload": variant,
                                  "payload_sha256": m["payload_sha256"], "stored_size": m["stored_size"]})
    group["operations"].append(entry)
sys.path.insert(0, str(root / "tools"))
import xpac  # noqa: E402  (the archive's rows and the composed-archive hash, standard library only)
assert group["path"] == xpac.ARCHIVE and group["base_sha256"] == xpac.BASE_SHA256
_, rows = xpac.read_rows(xpac.GAME / xpac.ARCHIVE)
size, count, composed = xpac.compose(xpac.GAME / xpac.ARCHIVE, rows, {int(op["key"], 16): (mod / op["payload"]).read_bytes() for op in group["operations"]})
group.update(expected_composed_sha256=composed, expected_composed_size=size, expected_member_count=count)
plan = {"schema": 1, "archive_groups": [group]}
(mod / "plan.json").write_bytes((json.dumps(plan, indent=2) + "\n").encode("ascii"))

version = re.search(r'#define MOD_VERSION "([^"]+)"', (root / "src/steam_mp.cpp").read_text()).group(1)
# The loader features the plan's key-picture operations use: ChaoGarage 1.1.0 then says "needs a newer ChaoGarage loader" (with the
# download page) instead of refusing every mod for a conflict with Metal Sonic. Loaders before 1.1.0 don't read this file.
desc = {"manifest_schema": "allstars-mod/1", "must_understand": ["loader-requirements/1"], "id": "steam-multiplayer", "name": "Steam Multiplayer",
        "version": version, "loader": {"capabilities": ["xpac-member-variants", "xpac-member-optional"]}}
(mod / "allstars-mod.json").write_bytes((json.dumps(desc, indent=2) + "\n").encode("ascii"))
for doc in ("README.txt", "CHANGELOG.txt"):  # the version stamped in; Windows line ends for Notepad
    text = (root / "package" / doc).read_text(encoding="utf-8").replace("\r\n", "\n").replace("{VERSION}", version)
    (mod / doc).write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
ini = (
    "[mod]\r\nschema=1\r\nid=steam-multiplayer\r\nname=Steam Multiplayer\r\n"
    f"version={version}\r\nruntime=runtime\\SteamMultiplayer.dll\r\nplan=plan.json\r\n"
    f"plan_sha256={sha(mod / 'plan.json')}\r\nruntime_sha256={sha(mod / 'runtime' / 'SteamMultiplayer.dll')}\r\n"
)
(mod / "mod.ini").write_bytes(ini.encode("ascii"))
for p in sorted(mod.rglob("*")):
    if p.is_file():
        print(sha(p), p.relative_to(mod).as_posix())
# The separate "Steam Multiplayer Keys" folder of builds m7zz-m8c is gone (its pictures are in this mod's plan now).
if (root / "out" / "Steam Multiplayer Keys").exists():
    shutil.rmtree(root / "out" / "Steam Multiplayer Keys")
