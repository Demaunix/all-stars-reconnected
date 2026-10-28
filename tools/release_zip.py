"""The public release zip: out/Steam Multiplayer/ (as tools/package.py built it) as one folder "Steam Multiplayer" inside
out/release/SteamMultiplayer-<version>.zip, with SHA256SUMS.txt beside it. Entries are sorted and dated 2026-01-01 so the same
build gives the same zip bytes. Refuses a build that carries debug settings ([Debug] in config.ini) or files that are not the
mod's (logs, caches), and checks that the zip reads back byte-identical to the folder.

    python -I tools/release_zip.py <repo root>
"""
import hashlib
import re
import sys
import zipfile
from pathlib import Path

DATE = (2026, 1, 1, 0, 0, 0)
ALLOWED_TOP = {"mod.ini", "plan.json", "allstars-mod.json", "config.ini", "README.txt", "CHANGELOG.txt", "runtime", "payloads"}


def sha(data: bytes) -> str:
    """SHA-256 hex."""
    return hashlib.sha256(data).hexdigest()


def main() -> None:
    """Build the zip and its checksums."""
    root = Path(sys.argv[1]).resolve()
    mod = root / "out" / "Steam Multiplayer"
    ini = (mod / "mod.ini").read_text(encoding="ascii")
    version = re.search(r"^version=(.+)$", ini, re.M).group(1).strip()
    config = (mod / "config.ini").read_text(encoding="utf-8")
    assert "[Debug]" not in config, "config.ini carries debug settings"
    files = sorted(p for p in mod.rglob("*") if p.is_file())
    tops = {p.relative_to(mod).parts[0] for p in files}
    assert tops <= ALLOWED_TOP, f"files that are not the mod's: {sorted(tops - ALLOWED_TOP)}"
    out = root / "out" / "release"
    out.mkdir(parents=True, exist_ok=True)
    zpath = out / f"SteamMultiplayer-{version}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for p in files:
            info = zipfile.ZipInfo("Steam Multiplayer/" + p.relative_to(mod).as_posix(), DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, p.read_bytes())
    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        assert len(names) == len(files)
        for p in files:
            assert z.read("Steam Multiplayer/" + p.relative_to(mod).as_posix()) == p.read_bytes(), p
    sums = f"{sha(zpath.read_bytes())}  {zpath.name}\n"
    for name in ("runtime/SteamMultiplayer.dll", "runtime/steam_api.dll"):
        sums += f"{sha((mod / name).read_bytes())}  Steam Multiplayer/{name}\n"
    (out / "SHA256SUMS.txt").write_text(sums, encoding="ascii", newline="\n")
    print(f"{zpath} ({zpath.stat().st_size} bytes, {len(files)} files)")
    print(sums, end="")


if __name__ == "__main__":
    main()
