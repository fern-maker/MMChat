// Portable MMChat network client (Windows: Winsock + Win32 threads, elsewhere: BSD sockets + pthreads).
//
// All networking runs on a background thread. Every function here returns immediately
// and is safe to call once per frame from the game thread.

#ifndef MMCHAT_CHAT_CLIENT_H
#define MMCHAT_CHAT_CLIENT_H

#include <stddef.h>
#include <stdint.h>

#define MMCHAT_PROTOCOL_VERSION "1"
#define MMCHAT_MAX_LINE 512

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

#endif
