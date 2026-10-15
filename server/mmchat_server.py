#!/usr/bin/env python3
"""MMChat relay server.

One player runs this on their machine; everyone else (including the host's own
game) connects to it. It relays chat lines to every connected player.

Standard library only, Python 3.8+, works on macOS, Windows and Linux.

Protocol: UTF-8 text lines terminated by "\\n", fields separated by tabs.
  Client -> server
    HELLO <version> <name> <password>   first line, required
    SAY <text>
    WHERE <scene id> <day>              scene -1 means title screen / menus
    COOP <group>                        join a co-op group ("" leaves); sent before any SYNC
    SYNC <key> <value>                  report a shared progression value
    PING
  Server -> client
    WELCOME <assigned name>             name may get a numeric suffix if taken
    ERROR <reason>                      server closes the connection afterwards
    CHAT <name> <text>
    INFO <text>                         joins, leaves, notices
    ROSTER <n> [<name> <scene id> <day>] * n
                                        sent to everyone on join, leave and move
    SYNC <key> <merged value> <who>     group state: all of it after COOP, then each change
    PONG

Co-op sync: keys are 32-bit numbers whose bits 12-15 give the merge rule (1 = OR, 2 = MAX), so the
server merges values without knowing what they mean. Each group's merged state is saved to
<state dir>/<group>.json, so progress survives server restarts. Delete that file to reset a group.
"""

import argparse
import asyncio
import json
import os
import pathlib
import re
import socket
import time

PROTOCOL_VERSION = "3"
DEFAULT_PORT = 47474
MAX_LINE_BYTES = 512
MAX_NAME_CHARS = 16
MAX_TEXT_CHARS = 200
MAX_CLIENTS = 16
HELLO_TIMEOUT_S = 10
IDLE_TIMEOUT_S = 120
RATE_LIMIT_MESSAGES = 5
RATE_LIMIT_WINDOW_S = 3.0
# Location updates only happen on scene changes; anything faster is a broken or abusive client.
WHERE_LIMIT = 10
WHERE_WINDOW_S = 3.0
SCENE_NONE = -1
MAX_SCENE_ID = 255
MAX_DAY = 4
# SYNC lines arrive in bursts when a player (re)joins, but only rarely afterwards.
SYNC_LIMIT = 300
SYNC_WINDOW_S = 3.0
SYNC_OR = 1
SYNC_MAX = 2
MAX_SYNC_KEYS = 256
MAX_GROUP_CHARS = 32
DEFAULT_STATE_DIR = pathlib.Path(__file__).resolve().parent / "coop_state"


def clean(text, limit):
    """Drops control characters (tabs and newlines would break the framing) and trims."""
    text = "".join(ch for ch in text if ch.isprintable())
    return text.strip()[:limit]


def parse_int(text, low, high):
    """Returns the integer in `text` if it is within [low, high], else None."""
    try:
        value = int(text)
    except ValueError:
        return None
    return value if low <= value <= high else None


class RateLimiter:
    def __init__(self, limit, window):
        self.limit = limit
        self.window = window
        self.recent = []

    def allow(self):
        now = time.monotonic()
        self.recent = [t for t in self.recent if now - t < self.window]
        if len(self.recent) >= self.limit:
            return False
        self.recent.append(now)
        return True


def sync_merge(key, a, b):
    return a | b if (key >> 12) & 0xF == SYNC_OR else max(a, b)


class CoopGroup:
    """One co-op group's merged progression, persisted as JSON."""

    def __init__(self, name, state_dir):
        self.name = name
        self.path = state_dir / f"{name}.json"
        self.values = {}  # key -> [value, who]
        if self.path.exists():
            try:
                data = json.loads(self.path.read_text(encoding="utf-8"))
                self.values = {int(k): [int(v[0]), str(v[1])] for k, v in data["values"].items()}
            except (OSError, ValueError, KeyError, TypeError, IndexError) as error:
                raise RuntimeError(f"Could not read {self.path}: {error}") from error

    def merge(self, key, value, who):
        """Returns the new merged value if it changed, else None."""
        current = self.values.get(key, [0, ""])[0]
        merged = sync_merge(key, current, value)
        if merged == current:
            return None
        self.values[key] = [merged, who]
        self.save()
        return merged

    def save(self):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        data = {"group": self.name, "values": {str(k): v for k, v in sorted(self.values.items())}}
        # Write then rename, so a crash mid-write can't leave a half-written file.
        temp = self.path.with_suffix(".json.tmp")
        temp.write_text(json.dumps(data, indent=1), encoding="utf-8")
        os.replace(temp, self.path)


class Client:
    def __init__(self, name, writer):
        self.name = name
        self.writer = writer
        self.scene = SCENE_NONE
        self.day = 0
        self.group = None
        self.chat_limiter = RateLimiter(RATE_LIMIT_MESSAGES, RATE_LIMIT_WINDOW_S)
        self.where_limiter = RateLimiter(WHERE_LIMIT, WHERE_WINDOW_S)
        self.sync_limiter = RateLimiter(SYNC_LIMIT, SYNC_WINDOW_S)

    def send(self, *fields):
        # A dropped connection lingers until its handler notices; writing to it only spams warnings.
        if self.writer.is_closing():
            return
        self.writer.write(("\t".join(fields) + "\n").encode("utf-8"))


class ChatServer:
    def __init__(self, password, state_dir):
        self.password = password
        self.state_dir = state_dir
        self.clients = {}
        self.groups = {}

    def get_group(self, name):
        if name not in self.groups:
            self.groups[name] = CoopGroup(name, self.state_dir)
        return self.groups[name]

    def handle_coop(self, client, group_name):
        group_name = group_name.strip()
        if group_name and not re.fullmatch(rf"[A-Za-z0-9_-]{{1,{MAX_GROUP_CHARS}}}", group_name):
            client.send("INFO", "Invalid co-op group name")
            return
        try:
            client.group = self.get_group(group_name) if group_name else None
        except RuntimeError as error:
            # A corrupt state file must not be overwritten or crash the server; the host has to look at it.
            client.group = None
            client.send("INFO", "The server couldn't load this co-op group's saved progress; ask the host to check the server window")
            self.log(f"ERROR: {error}")
            return
        if client.group is None:
            return
        # Bring the joiner up to date with everything the group has.
        for key, (value, who) in sorted(client.group.values.items()):
            client.send("SYNC", str(key), str(value), who)
        members = sum(1 for c in self.clients.values() if c.group is client.group)
        self.log(f"{client.name} joined co-op group '{group_name}' "
                 f"({members} in group, {len(client.group.values)} shared values so far)")

    def handle_sync(self, client, key_text, value_text):
        if not client.sync_limiter.allow():
            return
        if client.group is None:
            client.send("INFO", "Not in a co-op group; progress wasn't shared (join one first)")
            return
        key = parse_int(key_text, 0, 0xFFFF)
        value = parse_int(value_text, 0, 0xFFFFFFFF)
        if key is None or value is None or (key >> 12) & 0xF not in (SYNC_OR, SYNC_MAX):
            return
        group = client.group
        if key not in group.values and len(group.values) >= MAX_SYNC_KEYS:
            return
        merged = group.merge(key, value, client.name)
        if merged is None:
            return
        self.log(f"co-op '{group.name}': {client.name} added progress to key {key:#06x} (now {merged:#010x})")
        for other in list(self.clients.values()):
            if other.group is group:
                other.send("SYNC", str(key), str(merged), client.name)

    def log(self, message):
        print(time.strftime("%H:%M:%S"), message, flush=True)

    def broadcast(self, *fields):
        for client in list(self.clients.values()):
            client.send(*fields)

    def broadcast_roster(self):
        fields = ["ROSTER", str(len(self.clients))]
        for client in self.clients.values():
            fields += [client.name, str(client.scene), str(client.day)]
        self.broadcast(*fields)

    def unique_name(self, wanted):
        name = wanted or "Player"
        suffix = 2
        while name.lower() in (n.lower() for n in self.clients):
            tail = str(suffix)
            name = (wanted or "Player")[: MAX_NAME_CHARS - len(tail)] + tail
            suffix += 1
        return name

    async def read_line(self, reader, timeout):
        line = await asyncio.wait_for(reader.readline(), timeout)
        if not line:
            raise ConnectionError("closed")
        if len(line) > MAX_LINE_BYTES or not line.endswith(b"\n"):
            raise ValueError("line too long")
        return line.decode("utf-8", errors="replace").rstrip("\r\n").split("\t")

    async def handle(self, reader, writer):
        peer = writer.get_extra_info("peername")
        client = None
        try:
            fields = await self.read_line(reader, HELLO_TIMEOUT_S)
            if len(fields) != 4 or fields[0] != "HELLO":
                writer.write(b"ERROR\tExpected HELLO\n")
                return
            _, version, wanted, password = fields
            if version != PROTOCOL_VERSION:
                writer.write(f"ERROR\tMMChat version mismatch with the server (protocol {clean(version, 8)} vs {PROTOCOL_VERSION}); everyone needs the same release\n".encode())
                return
            if self.password and password != self.password:
                writer.write(b"ERROR\tWrong password\n")
                self.log(f"rejected {peer}: wrong password")
                return
            if len(self.clients) >= MAX_CLIENTS:
                writer.write(b"ERROR\tServer is full\n")
                return

            client = Client(self.unique_name(clean(wanted, MAX_NAME_CHARS)), writer)
            self.clients[client.name] = client
            client.send("WELCOME", client.name)
            self.broadcast("INFO", f"{client.name} joined")
            self.broadcast_roster()
            self.log(f"{client.name} joined from {peer[0]} ({len(self.clients)} online)")

            while True:
                fields = await self.read_line(reader, IDLE_TIMEOUT_S)
                if fields[0] == "SAY" and len(fields) == 2:
                    text = clean(fields[1], MAX_TEXT_CHARS)
                    if not text:
                        continue
                    if not client.chat_limiter.allow():
                        client.send("INFO", "Slow down, you're sending messages too fast")
                        continue
                    self.broadcast("CHAT", client.name, text)
                    self.log(f"<{client.name}> {text}")
                elif fields[0] == "WHERE" and len(fields) == 3:
                    scene = parse_int(fields[1], SCENE_NONE, MAX_SCENE_ID)
                    day = parse_int(fields[2], 0, MAX_DAY)
                    if scene is None or day is None:
                        continue
                    if (scene, day) == (client.scene, client.day):
                        continue
                    client.scene, client.day = scene, day
                    # Over the limit, the change still shows up in the next roster anyone triggers.
                    if client.where_limiter.allow():
                        self.broadcast_roster()
                elif fields[0] == "COOP" and len(fields) == 2:
                    self.handle_coop(client, fields[1])
                elif fields[0] == "SYNC" and len(fields) == 3:
                    self.handle_sync(client, fields[1], fields[2])
                elif fields[0] == "PING":
                    client.send("PONG")
                else:
                    client.send("ERROR", "Unknown command")
                    return
        except (ConnectionError, asyncio.TimeoutError, ValueError, OSError):
            pass
        finally:
            if client is not None and self.clients.get(client.name) is client:
                del self.clients[client.name]
                self.broadcast("INFO", f"{client.name} left")
                self.broadcast_roster()
                self.log(f"{client.name} left ({len(self.clients)} online)")
            try:
                await writer.drain()
            except (ConnectionError, OSError):
                pass
            writer.close()


def lan_address():
    """Best guess at this machine's LAN IP, for telling friends where to connect."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))  # No packet is sent; this only picks a route.
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


async def main():
    parser = argparse.ArgumentParser(description="MMChat relay server")
    parser.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all interfaces)")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--password", default="", help="require this password to join")
    parser.add_argument("--state-dir", type=pathlib.Path, default=DEFAULT_STATE_DIR,
                        help="folder for co-op progress files (default: coop_state next to this script)")
    args = parser.parse_args()

    chat = ChatServer(args.password, args.state_dir)
    server = await asyncio.start_server(chat.handle, args.host, args.port)
    chat.log(f"MMChat server listening on port {args.port}")
    chat.log(f"Friends on your network connect to {lan_address()}:{args.port}")
    if args.password:
        chat.log("Password required to join")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
