#!/usr/bin/env python3
"""MMChat relay server.

One player runs this on their machine; everyone else (including the host's own
game) connects to it. It relays chat lines to every connected player.

Standard library only, Python 3.8+, works on macOS, Windows and Linux.

Protocol: UTF-8 text lines terminated by "\\n", fields separated by tabs.
  Client -> server
    HELLO <version> <name> <password>   first line, required
    SAY <text>
    PING
  Server -> client
    WELCOME <assigned name>             name may get a numeric suffix if taken
    ERROR <reason>                      server closes the connection afterwards
    CHAT <name> <text>
    INFO <text>                         joins, leaves, notices
    PONG
"""

import argparse
import asyncio
import socket
import time

PROTOCOL_VERSION = "1"
DEFAULT_PORT = 47474
MAX_LINE_BYTES = 512
MAX_NAME_CHARS = 16
MAX_TEXT_CHARS = 200
MAX_CLIENTS = 16
HELLO_TIMEOUT_S = 10
IDLE_TIMEOUT_S = 120
RATE_LIMIT_MESSAGES = 5
RATE_LIMIT_WINDOW_S = 3.0


def clean(text, limit):
    """Drops control characters (tabs and newlines would break the framing) and trims."""
    text = "".join(ch for ch in text if ch.isprintable())
    return text.strip()[:limit]


class Client:
    def __init__(self, name, writer):
        self.name = name
        self.writer = writer
        self.recent = []

    def send(self, *fields):
        self.writer.write(("\t".join(fields) + "\n").encode("utf-8"))

    def allow_message(self):
        now = time.monotonic()
        self.recent = [t for t in self.recent if now - t < RATE_LIMIT_WINDOW_S]
        if len(self.recent) >= RATE_LIMIT_MESSAGES:
            return False
        self.recent.append(now)
        return True


class ChatServer:
    def __init__(self, password):
        self.password = password
        self.clients = {}

    def log(self, message):
        print(time.strftime("%H:%M:%S"), message, flush=True)

    def broadcast(self, *fields):
        for client in list(self.clients.values()):
            client.send(*fields)

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
                writer.write(f"ERROR\tServer speaks protocol {PROTOCOL_VERSION}, you sent {clean(version, 8)}\n".encode())
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
            self.log(f"{client.name} joined from {peer[0]} ({len(self.clients)} online)")

            while True:
                fields = await self.read_line(reader, IDLE_TIMEOUT_S)
                if fields[0] == "SAY" and len(fields) == 2:
                    text = clean(fields[1], MAX_TEXT_CHARS)
                    if not text:
                        continue
                    if not client.allow_message():
                        client.send("INFO", "Slow down, you're sending messages too fast")
                        continue
                    self.broadcast("CHAT", client.name, text)
                    self.log(f"<{client.name}> {text}")
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
