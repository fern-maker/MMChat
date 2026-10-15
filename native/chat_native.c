// Native side of MMChat: thin wrappers that expose chat_client.c to the game.
//
// The recomp runtime loads this library from the mods folder (next to the .nrm),
// checks `recomp_api_version`, then resolves every function listed under
// `native_libraries` in mod.toml. Each export uses the recompiled-function ABI:
// it receives the game's RDRAM and the MIPS register context. Arguments arrive
// in a0-a3 (r4-r7) and the return value goes in v0 (r2), per the MIPS o32 ABI.

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "chat_client.h"

#if defined(_WIN32)
#   define EXPORT __declspec(dllexport)
#else
#   define EXPORT __attribute__((visibility("default")))
#endif

// Mirrors recomp_context from N64Recomp's include/recomp.h. Field order matters.
typedef uint64_t gpr;

typedef union {
    double d;
    struct { float fl; float fh; };
    struct { uint32_t u32l; uint32_t u32h; };
    uint64_t u64;
} fpr;

typedef struct {
    gpr r0,  r1,  r2,  r3,  r4,  r5,  r6,  r7,
        r8,  r9,  r10, r11, r12, r13, r14, r15,
        r16, r17, r18, r19, r20, r21, r22, r23,
        r24, r25, r26, r27, r28, r29, r30, r31;
    fpr f0,  f1,  f2,  f3,  f4,  f5,  f6,  f7,
        f8,  f9,  f10, f11, f12, f13, f14, f15,
        f16, f17, f18, f19, f20, f21, f22, f23,
        f24, f25, f26, f27, f28, f29, f30, f31;
    uint64_t hi, lo;
    uint32_t* f_odd;
    uint32_t status_reg;
    uint8_t mips3_float_mode;
} recomp_context;

EXPORT uint32_t recomp_api_version = 1;

// RDRAM stores each 32-bit word in host byte order, so individual bytes live at
// (address ^ 3). Addresses are sign-extended KSEG0 values (0xFFFFFFFF8xxxxxxx).
static uint8_t* rdram_byte(uint8_t* rdram, gpr vaddr) {
    return rdram + ((vaddr ^ 3) - 0xFFFFFFFF80000000ULL);
}

// Copies a NUL-terminated string out of game memory into a host buffer.
static void read_game_string(uint8_t* rdram, gpr vaddr, char* out, size_t out_len) {
    size_t i = 0;
    for (; i + 1 < out_len; i++) {
        char c = (char)*rdram_byte(rdram, vaddr + i);
        if (c == '\0') {
            break;
        }
        out[i] = c;
    }
    out[i] = '\0';
}

// Copies a host string into a game buffer of `out_len` bytes, always NUL-terminated.
// Returns the number of characters written, excluding the terminator.
static uint32_t write_game_string(uint8_t* rdram, gpr vaddr, uint32_t out_len, const char* in) {
    if (out_len == 0) {
        return 0;
    }
    uint32_t i = 0;
    for (; i + 1 < out_len && in[i] != '\0'; i++) {
        *rdram_byte(rdram, vaddr + i) = (uint8_t)in[i];
    }
    *rdram_byte(rdram, vaddr + i) = 0;
    return i;
}

static uint32_t ping_count = 0;

// s32 chat_native_ping(const char* message, char* out, u32 out_len)
// Echoes `message` back with a call counter, proving argument passing, string
// reads and writes through RDRAM, return values, and state kept between calls.
EXPORT void chat_native_ping(uint8_t* rdram, recomp_context* ctx) {
    char message[128];
    char reply[256];

    read_game_string(rdram, ctx->r4, message, sizeof(message));
    ping_count++;
    snprintf(reply, sizeof(reply), "Native says: \"%s\" (call #%u)", message, ping_count);
    fprintf(stderr, "[mm_chat_native] ping #%u: %s\n", ping_count, message);

    uint32_t written = write_game_string(rdram, ctx->r5, (uint32_t)ctx->r6, reply);
    ctx->r2 = (gpr)(int32_t)written;
}

static void return_s32(recomp_context* ctx, int32_t value) {
    ctx->r2 = (gpr)value;
}

// s32 chat_connect(const char* host, u32 port, const char* name, const char* password)
EXPORT void chat_connect(uint8_t* rdram, recomp_context* ctx) {
    char host[256];
    char name[32];
    char password[64];
    read_game_string(rdram, ctx->r4, host, sizeof(host));
    read_game_string(rdram, ctx->r6, name, sizeof(name));
    read_game_string(rdram, ctx->r7, password, sizeof(password));
    uint32_t port = (uint32_t)ctx->r5;
    if (port == 0 || port > 65535) {
        return_s32(ctx, 0);
        return;
    }
    return_s32(ctx, mmchat_connect(host, (uint16_t)port, name, password));
}

// void chat_disconnect(void)
EXPORT void chat_disconnect(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    mmchat_disconnect();
}

// s32 chat_status(void): 0 disconnected, 1 connecting, 2 connected, 3 error
EXPORT void chat_status(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    return_s32(ctx, (int32_t)mmchat_status());
}

// s32 chat_get_error(char* out, u32 out_len)
EXPORT void chat_get_error(uint8_t* rdram, recomp_context* ctx) {
    char error[160];
    mmchat_get_error(error, sizeof(error));
    return_s32(ctx, (int32_t)write_game_string(rdram, ctx->r4, (uint32_t)ctx->r5, error));
}

// s32 chat_get_name(char* out, u32 out_len)
EXPORT void chat_get_name(uint8_t* rdram, recomp_context* ctx) {
    char name[32];
    mmchat_get_name(name, sizeof(name));
    return_s32(ctx, (int32_t)write_game_string(rdram, ctx->r4, (uint32_t)ctx->r5, name));
}

// s32 chat_send(const char* text): 1 if queued
EXPORT void chat_send(uint8_t* rdram, recomp_context* ctx) {
    char text[MMCHAT_MAX_LINE];
    read_game_string(rdram, ctx->r4, text, sizeof(text));
    return_s32(ctx, mmchat_send(text));
}

// void chat_set_location(s32 scene, s32 day)
EXPORT void chat_set_location(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    mmchat_set_location((int)(int32_t)ctx->r4, (int)(int32_t)ctx->r5);
}

// u32 chat_roster_version(void)
EXPORT void chat_roster_version(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    return_s32(ctx, (int32_t)mmchat_roster_version());
}

// s32 chat_roster_count(void)
EXPORT void chat_roster_count(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    return_s32(ctx, mmchat_roster_count());
}

// s32 chat_roster_get(s32 index, char* out, u32 out_len): line length, or -1 if out of range
EXPORT void chat_roster_get(uint8_t* rdram, recomp_context* ctx) {
    char line[MMCHAT_MAX_LINE];
    if (mmchat_roster_get((int)(int32_t)ctx->r4, line, sizeof(line)) < 0) {
        return_s32(ctx, -1);
        return;
    }
    return_s32(ctx, (int32_t)write_game_string(rdram, ctx->r5, (uint32_t)ctx->r6, line));
}

// s32 chat_poll(char* out, u32 out_len): line length, or -1 if no new lines
EXPORT void chat_poll(uint8_t* rdram, recomp_context* ctx) {
    char line[MMCHAT_MAX_LINE];
    if (mmchat_poll(line, sizeof(line)) < 0) {
        return_s32(ctx, -1);
        return;
    }
    return_s32(ctx, (int32_t)write_game_string(rdram, ctx->r4, (uint32_t)ctx->r5, line));
}
