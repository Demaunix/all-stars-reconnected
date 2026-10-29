"""A local test release for the in-game auto-update test (notes/COMBINED-TEST-1.1.1.md): out/Steam Multiplayer copied with
mod.ini's version set to the given number, a STORED zip, and update.txt + .sig from tools/release_update.py, all in
build/combined-test/release. Signed with the REAL current key (the DLL trusts nothing else), so it stays in build/ and
update.txt/.sig are deleted right after the test (`--clean`).

    python -I tools/make_test_release.py <repo root> <version>      e.g. 1.1.1.1
    python -I tools/make_test_release.py <repo root> --clean
"""
import re
import shutil
import subprocess
import sys
import zipfile
from pathlib import Path


def main() -> None:
    """Make (or clean) the test release."""
    root = Path(sys.argv[1]).resolve()
    work = root / "build" / "combined-test"
    release = work / "release"
    if sys.argv[2] == "--clean":
        for p in (release / "update.txt", release / "update.txt.sig"):
            if p.exists():
                p.unlink()
                print(f"deleted {p}")
        return
    version = sys.argv[2]
    assert re.fullmatch(r"\d{1,6}(\.\d{1,6}){0,3}", version), version
    nxt = work / "next" / "Steam Multiplayer"
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(root / "out" / "Steam Multiplayer", nxt)
    for log in nxt.glob("*.log"):
        log.unlink()
    ini = (nxt / "mod.ini").read_bytes().decode("ascii")
    assert ini.count("\nversion=") == 1
    ini = re.sub(r"\nversion=[^\r\n]*", "\nversion=" + version, ini)
    (nxt / "mod.ini").write_bytes(ini.encode("ascii"))
    release.mkdir(parents=True)
    zpath = release / f"SteamMultiplayer-{version}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_STORED) as z:
        for p in sorted(q for q in nxt.rglob("*") if q.is_file()):
            z.write(p, "Steam Multiplayer/" + p.relative_to(nxt).as_posix())
    subprocess.run([sys.executable, "-I", str(root / "tools" / "release_update.py"), str(root), "--mod", str(nxt), "--zip", str(zpath),
                    "--out", str(release)], check=True)
    print(f"test release {version}: {release}")


if __name__ == "__main__":
    main()
