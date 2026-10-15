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
  corner while the window is closed.

Messages are not encrypted; chat with people you trust.
