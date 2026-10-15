// Portable MMChat network client (Windows: Winsock + Win32 threads, elsewhere: BSD sockets + pthreads).
//
// All networking runs on a background thread. Every function here returns immediately
// and is safe to call once per frame from the game thread.

#ifndef MMCHAT_CHAT_CLIENT_H
#define MMCHAT_CHAT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define MMCHAT_PROTOCOL_VERSION "4"
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

// ---- Co-op sync ----
//
// Shared progression is a set of 32-bit values identified by keys. The merge rule is part of the key
// (bits 12-15), so the server and this library can merge without knowing what a key means.

#define MMCHAT_SYNC_OP(key) (((key) >> 12) & 0xF)
#define MMCHAT_SYNC_OR 1  // Bit sets: a bit set by anyone is set for everyone.
#define MMCHAT_SYNC_MAX 2 // Levels: everyone gets the highest value.
#define MMCHAT_SYNC_ADD 3 // Counters: the server sums each player's contribution.
#define MMCHAT_SYNC_SLOTS 256

// Joins a co-op group (the co-op save name), or leaves with "". Changing groups clears the cached state.
// Cheap to call every frame.
void mmchat_coop_set_group(const char* group);

// 1 once the server has sent this group's whole state on the current connection; 0 while
// disconnected or just after joining. ADD keys need it: until then their totals are incomplete.
int mmchat_sync_ready(void);
// Increments whenever any merged value changes.
uint32_t mmchat_sync_version(void);
// The group's merged value for `key` (0 if nothing is known yet). For ADD keys, the total including
// our own latest contribution even before the server has confirmed it.
uint32_t mmchat_sync_get(uint32_t key);
// ADD keys: our own contribution to the total (0 for other keys).
uint32_t mmchat_sync_get_mine(uint32_t key);
// Name of the player whose update last changed `key`. Returns its length.
size_t mmchat_sync_get_who(uint32_t key, char* out, size_t out_len);
// Merges a local value into the cache and sends it if that changed anything. For ADD keys `value` is
// our whole contribution so far (not an increment), so resending it can't count twice.
// Works offline too: unsent changes go out after (re)connecting. Returns 1 if the cache changed.
int mmchat_sync_report(uint32_t key, uint32_t value);

#endif
