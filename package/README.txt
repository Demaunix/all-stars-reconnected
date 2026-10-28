Steam Multiplayer for Sonic & SEGA All-Stars Racing (PC, Steam)
Version {VERSION}

Online races with your friends over Steam, through the game's own Multiplayer
menus: Play with Friends, Play with Everyone, Steam "Join Game" and invites.
Up to 8 players per lobby, as on the consoles; the other grid places are computer
racers that look the same on every screen.


REQUIREMENTS
- Sonic & SEGA All-Stars Racing on Steam (PC). Other versions don't work.
- The ChaoGarage mod loader, version 1.2.0 or newer (both files):
  https://github.com/Demaunix/ChaoGarage/releases/latest
  ChaoGarage 1.1.0 says "needs a newer ChaoGarage loader" and starts no mod. Older
  ones can't start this mod together with Metal Sonic (no mod starts then).
  Mod zips that came with their own ChaoGarage.dll and dinput8.dll (for example
  Metal Sonic 1.1.0 Beta 3) have an older one: replace both files.
- Every player needs the same mods, and the same version of this mod. The host
  can leave Metal Sonic and Egg Hangar OFF for a game (see HOW TO PLAY); then players
  without them can join too. A join with a different mod set is refused with a
  message naming the difference; SteamMultiplayer.log in this mod's folder lists
  every difference.
- Metal Sonic: the standalone Metal Sonic 1.1.2 and the Roster Pack's Metal Sonic
  both work online, but not with each other (their racer lists differ). The
  Roster Pack's Metal Sonic needs the All-Stars Roster Pack, which every player
  needs too, even when Metal is OFF. The standalone Metal Sonic 1.1.0 Beta 3 isn't
  supported: update it to 1.1.2.


INSTALL
1. Back up your save: copy ssr_save.bin and settings.ini from the game folder
   (Steam: right-click the game > Manage > Browse local files) to a safe place.
2. Install ChaoGarage 1.2.0 or newer: copy BOTH ChaoGarage.dll and dinput8.dll
   from its zip into the game folder (both from the same release), replacing
   any older copies.
   If the game folder already has a dinput8.dll from another tool, move it away first.
3. Copy the "Steam Multiplayer" folder from this zip into the game's "mods" folder,
   so you get: <game folder>\mods\Steam Multiplayer\mod.ini
4. Start the game from Steam.
The A key's picture (INVITE FRIENDS, VIEW PROFILE) and the lobby's VIEW STEAM
PROFILE wording come with this mod (it also carries a version of those menu files
made for Metal Sonic's). If a later Metal Sonic changes them again, Metal Sonic's
own version is used and only that picture or wording is missing.

UNINSTALL
Delete the folder <game folder>\mods\Steam Multiplayer\. Nothing else is changed.


HOW TO PLAY
- Multiplayer > Play with Friends (friends' games) or Play with Everyone (public).
  Create a game, or pick one from the list and press Enter to join.
- With Metal Sonic or Egg Hangar installed, CREATE asks "Would you like to enable
  your modifications?". NO: the game is played without them. YES: a list lets you
  turn each one ON or OFF (Up/Down pick, Left/Right switch, Enter creates the game).
  Esc closes the question without creating a game. A mod that is OFF isn't used in
  that game, and players who don't have it can join.
- Pick your racer. The host chooses the course, laps and rules in Settings and
  starts the race when everyone is ready. With the Egg Hangar mod installed (by
  every player, like any mod) the course list also has Egg Hangar, as the consoles'
  Metal Sonic pack added it to their online lobby.
- Steam needs a short while after the game starts before online races can begin
  (up to a few minutes on some PCs). The host's Start waits for everyone by itself.
- At the course intro, Enter skips it on your own screen; left alone it ends once
  the announcer and the music have finished. The racer intro follows (Enter skips it
  too), and the countdown starts together on every screen.
- After the race, RETURN TO LOBBY brings everyone back to the lobby. The pause
  menu (Esc) also has RETURN TO LOBBY.
- Steam "Join Game" on a friend's name, or an invite, also works, even when the
  game is not running yet. Joining while a race runs: you wait in the lobby and
  see the race's progress; the racers see "X HAS JOINED THE LOBBY."
- If the host leaves the lobby, the lobby goes on with a new host. If the other
  players leave mid-race, the console's message tells you and takes you back.
- The lobby works as the console and Mac versions did, with the Mac's keys: Up/Down
  move the red bar over the players. F1 on your own bar changes your racer; F1 on
  another player's bar opens their menu: VIEW STEAM PROFILE, KICK (host only; it asks
  first, and a kicked player can't rejoin that lobby), RETURN TO LOBBY. Enter starts
  the race (host); it says why when it can't (one player only, not all ready). F2 is
  the host's SETTINGS, the A key is INVITE FRIENDS (see [Menus] InviteKey below). Esc asks
  before you leave; a host leaving ends the session for the other players.
- Lost connections, a full game, a failed create or a kick show the console's
  messages; OK takes you back to the main menu.
- Voice chat: hold V to talk to the players in your lobby, in the lobby and during
  the race (the speech bubble on a player's bar shows who is talking). See [Voice]
  below for an open microphone, another key, the volume, or muting players.
- The game list: your friends' games first, then public games. STATUS says LOBBY,
  or GAME while that game races. MODS shows the Metal Sonic and Egg Hangar icons of
  a game that uses them: a red X means you don't have that mod, an orange dash means
  you have another version of it (joining is refused and says why). A game of another
  version of this mod, or with other mods, is listed as "(other version)" /
  "(other mods)" and can't be joined.
  F2 searches again (REFRESH), A shows the selected host's Steam profile (VIEW
  PROFILE), F1 creates a game. On the results screen, Up/Down pick a player and F1
  (VIEW PROFILE) shows their Steam profile.
- If Steam loses its connection, the lobby ends with "The connection to Steam has
  been lost." and online games can't be found or hosted until Steam is back.
- Controllers: the menus show the keyboard's keys; on an Xbox-style pad A = Enter
  (START RACE, select), B = Esc (BACK), X = F1 (CHANGE RACER, a player's menu),
  Y = F2 (SETTINGS), the D-pad = the arrow keys. INVITE FRIENDS and push-to-talk
  are keyboard-only for now (see KNOWN ISSUES).


SETTINGS (config.ini in this folder; read when the game starts)
- [Background] RunWhenUnfocused=1 keeps the game running while its window is not
  focused or is being dragged (needed online). Keys are ignored while another
  window has focus or the game is minimized. 0 = the game pauses as before.
- [Menus] Enabled=0 gives back the game's own Multiplayer menu (split screen only).
- [Menus] InviteKey=A: INVITE FRIENDS in the lobby and VIEW PROFILE on the game list
  (the consoles' triangle / Y button; same key names as the voice chat's Key). Its
  footer picture is always the A cap; another key still works, but the footer
  shows the A cap.
- [Race] HostRunsAI=1: as host, your game drives the computer racers for everyone.
- [Race] ItemSync=1: as host, everyone's items and All-Star moves show on every
  screen, and so do the drivers' reactions (taunts, celebrations).
Leave the [Race] settings at 1 unless you are looking into a problem.
- [Voice] Mode=ptt (push-to-talk, hold Key), open (open microphone while in a
  lobby) or off; Key=V (a letter or digit, F1-F24, SPACE, CTRL, SHIFT, ALT...; it
  works only while the game window is in front and the Steam overlay is closed);
  Volume=100 (0-300 percent; above 100 makes quiet players louder); Muted= Steam
  IDs (17-digit numbers, comma-separated) you
  don't want to hear (always muted). In the lobby, F1 on a player's bar > MUTE
  mutes that player for you until the game closes (UNMUTE in the same place undoes
  it); a muted player's bar is yellow (with another mod's version of the lobby
  screen it stays blue and says "(muted)"). Voice goes through Steam; Steam's
  microphone choice applies: if you sound quiet or rough to the others, check
  Steam > Settings > Voice (the input device, its volume, noise suppression, and
  Steam's own microphone test there).


KNOWN ISSUES
- Pausing doesn't stop an online race, as on the consoles: the computer drives your
  car until you pick CONTINUE. On Egg Hangar it can keep missing the jumps just
  after the start line while it drives; pick CONTINUE and drive yourself.
- A player who leaves during a race: their car goes on as a computer racer. If the
  host leaves during a race, the race ends for everyone (the host is asked first).
- There is no restart during an online race; RETURN TO LOBBY instead.
- Voice chat plays through its own sound output (XAudio2); Windows 7/8 PCs without
  xaudio2_9.dll hear nothing (the log says so), their own voice still goes out.
  Voice is played the moment it arrives, so on a poor connection it can sound
  choppy.
- INVITE FRIENDS and push-to-talk have no controller button yet: invite through
  the Steam overlay (Shift+Tab), or set [Voice] Mode=open to talk without a key.
- Tested with up to 4 players over the internet. Lobbies of 5 to 8 players have
  not been tested yet.


TROUBLESHOOTING
- The game went invisible after Alt+Tab in fullscreen and won't come back: end it in
  Task Manager and send us mods\Steam Multiplayer\SteamMultiplayer.log (it says what
  the game was doing). Meanwhile, playing in a window (settings.ini WINDOWED=1 in
  the game's folder) is worth a try.
- Steam says "Application load error 3:0000065432" when you start the game: in
  Steam, right-click the game > Properties > Installed Files > Verify integrity of
  game files, then start it again. Your mods folder and this mod stay as they are.


FILES
- runtime\SteamMultiplayer.dll   the mod
- runtime\steam_api.dll          Valve's Steamworks SDK 1.65 redistributable (32-bit)
- config.ini                     settings (above)
- SteamMultiplayer.log           written while the game runs; send it along with
                                 any bug report
