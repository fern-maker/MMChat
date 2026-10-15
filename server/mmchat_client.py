#!/usr/bin/env python3
"""Command-line MMChat client, for testing the server without the game.

Usage: python3 mmchat_client.py <name> [--host 127.0.0.1] [--port 47474] [--password ...]
Type a line and press Enter to send it. Commands:
  /who                  show who's online and where
  /where <scene> <day>  pretend to be in a scene (decimal id, -1 = menus), e.g. /where 111 1
Ctrl+C (or Ctrl+D) quits.
"""

import argparse
import socket
import sys
import threading

PROTOCOL_VERSION = "2"
roster = []


def print_roster():
    print(f"* {len(roster)} online:", flush=True)
    for name, scene, day in roster:
        print(f"*   {name}  scene {scene}  day {day}", flush=True)


def receive(sock):
    global roster
    buffer = b""
    while True:
        data = sock.recv(4096)
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47474)
    parser.add_argument("--password", default="")
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.settimeout(None)
    sock.sendall(f"HELLO\t{PROTOCOL_VERSION}\t{args.name}\t{args.password}\n".encode("utf-8"))
    threading.Thread(target=receive, args=(sock,), daemon=True).start()

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
            elif text:
                sock.sendall(f"SAY\t{text}\n".encode("utf-8"))
    except KeyboardInterrupt:
        pass
    sock.close()


if __name__ == "__main__":
    main()
