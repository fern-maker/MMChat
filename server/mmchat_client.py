#!/usr/bin/env python3
"""Command-line MMChat client, for testing the server without the game.

Usage: python3 mmchat_client.py <name> [--host 127.0.0.1] [--port 47474] [--password ...] [--coop <group>]
Type a line and press Enter to send it. Commands:
  /who                  show who's online and where
  /where <scene> <day>  pretend to be in a scene (decimal id, -1 = menus), e.g. /where 111 1
  /coop <group>         join a co-op group (the co-op save name, e.g. coop)
  /sync <key> <value>   report shared progression; numbers may be hex, e.g. /sync 0x1001 0x100
  /state                show the co-op values received so far
Ctrl+C (or Ctrl+D) quits.

Mask keys for /sync: key 0x1001, one bit per mask slot in this order (bit 0 first):
  Postman, All-Night, Blast, Stone, Great Fairy, Deku, Keaton, Bremen, Bunny, Don Gero, Scents, Goron,
  Romani, Circus Leader, Kafei, Couple's, Truth, Zora, Kamaro, Gibdo, Garo, Captain's, Giant's
  e.g. Bunny Hood alone is /sync 0x1001 0x100
Other keys: 0x1002 quest bits (bit 6 Sonata, 7 Lullaby, 8 Bossa Nova, 9 Elegy, 10 Oath, 13 Healing,
  14 Epona, 15 Soaring, 16 Storms, 18 Notebook; 0-3 boss remains), 0x1003 owl statues, 0x1004 dungeon
  maps/compasses, 0x1007 abilities (1 magic, 2 double magic, 4 double defense), 0x2101 quiver level (1-3),
  0x2102 bomb bag level (1-3), 0x2103 wallet level (1-2), 0x2104 heart capacity (16 per heart, e.g. 0x50 = 5)
  0x1008 items, one bit per inventory slot (bit 1 Bow, 2 Fire Arrow, 3 Ice Arrow, 4 Light Arrow, 6 Bomb Bag,
  7 Bombchus, 8 Deku Sticks, 9 Deku Nuts, 12 Powder Keg, 13 Pictograph Box, 14 Lens of Truth, 15 Hookshot,
  16 Great Fairy's Sword; e.g. the Hookshot is /sync 0x1008 0x8000), 0x1009 equipment (1 Gilded Sword,
  2 Mirror Shield)
"""

import argparse
import socket
import sys
import threading

PROTOCOL_VERSION = "3"
MASK_KEY = 0x1001
MASK_NAMES = ["Postman's Hat", "All-Night", "Blast", "Stone", "Great Fairy's", "Deku", "Keaton", "Bremen",
              "Bunny Hood", "Don Gero's", "Scents", "Goron", "Romani's", "Circus Leader's", "Kafei's",
              "Couple's", "Truth", "Zora", "Kamaro's", "Gibdo", "Garo's", "Captain's Hat", "Giant's"]
roster = []
sync_state = {}
group = None


QUEST_NAMES = {0: "Odolwa's Remains", 1: "Goht's Remains", 2: "Gyorg's Remains", 3: "Twinmold's Remains",
               6: "Sonata", 7: "Goron Lullaby", 8: "Bossa Nova", 9: "Elegy", 10: "Oath to Order",
               11: "Saria's Song", 13: "Song of Healing", 14: "Epona's Song", 15: "Song of Soaring",
               16: "Song of Storms", 17: "Sun's Song", 18: "Bombers' Notebook", 24: "Lullaby Intro"}
ABILITY_NAMES = {0: "magic", 1: "double magic", 2: "double defense"}
ITEM_NAMES = {1: "Bow", 2: "Fire Arrow", 3: "Ice Arrow", 4: "Light Arrow", 6: "Bomb Bag", 7: "Bombchus",
              8: "Deku Sticks", 9: "Deku Nuts", 12: "Powder Keg", 13: "Pictograph Box", 14: "Lens of Truth",
              15: "Hookshot", 16: "Great Fairy's Sword"}
EQUIPMENT_NAMES = {0: "Gilded Sword", 1: "Mirror Shield"}
# Keys from src/coop_sync.c. Bits 12-15 are the merge rule (1 = OR, 2 = MAX).
KEY_LABELS = {0x1001: "masks", 0x1002: "quest", 0x1003: "owl statues (bits)", 0x1004: "dungeon maps/compasses (bits)",
              0x1005: "regions visited (bits)", 0x1006: "map clouds (bits)", 0x1007: "abilities",
              0x2101: "quiver level", 0x2102: "bomb bag level", 0x2103: "wallet level",
              0x2104: "heart capacity (16 per heart)"}
for i in range(7):
    KEY_LABELS[0x1010 + i] = f"Tingle map scenes {i} (bits)"


def describe(key, value):
    if key == MASK_KEY:
        masks = [name for bit, name in enumerate(MASK_NAMES) if value & (1 << bit)]
        return f"masks: {', '.join(masks) or 'none'}"
    if key == 0x1002:
        return f"quest: {', '.join(n for b, n in QUEST_NAMES.items() if value & (1 << b)) or 'none'}"
    if key == 0x1007:
        return f"abilities: {', '.join(n for b, n in ABILITY_NAMES.items() if value & (1 << b)) or 'none'}"
    if key == 0x1008:
        return f"items: {', '.join(n for b, n in ITEM_NAMES.items() if value & (1 << b)) or 'none'}"
    if key == 0x1009:
        return f"equipment: {', '.join(n for b, n in EQUIPMENT_NAMES.items() if value & (1 << b)) or 'none'}"
    if key in KEY_LABELS:
        return f"{KEY_LABELS[key]} = {value:#x}"
    return f"key {key:#06x} = {value:#010x}"


def print_state():
    if not sync_state:
        print(f"* co-op group '{group}' has no shared progress yet", flush=True)
    for key, value in sorted(sync_state.items()):
        print(f"*   {describe(key, value)}", flush=True)


def join_group(sock, name):
    """Joins a co-op group, then shows what the group already has once the server's dump arrives."""
    global group
    group = name
    sync_state.clear()
    sock.sendall(f"COOP\t{name}\n".encode("utf-8"))
    print(f"* joined co-op group '{name}'; its current state:", flush=True)
    threading.Timer(0.5, print_state).start()


def print_roster():
    print(f"* {len(roster)} online:", flush=True)
    for name, scene, day in roster:
        print(f"*   {name}  scene {scene}  day {day}", flush=True)


def receive(sock):
    global roster
    buffer = b""
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            return  # The socket was closed because we're quitting.
        if not data:
            print("* disconnected", flush=True)
            return
        buffer += data
        while b"\n" in buffer:
            line, buffer = buffer.split(b"\n", 1)
            fields = line.decode("utf-8", errors="replace").split("\t")
            if fields[0] == "CHAT" and len(fields) == 3:
                print(f"{fields[1]}: {fields[2]}", flush=True)
            elif fields[0] in ("INFO", "WELCOME", "ERROR") and len(fields) == 2:
                print(f"* {fields[0].lower()}: {fields[1]}", flush=True)
            elif fields[0] == "ROSTER" and len(fields) >= 2:
                rest = fields[2:]
                roster = [tuple(rest[i:i + 3]) for i in range(0, len(rest) - 2, 3)]
            elif fields[0] == "SYNC" and len(fields) == 4:
                key, value = int(fields[1]), int(fields[2])
                sync_state[key] = value
                print(f"* sync: {describe(key, value)} (last change by {fields[3] or '?'})", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47474)
    parser.add_argument("--password", default="")
    parser.add_argument("--coop", metavar="GROUP", help="join this co-op group right away (the co-op save name)")
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.settimeout(None)
    sock.sendall(f"HELLO\t{PROTOCOL_VERSION}\t{args.name}\t{args.password}\n".encode("utf-8"))
    threading.Thread(target=receive, args=(sock,), daemon=True).start()
    if args.coop:
        join_group(sock, args.coop)

    try:
        for line in sys.stdin:
            text = line.rstrip("\r\n").replace("\t", " ")
            if text == "/who":
                print_roster()
            elif text.startswith("/where "):
                parts = text.split()
                if len(parts) == 3:
                    sock.sendall(f"WHERE\t{parts[1]}\t{parts[2]}\n".encode("utf-8"))
                else:
                    print("* usage: /where <scene> <day>", flush=True)
            elif text.startswith("/coop "):
                join_group(sock, text[6:].strip())
            elif text.startswith("/sync "):
                parts = text.split()
                try:
                    key, value = int(parts[1], 0), int(parts[2], 0)
                    sock.sendall(f"SYNC\t{key}\t{value}\n".encode("utf-8"))
                except (IndexError, ValueError):
                    print("* usage: /sync <key> <value>", flush=True)
            elif text == "/state":
                if group is None:
                    print("* not in a co-op group; use /coop <group>", flush=True)
                else:
                    print_state()
            elif text:
                sock.sendall(f"SAY\t{text}\n".encode("utf-8"))
    except KeyboardInterrupt:
        pass
    # Shut down first so the reader thread wakes up and exits before the interpreter does.
    try:
        sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass
    sock.close()


if __name__ == "__main__":
    main()
