"""Stage 5 (ours): the Search/Create/Lobby flow on Steam through our group-0x7A natives.

Replaces only search.make_chunks' Search screen and lobby wrapper; the lobby, racer popup and
Settings stages underneath are Codex's, unchanged. Native numbers: src/natives.h. Design and the
decoded original (Mac) screens: notes/STEAM-BRIDGE.md.

Search screen (FLOW == 0), ACTION states:
  -2 start a search when the screen is ready   -3 searching (the SEARCHING spinner)   -4 browsing result rows
  -6 show the error notice   -1 notice open   0/1/2 notice answered Refresh/Create/Cancel
  -7 back from an online race: re-enter the lobby
Lobby (FLOW == 1): Create and Join show it at once with the CREATING / JOINING spinner and wait (WAITING) until Steam
is done; leaves Steam on Back / racer-popup cancel; redraws the other players' rows when the Steam snapshot changes;
on a Steam error (host left, creation or join failed) returns to Search with a notice.
"""
import struct

from netmenu import racer, search, settings

base, racer, Code = search.base, search.racer, search.Code
FLOW, ACTION, AUTO_CREATE, CREATING, CREATE_RESULT = search.FLOW, search.ACTION, search.AUTO_CREATE, search.CREATING, search.CREATE_RESULT
SEL, PREV, ROW_COUNT, LOBBY_REV, MEMBERS, LAST_ERROR, START_RESULT = range(0x4b, 0x52)
HILITE, WANT = 0x52, 0x53  # the lobby bar shown highlighted (this player's; -1 after Show), and the one wanted
RACER_KEY = 0x54           # draw_bars: the bar's racer as read once (a hash, or int 0)
LAST_HOST_CHANGE = 0x55    # the host handover count last noticed (M7 host migration)
CURSOR, POPUP_STATE = 0x56, 0x57  # the lobby cursor over the bars (-1 = not set yet: this player's bar); the player popup / confirms' state
ERR_MAIN = 0x58            # the search screen's error notice was the console's OK box: its answer goes back to the main menu
ME_LAST = 0x59             # this player's bar at the last draw (the cursor follows this player's bar when the bars shift)
WAITING = 0x5a             # M8: the lobby shows the console's CREATING / JOINING spinner until Steam's create / join is done
SPIN_AGE = 0x5b            # M8: frames the busy spinner has been up (it stays one full turn, so a quick Steam answer doesn't flicker it)
AUTO_HELD = 0x5c           # M8: the racer picker's first opening waits while a race runs here (a mid-race joiner, re\m8-audit A4)
FLAG_KEY = 0x5d            # M8: show_results: the row's flag sprite as read once (a hash, or int 0)
# 1.1 ENABLE MODIFICATIONS (notes\ENABLE-MODS.md): the CREATE question's state (0 none; see mods_frame) and its answers so far
# (1 = on: Metal Sonic, Egg Hangar; first "installed").
MODS_STATE, MODS_METAL, MODS_EGG = 0x5e, 0x5f, 0x60
# The MODIFICATIONS popup (mods_open): its cursor row, its row count, Metal Sonic installed (row 0 is Metal's), our 2 text buffers.
MODS_ROW, MODS_ROWS, MODS_HAVE_METAL, MODS_BUFS = 0x61, 0x62, 0x63, 0x64
GLOBAL_COUNT = 0x65
RETURNED = 0x22  # stock global: 1 = front_main returned through export 0x45b79703 (notes/RETURN-TO-LOBBY.md)

# src/natives.h
NAT_STATE, NAT_ERROR, NAT_HOST, NAT_SEARCH, NAT_JOIN, NAT_LEAVE = 0x7a01, 0x7a02, 0x7a03, 0x7a04, 0x7a05, 0x7a06
NAT_ROWS, NAT_ROW_TEXT, NAT_OTHERS, NAT_OTHER_TEXT, NAT_IS_HOST, NAT_REVISION = 0x7a07, 0x7a08, 0x7a09, 0x7a0a, 0x7a0b, 0x7a0c
# M3 (notes/RACE-BRIDGE.md): member data, the host's Start, and the race start sequence
NAT_SET_LOCAL, NAT_SET_RULES, NAT_CAN_START, NAT_START = 0x7a0d, 0x7a0e, 0x7a0f, 0x7a10
NAT_RACE_POLL, NAT_RACE_SLOT, NAT_RACE_COURSE, NAT_RACE_LAUNCHED = 0x7a11, 0x7a12, 0x7a13, 0x7a14
NAT_FRAME = 0x7a16
FRAME_START_RACE, FRAME_START_SENT, FRAME_START_REFUSED, FRAME_JOIN, FRAME_POPUP, FRAME_START_ALONE, FRAME_START_NOT_READY = 1, 2, 3, 4, 5, 6, 7  # kFrame*
FRAME_POPUP_MUTED = 8  # kFramePopupMuted (M8): the popup about a muted player (its button says UNMUTE)
# M6: the lobby bars (re\m6-lobby\FINDINGS.md)
NAT_MEMBER_RACER, NAT_MEMBER_FIELD, NAT_MEMBER_TEXT, NAT_HOST_RULE = 0x7a17, 0x7a18, 0x7a19, 0x7a1a
NAT_JOIN_POLL, NAT_JOIN_TAKE = 0x7a1c, 0x7a1d  # a Steam Join Game / invite waiting for the menus (re\m6-join\FINDINGS.md)
NAT_HOST_CHANGES = 0x7a1e  # M7: host handovers in this lobby (Steam passed the lobby on when the host left)
NAT_MEMBER_ICON = 0x7a1f   # M7: (bar, racer icon) for the lobby's race bar (a race running here while this player waits)
NAT_ROW_PROFILE = 0x7a21   # M7: (row) the search row's host in the Steam overlay (the console's VIEW GAMER CARD on F2)
NAT_ROW_RACING = 0x7a22    # M7: (row) -> 1 while that lobby's race runs: the STATUS column says GAME, else LOBBY
NAT_ROW_FLAG = 0x7a27      # M8: (row) -> the row host's license flag sprite (a hash), or 0 (src/natives.h kNatRowFlag)
NAT_MODS = 0x7a29          # 1.1: (field, value): 0/1 Metal Sonic / Egg Hangar installed; 2 the next lobby's mods (src/natives.h kNatMods)
NAT_ROW_MODS = 0x7a2a      # 1.1: (row) -> the row's mod icons sprite (a hash), or 0 (src/natives.h kNatRowMods)
NAT_DIALOG_BACK = 0x7a2b   # 1.1: () -> 1: back pressed this frame in the dialog layer (src/natives.h kNatDialogBack)
SIGNAL_ART = 0xcb541e46    # tools\mod_icons.py SIGNAL_WIDE: the stock signal bars (7bf528a4) right-aligned in the widened cell
# The SIGNAL column's header -> "MODS" (the console's TU2 put "DLC" there): its three text nodes and our key in the search scene's
# text (tools\mod_icons.py HEADER_NODES / HEADER_KEY = path_hash("STEAMMP_MODS_COLUMN")).
MODS_HEADER_NODES, MODS_HEADER_KEY = (0x8935d3e3, 0x9b3a9173, 0x7791ab5d), 0xfc762fec
NAT_KICK = 0x7a23          # M7: (member) the host kicks that player out of the lobby (KICK; src/natives.h kNatKick)
NAT_MEMBER_PROFILE = 0x7a24  # M7: (member) that player's Steam profile (the lobby popup's VIEW STEAM PROFILE)
NAT_MEMBER_MUTE = 0x7a26     # M8: (member) the lobby popup's MUTE: toggles hearing that player's voice chat (src/natives.h kNatMemberMute)
NAT_INVITE = 0x7a1b          # M6: () -> 1 asked / 0: INVITE FRIENDS, the Steam overlay's invite dialog for this lobby
NAT_INVITE_KEY = 0x7a25      # M7: () -> 1 when the A key (config [Menus] InviteKey) was pressed: INVITE FRIENDS / VIEW PROFILE
STATUS_LOBBY, STATUS_GAME = 0xcf13b055, 0x4bca8636  # fe_net_lobby_EN keys, as the Mac's 021f(node4, key) (notes\STEAM-BRIDGE.md)
MEMBER_READY, MEMBER_IS_ME, MEMBER_COUNT, MEMBER_TALKING, MEMBER_RACING_HERE = 0, 1, 2, 3, 4
PRIMARY_PAD = 0x0400  # the pad index fe_charselscreen gives the local player
START_KEY = 0x0419    # the host's START RACE: Enter on the keyboard (the Mac's START; a pad's A), pressed this frame
SELECT_KEY = 0x041f   # F2 on the keyboard, Y on a pad: the Mac's triangle: on the game list REFRESH (the Mac's); in the lobby
                      # SETTINGS (host; LOBBY_SETTINGS_KEY)
# The lobby's keys (the user's layout, 2026-10-23 evening: SETTINGS and CHANGE RACER swapped): F1 = CHANGE RACER on your own bar
# and SELECT (the player popup) on another's, one key for "this bar" as the console's one button; F2 = SETTINGS (host only).
LOBBY_SELECT_KEY, LOBBY_SETTINGS_KEY = 0x041d, 0x041f
# The lobby cursor (PS3/360/Mac, re\m7-flows\FINDINGS.md A): Up/Down move the red bar over every player's bar, for everyone; F2 on
# another player's bar opens the console's player popup (src/natives.h kNatFrame arg 13); Esc = BACK asks first (cursor_keys).
# The user's layout (2026-10-23): the Mac's keys, plus INVITE FRIENDS on the A key (NAT_INVITE_KEY; the consoles' triangle).
UP_KEY, DOWN_KEY, BACK_KEY = 0x0421, 0x0422, 0x041a
NATIVES = {0x7a01, 0x7a02, 0x7a03, 0x7a04, 0x7a05, 0x7a06, 0x7a07, 0x7a08, 0x7a09, 0x7a0a, 0x7a0b, 0x7a0c,
           0x7a0d, 0x7a0e, 0x7a0f, 0x7a10, 0x7a11, 0x7a12, 0x7a13, 0x7a14, 0x7a16, 0x7a17, 0x7a18, 0x7a19, 0x7a1a, 0x7a1c, 0x7a1d,
           NAT_INVITE, 0x7a1e, 0x7a1f, 0x7a21, 0x7a22, NAT_KICK, NAT_MEMBER_PROFILE, NAT_MEMBER_MUTE, NAT_INVITE_KEY, PRIMARY_PAD, START_KEY, SELECT_KEY,
           0x020f, NAT_ROW_FLAG, racer.NAT_RACER_ON, NAT_MODS, NAT_ROW_MODS, NAT_DIALOG_BACK}  # (020f: the spinner's looping dots, as the stock search start's)
# fe_game_bridge and the messages fe_charselscreen / fe_trackselect send it for a Single Race
BRIDGE, MSG_CHARS, MSG_DEVICES, MSG_COURSE, MSG_START = 0x44c833f0, 0xb27af30e, 0x71612751, 0x8af64da9, 0x9b3cf8f9
# src/lobby.h
OFF, IDLE, CREATING_, SEARCHING, RESULTS, JOINING, IN_LOBBY, ERROR = range(8)
ERR_CREATE, ERR_JOIN, ERR_MODS, ERR_GONE, ERR_SEARCH, ERR_VERSION, ERR_KICKED, ERR_STEAM_LOST, ERR_STEAM_OFF, ERR_JOIN_GONE = 1, 2, 3, 4, 5, 6, 7, 8, 9, 10

SEARCH_TUPLES, LOBBY_TUPLES = 0x0f, 0x10  # stock constructor globals (notes/STEAM-BRIDGE.md)
ROW_HIGHLIGHT, ROW_PLAIN, ROW_RESET = 0xd279b4c1, 0x0d09208a, 0x74e7e7bc
# M8: the console's busy spinner (re\m8-spinners\FINDINGS.md): group fff27543 of navigator_info's scene 3b32ee30 (created by the
# network page), a ring of dots spinning by itself once started, with one label in three text layers. SEARCHING on the game list
# while Steam searches; CREATING / JOINING in the lobby (the retained lobby Show 33d02ae6 writes which, by global 0x34: -1 create)
# until Steam's create / join is done. Fades take 10 frames; the 5-argument 020e ending in 0 (the 4-argument form re-windows the
# dots: they freeze); 0215 on the place group re-poses its children, so the spin and a fade always follow it.
SPIN_PLACE, SPIN_GROUP, SPIN_DOTS, SPIN_TEXTS = 0x65c8b901, 0xfff27543, 0x75d34984, (0x9e70d8aa, 0xc43598db, 0x98212984)
TXT_SEARCHING = 0x3b32ee30
SPIN_AT_SEARCH, SPIN_HIDDEN, SPIN_SHOWN = 0xa363a411, 0xa56426e2, 0x3ecdb4b9
SPIN_MIN = 32  # frames: one turn of the dots
SHOW_LABEL = 'lobby_show_entry'  # the lobby Show block, a subroutine after the module's code (16-bit jump room)
DRAW_LABEL = 'lobby_bars_entry'  # the lobby bars, likewise
FOOTER_LABEL = 'lobby_footer_entry'  # the lobby footer and title, likewise
REFUSAL_LABEL = 'mods_refusal_entry'  # 1.1: the join refusal pop-up naming the mods (16 texts), likewise
# fe_navigator footer icons (re\m6-lobby\FOOTER-FINDINGS.md): 0 Enter, 1 Esc, 2 F1, 3 F2, 9 the A key (our cap, tools\footer_keys.py);
# labels from navigator_info: INVITE FRIENDS, SETTINGS, BACK, CHANGE RACER, START RACE, SELECT. The user's layout (2026-10-23): the
# Mac's lobby (re\m7-flows\FINDINGS.md A: Enter = START, F2 = CHANGE RACER on your own bar / SELECT on another's, F1 = SETTINGS,
# Esc = BACK) with INVITE FRIENDS (a PS3-retail addition the Mac lacked) on the A key. Order: the Mac's (BACK, SETTINGS, CHANGE
# RACER, START; a guest CHANGE RACER, BACK) with INVITE first. The F2 label follows the lobby cursor (SELECT on another player's
# bar: the player popup); a guest's footer drops CHANGE RACER while a race runs here (re\m7-flows\FINDINGS.md B.6).
# Icons: 2 = F1, 3 = F2 (the user's swap: CHANGE RACER / SELECT on F1, SETTINGS on F2); BACK between CHANGE RACER and START RACE.
F_INVITE, F_SETTINGS, F_BACK, F_RACER, F_START, F_SELECT = (9, 0x84dfa8a5), (3, 0x08d2a070), (1, 0x3a648438), (2, 0xfb59b2c4), (0, 0x9e981619), (2, 0xf1141a0a)
HOST_FOOTER = (F_INVITE, F_SETTINGS, F_RACER, F_BACK, F_START)
GUEST_FOOTER = (F_INVITE, F_RACER, F_BACK)
RACING_FOOTER = (F_INVITE, F_BACK)  # a guest's while a race runs here


def on_other(footer):
    """The footer with the cursor on another player's bar: CHANGE RACER reads SELECT."""
    return tuple(F_SELECT if e == F_RACER else e for e in footer)
# Lobby bar k's scene nodes (stock constructor global 0x10): [group, badge, name, portrait, voice, ready check, name buffer]
BAR_GROUP, BAR_BADGE, BAR_NAME, BAR_PORTRAIT, BAR_VOICE, BAR_READY, BAR_BUFFER = range(7)
BAR_GROUPS = (0xcec457a2, 0xc1afeaef, 0x77e51b23, 0xcfd654a0, 0x8601b69f, 0x1055715d, 0xe47d740b, 0x56e40343)
BADGES = (0x106e548c, 0xa579ecc3, 0x9bc6eef3, 0x88ddffbf, 0xf7b4fb3b, 0xea5e1e3e, 0xf07b78ba, 0xe9521b6e)  # 1P..8P art
UNKNOWN_PORTRAIT = 0x70e16366  # the "?" portrait: no racer picked yet
PORTRAITS = 0x13               # stock global: character hash -> [lobby portrait, popup portrait, name key, race icon]

NO_GAMES = 'No matches were found! Would you like to create a match?'  # the console's DA3E45C7 wording (definition 28BC494F)
OFFLINE = 'Steam is not available, so online games cannot be found or hosted. Make sure Steam is running and online.'
# (code, text, definition, back to the main menu): the console's own dialogs (its wording, OK only, then the main menu, as the
# PS3's error callbacks 559b6ed4 / afddedd6 do; re\m7-flows\FINDINGS.md B.8) where the console had one; our wording on the search
# screen's REFRESH / CREATE / CANCEL notice where it had none (mods, version, Steam).
OK_CREATE, OK_FULL, OK_HOST_LOST, OK_KICKED, OK_SESSION_GONE = 0x204e0232, 0x28fdd773, 0x8374a07b, 0x14a011b9, 0x2e61efaa
JOIN_FAILED = 'Unable to join the match because it is full, or there are no public slots available.'  # 28FDD773, OK, main menu
ERRORS = [
    (ERR_MODS, "Could not join: your mods are different from the host's. SteamMultiplayer.log in the mod's folder lists the differences.", None, False),
    (ERR_VERSION, 'Could not join: the host uses a different version of the Steam Multiplayer mod.', None, False),
    (ERR_GONE, 'There was an error and the connection to the host was lost.', OK_HOST_LOST, True),
    (ERR_CREATE, 'Unable to create match', OK_CREATE, True),
    (ERR_JOIN, JOIN_FAILED, OK_FULL, True),
    (ERR_JOIN_GONE, 'The game session is no longer available.', OK_SESSION_GONE, True),  # (the lobby was gone when the join arrived)
    (ERR_SEARCH, 'The game search failed. Please try again.', None, False),
    (ERR_KICKED, 'You have been kicked from the session.', OK_KICKED, True),
    (ERR_STEAM_LOST, 'The connection to Steam has been lost.', None, False),  # (the console's 4EFA0B1D wording; no PC definition)
    (ERR_STEAM_OFF, 'Steam is not connected to the Steam servers, so online games cannot be found or hosted.', None, False),
]
UNKNOWN_ERROR = 'Could not join or create that game. Please try again.'  # an error code this script doesn't know (REFRESH / CREATE / CANCEL)
# The lobby's player popup (PS3/360: Enter on another player's bar; the host's 39A06F99 / a guest's 1B433955, rebuilt from two
# unused PC definitions by build_menus.py) and the console's lobby confirms and refusals.
# M8: both definitions are rebuilt with MUTE (an earlier console build's popup had MUTE; re\m8-mute\FINDINGS.md: the PC scene has a 4th button),
# and a muted player's popups say UNMUTE instead (two more definitions); the host's moved off 77B81411, the memory-card autosave's
# (build_menus.py dialogs_with_mute).
POPUP_HOST, POPUP_GUEST = 0x940fe7b4, 0xd4879e10  # VIEW STEAM PROFILE / MUTE / KICK / RETURN TO LOBBY; VIEW STEAM PROFILE / MUTE / RETURN TO LOBBY
POPUP_HOST_UNMUTE, POPUP_GUEST_UNMUTE = 0x504acb9a, 0x18c51485  # the same with UNMUTE (the same answers)
POPUP_TEXT = "Select which action you'd like to perform on this player."
POPUP_VIEW, POPUP_MUTE, POPUP_KICK = 0, 1, 2  # the answer indexes (RETURN TO LOBBY: 3 for the host, 2 for a guest)
POPUP_HOST_STATE, POPUP_GUEST_STATE = 1, 40  # the popup's POPUP_STATE; answers = state + 1 + index (host 2-5, guest 41-43)
NOTICE_ALONE, NOTICE_QUIT_SESSION = 0xf2bee196, 0xdd492af5
ALONE_TEXT = 'There must be at least two players in the lobby to start a game.'
NOT_READY_TEXT = 'Not all players are ready.'
LEAVE_TEXT = 'Are you sure you want to leave the lobby?'
QUIT_TEXT = 'Are you sure you want to quit? This will end the session for other players.'
# KICK (M7): the host's confirm (the console's 693E82B7 wording) on a NO / YES definition.
KICK_ASK = 'Are you sure you wish to kick this player from the match?'
# Host migration (M7): the host left and Steam passed the lobby on; the game goes on with the new host (1P).
HOST_NOW = 'The host left this game. You are the host now: you choose the settings and start the race.'
HOST_CHANGED = 'The host left this game. The player with the 1P badge is the host now.'
# 1.1: the join refusal pop-up (the user, 2026-10-24): each Egg Hangar / Metal Sonic difference on its own line with the game list's
# mark (tools\dialog_icons.py: U+F001 the red X = not installed, U+F002 the orange dash = another version), then whether other mods
# differ too. Order: src\modset.h RefusalText (Egg's mark, then Metal's, none / dash / X; each without and with the other mods).
REFUSAL_X, REFUSAL_DASH = '', ''


def _refusal(egg, metal, rest):
    lines = ["You can't join this game:", '']
    for name, mark in (('Egg Hangar', egg), ('Metal Sonic', metal)):
        if mark == 1:
            lines.append(REFUSAL_DASH + ' ' + name + ': a different version')
        elif mark == 2:
            lines.append(REFUSAL_X + ' ' + name + ': not installed')
    if rest:
        lines += ['', 'Other mods are different too (SteamMultiplayer.log lists them).']
    return '\n'.join(lines)


MODS_REFUSALS = [_refusal(e, m, rest) for e in range(3) for m in range(3) if e or m for rest in (0, 1)]
assert len(MODS_REFUSALS) == 16 and all(len(b.encode('utf-8')) < 300 for b in MODS_REFUSALS)
for body in [e[1] for e in ERRORS] + [NO_GAMES, OFFLINE, UNKNOWN_ERROR, HOST_NOW, HOST_CHANGED, KICK_ASK, POPUP_TEXT, ALONE_TEXT, NOT_READY_TEXT, LEAVE_TEXT, QUIT_TEXT]:
    assert body.isascii() and len(body) < 160, body


def nat(c, number, argc=0, drop=False):
    return c.native(number, argc, drop=drop)


def element(c, table, row, index, as_node=True):
    """Push table[row][index]; row is an int or ('global', slot)."""
    c.get(table)
    c.get(row[1]) if isinstance(row, tuple) else c.num(row)
    c.raw(b'\x23').num(index).raw(b'\x23')
    if as_node:
        c.raw(b'\x3c')
    return c


NOTICE_SEARCH = 0x28bc494f  # dialogs definition "No matches were found!": REFRESH / CREATE / CANCEL (the search screen's answers)
NOTICE_OK = 0x0133bf1a      # "Not all players are ready.": OK only (re\m7-messages\out\deftable.md); the lobby ignores the answer
NOTICE_YES_NO = 0xf1898138  # "Are you sure you want to leave the lobby?": NO / YES (the guest's leave confirm; also the kick confirm's buttons)
KICK_YES = 1                # YES's button index in the NO / YES definitions (f1898138, dd492af5; the deftable's order, as REFRESH / CREATE / CANCEL = 0/1/2)


def notice(c, body, definition=NOTICE_SEARCH, to_main=False):
    """The dialog with our text as its body (free text needs the CHOICE callback); the definition gives the buttons. to_main: the
    console's OK box on the search screen, whose answer goes back to the main menu (ERR_MAIN; every other notice clears it)."""
    c.assign(ACTION, -1).assign(ERR_MAIN, 1 if to_main else 0)
    c.ident(search.DIALOGS).ident(0xdfea0f93).ident(definition).ident(search.NETWORK).ident(search.CHOICE).string(body).array(4).native(0x160c, 3)


def lobby_notice(c, body, definition, state=30):
    """A lobby dialog: POPUP_STATE says which question is up (1 the host's player popup, 40 a guest's, 10 the kick confirm, 20 the
    leave confirm, 30 a plain notice), so callback_entry can route its answer; the frame that takes the answer does nothing else
    (the Enter/Esc that answered must not also start the race, leave, or open the racer picker)."""
    c.assign(POPUP_STATE, state)
    notice(c, body, definition)


def capture_error(c, tag):
    """LAST_ERROR = the snapshot's error code, or -1 if Steam is off. Leaving resets the snapshot's."""
    nat(c, NAT_ERROR).put(LAST_ERROR)
    nat(c, NAT_STATE).num(OFF).raw(b'\x4a').branch(tag + '_on')
    c.assign(LAST_ERROR, -1).label(tag + '_on')


def error_notice(c, tag, end):
    """Notice for LAST_ERROR (capture_error). end: the subroutines' chunk (the mods refusal's texts live there: jump room)."""
    c.get(LAST_ERROR).num(-1).raw(b'\x4a').branch(tag + '_not_off')
    notice(c, OFFLINE)
    c.branch(tag + '_done', 0x15).label(tag + '_not_off')
    for code, body, definition, to_main in ERRORS:
        c.get(LAST_ERROR).num(code).raw(b'\x4a').branch(tag + '_not_' + str(code))
        if code == ERR_MODS:  # (1.1: the pop-up naming the mods, a subroutine)
            c.call_chunk(end, REFUSAL_LABEL).branch(tag + '_done', 0x15).label(tag + '_not_' + str(code))
            continue
        notice(c, body, definition, to_main) if to_main else notice(c, body)
        c.branch(tag + '_done', 0x15).label(tag + '_not_' + str(code))
    notice(c, UNKNOWN_ERROR)
    c.label(tag + '_done')


def mods_refusal(c):
    """ERR_MODS's notice: the text naming the lobby's Egg Hangar / Metal Sonic differences (MODS_REFUSALS, by RefusalText), else the
    plain one."""
    c.num(3).num(0)
    nat(c, NAT_MODS, 2).put(FLAG_KEY)
    for i, refusal in enumerate(MODS_REFUSALS, 1):
        c.get(FLAG_KEY).num(i).raw(b'\x4a').branch('refusal_not_' + str(i))
        notice(c, refusal)
        c.branch('refusal_done', 0x15).label('refusal_not_' + str(i))
    notice(c, next(e[1] for e in ERRORS if e[0] == ERR_MODS))
    c.label('refusal_done')


def footer(c, browsing):
    c.ident(0xcc7b6d70).ident(0x55379672)
    # The Mac's search footer (re\m7-flows\FINDINGS.md A; the PS3 retail put REFRESH on START and VIEW PROFILE on triangle): 6 SELECT
    # f1141a0a, 3 REFRESH 72c62bd9 (F2), 9 VIEW PROFILE d1386d0c (the A key: icon 9 = character F007, our cap in the navigator
    # scenes, tools\footer_keys.py; the console's VIEW GAMER CARD), 2 CREATE GAME d9529529 (F1), 1 BACK 3a648438, 0 JOIN
    # 4b519115 (Enter). While searching (no rows): CREATE GAME, BACK.
    pairs = [(6, 0xf1141a0a), (3, 0x72c62bd9), (9, 0xd1386d0c), (2, 0xd9529529), (1, 0x3a648438), (0, 0x4b519115)] if browsing else [(2, 0xd9529529), (1, 0x3a648438)]
    for icon, label in pairs:
        c.num(icon).ident(label)
    c.array(2 * len(pairs)).array(1).native(0x1607, 3)


def hide_search_rows(c):
    for row in search.SEARCH_ROWS:
        c.ident(row).native(0x0218, 1)


def spinner_on(c, key, place):
    """The busy spinner with label `key` at `place`, fading in (the console's search start, stock 0x168e..0x1714)."""
    for t in SPIN_TEXTS:
        c.ident(t).ident(key).native(0x021f, 2)
    c.ident(SPIN_PLACE).ident(place).native(0x0215, 2)
    c.ident(SPIN_DOTS).ident(0x4240c67e).ident(0x4240c67e).ident(0x1de4ecc9).num(0).native(0x020f, 5)
    spinner_fade_in(c)
    c.assign(SPIN_AGE, 0)


def spinner_fade_in(c):
    c.ident(SPIN_GROUP).ident(SPIN_HIDDEN).ident(SPIN_SHOWN).num(0).num(0).native(0x020e, 5)


def spinner_fade_out(c):
    """Only while it is shown: a fade-out starts from full alpha, so on a hidden spinner it would flash for 10 frames."""
    c.ident(SPIN_GROUP).ident(SPIN_SHOWN).ident(SPIN_HIDDEN).num(0).num(0).native(0x020e, 5)


def spinner_off(c):
    """Hidden at once (safe to repeat)."""
    c.ident(SPIN_GROUP).ident(SPIN_HIDDEN).native(0x0215, 2)


def spinner_aged(c, tag):
    """Counts this frame; ends the frame (search_done) until the spinner has been up for SPIN_MIN frames."""
    c.get(SPIN_AGE).num(1).raw(b'\x42').put(SPIN_AGE)
    c.get(SPIN_AGE).num(SPIN_MIN).raw(b'\x46').branch(tag)
    c.branch('search_done', 0x15).label(tag)


def mods_header(c):
    """The SIGNAL column's header reads MODS (cheap: set at each search, each list draw and each error notice; the screen's own
    redraw puts SIGNAL back, e.g. behind a join refusal: the user's mods.5 screenshot)."""
    for node in MODS_HEADER_NODES:
        c.ident(node).ident(MODS_HEADER_KEY).native(0x021f, 2)


def start_search(c):
    hide_search_rows(c)
    footer(c, False)
    mods_header(c)
    c.get(0x23)
    nat(c, NAT_SEARCH, 1, drop=True)
    c.assign(ACTION, -3).assign(ROW_COUNT, 0)
    spinner_on(c, TXT_SEARCHING, SPIN_AT_SEARCH)  # (also on REFRESH and after a lobby was left)


def highlight(c, slot, on):
    element(c, SEARCH_TUPLES, ('global', slot), 0)
    c.ident(ROW_PLAIN if on else ROW_HIGHLIGHT).ident(ROW_HIGHLIGHT if on else ROW_PLAIN).num(0).native(0x020e, 4)


def show_results(c):
    """Rows 0..ROW_COUNT-1 from the snapshot: host name and players; the selection starts at row 0."""
    mods_header(c)
    for i in range(len(search.SEARCH_ROWS)):
        tag = 'result_row_' + str(i)
        element(c, SEARCH_TUPLES, i, 0).ident(ROW_RESET).native(0x0215, 2)
        c.get(ROW_COUNT).num(i).raw(b'\x48').branch(tag + '_hide')
        element(c, SEARCH_TUPLES, i, 0).native(0x0217, 1)
        for node, buf, field in ((2, 6, 0), (3, 7, 1)):
            element(c, SEARCH_TUPLES, i, node)
            element(c, SEARCH_TUPLES, i, buf, as_node=False).native(0x021e, 2)
            c.num(i).num(field)
            element(c, SEARCH_TUPLES, i, buf, as_node=False)
            nat(c, NAT_ROW_TEXT, 3, drop=True)
        # The flag cell (node 1; re\m8-flag\FINDINGS.md): the host's license flag, as the console's (PS3 0x1fdd: 0228(row[1],
        # g14[code])); hidden when unknown (its default sprite is a placeholder flag).
        c.num(i)
        nat(c, NAT_ROW_FLAG, 1).put(FLAG_KEY)  # (read once, test, then use: as draw_bars' RACER_KEY)
        c.get(FLAG_KEY).branch(tag + '_no_flag')
        element(c, SEARCH_TUPLES, i, 1)
        c.get(FLAG_KEY).raw(b'\x3c').native(0x0228, 2)
        element(c, SEARCH_TUPLES, i, 1).native(0x0217, 1)
        c.branch(tag + '_flag_done', 0x15).label(tag + '_no_flag')
        element(c, SEARCH_TUPLES, i, 1).native(0x0218, 1)
        c.label(tag + '_flag_done')
        # 1.1: the SIGNAL cell (node 5) shows the lobby's mod icons (the console's TU2 DLC column drew there; tools\mod_icons.py),
        # else the stock signal bars (a row is reused for another lobby on REFRESH).
        c.num(i)
        nat(c, NAT_ROW_MODS, 1).put(FLAG_KEY)
        c.get(FLAG_KEY).branch(tag + '_no_mods')
        element(c, SEARCH_TUPLES, i, 5)
        c.get(FLAG_KEY).raw(b'\x3c').native(0x0228, 2)
        c.branch(tag + '_mods_done', 0x15).label(tag + '_no_mods')
        element(c, SEARCH_TUPLES, i, 5)
        c.ident(SIGNAL_ART).raw(b'\x3c').native(0x0228, 2)
        c.label(tag + '_mods_done')
        # The STATUS column, as the Mac's row loop: GAME while that lobby's race runs, else LOBBY (021f(node4, key)).
        c.num(i)
        nat(c, NAT_ROW_RACING, 1).branch(tag + '_lobby')
        element(c, SEARCH_TUPLES, i, 4).ident(STATUS_GAME).native(0x021f, 2)
        c.branch(tag + '_status_done', 0x15).label(tag + '_lobby')
        element(c, SEARCH_TUPLES, i, 4).ident(STATUS_LOBBY).native(0x021f, 2)
        c.label(tag + '_status_done')
        element(c, SEARCH_TUPLES, i, 4).native(0x0217, 1)
        c.branch(tag + '_done', 0x15).label(tag + '_hide')
        element(c, SEARCH_TUPLES, i, 0).native(0x0218, 1)
        c.label(tag + '_done')
    footer(c, True)
    c.assign(SEL, 0).assign(PREV, 0).assign(base.DELAY, 2)
    highlight(c, SEL, True)


def host_and_enter(c):
    """Create the Steam lobby, then the shared lobby entry (shared_host)."""
    c.branch('shared_host', 0x15)


# 1.1 ENABLE MODIFICATIONS: the console's CREATE question (TU2 fe_network_game 0x22a6 / 0x27c6 / 0x4692: EB933573 NO / YES, only
# with the DLC installed; re\m9-modifications\FINDINGS.md), "modifications" for "downloadable content" (the user's words). With
# installed mods, YES opens the MODIFICATIONS popup (the user's design, 2026-10-24: the lobby's SETTINGS popup with an ON / OFF row
# per installed mod; the console's one pack had no such list). Our text in the NO / YES definition.
MODS_QUESTION = 'Would you like to enable your modifications?'
# Esc closes the question (the user, 2026-10-24, instead of a CANCEL button; an addition: no PC or console dialog closes on back;
# re\m9-esc\FINDINGS.md): search_active's busy branch, while the question is up, posts the dialog's own close (as front_main
# +0x707), whose callback answers -1 (+ 8: closed: no lobby, search again).
DIALOG_CLOSE, DIALOG_SCENE = 0x3cb781ef, 0xbf85b602
MODS_ASKED = 1  # MODS_STATE while the question is up; its answer: + 1 NO, + 2 YES, + 8 closed
MODS_OPEN, MODS_CLOSING_OK, MODS_CLOSING_BACK = 30, 31, 32  # the popup: open; closing after Enter / after Esc
# The popup is the Settings popup (fe_net_lobby_popups root eb17fb50, attached as scene 423a1091 the way the lobby Show does), its
# one-line bars 2 and 3 (LAPS, PRIVATE SLOTS) relabelled through our two text buffers and put back on close
# (re\m9-modifications\POPUP-FINDINGS.md); every other bar hidden. Values: the Settings' own ON / OFF keys.
POPUPS_SCENE, POPUP_HIDDEN, POPUP_OPEN_FROM, POPUP_OPEN_TO, POPUP_CLOSE_FROM = 0x423a1091, 0x36a7542b, 0xb2244a6d, 0x02f0b24c, 0xc1dd7351
MODS_BARS = settings.BARS[1:3]
MODS_LABELS = ((0x43f6685c, 0xbec46a65, 0x7ad9ac6c), (0x3a5d4dbc, 0x6c59fcf9, 0x1b897197))  # each bar's two labels, its own text key
MODS_VALUES = settings.TEXT_PAIRS[1:3]
TXT_ON, TXT_OFF = 0xffc1df26, 0x724c3b00
BAR_PLAIN, BAR_LIT = 0x0d09208a, 0xd279b4c1


def mods_ask(c, body, state):
    c.assign(MODS_STATE, state)
    notice(c, body, NOTICE_YES_NO)
    c.branch('search_done', 0x15)


def mods_on_off(c, key, slot, tag):
    c.get(slot).branch(tag + '_off')
    c.ident(key).ident(TXT_ON).native(0x021f, 2).branch(tag + '_done', 0x15)
    c.label(tag + '_off').ident(key).ident(TXT_OFF).native(0x021f, 2).label(tag + '_done')


def mods_row_value(c, key, row, tag):
    """Row's value text: row 0 is Metal Sonic when installed, else Egg Hangar; row 1 (both installed) Egg Hangar."""
    if row == 1:
        return mods_on_off(c, key, MODS_EGG, tag)
    c.get(MODS_HAVE_METAL).branch(tag + '_egg')
    mods_on_off(c, key, MODS_METAL, tag + '_metal')
    c.branch(tag + '_end', 0x15).label(tag + '_egg')
    mods_on_off(c, key, MODS_EGG, tag + '_egg_value')
    c.label(tag + '_end')


def mods_render(c, tag):
    for row, bar in enumerate(MODS_BARS):
        c.get(MODS_ROW).num(row).raw(b'\x4a').branch(tag + '_plain_' + str(row))
        c.ident(bar).ident(BAR_LIT).native(0x0215, 2).branch(tag + '_lit_' + str(row), 0x15)
        c.label(tag + '_plain_' + str(row)).ident(bar).ident(BAR_PLAIN).native(0x0215, 2).label(tag + '_lit_' + str(row))
        for i, key in enumerate(MODS_VALUES[row]):
            mods_row_value(c, key, row, tag + '_value_' + str(row) + '_' + str(i))


def mods_buffer(c, row):
    return c.get(MODS_BUFS).num(row).raw(b'\x23')


def mods_open(c):
    """YES: the popup lists the installed mods, each ON (the host's YES) until changed with Left / Right."""
    c.get(MODS_METAL).put(MODS_HAVE_METAL)
    c.get(MODS_METAL).get(MODS_EGG).raw(b'\x42').put(MODS_ROWS)
    c.assign(MODS_ROW, 0)
    c.ident(POPUPS_SCENE).native(0x0205, 1)
    c.ident(racer.POPUP).ident(POPUP_HIDDEN).native(0x0215, 2)
    c.ident(settings.SETTINGS).ident(POPUP_HIDDEN).native(0x0215, 2)
    for bar in settings.BARS:
        c.ident(bar).native(0x0218, 1)
    c.ident(MODS_BARS[0]).native(0x0217, 1)
    c.get(MODS_ROWS).num(1).raw(b'\x48').branch('mods_open_one_row')
    c.ident(MODS_BARS[1]).native(0x0217, 1)
    c.label('mods_open_one_row')
    for row, (a, b, _) in enumerate(MODS_LABELS):
        for key in (a, b):
            c.ident(key)
            mods_buffer(c, row).native(0x021e, 2)
    c.get(MODS_HAVE_METAL).branch('mods_open_egg_first')
    mods_buffer(c, 0).string('METAL SONIC').native(0x1613, 2)
    c.branch('mods_open_labels', 0x15).label('mods_open_egg_first')
    mods_buffer(c, 0).string('EGG HANGAR').native(0x1613, 2)
    c.label('mods_open_labels')
    mods_buffer(c, 1).string('EGG HANGAR').native(0x1613, 2)
    mods_render(c, 'mods_open_render')
    racer.sound(c, 'FE_SELECT')
    c.ident(settings.SETTINGS).ident(POPUP_OPEN_FROM).ident(POPUP_OPEN_TO).num(0).native(0x020e, 4)
    c.call(0x1fdd)  # the Settings popup's own footer
    c.assign(base.DELAY, 2).assign(MODS_STATE, MODS_OPEN)
    c.branch('search_done', 0x15)


def mods_popup_frame(c):
    """While the popup is open: Up / Down pick a row, Left / Right switch it, Enter creates the game, Esc goes back."""
    c.get(MODS_STATE).num(MODS_OPEN).raw(b'\x4a').branch('mods_not_open')
    c.ident(settings.SETTINGS).native(0x0213, 1, drop=False).branch('search_done')  # (opening)
    c.get(base.DELAY).num(0).raw(b'\x48').branch('mods_inputs')
    c.get(base.DELAY).num(1).raw(b'\x43').put(base.DELAY).branch('search_done', 0x15)
    c.label('mods_inputs').native(BACK_KEY, 0, drop=False).branch('mods_no_back')
    racer.sound(c, 'FE_BACK')
    c.assign(MODS_STATE, MODS_CLOSING_BACK).branch('mods_close', 0x15)
    c.label('mods_no_back').native(START_KEY, 0, drop=False).branch('mods_no_enter')
    racer.sound(c, 'FE_SELECT')
    c.assign(MODS_STATE, MODS_CLOSING_OK)
    c.label('mods_close').ident(settings.SETTINGS).ident(POPUP_CLOSE_FROM).ident(POPUP_HIDDEN).num(0).native(0x020e, 4)
    c.branch('search_done', 0x15)
    c.label('mods_no_enter').native(UP_KEY, 0, drop=False).branch('mods_no_up')
    c.get(MODS_ROW).num(0).raw(b'\x48').branch('search_done')
    c.get(MODS_ROW).num(1).raw(b'\x43').put(MODS_ROW).branch('mods_changed', 0x15)
    c.label('mods_no_up').native(DOWN_KEY, 0, drop=False).branch('mods_no_down')
    c.get(MODS_ROW).get(MODS_ROWS).num(1).raw(b'\x43\x46').branch('search_done')
    c.get(MODS_ROW).num(1).raw(b'\x42').put(MODS_ROW).branch('mods_changed', 0x15)
    c.label('mods_no_down').native(0x0423, 0, drop=False).branch('mods_no_left')
    c.branch('mods_switch', 0x15)
    c.label('mods_no_left').native(0x0424, 0, drop=False).branch('search_done')
    c.label('mods_switch')  # (left or right: one switch between ON and OFF)
    c.get(MODS_ROW).num(1).raw(b'\x4a').branch('mods_switch_first')
    c.num(1).get(MODS_EGG).raw(b'\x43').put(MODS_EGG).branch('mods_changed', 0x15)
    c.label('mods_switch_first').get(MODS_HAVE_METAL).branch('mods_switch_egg')
    c.num(1).get(MODS_METAL).raw(b'\x43').put(MODS_METAL).branch('mods_changed', 0x15)
    c.label('mods_switch_egg').num(1).get(MODS_EGG).raw(b'\x43').put(MODS_EGG)
    c.label('mods_changed')
    racer.sound(c, 'FE_MOVE')
    mods_render(c, 'mods_changed_render')
    c.branch('search_done', 0x15)
    # Closing: once the close animation has finished, the bars and labels go back to the lobby Settings' own and the scene is released.
    c.label('mods_not_open').get(MODS_STATE).num(MODS_CLOSING_OK).raw(b'\x4a').get(MODS_STATE).num(MODS_CLOSING_BACK).raw(b'\x4a\x50')
    c.branch('mods_not_popup')
    c.ident(settings.SETTINGS).native(0x0213, 1, drop=False).branch('search_done')
    for (a, b, text) in MODS_LABELS:
        for key in (a, b):
            c.ident(key).ident(text).native(0x021f, 2)
    for bar in settings.BARS:
        c.ident(bar).native(0x0217, 1)
        c.ident(bar).ident(BAR_PLAIN).native(0x0215, 2)
    c.ident(POPUPS_SCENE).native(0x0206, 1)
    c.get(MODS_STATE).num(MODS_CLOSING_OK).raw(b'\x4a').branch('mods_back')
    c.branch('mods_create', 0x15)
    c.label('mods_back').assign(MODS_STATE, 0).assign(ACTION, -2).branch('search_done', 0x15)
    c.label('mods_not_popup')


def create_request(c, tag):
    """CREATE on the search screen: the question when a toggleable mod is installed, else straight to the lobby (as 1.0.0)."""
    c.num(0).num(0)
    nat(c, NAT_MODS, 2).put(MODS_METAL)
    c.num(1).num(0)
    nat(c, NAT_MODS, 2).put(MODS_EGG)
    c.get(MODS_METAL).get(MODS_EGG).raw(b'\x42').num(0).raw(b'\x48').branch(tag + '_plain')
    mods_ask(c, MODS_QUESTION, MODS_ASKED)
    c.label(tag + '_plain')
    c.branch('mods_create', 0x15)


def mods_frame(c):
    """The search frame's first step while the question runs (the dialog is idle: answered, or posted and not shown yet)."""
    c.get(MODS_STATE).branch('mods_idle')
    mods_popup_frame(c)
    c.get(MODS_STATE).num(MODS_ASKED).raw(b'\x4a').branch('mods_not_waiting')  # posted, no answer yet
    c.branch('search_done', 0x15).label('mods_not_waiting')
    # Closed (Esc): no lobby, as the console's -1; search again (the notice set ACTION -1).
    c.get(MODS_STATE).num(MODS_ASKED + 8).raw(b'\x4a').branch('mods_not_closed')
    c.assign(MODS_STATE, 0).assign(ACTION, -2).branch('search_done', 0x15)
    c.label('mods_not_closed')
    c.get(MODS_STATE).num(MODS_ASKED + 1).raw(b'\x4a').branch('mods_not_no')
    c.assign(MODS_METAL, 0).assign(MODS_EGG, 0).branch('mods_create', 0x15)  # NO: every modification off
    c.label('mods_not_no').get(MODS_STATE).num(MODS_ASKED + 2).raw(b'\x4a').branch('mods_not_yes')
    mods_open(c)  # YES
    c.label('mods_not_yes').assign(MODS_STATE, 0).assign(ACTION, -2).branch('search_done', 0x15)  # (no other state: as closed)
    # The answers go to the lobby about to be created (lobby data "mods" leaves the off ones out), then CREATE as before.
    c.label('mods_create').num(2).get(MODS_METAL).get(MODS_EGG).raw(b'\x42').get(MODS_EGG).raw(b'\x42')
    nat(c, NAT_MODS, 2, drop=True)
    c.assign(MODS_STATE, 0)
    host_and_enter(c)
    c.label('mods_idle')


def shared_host(c):
    """One copy: post Host; if Steam can't take it (off, queue full), the notice instead of a lobby."""
    c.label('shared_host').get(0x23)
    nat(c, NAT_HOST, 1).branch('shared_host_refused')
    c.branch('shared_enter_lobby', 0x15)
    c.label('shared_host_refused')
    capture_error(c, 'shared_host_error')
    c.branch('shared_error_notice', 0x15)


def shared_enter_lobby(c, show_anchor):
    """Search -> local lobby (Codex's create path), and back into it after a race. One copy; the Show
    block (1.9 KB) is a subroutine outside the message loop, whose back-jump is 16-bit."""
    # Back from an online race (ACTION -7, notes/RETURN-TO-LOBBY.md): the same Steam lobby, racer,
    # course and rules (this instance's globals live through the race); no racer popup, no Steam call.
    c.label('shared_reenter_lobby')
    c.assign(CREATING, 0).assign(CREATE_RESULT, 0).assign(AUTO_CREATE, 0).assign(START_RESULT, 0)
    c.assign(0x34, -1).assign(WAITING, 0).assign(AUTO_HELD, 0)
    c.branch('shared_lobby_tail', 0x15)
    # M8, as the consoles: a join shows the lobby at once with JOINING (0x34 0), a create with CREATING (0x34 -1); the lobby frame
    # then waits (WAITING) until Steam's join / create is done, and only then draws the bars and opens the racer picker.
    c.label('shared_join_lobby').assign(0x34, 0).branch('shared_fresh_lobby', 0x15)
    c.label('shared_enter_lobby').assign(0x34, -1)
    c.label('shared_fresh_lobby').assign(WAITING, 1).assign(AUTO_HELD, 0)
    c.assign(CREATING, 1).assign(CREATE_RESULT, 0).assign(AUTO_CREATE, 1)
    c.assign(base.LAPS, 3).assign(base.AI, 1).assign(base.ITEMS, 1).assign(base.CATCHUP, 1).assign(settings.COURSE, 0)
    c.get(0x23).branch('privacy_anyone').assign(base.PRIVATE_SLOTS, 7).branch('privacy_done', 0x15)
    c.label('privacy_anyone').assign(base.PRIVATE_SLOTS, 0).label('privacy_done')
    c.assign(LAST_HOST_CHANGE, 0)  # a fresh lobby (after a race the count carries over: a handover during it gets its notice)
    c.label('shared_lobby_tail')
    c.assign(ACTION, -1).assign(FLOW, 1).assign(LOBBY_REV, -1).assign(MEMBERS, 0).assign(HILITE, -1)  # (Show resets the bars)
    c.assign(CURSOR, -1).assign(ME_LAST, -1).assign(POPUP_STATE, 0).assign(ERR_MAIN, 0)  # the cursor starts on this player's bar (draw_bars); no dialog
    search.forest(c, 0x0a, True)
    c.ident(search.SEARCH_SCENE).native(0x0206, 1)
    c.call_chunk(show_anchor, SHOW_LABEL)
    # The Show ran the console's CREATING / JOINING start, then preview2's fade-out; while waiting, fade in again (this call's window
    # wins), else (back from a race) hide it at once.
    c.get(WAITING).branch('shared_lobby_no_wait')
    spinner_fade_in(c)
    # The console's waiting lobby: no bars (the Show leaves bar 0 up; draw_bars shows it again after the wait, LOBBY_REV is -1) and
    # a BACK-only footer (the real one comes with the first draw after the wait).
    c.ident(BAR_GROUPS[0]).native(0x0218, 1)
    footer_pairs(c, (F_BACK,))
    c.assign(SPIN_AGE, 0).branch('search_done', 0x15)
    c.label('shared_lobby_no_wait')
    spinner_off(c)
    c.branch('search_done', 0x15)


def shared_error_notice(c, end):
    c.label('shared_error_notice')
    spinner_off(c)
    mods_header(c)
    error_notice(c, 'error_notice', end)
    c.branch('search_done', 0x15)


def leave_lobby_to_search(c, tag, then_action):
    capture_error(c, tag + '_error')
    nat(c, NAT_LEAVE, drop=True)
    search.remove_lobby(c)
    search.search_show(c, tag)
    c.assign(ACTION, then_action)


def draw_bars(c):
    """The lobby bars as the console drew them (Mac export ea107c57, re\\m6-lobby\\FINDINGS.md): bar k =
    lobby member k (host first, badge kP), each with its Steam name, its racer's portrait or "?" while it
    has none, and the ready check; this player's bar highlighted. A subroutine (call_chunk), so the
    message loop stays short; drawn whenever the Steam snapshot changes."""
    c.num(0).num(MEMBER_COUNT)
    nat(c, NAT_MEMBER_FIELD, 2).put(MEMBERS)
    for k in range(len(BAR_GROUPS)):
        tag = 'bar_' + str(k)
        c.get(MEMBERS).num(k).raw(b'\x48').branch(tag + '_hide')
        element(c, LOBBY_TUPLES, k, BAR_GROUP).native(0x0217, 1)
        element(c, LOBBY_TUPLES, k, BAR_NAME)
        element(c, LOBBY_TUPLES, k, BAR_BUFFER, as_node=False).native(0x021e, 2)
        c.num(k)
        element(c, LOBBY_TUPLES, k, BAR_BUFFER, as_node=False)
        nat(c, NAT_MEMBER_TEXT, 2, drop=True)
        # The portrait table needs a hash key (an int would find some other racer): read once, test, then look up.
        c.num(k)
        nat(c, NAT_MEMBER_RACER, 1).put(RACER_KEY)
        c.get(RACER_KEY).branch(tag + '_unknown')
        element(c, LOBBY_TUPLES, k, BAR_PORTRAIT)
        c.get(PORTRAITS).get(RACER_KEY).raw(b'\x23').num(0).raw(b'\x23\x3c').native(0x0228, 2)
        # The racer's in-race icon (the portrait table's 4th entry) for the race bar's marker (kNatMemberIcon).
        c.num(k)
        c.get(PORTRAITS).get(RACER_KEY).raw(b'\x23').num(3).raw(b'\x23')
        nat(c, NAT_MEMBER_ICON, 2, drop=True)
        c.branch(tag + '_portrait_done', 0x15).label(tag + '_unknown')
        element(c, LOBBY_TUPLES, k, BAR_PORTRAIT).ident(UNKNOWN_PORTRAIT).raw(b'\x3c').native(0x0228, 2)
        c.num(k).num(0)
        nat(c, NAT_MEMBER_ICON, 2, drop=True)
        c.label(tag + '_portrait_done')
        # The voice icon (the node's own art 24064704, a speech bubble), as the console drew it (Mac: `voice r[8] ? show : hide`):
        # shown while that player talks (Steam voice, M7; re\m7-voice\FINDINGS.md).
        c.num(k).num(MEMBER_TALKING)
        nat(c, NAT_MEMBER_FIELD, 2).branch(tag + '_quiet')
        element(c, LOBBY_TUPLES, k, BAR_VOICE).native(0x0217, 1)
        c.branch(tag + '_voice_done', 0x15).label(tag + '_quiet')
        element(c, LOBBY_TUPLES, k, BAR_VOICE).native(0x0218, 1)
        c.label(tag + '_voice_done')
        c.num(k).num(MEMBER_READY)
        nat(c, NAT_MEMBER_FIELD, 2).branch(tag + '_not_ready')
        element(c, LOBBY_TUPLES, k, BAR_READY).native(0x0217, 1)
        c.branch(tag + '_ready_done', 0x15).label(tag + '_not_ready')
        element(c, LOBBY_TUPLES, k, BAR_READY).native(0x0218, 1)
        c.label(tag + '_ready_done')
        element(c, LOBBY_TUPLES, k, BAR_BADGE).native(0x0217, 1)
        element(c, LOBBY_TUPLES, k, BAR_BADGE).ident(BADGES[k]).raw(b'\x3c').native(0x0228, 2)
        c.branch(tag + '_done', 0x15).label(tag + '_hide')
        element(c, LOBBY_TUPLES, k, BAR_GROUP).native(0x0218, 1)
        c.label(tag + '_done')
    # The highlighted bar is the CURSOR (Mac 0x3696 / PS3: the row highlight from 0d09208a to d279b4c1 follows g2d, which starts
    # on this player's bar); the cursor is set to this player's bar when not set yet, or when it points past the bars.
    c.assign(WANT, -1)
    for k in range(len(BAR_GROUPS)):
        c.num(k).num(MEMBER_IS_ME)
        nat(c, NAT_MEMBER_FIELD, 2).branch('bar_me_not_' + str(k))
        c.assign(WANT, k).label('bar_me_not_' + str(k))
    # The cursor follows this player's own bar when the bars shift under it (a player above left; this game became the host): the
    # console's cursor was a plain index, but here Enter would then open another player's popup instead of START / CHANGE RACER.
    c.get(CURSOR).get(ME_LAST).raw(b'\x4a').branch('bar_cursor_kept')
    c.get(WANT).put(CURSOR)
    c.label('bar_cursor_kept').get(WANT).put(ME_LAST)
    c.get(CURSOR).num(0).raw(b'\x46').branch('bar_cursor_set')  # (cursor < 0: not set yet)
    c.branch('bar_cursor_me', 0x15).label('bar_cursor_set')
    c.get(CURSOR).get(MEMBERS).raw(b'\x46').branch('bar_cursor_me')  # (cursor past the bars: that player left)
    c.get(CURSOR).put(WANT).branch('bar_cursor_done', 0x15)
    c.label('bar_cursor_me').get(WANT).put(CURSOR)
    c.label('bar_cursor_done')
    c.get(WANT).get(HILITE).raw(b'\x4a').branch('bar_highlight_move')
    c.branch('bar_highlight_done', 0x15).label('bar_highlight_move')
    for k, group in enumerate(BAR_GROUPS):
        c.get(HILITE).num(k).raw(b'\x4a').branch('bar_unlit_' + str(k))
        c.ident(group).ident(ROW_HIGHLIGHT).ident(ROW_PLAIN).num(0).native(0x020e, 4)
        c.label('bar_unlit_' + str(k))
        c.get(WANT).num(k).raw(b'\x4a').branch('bar_lit_' + str(k))
        c.ident(group).ident(ROW_PLAIN).ident(ROW_HIGHLIGHT).num(0).native(0x020e, 4)
        c.label('bar_lit_' + str(k))
    c.get(WANT).put(HILITE)
    c.label('bar_highlight_done')
    # A guest's left panel shows the host's rules (course, laps, private slots, AI, power-ups, catch-up);
    # the native gives -1 to the host itself, before the host has published, or for a value out of range.
    for i, slot in enumerate((settings.COURSE,) + settings.RULE_SLOTS):
        tag = 'host_rule_' + str(i)
        c.num(i)
        nat(c, NAT_HOST_RULE, 1).put(RACER_KEY)
        c.get(RACER_KEY).num(-1).raw(b'\x4b').branch(tag)
        c.get(RACER_KEY).put(slot).assign(base.DIRTY, 1)
        c.label(tag)
    # The footer follows the host/guest role (a creating host only becomes host once Steam answers); the
    # racer and Settings popups draw their own, and redraw ours when they close.
    c.get(racer.MODE).get(settings.SETTINGS_MODE).raw(b'\x50').branch('bars_footer')
    c.branch('bars_footer_done', 0x15).label('bars_footer')
    lobby_footer(c, 'bars_footer')
    c.label('bars_footer_done')


def footer_pairs(c, footer):
    c.ident(0xcc7b6d70).ident(0x55379672)
    for icon, label in footer:
        c.num(icon).ident(label)
    c.array(2 * len(footer)).array(1).native(0x1607, 3)


def lobby_footer(c, tag):
    """The lobby's footer, then the LOBBY title (settings.lobby_hints'). Host: INVITE FRIENDS (A), BACK, SETTINGS (F1),
    CHANGE RACER (F2) or SELECT with the cursor on another player's bar (the player popup), Enter START RACE; guest: INVITE
    FRIENDS, CHANGE RACER / SELECT (not while a race runs here), BACK."""
    # RACER_KEY (scratch) = 1 with the cursor on another player's bar (one that exists), else 0.
    c.assign(RACER_KEY, 0)
    c.get(CURSOR).num(0).raw(b'\x46').branch(tag + '_cursor_set')
    c.branch(tag + '_cursor_done', 0x15).label(tag + '_cursor_set')
    c.get(CURSOR).get(MEMBERS).raw(b'\x46').branch(tag + '_cursor_done')
    c.get(CURSOR).num(MEMBER_IS_ME)
    nat(c, NAT_MEMBER_FIELD, 2).branch(tag + '_cursor_other')
    c.branch(tag + '_cursor_done', 0x15).label(tag + '_cursor_other')
    c.assign(RACER_KEY, 1).label(tag + '_cursor_done')
    nat(c, NAT_IS_HOST).branch(tag + '_guest')
    c.get(RACER_KEY).branch(tag + '_host_own')
    footer_pairs(c, on_other(HOST_FOOTER))
    c.branch(tag + '_title', 0x15).label(tag + '_host_own')
    footer_pairs(c, HOST_FOOTER)
    c.branch(tag + '_title', 0x15).label(tag + '_guest')
    c.num(0).num(MEMBER_RACING_HERE)
    nat(c, NAT_MEMBER_FIELD, 2).branch(tag + '_guest_idle')
    c.get(RACER_KEY).branch(tag + '_racing_own')
    footer_pairs(c, (F_INVITE, F_SELECT, F_BACK))
    c.branch(tag + '_title', 0x15).label(tag + '_racing_own')
    footer_pairs(c, RACING_FOOTER)
    c.branch(tag + '_title', 0x15).label(tag + '_guest_idle')
    c.get(RACER_KEY).branch(tag + '_guest_own')
    footer_pairs(c, on_other(GUEST_FOOTER))
    c.branch(tag + '_title', 0x15).label(tag + '_guest_own')
    footer_pairs(c, GUEST_FOOTER)
    c.label(tag + '_title')
    c.ident(0xcc7b6d70).ident(0xce3d133a).ident(0x8d9e062e).array(1).native(0x1607, 3)


def start_race(c):
    """The armed contract's race: the messages a stock Single Race sends fe_game_bridge, in one frame
    (8 characters, this player's pad at its creation index and -1 = AI elsewhere, the course by
    name, then start). Our 0905/0900 hooks turn the bridge's racer calls into the contract's."""
    c.ident(BRIDGE).ident(MSG_CHARS)
    for k in range(8):
        c.num(k).num(0)
        nat(c, NAT_RACE_SLOT, 2)
    c.array(8).native(0x160c, 3, drop=False)  # 0 = queued; 1 = the bridge is not running
    c.num(0).raw(b'\x4a').branch('race_launch_failed')
    c.ident(BRIDGE).ident(MSG_DEVICES)
    for k in range(8):
        c.num(k).num(1)
        nat(c, NAT_RACE_SLOT, 2)
    c.array(8).native(0x160c, 3)
    c.ident(BRIDGE).ident(MSG_COURSE)
    nat(c, NAT_RACE_COURSE)
    c.native(0x160c, 3)
    c.ident(BRIDGE).ident(MSG_START).raw(b'\x31').native(0x160c, 3)  # 0x31 pushes "none": no payload
    c.num(1).branch('race_launch_report', 0x15)
    c.label('race_launch_failed').num(0)
    c.label('race_launch_report')
    nat(c, NAT_RACE_LAUNCHED, 1, drop=True)


def race_frame(c):
    """Every lobby frame, one native does the work (src/natives.cpp kNatFrame): publishes our racer,
    ready flag and (host) rules, polls for an armed race, and handles the host's Start key (Enter) and
    F2 with the lobby cursor on another player's bar (the console's player popup); then the cursor keys,
    INVITE FRIENDS (the A key) and F2 on your own bar (cursor_keys)."""
    for slot in (base.CHARACTER, racer.MODE, settings.COURSE, base.LAPS, base.PRIVATE_SLOTS, base.AI, base.ITEMS,
                 base.CATCHUP, settings.SETTINGS_MODE):
        c.get(slot)
    c.native(START_KEY, 0, drop=False).native(PRIMARY_PAD, 0, drop=False)
    c.get(CREATING)  # 1 until this player has picked a racer in this lobby: the others show "?"
    c.native(LOBBY_SELECT_KEY, 0, drop=False)
    c.get(CURSOR)  # the lobby cursor (-1 = this player's bar): F1 on another player's bar opens the popup
    nat(c, NAT_FRAME, 14).put(START_RESULT)
    c.get(START_RESULT).num(FRAME_START_SENT).raw(b'\x4a').branch('race_not_sent')
    racer.sound(c, 'FE_SELECT')
    c.label('race_not_sent')
    c.get(START_RESULT).num(FRAME_START_REFUSED).raw(b'\x4a').branch('race_not_refused')
    racer.sound(c, 'FE_BACK')  # why: SteamMultiplayer.log ("host Start: refused (code n)")
    c.label('race_not_refused')
    c.get(START_RESULT).num(FRAME_START_RACE).raw(b'\x4a').branch('race_not_started')
    start_race(c)
    c.label('race_not_started')
    # A Steam Join Game / invite: leave this lobby; the search screen then takes it.
    c.get(START_RESULT).num(FRAME_JOIN).raw(b'\x4a').branch('race_not_join')
    leave_lobby_to_search(c, 'steam_join', -2)
    c.branch('search_done', 0x15)
    c.label('race_not_join')
    # The console's player popup (F2 with the cursor on another player's bar): the host's VIEW STEAM PROFILE / MUTE / KICK /
    # RETURN TO LOBBY, a guest's VIEW STEAM PROFILE / MUTE / RETURN TO LOBBY. The answer comes back through callback_entry into POPUP_STATE and
    # is taken at the top of the next lobby frame (make_chunks).
    c.get(START_RESULT).num(FRAME_POPUP).raw(b'\x4a').branch('race_not_popup')
    racer.sound(c, 'FE_SELECT')
    nat(c, NAT_IS_HOST).branch('race_popup_guest')
    lobby_notice(c, POPUP_TEXT, POPUP_HOST, POPUP_HOST_STATE)  # the answer is routed by the popup posted (1 host, 40 guest), not by the live host flag
    c.branch('search_done', 0x15).label('race_popup_guest')
    lobby_notice(c, POPUP_TEXT, POPUP_GUEST, POPUP_GUEST_STATE)
    c.branch('search_done', 0x15).label('race_not_popup')
    # The same about a muted player (M8): UNMUTE where MUTE was; the answers are the same (MUTE toggles).
    c.get(START_RESULT).num(FRAME_POPUP_MUTED).raw(b'\x4a').branch('race_not_popup_muted')
    racer.sound(c, 'FE_SELECT')
    nat(c, NAT_IS_HOST).branch('race_popup_muted_guest')
    lobby_notice(c, POPUP_TEXT, POPUP_HOST_UNMUTE, POPUP_HOST_STATE)
    c.branch('search_done', 0x15).label('race_popup_muted_guest')
    lobby_notice(c, POPUP_TEXT, POPUP_GUEST_UNMUTE, POPUP_GUEST_STATE)
    c.branch('search_done', 0x15).label('race_not_popup_muted')
    # The host's Start refused with the console's dialogs (OK): fewer than two players / not everyone ready.
    c.get(START_RESULT).num(FRAME_START_ALONE).raw(b'\x4a').branch('race_not_alone')
    lobby_notice(c, ALONE_TEXT, NOTICE_ALONE)
    c.branch('search_done', 0x15).label('race_not_alone')
    c.get(START_RESULT).num(FRAME_START_NOT_READY).raw(b'\x4a').branch('race_not_unready')
    lobby_notice(c, NOT_READY_TEXT, NOTICE_OK)
    c.branch('search_done', 0x15).label('race_not_unready')
    cursor_keys(c)
    c.label('race_frame_done')


def cursor_keys(c):
    """The lobby cursor (PS3/360/Mac): Up/Down move it over the players' bars, for everyone (the red bar follows: draw_bars; the
    footer's F2 label with it); Esc = BACK asks first (a guest "leave the lobby?", a host with other players "quit? This will
    end the session for other players"; a host alone leaves at once through the inherited Back); the A key = INVITE FRIENDS
    (the Steam overlay's invite dialog); F2 on another player's bar ends the frame here when the popup was refused (a test
    player's bar), so the inherited code doesn't open the racer picker for it; F2 on your own bar reaches the inherited
    lobby_accept = CHANGE RACER. Not while the racer popup or the Settings panel is open (they take the keys themselves), nor
    during the inherited frame's key debounce (base.DELAY)."""
    c.get(racer.MODE).get(settings.SETTINGS_MODE).raw(b'\x50').branch('cursor_no_modal')
    c.branch('cursor_keys_done', 0x15)
    c.label('cursor_no_modal').get(base.DELAY).num(0).raw(b'\x48').branch('cursor_live')
    c.branch('cursor_keys_done', 0x15)
    c.label('cursor_live')
    # Down: the bar below, never past the last one; Up: the bar above, never above the first.
    c.native(DOWN_KEY, 0, drop=False).branch('cursor_not_down')
    c.get(CURSOR).num(1).raw(b'\x42').get(MEMBERS).raw(b'\x46').branch('cursor_keys_done')
    c.get(CURSOR).num(1).raw(b'\x42').put(CURSOR).branch('cursor_moved', 0x15)
    c.label('cursor_not_down').native(UP_KEY, 0, drop=False).branch('cursor_not_up')
    c.get(CURSOR).num(0).raw(b'\x48').branch('cursor_keys_done')
    c.get(CURSOR).num(1).raw(b'\x43').put(CURSOR)
    c.label('cursor_moved')
    racer.sound(c, 'FE_MOVE')
    c.assign(LOBBY_REV, -1).branch('cursor_keys_done', 0x15)  # (the bars redraw: the highlight and the footer follow)
    # Esc: the confirm for a guest, or a host with other players; a host alone falls through to the inherited Back.
    c.label('cursor_not_up').native(BACK_KEY, 0, drop=False).branch('cursor_not_back')
    nat(c, NAT_IS_HOST).branch('cursor_back_guest')
    c.get(MEMBERS).num(1).raw(b'\x48').branch('cursor_keys_done')
    lobby_notice(c, QUIT_TEXT, NOTICE_QUIT_SESSION, 20)
    c.branch('search_done', 0x15).label('cursor_back_guest')
    lobby_notice(c, LEAVE_TEXT, NOTICE_YES_NO, 20)
    c.branch('search_done', 0x15)
    # INVITE FRIENDS: the A key (config [Menus] InviteKey), the Steam overlay's invite dialog for this lobby.
    c.label('cursor_not_back')
    nat(c, NAT_INVITE_KEY).branch('cursor_no_invite')
    nat(c, NAT_INVITE).branch('cursor_keys_done')
    racer.sound(c, 'FE_SELECT')
    c.branch('cursor_keys_done', 0x15)
    # F2 with the cursor on another player's bar: the frame native opened the popup (and ended the frame) or refused it (a test
    # player's bar: FE_BACK above); either way the inherited CHANGE RACER must not run for this press.
    c.label('cursor_no_invite').native(LOBBY_SELECT_KEY, 0, drop=False).branch('cursor_keys_done')
    c.get(CURSOR).num(0).raw(b'\x46').branch('cursor_select_set')
    c.branch('cursor_keys_done', 0x15).label('cursor_select_set')
    c.get(CURSOR).get(MEMBERS).raw(b'\x46').branch('cursor_keys_done')
    c.get(CURSOR).num(MEMBER_IS_ME)
    nat(c, NAT_MEMBER_FIELD, 2).branch('cursor_select_other')
    c.branch('cursor_keys_done', 0x15).label('cursor_select_other')
    c.branch('search_done', 0x15)
    c.label('cursor_keys_done')


def draw_lobby_rules(c):
    """settings.draw_lobby without bar 0's name and portrait (Codex's offline row showed this player's
    racer there): the bars are draw_bars'."""
    for index, (key, slot) in enumerate(zip(base.RULE_KEYS, settings.RULE_SLOTS)):
        c.ident(key).get(0x16).num(index).raw(b'\x23').get(slot).raw(b'\x23\x3c').native(0x021f, 2)
    c.get(settings.COURSE_COUNT).num(0).raw(b'\x48').branch('lobby_course_unavailable')
    c.ident(settings.COURSE_ART).native(0x0217, 1)
    c.ident(settings.COURSE_NAME).native(0x0217, 1)
    c.ident(settings.COURSE_ART)
    settings.course_tuple(c).num(0).raw(b'\x23\x3c').native(0x0228, 2)
    c.ident(settings.COURSE_NAME)
    settings.course_tuple(c).num(1).raw(b'\x23\x3c').native(0x021f, 2)
    c.branch('lobby_course_done', 0x15).label('lobby_course_unavailable')
    c.ident(settings.COURSE_ART).native(0x0218, 1)
    c.ident(settings.COURSE_NAME).native(0x0218, 1)
    c.label('lobby_course_done')
    c.assign(base.DIRTY, 0)


_original_lobby_frame = settings.lobby_frame


def lobby_frame_with_leave():
    """settings.lobby_frame, plus leaving the Steam lobby when Back returns to the main menu."""
    c = _original_lobby_frame()
    at = c.labels['plain_render']
    # The Back branch ends with `assign(0x20, 1)` then a jump to plain_done; leave just before it.
    fragment = Code()
    nat(fragment, NAT_LEAVE, drop=True)
    back = c.data.rfind(b'\x51\x01\x27\x20\x00', 0, at)
    assert back > 0
    search.insert_fragment(c, back, fragment)
    return c


def make_chunks(original):
    captured = {}
    old_make, old_close, old_frame, old_draw = racer.make_chunks, racer.close_popup, settings.lobby_frame, settings.draw_lobby
    old_bounds, old_hints = settings.BOUNDS, settings.lobby_hints
    end = base.u32(original, 12) + base.u32(original, 16)  # the subroutines' chunk: after the module's code

    def close_with_reason(c, commit):
        tag = 'creation_close_' + str(commit)
        c.get(CREATING).branch(tag).assign(CREATE_RESULT, 1 if commit else 2).label(tag)
        old_close(c, commit)

    def make_with_entry(original):
        chunks, detail = old_make(original)
        c = chunks[0x1672]
        at = c.labels['lobby_ready']
        fragment = Code().get(AUTO_CREATE).branch('creation_normal_guard').branch('lobby_accept', 0x15).label('creation_normal_guard')
        search.insert_fragment(c, at, fragment, 'lobby_ready')
        detail['base_frame_offset_in_wrapper'] += len(fragment.data)
        at = c.labels['lobby_accept']
        op = next(o for o in base.instructions(c.data, at, at + 8, '<'))
        assert op['opcode'] == 0x32 and struct.unpack_from('<H', c.data, at + 2)[0] == 0x419
        # CHANGE RACER on F1 (the user's layout of 2026-10-23 evening; before, F2 = the Mac's triangle): Enter is the host's START
        # RACE. F1 with the cursor on another player's bar never reaches this code (race_frame / cursor_keys end the frame first).
        struct.pack_into('<H', c.data, at + 2, LOBBY_SELECT_KEY)
        fragment = Code().get(AUTO_CREATE).raw(b'\x50').get(AUTO_CREATE).branch('creation_attempt_marked')
        fragment.assign(AUTO_CREATE, 2).label('creation_attempt_marked')
        search.insert_fragment(c, op['end'], fragment)
        detail['base_frame_offset_in_wrapper'] += len(fragment.data)
        captured['racer_frame'] = c
        return chunks, detail

    try:
        racer.make_chunks, racer.close_popup, settings.lobby_frame = make_with_entry, close_with_reason, lobby_frame_with_leave
        settings.draw_lobby = draw_lobby_rules
        settings.lobby_hints = lambda c: c.call_chunk(end, FOOTER_LABEL)  # every lobby footer: ours (host or guest)
        settings.BOUNDS = ((1, 9),) + old_bounds[1:]  # laps 1-9 (the label table has 0-9; Codex's preview stopped at 3)
        chunks, detail = settings.make_chunks(original)
    finally:
        racer.make_chunks, racer.close_popup, settings.lobby_frame = old_make, old_close, old_frame
        settings.draw_lobby, settings.BOUNDS, settings.lobby_hints = old_draw, old_bounds, old_hints
    lobby_show = chunks[0xf95]
    for slot, value in ((FLOW, 0), (ACTION, -1), (AUTO_CREATE, 0), (CREATING, 0), (CREATE_RESULT, 0),
                        (SEL, 0), (PREV, 0), (ROW_COUNT, 0), (LOBBY_REV, -1), (MEMBERS, 0), (LAST_ERROR, 0), (START_RESULT, 0), (HILITE, -1), (WANT, -1), (RACER_KEY, 0),
                        (LAST_HOST_CHANGE, 0), (CURSOR, -1), (POPUP_STATE, 0), (ERR_MAIN, 0), (ME_LAST, -1), (WAITING, 0), (SPIN_AGE, 0), (AUTO_HELD, 0), (FLAG_KEY, 0),
                        (MODS_STATE, 0), (MODS_METAL, 0), (MODS_EGG, 0), (MODS_ROW, 0), (MODS_ROWS, 0), (MODS_HAVE_METAL, 0)):
        chunks[0xf7f].assign(slot, value)
    for _ in range(2):  # the MODIFICATIONS popup's row labels (as Preview2's buffers)
        chunks[0xf7f].raw(b'\x55\x60').native(0x1610, 1, drop=False)
    chunks[0xf7f].array(2).put(MODS_BUFS)
    initial = Code()
    search.search_show(initial, 'initial')  # ends with ACTION = -2: search once the screen is ready
    # front_main came back from an online race through export 0x45b79703 (g22 = 1; the lobby Show
    # clears it): re-enter the lobby instead of searching (ACTION -7).
    initial.get(RETURNED).branch('initial_search').assign(RETURNED, 0).assign(ACTION, -7).label('initial_search')
    chunks[0xf95] = initial
    inherited = chunks[0x1672]
    # Only the host opens Settings (F2 since the user's swap of 2026-10-23 evening; Codex's F1), as on the console: a guest's F2
    # does nothing.
    at = inherited.labels['settings_entry']
    ops = list(base.instructions(inherited.data, at, at + 11, '<'))
    assert [o['opcode'] for o in ops[:2]] == [0x32, 0x14] and struct.unpack_from('<H', inherited.data, at + 2)[0] == 0x041d
    struct.pack_into('<H', inherited.data, at + 2, LOBBY_SETTINGS_KEY)
    search.insert_fragment(inherited, ops[1]['end'], nat(Code(), NAT_IS_HOST).branch('settings_run_racer'))
    end = base.u32(original, 12) + base.u32(original, 16)  # the subroutines' chunk: after the module's code

    c = Code().get(FLOW).branch('search_active')
    # ---- lobby
    # M8, the console's wait (Mac 0x2808 / 0x29b0): while Steam creates or joins the lobby, the Show's empty panel and the spinner
    # stay and nothing else runs (no bars, no racer picker); Back cancels and leaves, as the console's; once Steam is done (and the
    # spinner has turned once) it fades out and the lobby goes on: in the lobby the bars and the racer picker, a failure the lost-lobby
    # path below (back to the search screen with the error notice).
    c.get(WAITING).branch('lobby_not_waiting')
    c.native(BACK_KEY, 0, drop=False).branch('lobby_wait_no_back')
    c.assign(WAITING, 0)
    spinner_off(c)
    nat(c, NAT_LEAVE, drop=True)
    search.return_mainmenu(c)  # (plays FE_BACK itself)
    c.branch('search_done', 0x15).label('lobby_wait_no_back')
    nat(c, NAT_STATE).num(CREATING_).raw(b'\x4a')
    nat(c, NAT_STATE).num(JOINING).raw(b'\x4a\x50').branch('lobby_wait_answered')
    c.get(SPIN_AGE).num(1).raw(b'\x42').put(SPIN_AGE)
    c.branch('search_done', 0x15).label('lobby_wait_answered')
    spinner_aged(c, 'lobby_wait_aged')
    c.assign(WAITING, 0)
    spinner_fade_out(c)
    c.label('lobby_not_waiting')
    nat(c, NAT_REVISION).get(LOBBY_REV).raw(b'\x4a').branch('lobby_redraw')
    c.branch('lobby_drawn', 0x15).label('lobby_redraw')
    nat(c, NAT_REVISION).put(LOBBY_REV)
    c.call_chunk(end, DRAW_LABEL)
    c.label('lobby_drawn')
    # While a dialog is up (a notice, the player popup, a confirm), the lobby takes no keys and runs no frame logic: Esc would
    # also run Back (leaving the lobby), Enter would also pick a popup's choice, a Steam join would leave with the dialog open
    # (the search screen gates on the same flag). The lobby's own exits (a lost lobby, below) wait here too: the screen is never
    # left while one of its dialogs is up, or its answer would reach the search screen's callback as a REFRESH / CREATE / CANCEL
    # choice (found by the m7zw check).
    c.ident(search.DIALOGS).ident(0x53335ccb).native(0x1607, 2, drop=False).branch('lobby_dialog_idle')
    c.branch('search_done', 0x15).label('lobby_dialog_idle')
    # The lobby's dialogs' answers (callback_entry -> POPUP_STATE): the host's player popup (2 VIEW STEAM PROFILE, 3 MUTE, 4 KICK,
    # 5 RETURN TO LOBBY), a guest's (41 VIEW STEAM PROFILE, 42 MUTE, 43 RETURN TO LOBBY), 9 a popup closed, the kick confirm (12 YES, 13 NO), the leave
    # confirm (22 YES, 23 NO) and a plain notice (33). The frame that takes an answer does nothing else: the Enter/Esc that
    # answered may still read as pressed (Start / Back / the racer picker). Any other value above 1 (an unknown answer) is reset.
    c.get(POPUP_STATE).num(2).raw(b'\x46').branch('popup_answered')
    c.branch('popup_answer_done', 0x15).label('popup_answered')
    # A question still up (40, 10, 20, 30; 1 is below 2) is left alone: should the dialog take a frame to appear, this block
    # must not mistake its state for an answer (the OK's Enter would then start the race again, a kick would be lost).
    c.get(POPUP_STATE).num(POPUP_GUEST_STATE).raw(b'\x4a').get(POPUP_STATE).num(10).raw(b'\x4a\x50').get(POPUP_STATE).num(20).raw(b'\x4a\x50')
    c.get(POPUP_STATE).num(30).raw(b'\x4a\x50').branch('popup_taken')
    c.branch('popup_answer_done', 0x15).label('popup_taken')
    c.get(POPUP_STATE).num(POPUP_HOST_STATE + 1 + POPUP_VIEW).raw(b'\x4a').get(POPUP_STATE).num(POPUP_GUEST_STATE + 1 + POPUP_VIEW).raw(b'\x4a\x50').branch('popup_not_view')
    racer.sound(c, 'FE_SELECT')
    c.get(CURSOR)
    nat(c, NAT_MEMBER_PROFILE, 1, drop=True)
    c.branch('popup_answer_reset', 0x15).label('popup_not_view')
    # MUTE (a toggle on this game only, about the player the popup was about); the bar then turns yellow (a redraw follows).
    c.get(POPUP_STATE).num(POPUP_HOST_STATE + 1 + POPUP_MUTE).raw(b'\x4a').get(POPUP_STATE).num(POPUP_GUEST_STATE + 1 + POPUP_MUTE).raw(b'\x4a\x50').branch('popup_not_mute')
    racer.sound(c, 'FE_SELECT')
    c.get(CURSOR)
    nat(c, NAT_MEMBER_MUTE, 1, drop=True)
    c.branch('popup_answer_reset', 0x15).label('popup_not_mute')
    c.get(POPUP_STATE).num(POPUP_HOST_STATE + 1 + POPUP_KICK).raw(b'\x4a').branch('popup_not_kick')
    lobby_notice(c, KICK_ASK, NOTICE_YES_NO, 10)  # the console's kick confirm (its kick native asked it)
    c.branch('search_done', 0x15)
    c.label('popup_not_kick').get(POPUP_STATE).num(12).raw(b'\x4a').branch('popup_not_kick_yes')
    c.get(CURSOR)  # (the native kicks the player the question was about, by Steam id; the bar is for the log)
    nat(c, NAT_KICK, 1).branch('popup_answer_reset')
    racer.sound(c, 'FE_SELECT')
    c.branch('popup_answer_reset', 0x15)
    c.label('popup_not_kick_yes').get(POPUP_STATE).num(22).raw(b'\x4a').branch('popup_answer_reset')
    # Leaving (the confirm's YES): the Steam lobby, then the inherited Back's way to the main menu (lobby_frame_with_leave).
    c.assign(POPUP_STATE, 0)
    nat(c, NAT_LEAVE, drop=True)
    search.return_mainmenu(c)
    c.branch('search_done', 0x15)
    c.label('popup_answer_reset').assign(POPUP_STATE, 0).assign(LOBBY_REV, -1)
    c.branch('search_done', 0x15)
    c.label('popup_answer_done')
    # Steam lost the lobby (host left, creation failed): back to Search once no modal is open.
    c.get(racer.MODE).get(settings.SETTINGS_MODE).raw(b'\x50').branch('lobby_no_modal')
    c.branch('lobby_steam_done', 0x15).label('lobby_no_modal')
    nat(c, NAT_STATE).num(ERROR).raw(b'\x4a')
    nat(c, NAT_STATE).num(OFF).raw(b'\x4a\x50').branch('lobby_no_error')  # error, or Steam gone
    leave_lobby_to_search(c, 'lobby_error', -6)
    c.branch('search_done', 0x15).label('lobby_no_error')
    c.label('lobby_steam_done')
    # Host migration (M7): the host left and Steam passed the lobby on (the bars and footer follow the new
    # host through the snapshot); one notice, once no popup or dialog is open, says who hosts now (a count above the
    # one last noticed; the count reads 0 outside a lobby).
    nat(c, NAT_HOST_CHANGES).get(LAST_HOST_CHANGE).raw(b'\x48').branch('host_change_done')
    c.get(racer.MODE).get(settings.SETTINGS_MODE).raw(b'\x50').branch('host_change_notice')
    c.branch('host_change_done', 0x15).label('host_change_notice')
    nat(c, NAT_HOST_CHANGES).put(LAST_HOST_CHANGE)
    nat(c, NAT_IS_HOST).branch('host_changed_guest')
    lobby_notice(c, HOST_NOW, NOTICE_OK)
    c.branch('search_done', 0x15).label('host_changed_guest')
    lobby_notice(c, HOST_CHANGED, NOTICE_OK)
    c.branch('search_done', 0x15)
    c.label('host_change_done')
    race_frame(c)
    # A player who joined while a race runs waits with the race bar and no racer picker; the picker opens by itself once that race
    # is over, as the console's (PS3 fe_network_game 0x2962..0x29be, 0x2b14..0x2bd2; re\m8-audit A4).
    # The request is parked in AUTO_HELD (AUTO_CREATE 0: the inherited frame's own copy of the picker's entry would open it too).
    c.get(AUTO_CREATE).num(1).raw(b'\x4a').branch('auto_not_new')
    c.num(0).num(MEMBER_RACING_HERE)
    nat(c, NAT_MEMBER_FIELD, 2).branch('auto_not_new')
    c.assign(AUTO_CREATE, 0).assign(AUTO_HELD, 1)
    c.label('auto_not_new')
    c.get(AUTO_HELD).branch('auto_not_held')
    c.num(0).num(MEMBER_RACING_HERE)
    nat(c, NAT_MEMBER_FIELD, 2).branch('auto_release')
    c.branch('auto_not_held', 0x15)
    c.label('auto_release').assign(AUTO_HELD, 0).assign(AUTO_CREATE, 1)
    c.label('auto_not_held')
    c.get(AUTO_CREATE).branch('regular_lobby_frame')
    settings.append_code(c, captured['racer_frame'])
    c.get(AUTO_CREATE).num(2).raw(b'\x4a').branch('search_done')
    c.assign(AUTO_CREATE, 0)
    c.get(racer.MODE).num(1).raw(b'\x4a').branch('creation_failed')
    c.branch('search_done', 0x15).label('creation_failed')
    c.assign(CREATE_RESULT, 2).branch('creation_result', 0x15)
    c.label('regular_lobby_frame')
    settings.append_code(c, inherited)
    c.label('creation_result').get(CREATING).branch('search_done')
    c.get(racer.MODE).num(0).raw(b'\x4a').branch('search_done')
    c.get(CREATE_RESULT).num(1).raw(b'\x4a').branch('creation_cancel_check')
    c.assign(CREATING, 0).assign(CREATE_RESULT, 0).branch('search_done', 0x15)
    c.label('creation_cancel_check').get(CREATE_RESULT).num(2).raw(b'\x4a').branch('search_done')
    leave_lobby_to_search(c, 'creation_cancel', -2)
    c.branch('search_done', 0x15)

    # ---- search
    c.label('search_active').ident(search.DIALOGS).ident(0x53335ccb).native(0x1607, 2, drop=False).branch('dialog_idle')
    # A dialog is up. 1.1: the modifications question closes on Esc once shown (its opening done, as its own Enter waits); the
    # frame does nothing else (the dialog's close answers next frame; its layer switch makes the held Esc "release first").
    c.get(MODS_STATE).num(MODS_ASKED).raw(b'\x4a').branch('search_done')
    c.ident(DIALOG_SCENE).native(0x0213, 1, drop=False).branch('search_done')
    nat(c, NAT_DIALOG_BACK).branch('search_done')
    racer.sound(c, 'FE_BACK')
    c.ident(search.DIALOGS).ident(DIALOG_CLOSE).raw(b'\x31').native(0x160c, 3)
    c.branch('search_done', 0x15)
    c.label('dialog_idle').ident(0x5cf9607b).native(0x0213, 1, drop=False).branch('search_done')
    mods_frame(c)  # 1.1: the CREATE question's answers come first; nothing else runs while it is up
    # A Steam Join Game / invite (from the main menu, a lobby just left, or this screen): join that lobby.
    nat(c, NAT_JOIN_POLL).branch('no_steam_join')
    nat(c, NAT_JOIN_TAKE).branch('no_steam_join')
    c.branch('shared_join_lobby', 0x15)  # (the lobby with JOINING until Steam's join is done, as the console's)
    c.label('no_steam_join')
    # -7: back from an online race
    c.get(ACTION).num(-7).raw(b'\x4a').branch('not_reenter')
    c.branch('shared_reenter_lobby', 0x15).label('not_reenter')
    # -2: start the search
    c.get(ACTION).num(-2).raw(b'\x4a').branch('not_start')
    start_search(c)
    c.branch('search_done', 0x15).label('not_start')
    # -6: a lobby error brought us back here
    c.get(ACTION).num(-6).raw(b'\x4a').branch('not_lobby_error')
    c.branch('shared_error_notice', 0x15).label('not_lobby_error')
    # -3: searching
    c.get(ACTION).num(-3).raw(b'\x4a').branch('not_searching')
    c.native(0x041a, 0, drop=False).branch('searching_no_back')
    nat(c, NAT_LEAVE, drop=True)  # cancels the search
    search.return_mainmenu(c)
    c.branch('search_done', 0x15).label('searching_no_back')
    c.native(0x041d, 0, drop=False).branch('searching_no_create')
    racer.sound(c, 'FE_SELECT')
    create_request(c, 'searching_create')
    c.label('searching_no_create')
    nat(c, NAT_STATE).num(SEARCHING).raw(b'\x4a').branch('search_answered')
    c.get(SPIN_AGE).num(1).raw(b'\x42').put(SPIN_AGE)
    c.branch('search_done', 0x15).label('search_answered')
    spinner_aged(c, 'search_aged')  # (the SEARCHING spinner turns once at least, then fades out as the console's at the answer)
    spinner_fade_out(c)
    nat(c, NAT_STATE).num(RESULTS).raw(b'\x4a').branch('search_failed')
    nat(c, NAT_ROWS).put(ROW_COUNT)
    c.get(ROW_COUNT).num(0).raw(b'\x48').branch('no_games')
    c.assign(ACTION, -4)
    show_results(c)
    c.branch('search_done', 0x15).label('no_games')
    notice(c, NO_GAMES)
    c.branch('search_done', 0x15).label('search_failed')
    capture_error(c, 'search_error_code')
    c.branch('shared_error_notice', 0x15).label('not_searching')
    # -4: browsing rows
    c.get(ACTION).num(-4).raw(b'\x4a').branch('not_browsing')
    c.get(base.DELAY).num(0).raw(b'\x48').branch('browse_inputs')
    c.get(base.DELAY).num(1).raw(b'\x43').put(base.DELAY).branch('search_done', 0x15)
    c.label('browse_inputs').native(0x041a, 0, drop=False).branch('browse_create')
    search.return_mainmenu(c)
    c.branch('search_done', 0x15)
    c.label('browse_create').native(0x041d, 0, drop=False).branch('browse_join')
    racer.sound(c, 'FE_SELECT')
    create_request(c, 'browse_create')
    c.label('browse_join').native(0x0419, 0, drop=False).branch('browse_profile')
    c.get(SEL)
    nat(c, NAT_JOIN, 1).branch('search_done')
    racer.sound(c, 'FE_SELECT')
    c.branch('shared_join_lobby', 0x15)  # (the lobby with JOINING until Steam's join is done, as the console's)
    # The A key: the selected row's host's Steam profile (the console's VIEW PROFILE / GAMER CARD), through the overlay.
    c.label('browse_profile')
    nat(c, NAT_INVITE_KEY).branch('browse_refresh')
    c.get(SEL)
    nat(c, NAT_ROW_PROFILE, 1).branch('search_done')
    racer.sound(c, 'FE_SELECT')
    c.branch('search_done', 0x15)
    # F2: REFRESH (the Mac's; the console's START): search again.
    c.label('browse_refresh').native(SELECT_KEY, 0, drop=False).branch('browse_up')
    racer.sound(c, 'FE_SELECT')
    c.assign(ACTION, -2).branch('search_done', 0x15)
    c.label('browse_up').native(0x0421, 0, drop=False).branch('browse_down')
    c.get(SEL).num(0).raw(b'\x48').branch('search_done')
    c.get(SEL).num(1).raw(b'\x43').put(SEL).branch('browse_moved', 0x15)
    c.label('browse_down').native(0x0422, 0, drop=False).branch('search_done')
    c.get(SEL).get(ROW_COUNT).num(1).raw(b'\x43\x46').branch('search_done')
    c.get(SEL).num(1).raw(b'\x42').put(SEL)
    c.label('browse_moved')
    racer.sound(c, 'FE_MOVE')
    highlight(c, PREV, False)
    highlight(c, SEL, True)
    c.get(SEL).put(PREV).branch('search_done', 0x15)
    c.label('not_browsing')
    # (-5, a join waiting on this screen, is gone since M8: a join shows the lobby at once with JOINING, the lobby frame waits.)
    # 0/1/2: the notice was answered
    c.get(ACTION).num(0).raw(b'\x46').branch('have_action')
    c.branch('search_done', 0x15)
    c.label('have_action').get(ERR_MAIN).branch('have_action_choice')
    c.assign(ERR_MAIN, 0)  # the console's OK box: back to the main menu
    nat(c, NAT_LEAVE, drop=True)
    search.return_mainmenu(c)
    c.branch('search_done', 0x15)
    c.label('have_action_choice').get(ACTION).num(0).raw(b'\x4a').branch('create_action')
    c.assign(ACTION, -2).branch('search_done', 0x15)  # Refresh
    c.label('create_action').get(ACTION).num(1).raw(b'\x4a').branch('cancel_action')
    c.assign(ACTION, -2)  # (if the question is closed: search again)
    create_request(c, 'notice_create')
    c.label('cancel_action')
    nat(c, NAT_LEAVE, drop=True)
    search.return_mainmenu(c)
    c.branch('search_done', 0x15)
    shared_host(c)
    shared_enter_lobby(c, end)
    shared_error_notice(c, end)
    c.label('search_done')
    chunks[0x1672] = c

    # The notice's answer callback, as in search.make_chunks (close = Cancel), then the Show subroutine.
    callback = Code().branch('callback_after', 0x15).label('callback_entry')
    callback.get(FLOW).num(0).raw(b'\x4a').branch('callback_lobby')
    # On the search screen a pending lobby dialog's answer (the lobby was lost in the one frame before its dialog showed, so
    # the lobby's exit ran with it pending) is dropped: the search notice that follows is the one whose answer counts.
    # 1.1: the CREATE question's answers (mods_frame): + 1 NO, + 2 YES, + 8 closed (Esc).
    for s in (MODS_ASKED,):
        callback.get(MODS_STATE).num(s).raw(b'\x4a').branch('callback_mods_not_' + str(s))
        callback.raw(b'\x0b\x01').num(0).raw(b'\x46').branch('callback_mods_answer_' + str(s))
        callback.assign(MODS_STATE, s + 8).branch('callback_return', 0x15)
        callback.label('callback_mods_answer_' + str(s)).raw(b'\x0b\x01').num(s + 1).raw(b'\x42').put(MODS_STATE)
        callback.branch('callback_return', 0x15).label('callback_mods_not_' + str(s))
    callback.get(POPUP_STATE).branch('callback_search')
    callback.assign(POPUP_STATE, 0).branch('callback_return', 0x15)
    callback.label('callback_search').raw(b'\x0b\x01').num(-1).raw(b'\x4a').branch('callback_normal')
    callback.assign(ACTION, 2).branch('callback_return', 0x15)
    callback.label('callback_normal').raw(b'\x0b\x01').num(0).raw(b'\x46').branch('callback_upper')
    callback.branch('callback_return', 0x15)
    callback.label('callback_upper').raw(b'\x0b\x01').num(2).raw(b'\x48').branch('callback_store')
    callback.branch('callback_return', 0x15)
    callback.label('callback_store').raw(b'\x0b\x01').put(ACTION).branch('callback_return', 0x15)
    # The lobby's dialogs (lobby_notice): the answer goes into POPUP_STATE for the next lobby frame. The player popup (1 the
    # host's, 40 a guest's) -> the state + 1 + the button index (2..5 / 41..43; closed, -1 -> 9: nothing); the kick confirm (10) ->
    # 12 YES / 13 NO or closed; the leave confirm (20) -> 22 YES / 23 NO or closed; a plain notice (30) -> 33. Any other state:
    # the answer is ignored.
    callback.label('callback_lobby').get(POPUP_STATE).num(POPUP_HOST_STATE).raw(b'\x4a').get(POPUP_STATE).num(POPUP_GUEST_STATE).raw(b'\x4a\x50').branch('callback_not_popup')
    callback.raw(b'\x0b\x01').num(0).raw(b'\x46').branch('callback_popup_answer')
    callback.assign(POPUP_STATE, 9).branch('callback_return', 0x15)
    callback.label('callback_popup_answer').get(POPUP_STATE).num(1).raw(b'\x42').raw(b'\x0b\x01').raw(b'\x42').put(POPUP_STATE)
    callback.branch('callback_return', 0x15)
    callback.label('callback_not_popup').get(POPUP_STATE).num(10).raw(b'\x4a')
    callback.get(POPUP_STATE).num(20).raw(b'\x4a\x50').branch('callback_not_confirm')
    callback.get(POPUP_STATE).num(3).raw(b'\x42').put(POPUP_STATE)  # NO, or closed
    callback.raw(b'\x0b\x01').num(KICK_YES).raw(b'\x4a').branch('callback_return')
    callback.get(POPUP_STATE).num(1).raw(b'\x43').put(POPUP_STATE)  # YES
    callback.branch('callback_return', 0x15)
    callback.label('callback_not_confirm').get(POPUP_STATE).num(30).raw(b'\x4a').branch('callback_return')
    callback.assign(POPUP_STATE, 33)
    callback.label('callback_return').raw(b'\x17\x1b')
    callback.label(SHOW_LABEL)
    settings.append_code(callback, lobby_show)
    callback.raw(b'\x17\x1b')
    callback.label(DRAW_LABEL)
    draw_bars(callback)
    callback.raw(b'\x17\x1b')
    callback.label(FOOTER_LABEL)
    lobby_footer(callback, 'footer')
    callback.raw(b'\x17\x1b')
    callback.label(REFUSAL_LABEL)
    mods_refusal(callback)
    callback.raw(b'\x17\x1b').label('callback_after')
    chunks[end] = callback
    return chunks, dict(callback_original_anchor=end, callback_relative_entry=callback.labels['callback_entry'],
                        inherited_settings_details=detail)
