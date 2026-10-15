// Plays "Alice": drives the native library through its recomp exports (the same calls the mod makes),
// with a fake RDRAM. Run by tests/net_test.py, which plays the server's other players.
// Usage: net_harness <native library> <mode> <port>
//   modes: ok, badpw, refused, presence, bigroster, coop, add, addresume, drop
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef uint64_t gpr;
typedef union { double d; uint64_t u64; } fpr;
typedef struct { gpr r[32]; fpr f[32]; uint64_t hi, lo; uint32_t* f_odd; uint32_t status_reg; uint8_t m; } recomp_context;
typedef void (fn_t)(uint8_t*, recomp_context*);

#define K 0xFFFFFFFF80000000ULL
static uint8_t* rdram;
static void* lib;
static uint8_t* at(gpr v) { return rdram + ((v ^ 3) - K); }
static void put(gpr v, const char* s) { do { *at(v++) = (uint8_t)*s; } while (*s++); }
static void get(gpr v, char* o, size_t n) { size_t i = 0; while (i + 1 < n && (o[i] = (char)*at(v + i))) i++; o[i] = 0; }

static int64_t call(const char* name, gpr a0, gpr a1, gpr a2, gpr a3) {
    fn_t* f = (fn_t*)dlsym(lib, name);
    if (!f) { printf("missing export %s\n", name); exit(2); }
    recomp_context c; memset(&c, 0, sizeof c);
    c.r[4] = a0; c.r[5] = a1; c.r[6] = a2; c.r[7] = a3;
    f(rdram, &c);
    return (int64_t)c.r[2];
}

#define HOST (K + 0x1000)
#define NAME (K + 0x1100)
#define PASS (K + 0x1200)
#define TEXT (K + 0x1300)
#define OUT  (K + 0x2000)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static int wait_status(int want, double timeout) {
    double end = now() + timeout;
    while (now() < end) { if (call("chat_status", 0, 0, 0, 0) == want) return 1; usleep(20000); }
    return 0;
}

// Polls until a line equal to `want` arrives. Prints every line seen.
static int wait_line(const char* want, double timeout) {
    char line[512];
    double end = now() + timeout;
    while (now() < end) {
        int64_t n = call("chat_poll", OUT, 512, 0, 0);
        if (n >= 0) { get(OUT, line, sizeof line); printf("  line: %s\n", line); if (!strcmp(line, want)) return 1; }
        else usleep(20000);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 4) { printf("usage: net_harness <native library> <mode> <port>\n"); return 2; }
    lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { printf("dlopen failed\n"); return 2; }
    const char* mode = argv[2];
    gpr port = (gpr)atoi(argv[3]);
    rdram = calloc(1, 1 << 20);
    put(HOST, "127.0.0.1"); put(NAME, "Alice"); put(PASS, "pw");
    char buf[256];
    int ok = 0;

    if (!strcmp(mode, "ok")) {
        if (call("chat_connect", HOST, port, NAME, PASS) != 1) { printf("connect not started\n"); return 1; }
        if (!wait_status(2, 5)) { printf("never connected\n"); return 1; }
        call("chat_get_name", OUT, 64, 0, 0); get(OUT, buf, sizeof buf);
        printf("  assigned name: %s\n", buf);
        // Tab and newline must be neutralised, not break framing.
        put(TEXT, "hello\tfrom the\ngame");
        if (call("chat_send", TEXT, 0, 0, 0) != 1) { printf("send not queued\n"); return 1; }
        ok = !strcmp(buf, "Alice") && wait_line("Alice: hello from the game", 5) && wait_line("Bob: hi Alice", 5);
        call("chat_disconnect", 0, 0, 0, 0);
        ok = ok && call("chat_status", 0, 0, 0, 0) == 0;
        put(TEXT, "after disconnect");
        ok = ok && call("chat_send", TEXT, 0, 0, 0) == 0;
    }
    else if (!strcmp(mode, "badpw")) {
        put(PASS, "wrong");
        call("chat_connect", HOST, port, NAME, PASS);
        ok = wait_status(3, 5);
        call("chat_get_error", OUT, 160, 0, 0); get(OUT, buf, sizeof buf);
        printf("  error: %s\n", buf);
        ok = ok && !strcmp(buf, "Wrong password");
        call("chat_disconnect", 0, 0, 0, 0);
    }
    else if (!strcmp(mode, "refused")) {
        double start = now();
        call("chat_connect", HOST, port, NAME, PASS);
        ok = wait_status(3, 8);
        call("chat_get_error", OUT, 160, 0, 0); get(OUT, buf, sizeof buf);
        printf("  error after %.2fs: %s\n", now() - start, buf);
        call("chat_disconnect", 0, 0, 0, 0);
    }
    else if (!strcmp(mode, "presence")) {
        // Location set before connecting must still reach the server after WELCOME.
        call("chat_set_location", 0x6F, 2, 0, 0);
        call("chat_connect", HOST, port, NAME, PASS);
        if (!wait_status(2, 5)) { printf("never connected\n"); return 1; }
        printf("READY\n"); fflush(stdout);   // orchestrator moves Bob to Termina Field, day 1
        const char* want[2] = { "Alice (you) - South Clock Town, Day 2", "Bob - Termina Field, Day 1" };
        double end = now() + 5;
        uint32_t last_version = 0;
        while (now() < end && !ok) {
            uint32_t v = (uint32_t)call("chat_roster_version", 0, 0, 0, 0);
            if (v != last_version) {
                last_version = v;
                int64_t n = call("chat_roster_count", 0, 0, 0, 0);
                int matched = 0;
                printf("  roster v%u (%d):\n", v, (int)n);
                for (int i = 0; i < n; i++) {
                    call("chat_roster_get", i, OUT, 256, 0); get(OUT, buf, sizeof buf);
                    printf("    %s\n", buf);
                    for (int w = 0; w < 2; w++) if (!strcmp(buf, want[w])) matched++;
                }
                ok = n == 2 && matched == 2;
            }
            usleep(20000);
        }
        ok = ok && call("chat_roster_get", 5, OUT, 256, 0) == -1;
        call("chat_disconnect", 0, 0, 0, 0);
        ok = ok && call("chat_roster_count", 0, 0, 0, 0) == 0;
        printf("  roster cleared on disconnect: %s\n", call("chat_roster_count", 0, 0, 0, 0) == 0 ? "yes" : "no");
    }
    else if (!strcmp(mode, "bigroster")) {
        // Orchestrator has 15 bots with long multi-byte names connected.
        call("chat_connect", HOST, port, NAME, PASS);
        if (!wait_status(2, 5)) { printf("never connected\n"); return 1; }
        double end = now() + 5;
        int64_t n = 0;
        while (now() < end && (n = call("chat_roster_count", 0, 0, 0, 0)) != 16) usleep(20000);
        call("chat_roster_get", 15, OUT, 256, 0); get(OUT, buf, sizeof buf);
        printf("  roster count %d, last entry: %s\n", (int)n, buf);
        ok = n == 16 && call("chat_status", 0, 0, 0, 0) == 2;
        call("chat_disconnect", 0, 0, 0, 0);
    }
    else if (!strcmp(mode, "coop")) {
        // Progress made before connecting (offline) must reach the group once connected.
        put(TEXT, "testgrp");
        call("chat_coop_set_group", TEXT, 0, 0, 0);
        int changed = (int)call("chat_sync_report", 0x1001, 0x100, 0, 0);       // Bunny Hood
        int unchanged = (int)call("chat_sync_report", 0x1001, 0x100, 0, 0);     // same again: no-op
        call("chat_sync_report", 0x2005, 3, 0, 0);                              // a MAX key
        call("chat_connect", HOST, port, NAME, PASS);
        if (!wait_status(2, 5)) { printf("never connected\n"); return 1; }
        printf("READY\n"); fflush(stdout);  // orchestrator: Bob joins testgrp, adds Deku Mask, sends MAX 2
        double end = now() + 5;
        uint32_t masks = 0;
        while (now() < end && (masks = (uint32_t)call("chat_sync_get", 0x1001, 0, 0, 0)) != 0x120) usleep(20000);
        call("chat_sync_get_who", 0x1001, OUT, 64, 0); get(OUT, buf, sizeof buf);
        uint32_t maxval = (uint32_t)call("chat_sync_get", 0x2005, 0, 0, 0);
        printf("  report changed=%d repeat=%d, masks=%#x who=%s, max key=%u\n", changed, unchanged, masks, buf, maxval);
        ok = changed == 1 && unchanged == 0 && masks == 0x120 && !strcmp(buf, "Bob") && maxval == 3;
        // Invalid merge rule is refused.
        ok = ok && call("chat_sync_report", 0x4001, 1, 0, 0) == 0;
        // Switching groups wipes the cache so nothing leaks between groups.
        put(TEXT, "othergrp");
        call("chat_coop_set_group", TEXT, 0, 0, 0);
        uint32_t after = (uint32_t)call("chat_sync_get", 0x1001, 0, 0, 0);
        printf("  after switching group: masks=%#x\n", after);
        ok = ok && after == 0;
        usleep(300000);
        call("chat_disconnect", 0, 0, 0, 0);
    }
    else if (!strcmp(mode, "add")) {
        // ADD key 0x3001. Orchestrator: Bob has already contributed 3 to addgrp.
        put(TEXT, "addgrp");
        call("chat_coop_set_group", TEXT, 0, 0, 0);
        int offline = (int)call("chat_sync_report", 0x3001, 2, 0, 0);   // gained 2 while offline
        int ready_before = (int)call("chat_sync_ready", 0, 0, 0, 0);
        call("chat_connect", HOST, port, NAME, PASS);
        if (!wait_status(2, 5)) { printf("never connected\n"); return 1; }
        double end = now() + 5;
        while (now() < end && !call("chat_sync_ready", 0, 0, 0, 0)) usleep(20000);
        int ready = (int)call("chat_sync_ready", 0, 0, 0, 0);
        end = now() + 5;
        uint32_t total = 0;
        while (now() < end && (total = (uint32_t)call("chat_sync_get", 0x3001, 0, 0, 0)) != 5) usleep(20000);
        uint32_t mine = (uint32_t)call("chat_sync_get_mine", 0x3001, 0, 0, 0);
        printf("  offline report=%d ready before=%d after=%d, total=%u mine=%u\n", offline, ready_before, ready, total, mine);
        ok = offline == 1 && ready_before == 0 && ready == 1 && total == 5 && mine == 2;

        printf("READY\n"); fflush(stdout);  // orchestrator: Bob sends 4, 4 again, then 2
        end = now() + 5;
        while (now() < end && (total = (uint32_t)call("chat_sync_get", 0x3001, 0, 0, 0)) != 6) usleep(20000);
        call("chat_sync_get_who", 0x3001, OUT, 64, 0); get(OUT, buf, sizeof buf);
        usleep(300000);  // Bob's repeat and lower value must not change anything
        uint32_t settled = (uint32_t)call("chat_sync_get", 0x3001, 0, 0, 0);
        printf("  after Bob's +1: total=%u who=%s, settled=%u\n", total, buf, settled);
        ok = ok && total == 6 && settled == 6 && !strcmp(buf, "Bob");

        // Our own gain counts immediately (before the server confirms), and only once.
        int same = (int)call("chat_sync_report", 0x3001, 2, 0, 0);
        int more = (int)call("chat_sync_report", 0x3001, 3, 0, 0);
        uint32_t optimistic = (uint32_t)call("chat_sync_get", 0x3001, 0, 0, 0);
        usleep(500000);
        total = (uint32_t)call("chat_sync_get", 0x3001, 0, 0, 0);
        mine = (uint32_t)call("chat_sync_get_mine", 0x3001, 0, 0, 0);
        printf("  report same=%d more=%d, optimistic=%u, confirmed total=%u mine=%u\n", same, more, optimistic, total, mine);
        ok = ok && same == 0 && more == 1 && optimistic == 7 && total == 7 && mine == 3;

        // Switching groups: not ready until the new group's state has arrived.
        put(TEXT, "othergrp");
        call("chat_coop_set_group", TEXT, 0, 0, 0);
        int ready_switched = (int)call("chat_sync_ready", 0, 0, 0, 0);
        end = now() + 5;
        while (now() < end && !call("chat_sync_ready", 0, 0, 0, 0)) usleep(20000);
        uint32_t other = (uint32_t)call("chat_sync_get", 0x3001, 0, 0, 0);
        printf("  after switching group: ready=%d then %d, total=%u\n", ready_switched,
               (int)call("chat_sync_ready", 0, 0, 0, 0), other);
        ok = ok && ready_switched == 0 && call("chat_sync_ready", 0, 0, 0, 0) == 1 && other == 0;
        call("chat_disconnect", 0, 0, 0, 0);
        ok = ok && call("chat_sync_ready", 0, 0, 0, 0) == 0;
    }
    else if (!strcmp(mode, "addresume")) {
        // A fresh client (empty cache) learns its own contribution back from the server.
        put(TEXT, "addgrp");
        call("chat_coop_set_group", TEXT, 0, 0, 0);
        call("chat_connect", HOST, port, NAME, PASS);
        if (!wait_status(2, 5)) { printf("never connected\n"); return 1; }
        double end = now() + 5;
        while (now() < end && !call("chat_sync_ready", 0, 0, 0, 0)) usleep(20000);
        uint32_t total = (uint32_t)call("chat_sync_get", 0x3001, 0, 0, 0);
        uint32_t mine = (uint32_t)call("chat_sync_get_mine", 0x3001, 0, 0, 0);
        printf("  resumed: total=%u mine=%u\n", total, mine);
        ok = total == 7 && mine == 3;
        call("chat_disconnect", 0, 0, 0, 0);
    }
    else if (!strcmp(mode, "drop")) {
        call("chat_connect", HOST, port, NAME, PASS);
        if (!wait_status(2, 5)) { printf("never connected\n"); return 1; }
        printf("READY\n"); fflush(stdout);   // orchestrator kills the server now
        ok = wait_status(3, 5);
        call("chat_get_error", OUT, 160, 0, 0); get(OUT, buf, sizeof buf);
        printf("  error: %s\n", buf);
        call("chat_disconnect", 0, 0, 0, 0);
    }
    printf("%s %s\n", mode, ok ? "PASS" : "FAIL");
    return !ok;
}
