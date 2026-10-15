MMChat - text chat for Majora's Mask: Recompiled (v1.2.2 or newer)
=====================================================================

INSTALL (everyone)
  Copy everything in the "mods" folder into your game's mods folder:
    Windows: usually %LOCALAPPDATA%\Zelda64Recompiled\mods
    macOS:   ~/Library/Application Support/Zelda64Recompiled/mods
  (Extra files for the other OS are harmless.)
  Then enable "MMChat" in the game's Mods menu.

HOST THE SERVER (one person)
  Needs Python 3 (python.org; on Windows tick "Add Python to PATH").
    python3 server/mmchat_server.py --password yourpassword
  On Windows use "py" or "python" instead of "python3".
  It prints the address friends should use. Keep the window open while playing.

  Friends not on your home network: easiest is for everyone to install
  Tailscale (free) and use the host's Tailscale IP. Otherwise the host must
  forward TCP port 47474 on their router. Allow Python through the firewall
  if asked.

CHAT
  In the Mods menu, set Server (e.g. 100.101.102.103:47474), Your Name,
  and Server Password. Then in-game hold L and press R to open the chat
  window, press Connect, type, and press Send. New messages pop up in the
  corner while the window is closed. The right side of the window lists
  who's online and which area and day they're in.

CO-OP (shared progression)
  In the Mods menu, turn on "Co-op Save" and everyone uses the same
  "Co-op Save Name". File select then shows a separate co-op save (your
  normal saves are untouched); start a new file there. Masks, songs, boss
  remains, owl statues, maps, upgrades, heart containers and Great Fairy
  rewards one player gets are given to everyone in the group. Shared progress
  starts arriving
  once you've learned the Song of Time, so the opening plays normally.
  Not compatible with the "Ultimate 6 Save Slots" mod (disable it for co-op).
  The host's server keeps the group's progress in server/coop_state/.

  Everyone (and the server) needs the same MMChat release; mismatched
  versions get a "version mismatch" error when connecting.

Messages are not encrypted; chat with people you trust.
