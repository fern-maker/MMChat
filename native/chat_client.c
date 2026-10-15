#include "chat_client.h"
#include "scene_names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <winsock2.h>
#   include <ws2tcpip.h>
#   include <windows.h>
#   if defined(_MSC_VER)
#       pragma comment(lib, "ws2_32.lib")
#   endif
    typedef SOCKET socket_t;
#   define BAD_SOCKET INVALID_SOCKET
#   define close_socket closesocket
#   define SEND_FLAGS 0
    typedef HANDLE thread_t;
    static SRWLOCK lock = SRWLOCK_INIT;
#   define LOCK() AcquireSRWLockExclusive(&lock)
#   define UNLOCK() ReleaseSRWLockExclusive(&lock)
#else
#   include <errno.h>
#   include <fcntl.h>
#   include <netdb.h>
#   include <pthread.h>
#   include <sys/select.h>
#   include <sys/socket.h>
#   include <sys/types.h>
#   include <unistd.h>
    typedef int socket_t;
#   define BAD_SOCKET (-1)
#   define close_socket close
    // A send to a socket the server closed raises SIGPIPE, which would kill the game.
#   if defined(MSG_NOSIGNAL)
#       define SEND_FLAGS MSG_NOSIGNAL
#   else
#       define SEND_FLAGS 0
#   endif
    typedef pthread_t thread_t;
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#   define LOCK() pthread_mutex_lock(&lock)
#   define UNLOCK() pthread_mutex_unlock(&lock)
#endif

#define CONNECT_TIMEOUT_MS 5000
#define POLL_INTERVAL_MS 50
#define KEEPALIVE_SECONDS 30
#define INCOMING_SLOTS 64
#define OUTGOING_SLOTS 16
#define DISPLAY_LINE 320
// Roster lines carry every player, so they can be much longer than chat lines.
#define RECEIVE_BUFFER 8192
#define MAX_FIELDS (2 + MMCHAT_MAX_PLAYERS * 3)

// ---- Shared state (guarded by `lock`) ----

static MmchatStatus status = MMCHAT_DISCONNECTED;
static char last_error[160];
static char assigned_name[32];
static int stop_requested;

static char incoming[INCOMING_SLOTS][DISPLAY_LINE];
static int incoming_head, incoming_count;

static char outgoing[OUTGOING_SLOTS][MMCHAT_MAX_LINE];
static int outgoing_head, outgoing_count;

typedef struct {
    char name[64];
    int scene;
    int day;
} RosterEntry;

static RosterEntry roster[MMCHAT_MAX_PLAYERS];
static int roster_count;
static uint32_t roster_version;

// This player's location; `location_dirty` means the server hasn't been told yet.
static int my_scene = MMCHAT_SCENE_NONE;
static int my_day;
static int location_dirty;

// Connection parameters, copied for the worker thread.
static char conn_host[256];
static char conn_port[8];
static char conn_hello[MMCHAT_MAX_LINE];

// Only touched by the game thread.
static thread_t worker;
static int worker_running;

// ---- Helpers ----

static void copy_string(char* out, size_t out_len, const char* in) {
    if (out_len == 0) {
        return;
    }
    size_t n = strlen(in);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, in, n);
    out[n] = '\0';
}

// Tabs and newlines would break the line protocol, so they become spaces.
static void sanitize_field(char* out, size_t out_len, const char* in) {
    copy_string(out, out_len, in);
    for (char* c = out; *c != '\0'; c++) {
        if (*c == '\t' || *c == '\n' || *c == '\r') {
            *c = ' ';
        }
    }
}

// Caller must hold `lock`.
static void push_incoming_locked(const char* line) {
    if (incoming_count == INCOMING_SLOTS) {
        // Drop the oldest line rather than the newest.
        incoming_head = (incoming_head + 1) % INCOMING_SLOTS;
        incoming_count--;
    }
    int slot = (incoming_head + incoming_count) % INCOMING_SLOTS;
    copy_string(incoming[slot], DISPLAY_LINE, line);
    incoming_count++;
}

static void push_incoming(const char* line) {
    LOCK();
    push_incoming_locked(line);
    UNLOCK();
}

// Caller must hold `lock`.
static void clear_roster_locked(void) {
    if (roster_count != 0) {
        roster_count = 0;
        roster_version++;
    }
}

static void set_error(const char* message) {
    char line[DISPLAY_LINE];
    snprintf(line, sizeof(line), "* %s", message);
    LOCK();
    status = MMCHAT_ERROR;
    copy_string(last_error, sizeof(last_error), message);
    push_incoming_locked(line);
    clear_roster_locked();
    UNLOCK();
}

// Parses a whole decimal string (optionally negative). Returns 0 if it isn't one.
static int parse_int(const char* text, int* out) {
    char* end;
    long value = strtol(text, &end, 10);
    if (*text == '\0' || *end != '\0' || value < -100000 || value > 100000) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

static int should_stop(void) {
    LOCK();
    int stop = stop_requested;
    UNLOCK();
    return stop;
}

static int set_nonblocking(socket_t s, int enabled) {
#if defined(_WIN32)
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) {
        return 0;
    }
    flags = enabled ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(s, F_SETFL, flags) == 0;
#endif
}

static int send_all(socket_t s, const char* data, size_t len) {
    while (len > 0) {
        int sent = send(s, data, (int)len, SEND_FLAGS);
        if (sent <= 0) {
            return 0;
        }
        data += sent;
        len -= (size_t)sent;
    }
    return 1;
}

// Connects with a timeout so an unreachable host can't hang the worker for a minute.
static socket_t connect_with_timeout(const char* host, const char* port) {
    struct addrinfo hints;
    struct addrinfo* results = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &results) != 0 || results == NULL) {
        set_error("Could not find that server address");
        return BAD_SOCKET;
    }

    socket_t s = BAD_SOCKET;
    for (struct addrinfo* ai = results; ai != NULL && s == BAD_SOCKET; ai = ai->ai_next) {
        socket_t candidate = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (candidate == BAD_SOCKET) {
            continue;
        }
#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(candidate, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        set_nonblocking(candidate, 1);
        connect(candidate, ai->ai_addr, (int)ai->ai_addrlen);

        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(candidate, &writable);
        FD_SET(candidate, &failed);
        struct timeval tv = { CONNECT_TIMEOUT_MS / 1000, (CONNECT_TIMEOUT_MS % 1000) * 1000 };

        int ready = select((int)candidate + 1, NULL, &writable, &failed, &tv);
        int so_error = 1;
        socklen_t so_len = sizeof(so_error);
        if (ready > 0 && FD_ISSET(candidate, &writable)) {
            getsockopt(candidate, SOL_SOCKET, SO_ERROR, (char*)&so_error, &so_len);
        }
        if (so_error == 0 && set_nonblocking(candidate, 0)) {
            s = candidate;
        }
        else {
            close_socket(candidate);
        }
    }
    freeaddrinfo(results);

    if (s == BAD_SOCKET) {
        set_error("Could not connect (is the server running and reachable?)");
    }
    return s;
}

// Turns one server line into a display line, updating state as needed.
// Returns 0 if the server rejected us and the connection should end.
static int handle_server_line(char* line) {
    char* fields[MAX_FIELDS] = { 0 };
    int count = 0;
    char* cursor = line;
    while (count < MAX_FIELDS) {
        fields[count++] = cursor;
        char* tab = strchr(cursor, '\t');
        if (tab == NULL) {
            break;
        }
        *tab = '\0';
        cursor = tab + 1;
    }

    char display[DISPLAY_LINE];
    if (strcmp(fields[0], "CHAT") == 0 && count == 3) {
        snprintf(display, sizeof(display), "%s: %s", fields[1], fields[2]);
        push_incoming(display);
    }
    else if (strcmp(fields[0], "INFO") == 0 && count == 2) {
        snprintf(display, sizeof(display), "* %s", fields[1]);
        push_incoming(display);
    }
    else if (strcmp(fields[0], "WELCOME") == 0 && count == 2) {
        snprintf(display, sizeof(display), "* Connected as %s", fields[1]);
        LOCK();
        status = MMCHAT_CONNECTED;
        copy_string(assigned_name, sizeof(assigned_name), fields[1]);
        push_incoming_locked(display);
        location_dirty = 1; // A fresh session doesn't know where we are yet.
        UNLOCK();
    }
    else if (strcmp(fields[0], "ROSTER") == 0 && count >= 2) {
        // ROSTER <n> then <name> <scene> <day> for each player.
        int players;
        if (!parse_int(fields[1], &players) || players < 0 || players > MMCHAT_MAX_PLAYERS || count != 2 + players * 3) {
            return 1;
        }
        RosterEntry parsed[MMCHAT_MAX_PLAYERS];
        for (int i = 0; i < players; i++) {
            copy_string(parsed[i].name, sizeof(parsed[i].name), fields[2 + i * 3]);
            if (!parse_int(fields[3 + i * 3], &parsed[i].scene) || !parse_int(fields[4 + i * 3], &parsed[i].day)) {
                return 1;
            }
        }
        LOCK();
        memcpy(roster, parsed, sizeof(RosterEntry) * (size_t)players);
        roster_count = players;
        roster_version++;
        UNLOCK();
    }
    else if (strcmp(fields[0], "ERROR") == 0 && count == 2) {
        set_error(fields[1]);
        return 0;
    }
    // PONG and unknown lines are ignored so newer servers can add messages.
    return 1;
}

// ---- Worker thread ----

static void run_connection(void) {
    socket_t s = connect_with_timeout(conn_host, conn_port);
    if (s == BAD_SOCKET) {
        return;
    }
    if (!send_all(s, conn_hello, strlen(conn_hello))) {
        set_error("Connection lost while joining");
        close_socket(s);
        return;
    }

    static char buffer[RECEIVE_BUFFER]; // Only one worker runs at a time.
    size_t buffered = 0;
    time_t last_sent = time(NULL);

    while (!should_stop()) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(s, &readable);
        struct timeval tv = { 0, POLL_INTERVAL_MS * 1000 };
        int ready = select((int)s + 1, &readable, NULL, NULL, &tv);
        if (ready < 0) {
            set_error("Connection error");
            break;
        }

        if (ready > 0) {
            int received = recv(s, buffer + buffered, (int)(sizeof(buffer) - 1 - buffered), 0);
            if (received <= 0) {
                set_error("Disconnected from server");
                break;
            }
            buffered += (size_t)received;
            buffer[buffered] = '\0';

            char* line = buffer;
            char* newline;
            int keep_going = 1;
            while (keep_going && (newline = strchr(line, '\n')) != NULL) {
                *newline = '\0';
                if (newline > line && newline[-1] == '\r') {
                    newline[-1] = '\0';
                }
                keep_going = handle_server_line(line);
                line = newline + 1;
            }
            if (!keep_going) {
                break;
            }
            buffered = strlen(line);
            memmove(buffer, line, buffered);
            if (buffered >= sizeof(buffer) - 1) {
                // A line longer than the protocol allows; the server is misbehaving.
                set_error("Server sent an invalid message");
                break;
            }
        }

        // Send everything queued by the game thread, plus our location if it changed.
        char pending[OUTGOING_SLOTS + 1][MMCHAT_MAX_LINE];
        int pending_count = 0;
        LOCK();
        while (outgoing_count > 0) {
            memcpy(pending[pending_count++], outgoing[outgoing_head], MMCHAT_MAX_LINE);
            outgoing_head = (outgoing_head + 1) % OUTGOING_SLOTS;
            outgoing_count--;
        }
        if (location_dirty && status == MMCHAT_CONNECTED) {
            snprintf(pending[pending_count++], MMCHAT_MAX_LINE, "WHERE\t%d\t%d\n", my_scene, my_day);
            location_dirty = 0;
        }
        UNLOCK();

        int send_failed = 0;
        for (int i = 0; i < pending_count && !send_failed; i++) {
            send_failed = !send_all(s, pending[i], strlen(pending[i]));
        }
        if (pending_count == 0 && time(NULL) - last_sent >= KEEPALIVE_SECONDS) {
            send_failed = !send_all(s, "PING\n", 5);
            pending_count = 1;
        }
        if (send_failed) {
            set_error("Disconnected from server");
            break;
        }
        if (pending_count > 0) {
            last_sent = time(NULL);
        }
    }

    close_socket(s);
}

static void finish_connection(void) {
    LOCK();
    if (status != MMCHAT_ERROR) {
        status = MMCHAT_DISCONNECTED;
    }
    clear_roster_locked();
    UNLOCK();
}

#if defined(_WIN32)
static DWORD WINAPI worker_main(LPVOID unused) {
    (void)unused;
    run_connection();
    finish_connection();
    return 0;
}
#else
static void* worker_main(void* unused) {
    (void)unused;
    run_connection();
    finish_connection();
    return NULL;
}
#endif

// ---- Public API ----

static int ensure_network_started(void) {
#if defined(_WIN32)
    static int started = 0;
    if (!started) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            return 0;
        }
        started = 1;
    }
#endif
    return 1;
}

void mmchat_disconnect(void) {
    if (!worker_running) {
        return;
    }
    LOCK();
    stop_requested = 1;
    UNLOCK();
    // The worker notices within one poll interval, or after the connect timeout at worst.
#if defined(_WIN32)
    WaitForSingleObject(worker, INFINITE);
    CloseHandle(worker);
#else
    pthread_join(worker, NULL);
#endif
    worker_running = 0;
    LOCK();
    status = MMCHAT_DISCONNECTED;
    clear_roster_locked();
    UNLOCK();
}

int mmchat_connect(const char* host, uint16_t port, const char* name, const char* password) {
    mmchat_disconnect();
    if (!ensure_network_started()) {
        set_error("Could not start networking");
        return 0;
    }

    char clean_name[32];
    char clean_password[64];
    sanitize_field(clean_name, sizeof(clean_name), name);
    sanitize_field(clean_password, sizeof(clean_password), password);

    LOCK();
    copy_string(conn_host, sizeof(conn_host), host);
    snprintf(conn_port, sizeof(conn_port), "%u", (unsigned)port);
    snprintf(conn_hello, sizeof(conn_hello), "HELLO\t%s\t%s\t%s\n", MMCHAT_PROTOCOL_VERSION, clean_name, clean_password);
    status = MMCHAT_CONNECTING;
    stop_requested = 0;
    last_error[0] = '\0';
    assigned_name[0] = '\0';
    outgoing_head = outgoing_count = 0;
    UNLOCK();

#if defined(_WIN32)
    worker = CreateThread(NULL, 0, worker_main, NULL, 0, NULL);
    worker_running = worker != NULL;
#else
    worker_running = pthread_create(&worker, NULL, worker_main, NULL) == 0;
#endif
    if (!worker_running) {
        set_error("Could not start the network thread");
        return 0;
    }
    return 1;
}

MmchatStatus mmchat_status(void) {
    LOCK();
    MmchatStatus result = status;
    UNLOCK();
    return result;
}

size_t mmchat_get_error(char* out, size_t out_len) {
    LOCK();
    copy_string(out, out_len, last_error);
    UNLOCK();
    return out_len ? strlen(out) : 0;
}

size_t mmchat_get_name(char* out, size_t out_len) {
    LOCK();
    copy_string(out, out_len, assigned_name);
    UNLOCK();
    return out_len ? strlen(out) : 0;
}

int mmchat_send(const char* text) {
    char clean_text[MMCHAT_MAX_LINE - 8];
    sanitize_field(clean_text, sizeof(clean_text), text);
    if (clean_text[0] == '\0') {
        return 0;
    }

    int queued = 0;
    LOCK();
    if (status == MMCHAT_CONNECTED && outgoing_count < OUTGOING_SLOTS) {
        int slot = (outgoing_head + outgoing_count) % OUTGOING_SLOTS;
        snprintf(outgoing[slot], MMCHAT_MAX_LINE, "SAY\t%s\n", clean_text);
        outgoing_count++;
        queued = 1;
    }
    UNLOCK();
    return queued;
}

void mmchat_set_location(int scene, int day) {
    LOCK();
    if (scene != my_scene || day != my_day) {
        my_scene = scene;
        my_day = day;
        location_dirty = 1;
    }
    UNLOCK();
}

uint32_t mmchat_roster_version(void) {
    LOCK();
    uint32_t version = roster_version;
    UNLOCK();
    return version;
}

int mmchat_roster_count(void) {
    LOCK();
    int count = roster_count;
    UNLOCK();
    return count;
}

static const char* scene_display_name(int scene) {
    if (scene == MMCHAT_SCENE_NONE) {
        return "In menus";
    }
    if (scene >= 0 && scene < SCENE_NAME_COUNT && scene_names[scene] != NULL) {
        return scene_names[scene];
    }
    return "Unknown area";
}

int mmchat_roster_get(int index, char* out, size_t out_len) {
    char line[DISPLAY_LINE];
    int found = 0;
    LOCK();
    if (index >= 0 && index < roster_count) {
        const RosterEntry* entry = &roster[index];
        const char* you = strcmp(entry->name, assigned_name) == 0 ? " (you)" : "";
        // Days 1-3 are the three-day cycle; 0 and 4 are the intro and the final hours.
        if (entry->scene != MMCHAT_SCENE_NONE && entry->day >= 1 && entry->day <= 3) {
            snprintf(line, sizeof(line), "%s%s - %s, Day %d", entry->name, you, scene_display_name(entry->scene), entry->day);
        }
        else {
            snprintf(line, sizeof(line), "%s%s - %s", entry->name, you, scene_display_name(entry->scene));
        }
        found = 1;
    }
    UNLOCK();
    if (!found) {
        return -1;
    }
    copy_string(out, out_len, line);
    return out_len ? (int)strlen(out) : 0;
}

int mmchat_poll(char* out, size_t out_len) {
    int length = -1;
    LOCK();
    if (incoming_count > 0) {
        copy_string(out, out_len, incoming[incoming_head]);
        incoming_head = (incoming_head + 1) % INCOMING_SLOTS;
        incoming_count--;
        length = out_len ? (int)strlen(out) : 0;
    }
    UNLOCK();
    return length;
}
