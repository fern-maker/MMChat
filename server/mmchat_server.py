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
    PING
  Server -> client
    WELCOME <assigned name>             name may get a numeric suffix if taken
    ERROR <reason>                      server closes the connection afterwards
    CHAT <name> <text>
    INFO <text>                         joins, leaves, notices
    ROSTER <n> [<name> <scene id> <day>] * n
                                        sent to everyone on join, leave and move
    PONG
"""

import argparse
import asyncio
import socket
import time

PROTOCOL_VERSION = "2"
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


class Client:
    def __init__(self, name, writer):
        self.name = name
        self.writer = writer
        self.scene = SCENE_NONE
        self.day = 0
        self.chat_limiter = RateLimiter(RATE_LIMIT_MESSAGES, RATE_LIMIT_WINDOW_S)
        self.where_limiter = RateLimiter(WHERE_LIMIT, WHERE_WINDOW_S)

    def send(self, *fields):
        self.writer.write(("\t".join(fields) + "\n").encode("utf-8"))


class ChatServer:
    def __init__(self, password):
        self.password = password
        self.clients = {}

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
    args = parser.parse_args()

    chat = ChatServer(args.password)
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
