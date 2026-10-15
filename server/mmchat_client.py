#!/usr/bin/env python3
"""Command-line MMChat client, for testing the server without the game.

Usage: python3 mmchat_client.py <name> [--host 127.0.0.1] [--port 47474] [--password ...]
Type a line and press Enter to send it. Ctrl+C (or Ctrl+D) quits.
"""

import argparse
import socket
import sys
import threading


def receive(sock):
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


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("name")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=47474)
    parser.add_argument("--password", default="")
    args = parser.parse_args()

    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.settimeout(None)
    sock.sendall(f"HELLO\t1\t{args.name}\t{args.password}\n".encode("utf-8"))
    threading.Thread(target=receive, args=(sock,), daemon=True).start()

    try:
        for line in sys.stdin:
            text = line.rstrip("\r\n").replace("\t", " ")
            if text:
                sock.sendall(f"SAY\t{text}\n".encode("utf-8"))
    except KeyboardInterrupt:
        pass
    sock.close()


if __name__ == "__main__":
    main()
