# Auto-update: research (2026-10-27, researcher pass; read-only, nothing built)

The user's request (2026-10-27): after the DRIFT+ row, the public GitHub repo AND an auto-update, so changes to the user's other
mods (e.g. Metal Sonic) can bring matching Steam Multiplayer / ChaoGarage updates automatically; a "check for updates" setting
turns it off (then players reinstall by hand). **The user's decision (2026-10-27): auto-update is OFF by default** (players
turn it on themselves; also the safest framing for GameBanana's unknown rules). Earlier design notes: PROGRESS.md "auto-update for Steam Multiplayer" (signed,
the private key only on the user's PC, apply at the next restart through a helper, off switch in config.ini).
Labels: CONFIRMED (read in a source) / INFERRED / UNKNOWN.

## Constraints found
- ChaoGarage never contacts the network today (ChaoGarage's `docs/DESIGN.md` line 34; CONFIRMED); SHA256SUMS.txt "is not a
  signature" (line 36). The dinput8 + ChaoGarage pair refuses to load if the two come from different releases (line 10): an update
  replaces the whole pair.
- **GameBanana's rules: UNKNOWN** (the rules / submission-rules wiki pages didn't render for the researcher:
  https://gamebanana.com/rules, https://gamebanana.com/wikis/cats/51). Read them by hand or ask staff BEFORE shipping. For
  comparison, Nexus prohibits files that "connect to the internet to download or send information" unless crucial
  (https://help.nexusmods.com/article/28-file-submission-guidelines). Safest framing (INFERRED): opt-in check, downloads only what
  the player approves, disclosed on the page.
- GameBanana API (CONFIRMED): `https://gamebanana.com/apiv11/Mod/<id>/ProfilePage` has `_sVersion`, `_tsDateUpdated`, `_aFiles[]`
  (`_sFile`, `_sDownloadUrl`, `_tsDateAdded`, `_sVersion`). Reloaded-II uses GameBanana / GitHub Releases / NuGet as update
  sources (https://reloaded-project.github.io/Reloaded-II/EnablingUpdateSupport/).
- GitHub (CONFIRMED): unauthenticated 60 requests / hour / IP; `GET /repos/{owner}/{repo}/releases/latest` (full releases only;
  assets with `browser_download_url`, nullable `digest`); ETag / If-None-Match (an unauthenticated 304 still counts). Check once a
  day at most, fail quietly, respect `x-ratelimit-reset` / `x-poll-interval`; send a User-Agent (INFERRED).

## Signing
- Windows CNG has no Ed25519 signature algorithm (CONFIRMED,
  https://learn.microsoft.com/en-us/windows/win32/seccng/cng-algorithm-identifiers); it has ECDSA P-256 and SHA-256.
- Monocypher (2 files, CC0/BSD-2, ~50 KB; https://monocypher.org/) has Ed25519 (optional SHA-512 variant): the likely verifier.
- minisign (Ed25519, prehashed "ED" with Blake2b-512, a signed trusted comment that can carry version + file name, scrypt-encrypted
  secret key; https://jedisct1.github.io/minisign/).
- Suggested (INFERRED): sign SHA256SUMS.txt (minisign or the same scheme), verify that one signature in the DLL, then each file's
  SHA-256 (BCrypt); the signed comment carries the version and the updater refuses anything not newer; the private key stays off
  GitHub (an offline / backed-up drive, passphrase); two public keys embedded (current + spare) for rotation. Authenticode needs a
  paid certificate: out of scope.

## Applying
- Squirrel (Discord's family) installs each update into a new versioned folder and checks SHA1s from RELEASES (CONFIRMED,
  https://github.com/Squirrel/Squirrel.Windows/blob/develop/docs/using/update-process.md).
- INFERRED for us: download into a staged folder, verify there, swap at the next launch BEFORE mods load (dinput8 runs first),
  keep `.old` copies for rollback; the loader pair only as a whole. Whether dinput8 can do the swap: to check (it would be a
  ChaoGarage change: the user's explicit word).

## Privacy
- A check reveals the IP, a User-Agent and the timing to GitHub (INFERRED); send nothing else. Disclose it on the GameBanana page and
  the README, off switch in config.ini, a first-run notice.

## Not found
GameBanana's actual rule text; Reloaded-II's verification / swap details; BepInEx, r2modman / Thunderstore, MO2, Ultimate ASI Loader,
SpecialK.
