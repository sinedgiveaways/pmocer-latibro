# Steam Remote Play Together

Play local multiplayer (Orbitals is a 2-player game) with a friend who is not in the room. Steam streams the game's
window and sound to them, and their controller or keyboard becomes player 2. They need only Steam, or
the Steam Link app, or a browser with a guest link: not the game.

It is off by default and changes nothing until you set it up.

## What you need

- Steam installed, running and signed in on the PC that runs the game (the host). On Linux, Steam from Valve or your
  distribution: the Flatpak and Snap versions run in a sandbox the game cannot reach.
- A **Steam App ID** that your account owns (free games count once they are in your library) and that supports Remote
  Play Together. Steam treats orbitals-together as that game while it runs, so your friends see its name. Use an App ID you own or are
  allowed to use. RetroArch (`1118310`) is free and supports Remote Play Together.
- Steam's runtime library next to `orbitals-together` / `orbitals-together.exe`: `libsteam_api.so` on Linux, `steam_api64.dll` on Windows,
  Steamworks SDK 1.62 or newer.

## Set up

1. **The library.** After building, run:

   ```sh
   uv run tools/build/steamlib.py                    # Linux: build/dist/linux/libsteam_api.so
   uv run tools/build/steamlib.py --target windows   # Windows: build/dist/windows/steam_api64.dll
   ```

   It downloads Valve's SDK 1.65 copy (from the Steamworks.NET release, checked by sha256). Pass `--dest DIR` for
   another folder: the one that holds `orbitals-together`. The Linux AppImage runs its copy of `orbitals-together` in `~/.local/share/orbitals-together`,
   so that is the folder there.

2. **The App ID.** In `orbitals.ini` next to `orbitals-together`:

   ```ini
   [steam]
   steam_appid = 1118310
   ```

   or set the environment variable `ORBITALS_STEAM_APPID=1118310`. The menu has the same choice (Game > Steam
   Remote Play Together > Steam App ID; it takes effect at the next start).

   **Your own App ID in the menu.** Under that row, select **Enter a Steam App ID**, type the number with the
   keyboard's number keys (Backspace deletes a digit) and press Enter or A. orbitals-together runs as that game from the next start,
   and the Steam App ID row keeps the number in its list, so you can switch back to it later. The list is
   `steam_appids` in `orbitals.ini` (comma-separated, 16 at most: the oldest goes first); delete a number there to take it
   off the list.

   **Another Steam game.** Any game you own whose store page lists Remote Play Together can stand in for
   RetroArch. Put orbitals-together in that game's launch options (step 3) and set `steam_appid = auto` (in the menu:
   Automatic): orbitals-together then runs as whichever game Steam started it from. orbitals-together remembers the last one in
   `cache/steam-appid`, so a later start from its own folder (`orbitals-together.exe`) runs as that game too, as a fixed number
   would; your friends' controllers still need the start from Steam (step 3). A number works too (the App ID is the number
   in the game's store address, `store.steampowered.com/app/<number>/`), but it must be the game whose launch
   options you used: otherwise the log says `steam: Steam started orbitals-together as App ID ..., but steam_appid = ...`.
   Steam shows your friends the name of that game, and only games with Remote Play Together let them join.

   **The game's controller settings in Steam matter too.** Steam hands controllers to orbitals-together as it would to that game,
   so a game that uses Steam Input in its own way can leave a player without one. Tried on Windows (November 2026):

   | App ID | Your controller | Your friend's controller |
   |---|---|---|
   | 1118310 (RetroArch) | works | works |
   | 985890 (Streets of Rage 4) | works | works |
   | 1361510 (TMNT: Shredder's Revenge), Steam Input on | none: Steam lists no controller at all | none |
   | 1361510, Steam Input off (Properties > Controller) | works | listed, but no button reaches orbitals-together |

   To check another one, look in `logs/orbitals.log` after a friend has joined for `input: ... Steam virtual ...` and
   `input: gamepad ... for player 2 (... a Remote Play guest's)`, then make sure player 2 moves in the game. The log
   lists every controller SDL sees (`input: SDL lists N joysticks`) each time the list changes.

3. **Set the App ID's game to start orbitals-together** (needed for your friends' controllers). With the launch options
   below in place, the game can then be started either from its folder or with Play in Steam: the owner played with a
   friend on 27/11/2026 starting `orbitals-together.exe` from its folder, the friend's controller working. Without them
   invitations work but the guests' controllers did not reach the game (the earlier reading was that Steam only hands
   them to a game it starts, through its overlay). A Non-Steam Game shortcut does
   not work either: Steam then runs orbitals-together as the shortcut, which has no Remote Play Together. Instead, let Steam start orbitals-together
   in place of the App ID's own program:
   - install the App ID's game in Steam (RetroArch, free);
   - right-click it > Properties > General > Launch options, and enter the path of `orbitals-together.exe` in quotes followed by
     `%command%`, for example `"C:\Users\you\Downloads\orbitals-together\orbitals-together.exe" %command%`;
   - press Play on it. Steam starts `orbitals-together.exe` with the game's program as its last argument, which orbitals-together leaves out (the
     log says `boot: started by Steam's %command%`). Empty the launch options to get the real game back.

The log says `steam: running as App ID ...; Remote Play Together ready`. When something is missing, a notice at the
top of the picture says why, and the log has the line `steam: Remote Play Together is off: ...`.

## Play

Menu (F1) > Game > **Steam Remote Play Together**:

- **Invite friends**: the Steam overlay's Remote Play Together list. Beside it: how many guests are connected.
- **Copy guest link**: a link to send to anyone. They open it in a browser or the Steam Link app, no Steam friendship
  or account needed. The link is also written to the log.

**Picking local multiplayer** (2, 3 or 4 players) with fewer controllers than players asks how the other players
join:

- **Local players (connect more controllers)**: a notice says how many controllers are missing. Connect them, then
  pick the mode again.
- **Steam Remote Play Together (invite friends)**: Steam's invite list opens. Once your friends have joined and pressed
  a button on their controller, pick the mode again.

B or Esc closes the question without choosing. Either way the game goes back to its menu first, as it does without
Steam: the game does not wait inside its controller screen, because the picture (and with it the overlay and your
friends' stream) would freeze meanwhile. With a controller for every player, the game starts at once, with no
question. `steam_invite` in `orbitals.ini` changes this: `ask` (the default) as above; `auto` opens the invite list without
asking while no friend has joined yet or controllers are missing; `always` opens it every time; `off` never asks nor
opens it.

**Picking the game's Online Play** works the same way: the game's online mode needs Nintendo's servers, so the invite
list opens with a notice instead (once per 10 s). Once your friend has joined and pressed a button, go back and start a
local game for 2 players. The game itself still sees no network and shows its own "no connection" message, if any.
Without Steam set up, the notice says why Remote Play Together is off. `steam_invite = off` turns this off too.

When a guest joins, a notice says so. Their controller becomes the next free player (a notice says which). A guest
without a controller gets a player at the first key they press, and plays with the same keys as you (WASD, Space,
Shift...; Settings > Controls). While every guest has a controller, guests' keys give no player, so a guest with both
gets only one. Then choose the number of players in the game as usual: the game needs one controller
per player.

## Good to know

- Steam does not say whose a virtual controller is. Controllers appear in the order guests connect them. A keyboard
  becomes a player only while there are more guests than guest controllers and keyboard players, and not in a guest's first 5 seconds
  (their controller can take that long to appear). With one guest on a controller and another on a keyboard only,
  the first key pressed by either guest after 5 seconds takes the keyboard player. A controller you plug in yourself while a guest is
  connected counts as a guest's.
- Your own controller through Steam Input (Steam shows it as a "Steam Virtual Gamepad") stays player 1 and never
  becomes a second player.
- Guests cannot open the menu: Esc, F1, F10 and F11 stay yours. Their mouse does nothing.
- On Linux under Wayland, if guests see "Steam not launched with -pipewire", start Steam with `steam -pipewire` (or
  `-pipewire-dmabuf` when guests get a black picture).
- The App ID is read at start: changing it needs a restart of the game.
- A device loss with "instruction pointer invalid" in the log on NVIDIA cards is the port's known issue #22, with or
  without Steam; `skip_programs` in `orbitals.ini` helps find its cause (CHANGELOG, 0.1.10).

## Online play in a browser

Since 27/11/2026 the game's **Online Play** no longer opens Steam's invite list: it streams the game to a friend's
browser through Cloudflare (`runtime/stream/`). Steam Remote Play Together, above, stays for **Local Play**.

1. In the game's menu, choose **Online Play** (or F1 > Game > Online play in a browser > **Start and copy the link**).
   The first time, `cloudflared.exe` (55 MB, Cloudflare's own, checked by sha256) is downloaded next to
   `orbitals-together.exe`.
2. A few seconds later a notice says the link is copied: an address `https://<words>.trycloudflare.com`. Send it to
   your friend (chat, Discord...). Nothing else to set up: no Steam, no port to open.
3. Your friend opens it in Chrome, Edge or Firefox and presses **Sound: on**. Their gamepad, their keyboard (WASD,
   J/K/U/I, Q/E, Enter, Backspace) or, on a phone, the touch controls are player 2.
4. Start a **Local Play** game for 2 players.

Limits: one friend (the game has 2 players; with a second controller plugged in on your side there is no free
player). Picture 1280 x 720 at up to 30 frames a second, about 4.5 Mb/s of upload. Anyone with the link can join
while the stream runs: F1 > Game > **Stop** ends it, and quitting the game too. Windows only for now.

## Tested

Against Valve's SDK 1.65 library and a mock of Steam built from Valve's headers, with SDL virtual gamepads standing in
for the guests' controllers (13/11/2026): start-up, every symbol bound (Linux and Windows libraries), guests joining and
leaving, a guest's controller as player 2 with the host's Steam Input twin left out, a guest keyboard as another player (the tests then allowed 8 players; Orbitals allows 2), the
guest link, a missing overlay, and each failure (no library, no Steam, wrong App ID). Picking local multiplayer with too few controllers opens the invite list (13/11/2026, against the mock).
A real session with friends over Steam has not been run yet.
