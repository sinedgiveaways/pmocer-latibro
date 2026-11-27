# Steam Remote Play Together: status

16/11/2026, branch `cleanup/mkw-removal`. Read from the code and the build logs; nothing here was run with Orbitals.

## Verdict: partial

The feature is **implemented in full and builds**, and it is the same code that shipped in the earlier project's
release `v0.1.11-rpt8`. It was checked against a mock of Steam, never in a real session, and **not run with Orbitals**.
What decides whether it is useful there (does Orbitals offer local multiplayer, through which SDK calls) is not known
yet.

| | State |
|---|---|
| Steam API start, callbacks, sessions, overlay invite, guest link | Implemented |
| Guests' controllers as players, guests' keyboards as players | Implemented (generic: SDL gamepads and Steam direct input) |
| The "local players or Remote Play Together" choice when controllers are missing | Implemented, hooked on `nn::hid::ShowControllerSupport`; whether Orbitals calls it is unknown |
| Settings, menu rows, texts, guide (`docs/remote-play.md`) | Present, named `ORBITALS_STEAM_*` |
| Steam's library in the Windows package | In the zip since 16/11/2026 (`steamlib.py` run by the workflows, `release.py` packs it) |
| Tests | **None** for this code (`tests/` has no Steam test; CI only compiles it) |
| Run against a real Steam, with the first game or with Orbitals | **Not done** (a mock only, 13/11/2026) |

## Evidence

- **Same implementation as `v0.1.11-rpt8`.** The release `v0.1.11-rpt8` of `sinedgiveaways/mkw-recomp` (MIT, 14/11/2026)
  has `runtime/steam/{steam_rpt.c, steam_rpt.h, steam_flat.h}`. After mapping the names back (`ORBITALS_` to `MKW_`,
  `orbitals-together` to `mkw`, `orbitals.ini` to `mkw.ini`), the three files here have **0 differing lines** (488, 58
  and 121 lines). So the rename changed no behaviour, and the plan "follow rpt8" is already what the tree holds. The
  release's notes mention Steam only for the Steam Deck. The guide's own "Tested" section (`docs/remote-play.md`, 13/11/2026)
  says what was run: Valve's SDK 1.65 library and a mock of Steam built from Valve's headers, SDL virtual gamepads
  standing in for the guests' controllers, and "a real session with friends over Steam has not been run yet". The mock is
  not in the repository. So even for the first game this was checked against a mock, never against a live Steam.
- **Calls to Steam** (`steam_flat.h:97-113`, bound by name at run time, `steam_rpt.c:417` `steam_rpt_start`):
  `SteamInternal_SteamAPI_Init`, manual callback dispatch (`ManualDispatch_*`), `ISteamRemotePlay::GetSessionCount`,
  `GetSessionID`, `GetSessionSteamID`, `GetSessionClientName`, `ShowRemotePlayTogetherUI`,
  `BSendRemotePlayTogetherInvite` (guest link), `BEnableRemotePlayTogetherDirectInput`, `GetInput`,
  `SetMouseVisibility`; `ISteamFriends::GetFriendPersonaName`. Interfaces `SteamFriends018` and
  `STEAMREMOTEPLAY_INTERFACE_VERSION004` (SDK 1.62 or newer).
- **Callbacks.** `SteamAPI_RunCallbacks` is replaced by manual dispatch on a dedicated `steam` thread every 8 ms
  (`steam_rpt.c:360` `steam_main`, `:290` `callbacks`), so no Steam code runs on a game thread. Sessions are also read
  again once a second (`:269` `sync_sessions`).
- **Start order.** `main.c:654` calls `steam_rpt_start()` before `rt_boot()`, so the overlay can hook the Vulkan device
  and SDL sees Steam's virtual gamepads (`steam_rpt.c:482`, `SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD`). It saves
  and gives back the signal handlers Steam replaces (`plat_handlers_save`).
- **Guests as players.** Controllers: `input.c:162` `steam_virtual`, `:240` joins them as the next free player (player 2).
  Keyboards: `steam_rpt.c:313` `read_keys` and `input.c:574` `input_remote_attach`, on the host's key table, only for a
  guest without a controller and after the guest's first 5 s.
- **The choice.** `ov_input.c:204` `ov_controller_support` calls `steam_rpt_multiplayer(lo, have)` (`steam_rpt.c:183`):
  with fewer controllers than players, a menu asks "local players" or "Remote Play Together" and the second opens the
  invite list (`steam_invite` = ask, auto, always, off).
- **Menu.** `settings_tabs.cpp:237` onward, the group "Remote Play Together": App ID (off, automatic, RetroArch 1118310,
  typed ones), Invite friends, Copy guest link.
- **Build.** The Windows build of pull request #2 compiles these files with no warning of their own.

## Update 27/11/2026 (build `81c27bf`, branch `orbitals-port`)

- Orbitals now reaches its first level in split screen with two players, each on a controller (logs83:
  `input: gamepad "Xbox One Controller" for player 2`, `hid: controllers initialized (... 2 players with a
  controller)`). Item 1 below is no longer a blocker.
- The Windows package carries `steam_api64.dll` (checked in `orbitals-together-0.2.0-81c27bf-windows-x64.zip`). The
  `kiro-orbitals-together-exe` artifact is the executable alone, without it.
- With no App ID set, `steam_rpt_start` turns itself off without a log line (logs83 has no `steam:` line). That is
  intended.
- With two controllers connected, logs83 has no `hid: controller applet asks for ...` line, so Orbitals did not call
  `ShowControllerSupport` then. Whether it calls it with only one controller (when the automatic "local players or
  Remote Play Together" question would appear) is the first thing the real test below shows.
- **The real test (asked of the owner on 27/11/2026):**
  - Set up: RetroArch (1118310) in Steam with `"<game folder>\orbitals-together.exe" %command%` as its launch options,
    `[steam] steam_appid = 1118310` in `orbitals.ini`, the second controller unplugged (the guest is player 2;
    `INPUT_MAX_PLAYERS` is 2), and a start with Play from Steam.
  - The log lines to look for, in this order: `steam: running as App ID 1118310; Remote Play Together ready`,
    `steam: ... joined`, `input: gamepad ... for player 2 (... a Remote Play guest's)`.

- **Result (27/11/2026):** the owner ran it with a friend: it works.

## Online Play through Remote Play Together (27/11/2026)

The owner asked that the game's own Online Play entry work too, and chose option 1: Online Play goes through Remote Play
Together rather than a real online mode between two copies of the port (option 2, weeks of work on Nintendo's online
layer, both players owning the port).

- `steam_rpt_online(what)` (`steam_rpt.c`): with Steam ready it opens the invite list and shows
  `UI_NOTICE_RPT_ONLINE` (start a local game for 2 once the friend is in). Without Steam it shows
  `UI_NOTICE_RPT_ONLINE_OFF` with the reason. At most once per 10 s; `steam_invite = off` turns it off.
- Its triggers are the calls a console answers with a screen for the player, which only follow a player's action:
  `nn::nifm::HandleNetworkRequestErrorResult` (the network error applet, `ov_net.c`) and
  `nn::account::EnsureNetworkServiceAccountAvailable` (the account sign-in, `ov_system.c`). Every other online call
  (`IsNetworkAvailable`, `SubmitNetworkRequestAndWait`, `IsNetworkServiceAccountAvailable`, the account id and token
  calls) is only logged.
- Every online call is logged as `online: <call> (call N, frame F) from <caller>` (the first 4 per kind, then every
  100th), with ": a player's request" on the triggers. The game still gets "no network" and takes its offline path.
- **Not known yet:** whether Orbitals' Online Play makes either trigger call. Its main imports 13 account, 10 friends
  and 43 `nn::async` functions the runtime does not override (logs83's imports line). The test log of Online Play
  shows which calls come: if neither trigger comes, the hook moves to the one that does.

### Test of 27/11/2026 (log header `0.2.0-g81c27bf`): invalid, old executable

- The owner chose "Partie en ligne" and saw no invite message. The log's first lines say `Orbitals Together 0.2.0-g81c27bf`
  and `steam: running as App ID 1118310; Remote Play Together ready`: the executable was the one before `5ba7f29`, which
  has no `steam_rpt_online` and no `online:` lines. The log has none, so it answers nothing about Online Play.
- The video (20 s) does show that "Partie locale" with one controller already opens the automatic choice ("Joueurs
  locaux" or "Steam Remote Play Together (inviter des amis)"): `ShowControllerSupport` is called by Orbitals' local mode.
  The log has `friends: TryPopFriendInvitationNotificationInfo: no invitation (offline)` at the menu, from an existing override.
- Next: the same test with an executable whose log header says `g5ba7f29` (or `gd116ac3`).

- **Result (27/11/2026, executable of `d116ac3`):** the owner reports that "Partie en ligne" now opens the Steam invite
  list with the notice and it works.
- **Log of that run (`0.2.0-gd116ac3`):** the trigger is `nifm::HandleNetworkRequestErrorResult`, called from
  `main+0x175ac` right after `IsNetworkAvailable` and `SubmitNetworkRequestAndWait` (`main+0x17570`, `main+0x175a0`),
  at frames 1421, 1864 and 3874 (3 uses of the menu). Each prints `steam: the game asked for online play ...: opening
  the Remote Play Together invite list`. `account::EnsureNetworkServiceAccountAvailable` never came;
  `account::IsNetworkServiceAccountAvailable` came once at frame 0 (boot). A friend joined (`steam: ... joined (session
  2 ... 1 guests)`) after the second use. The 10 s limit let all three through.

### Two local controllers when started from Steam (27/11/2026, `0.1.0-g3868cc3`)

- The owner started from Steam with two Xbox controllers plugged in: only player 1 had one. The log listed both as
  Steam virtual gamepads only (`28de:11ff`, `XInput#0` and `XInput#1`), no direct controller: Steam Input wraps them.
  `scan_pads` gave a host's virtual gamepad player 1 at most, so the second was dropped.
- Fix (`input.c` `scan_pads`): with no direct controller, each host's virtual gamepad takes the next free player. With a
  direct controller, they are still skipped (often the same controller twice). A guest's gamepad still needs a free
  player, so with two local controllers a guest gets none (the limit of 2).
- **Confirmed in game (27/11/2026, `2106b33`):** the owner reports both local controllers work when started from Steam.

### Start from the folder (27/11/2026)

- The owner played with a friend starting `orbitals-together.exe` from its folder (not with Play in Steam), the
  friend's controller working, with RetroArch's launch options set to the exe and `steam_appid = 1118310`. The public
  READMEs say both starts work, with the launch options required.

## What is missing or unknown

1. **No run with Orbitals.** It does not reach a scene yet ([roadmap.md](roadmap.md)), so none of the above is
   observed on it: not the overlay, not a guest joining, not a controller becoming player 2.
2. **Does Orbitals ask for local multiplayer, and how?** The choice menu only appears if Orbitals calls
   `nn::hid::ShowControllerSupport` with at least 2 players (`ov_input.c:575`). Without it, the Invite and Copy-link
   buttons and the guests' controllers still work if Orbitals reads several npad ids, but nothing opens the invite
   list by itself. Check in `port_check.md` (PowerShell, from the folder holding it):
   `Select-String -Path port_check.md -Pattern 'ShowControllerSupport|SetSupportedNpadIdType|GetNpadStates|SetNpadJoyAssignment'`.
   The result says which `nn::hid` calls Orbitals imports.
3. **Player count (decided 16/11/2026): 2.** `INPUT_MAX_PLAYERS` (`input.h`) is 2, the applet answer is clamped to
   it (`ov_input.c`), the guide says "2-player game", and one guest is a player. A second Remote Play guest still joins
   the stream; their keys and controller give no player ("no free player" notice). Orbitals' layout of the applet
   argument is unchecked.
4. **The App ID is borrowed (decided 16/11/2026).** Steam only offers Remote Play Together for an App ID that is in
   the host's library and has the feature on. This project is not a Steam app, so the runtime starts the Steam API as
   an App ID the host already owns (RetroArch, 1118310, is the documented default) and Steam shows that game's name to
   the guests, as in the earlier project. The guide says "Use an App ID you own or are allowed to use".
5. **Steam's library in the package (decided 16/11/2026).** The Windows zip includes `steam_api64.dll`, as the earlier
   project's release did: `.github/workflows/build-windows.yml` and `port_windows.yml` run
   `tools/build/steamlib.py --target windows` before `release.py`, which adds the file to the zip when it is beside the
   exe (SDK 1.65 from the Steamworks.NET release, checked by sha256). A local build without it packs the same zip as
   before. The AppImage does not carry `libsteam_api.so` yet: a Linux player still runs `steamlib.py`.
6. **No automated test.** Pure logic exists that could be tested without Steam: `steam_rpt_saved_ids` and
   `steam_rpt_saved_add` (the typed App IDs list), `is_app_id`, and the choice of `steam_rpt_multiplayer` by mode.
   The Steam calls themselves need a Steam client.
7. **Linux only paths.** Flatpak and Snap Steam cannot work (the guide says so); the AppImage's data folder for
   the library is `~/.local/share/orbitals-together`, now correct after the rename.

## Plan

Following the shape of `v0.1.11-rpt8` (which this tree already is), what is left is validation and packaging, in order:

1. **Find what Orbitals imports** (item 2 above, one `Select-String`), and note it in this page. If it imports
   `ShowControllerSupport`, the automatic choice works as designed. If not, list the npad calls it uses and add the
   override for the one that asks for several players (no change to `steam_rpt.c`).
2. **A first run against a real Steam**, the check nobody has made: when Orbitals reaches a scene, with `steam_appid = 1118310` (or another owned App ID), Steam running and the
   library next to the exe. Look for the log lines `steam: running as App ID ...; Remote Play Together ready`, then
   `steam: ... joined (session ...)`, then `input: gamepad ... for player 2 (... a Remote Play guest's)`. The guide lists them under "Set up". Risk: unknown: the code has only met a mock of Steam, and Orbitals has to offer a scene with several players.
3. **Package the library**: done for the Windows zip (item 5); the AppImage is left. Risk: none known.
4. **Add the tests of item 6** as a C probe built the way `tests/c/` builds its probes, plus a CI step. Estimated: a
   few hours; no gameplay risk.
5. **Check the limit of 2** against Orbitals' first multiplayer scene.

Not planned: a Steamworks application for Orbitals, and any code from Eglantine's Remote Play extension (GPL-3.0; the
idea was the only thing taken, as `runtime/steam/README.md` says).
