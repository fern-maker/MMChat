#ifndef __CHAT_NATIVE_H__
#define __CHAT_NATIVE_H__

#include "modding.h"
#include "global.h"

// Functions provided by this mod's native library (native/chat_native.c).

typedef enum {
    CHAT_DISCONNECTED = 0,
    CHAT_CONNECTING = 1,
    CHAT_CONNECTED = 2,
    CHAT_ERROR = 3
} ChatStatus;

// Starts connecting in the background. Returns 1 if started.
RECOMP_IMPORT(".", s32 chat_connect(const char* host, u32 port, const char* name, const char* password));
RECOMP_IMPORT(".", void chat_disconnect(void));
RECOMP_IMPORT(".", s32 chat_status(void));
RECOMP_IMPORT(".", s32 chat_get_error(char* out, u32 out_len));
RECOMP_IMPORT(".", s32 chat_get_name(char* out, u32 out_len));
// Returns 1 if the message was queued.
RECOMP_IMPORT(".", s32 chat_send(const char* text));
// Pops one display line. Returns its length, or -1 if there are none.
RECOMP_IMPORT(".", s32 chat_poll(char* out, u32 out_len));

#endif
