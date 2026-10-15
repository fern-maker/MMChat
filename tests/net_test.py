"""End-to-end test: the real server, the native library (driven by net_harness as "Alice") and raw
socket clients playing everyone else. Covers chat, presence, co-op sync (OR / MAX / ADD), persistence
and connection failures.

Usage: python3 tests/net_test.py <server script> <net_harness binary> <native library>
(`make test` builds everything and runs this.) Uses localhost port 47999 and a temporary state folder.
"""
import json
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

server_py, harness, dylib = sys.argv[1:4]
PORT = 47999
STATE_DIR = pathlib.Path(tempfile.mkdtemp(prefix="mmchat_test_state_"))
results = {}


def start_server():
    p = subprocess.Popen([sys.executable, server_py, "--host", "127.0.0.1", "--port", str(PORT), "--password", "pw", "--state-dir", str(STATE_DIR)],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    for _ in range(50):
        try:
            socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
            return p
        except OSError:
            time.sleep(0.1)
    p.kill()
    print(p.stdout.read())
    raise SystemExit("server did not start")


def lines(sock):
    buf = b""
    while True:
        try:
            data = sock.recv(4096)
        except OSError:
            return  # The test closed this socket on purpose.
        if not data:
            return
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            yield line.decode().split("\t")


def bob(sock):
    """Replies to Alice's message, and records what Bob saw."""
    seen = []
    for fields in lines(sock):
        seen.append(fields)
        if fields[:2] == ["CHAT", "Alice"]:
            sock.sendall(b"SAY\thi Alice\n")
    results["bob_seen"] = seen


def run(mode, port=PORT):
    r = subprocess.run([harness, dylib, mode, str(port)], capture_output=True, text=True, timeout=30)
    print(r.stdout, end="")
    return r.returncode == 0


ok = True
server = start_server()
try:
    b = socket.create_connection(("127.0.0.1", PORT))
    b.sendall(b"HELLO\t4\tBob\tpw\n")
    threading.Thread(target=bob, args=(b,), daemon=True).start()
    time.sleep(0.3)
    ok &= run("ok")
    ok &= run("badpw")

    # Name collision: a second "Bob" must get a different name.
    b2 = socket.create_connection(("127.0.0.1", PORT))
    b2.sendall(b"HELLO\t4\tBob\tpw\n")
    welcome = b2.recv(4096).decode().split("\n")[0]
    print("  second Bob got:", welcome.replace("\t", " "))
    ok &= welcome == "WELCOME\tBob2"
    b2.close()

    # Rate limit: the 6th message inside 3 seconds is refused.
    b3 = socket.create_connection(("127.0.0.1", PORT))
    b3.sendall(b"HELLO\t4\tSpammer\tpw\n" + b"".join(b"SAY\tspam %d\n" % i for i in range(6)))
    time.sleep(0.5)
    got = b3.recv(65536).decode()
    limited = "Slow down" in got
    print("  rate limit triggered:", limited)
    ok &= limited
    b3.close()

    # Presence: Alice reports South Clock Town day 2; Bob moves to Termina Field day 1.
    h = subprocess.Popen([harness, dylib, "presence", str(PORT)], stdout=subprocess.PIPE, text=True)
    print(h.stdout.readline(), end="")
    b.sendall(b"WHERE\t45\t1\n")
    out = h.communicate(timeout=15)[0]
    print(out, end="")
    print("presence", "PASS" if h.returncode == 0 else "FAIL")
    ok &= h.returncode == 0

    # Bad WHERE values are ignored, not fatal.
    b.sendall(b"WHERE\tabc\t1\nWHERE\t999\t1\nWHERE\t5\t9\n")
    b.close()

    # Old clients get a clear version error.
    old = socket.create_connection(("127.0.0.1", PORT))
    old.sendall(b"HELLO\t1\tOldie\tpw\n")
    reply = old.recv(4096).decode()
    print("  old client got:", reply.strip().replace("\t", " "))
    ok &= reply.startswith("ERROR\tMMChat version mismatch")
    old.close()

    # Full server with long multi-byte names: the roster line is far longer than a chat line.
    time.sleep(0.3)
    bots = []
    for i in range(15):
        s = socket.create_connection(("127.0.0.1", PORT))
        s.sendall("HELLO\t4\tÄÖÜßÄÖÜßÄÖÜßÄÖÜß\tpw\n".encode() + b"WHERE\t111\t3\n")
        bots.append(s)
    ok &= run("bigroster")
    for s in bots:
        s.close()
    time.sleep(0.3)

    def read_syncs(sock, seconds):
        """Collects SYNC lines (key, value, who) arriving within `seconds`."""
        sock.settimeout(0.2)
        got, buf, end = [], b"", time.time() + seconds
        while time.time() < end:
            try:
                data = sock.recv(65536)
            except socket.timeout:
                continue
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                f = line.decode().split("\t")
                if f[0] == "SYNC":
                    got.append((int(f[1]), int(f[2]), f[3]) + tuple(int(x) for x in f[4:]))
                elif f[0] == "SYNCDONE":
                    got.append(("SYNCDONE", f[1]))
        return got

    def harness_until_ready(mode):
        h = subprocess.Popen([harness, dylib, mode, str(PORT)], stdout=subprocess.PIPE, text=True)
        for line in h.stdout:
            print(line, end="")
            if line.startswith("READY"):
                break
        return h

    # Co-op: Alice (native) reported progress offline, then connects; Bob joins the group afterwards.
    h = subprocess.Popen([harness, dylib, "coop", str(PORT)], stdout=subprocess.PIPE, text=True)
    print(h.stdout.readline(), end="")
    time.sleep(0.3)
    bob2 = socket.create_connection(("127.0.0.1", PORT))
    bob2.sendall(b"HELLO\t4\tBob\tpw\nCOOP\ttestgrp\n")
    dump = read_syncs(bob2, 0.5)
    print("  Bob's join dump:", dump)
    ok &= (4097, 0x100, "Alice") in dump and (8197, 3, "Alice") in dump
    bob2.sendall(b"SYNC\t4097\t32\nSYNC\t8197\t2\n")   # Deku Mask; a lower MAX value
    # Carol is in another group and must hear nothing about testgrp.
    carol = socket.create_connection(("127.0.0.1", PORT))
    carol.sendall(b"HELLO\t4\tCarol\tpw\nCOOP\tothergrp\n")
    out = h.communicate(timeout=15)[0]
    print(out, end="")
    print("coop", "PASS" if h.returncode == 0 else "FAIL")
    ok &= h.returncode == 0
    carol_got = read_syncs(carol, 0.5)
    print("  Carol (other group) received:", carol_got)
    ok &= carol_got == [("SYNCDONE", "othergrp")]  # her own (empty) group state only
    # Garbage is ignored: bad merge rule, out-of-range value, SYNC without a group.
    bob2.sendall(b"SYNC\t16385\t1\nSYNC\t4097\t99999999999\nSYNC\tabc\t1\n")
    carol.close()
    bob2.close()
    time.sleep(0.3)

    # ADD: each player's contribution is kept; the group total is their sum.
    bob3 = socket.create_connection(("127.0.0.1", PORT))
    bob3.sendall(b"HELLO\t4\tBob\tpw\nCOOP\taddgrp\nSYNC\t12289\t3\n")
    got = read_syncs(bob3, 0.5)
    print("  Bob joins addgrp and contributes 3:", got)
    ok &= got == [("SYNCDONE", "addgrp"), (12289, 3, "Bob", 3)]
    h = harness_until_ready("add")
    bob3.sendall(b"SYNC\t12289\t4\nSYNC\t12289\t4\nSYNC\t12289\t2\nSYNC\t12289\tabc\n")
    out = h.communicate(timeout=20)[0]
    print(out, end="")
    print("add", "PASS" if h.returncode == 0 else "FAIL")
    ok &= h.returncode == 0
    got = read_syncs(bob3, 0.5)
    print("  Bob saw:", got)
    # Alice's offline 2 (total 5), Bob's +1 (6), Alice's +1 (7); each line carries Bob's own share.
    ok &= got == [(12289, 5, "Alice", 3), (12289, 6, "Bob", 4), (12289, 7, "Alice", 4)]
    bob3.close()
    time.sleep(0.3)
finally:
    server.terminate()
    server.wait()
    log = server.stdout.read()

time.sleep(0.2)
ok &= run("refused")  # nothing listening now

# Server dies mid-session.
server = start_server()
h = subprocess.Popen([harness, dylib, "drop", str(PORT)], stdout=subprocess.PIPE, text=True)
first = h.stdout.readline()
print(first, end="")
server.kill()
server.wait()
out = h.communicate(timeout=15)[0]
print(out, end="")
ok &= h.returncode == 0

# Persistence: a restarted server still knows the group's progress.
saved = json.loads((STATE_DIR / "testgrp.json").read_text())
print("  saved file:", saved)
ok &= saved["values"].get("4097", [0])[0] == 0x120 and saved["values"].get("8197", [0])[0] == 3
server = start_server()
try:
    late = socket.create_connection(("127.0.0.1", PORT))
    late.sendall(b"HELLO\t4\tDana\tpw\nCOOP\ttestgrp\n")
    dump = read_syncs(late, 0.5)
    print("  after restart, Dana's dump:", dump)
    ok &= (4097, 0x120, "Bob") in dump
    late.close()

    # ADD contributions survive a restart, and a fresh client gets its own share back.
    saved = json.loads((STATE_DIR / "addgrp.json").read_text())
    print("  saved addgrp:", saved)
    ok &= saved["contrib"] == {"12289": {"alice": 3, "bob": 4}} and saved["values"]["12289"][0] == 7
    ok &= run("addresume")
    dana = socket.create_connection(("127.0.0.1", PORT))
    dana.sendall(b"HELLO\t4\tDana\tpw\nCOOP\taddgrp\n")
    got = read_syncs(dana, 0.5)
    print("  Dana's addgrp dump:", got)
    ok &= got == [(12289, 7, "Alice", 0), ("SYNCDONE", "addgrp")]
    dana.close()

    # A group file from v0.6.0 (no contributions) still loads, and gains them.
    (STATE_DIR / "legacy.json").write_text(json.dumps({"group": "legacy", "values": {"4097": [256, "Old"]}}))
    eve = socket.create_connection(("127.0.0.1", PORT))
    eve.sendall(b"HELLO\t4\tEve\tpw\nCOOP\tlegacy\nSYNC\t12289\t1\n")
    got = read_syncs(eve, 0.5)
    print("  legacy group:", got)
    saved = json.loads((STATE_DIR / "legacy.json").read_text())
    ok &= got == [(4097, 256, "Old"), ("SYNCDONE", "legacy"), (12289, 1, "Eve", 1)]
    ok &= saved["values"]["4097"] == [256, "Old"] and saved["contrib"] == {"12289": {"eve": 1}}
    eve.close()

    # A corrupt group file must not crash the server or be overwritten.
    (STATE_DIR / "broken.json").write_text("{not json")
    bad = socket.create_connection(("127.0.0.1", PORT))
    bad.sendall(b"HELLO\t4\tEve\tpw\nCOOP\tbroken\nSYNC\t4097\t1\nPING\n")
    bad.settimeout(1)
    reply = b""
    try:
        while b"PONG" not in reply:
            reply += bad.recv(4096)
    except socket.timeout:
        pass
    warned = b"couldn't load this co-op group" in reply and b"PONG" in reply
    untouched = (STATE_DIR / "broken.json").read_text() == "{not json"
    print("  corrupt file: warned and still serving:", warned, "| file left alone:", untouched)
    ok &= warned and untouched
    bad.close()
finally:
    server.terminate()
    server.wait()
    restart_log = server.stdout.read()
shutil.rmtree(STATE_DIR, ignore_errors=True)
if not ok:
    print("--- server log (first run) ---")
    print(log)
    print("--- server log (after restart) ---")
    print(restart_log)
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
