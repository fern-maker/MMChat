// Portable MMChat network client (Windows: Winsock + Win32 threads, elsewhere: BSD sockets + pthreads).
//
// All networking runs on a background thread. Every function here returns immediately
// and is safe to call once per frame from the game thread.

#ifndef MMCHAT_CHAT_CLIENT_H
#define MMCHAT_CHAT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define MMCHAT_PROTOCOL_VERSION "2"
#define MMCHAT_MAX_LINE 512
#define MMCHAT_MAX_PLAYERS 16
#define MMCHAT_SCENE_NONE (-1) // Title screen, file select, credits.

typedef enum {
    MMCHAT_DISCONNECTED = 0,
    MMCHAT_CONNECTING = 1,
    MMCHAT_CONNECTED = 2,
    MMCHAT_ERROR = 3, // Connection failed or dropped; mmchat_get_error has the reason.
} MmchatStatus;

// Starts connecting in the background, replacing any existing connection. Returns 1 if started.
int mmchat_connect(const char* host, uint16_t port, const char* name, const char* password);
void mmchat_disconnect(void);
MmchatStatus mmchat_status(void);

// Copies the last error into `out` (always NUL-terminated). Returns its length.
size_t mmchat_get_error(char* out, size_t out_len);
// Copies the name the server assigned (may differ from the requested one). Returns its length.
size_t mmchat_get_name(char* out, size_t out_len);

// Queues a chat message. Returns 1 if queued, 0 if not connected or the queue is full.
int mmchat_send(const char* text);

// Pops the oldest incoming display line ("Name: text" or "* notice") into `out`.
// Returns its length, or -1 if there is nothing to read.
int mmchat_poll(char* out, size_t out_len);

// Records where this player is. Cheap to call every frame; the server is only told about changes.
void mmchat_set_location(int scene, int day);

// Increments whenever the player list changes, so callers can skip redrawing otherwise.
uint32_t mmchat_roster_version(void);
int mmchat_roster_count(void);
// Copies roster entry `index` as a display line, e.g. "Alice - South Clock Town (Day 2)".
// Returns its length, or -1 if `index` is out of range.
int mmchat_roster_get(int index, char* out, size_t out_len);

#endif
