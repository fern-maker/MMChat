#include "modding.h"
#include "global.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "recompui.h"
#include "chat_native.h"
#include "chat_ui.h"
#include "coop.h"

#define DEFAULT_PORT 47474
#define HISTORY_LINES 64
#define LINE_CHARS 240
#define LOG_LABELS 14
#define OVERLAY_LABELS 4
#define OVERLAY_FRAMES 160 // About 8 seconds at the game's 20 updates per second.
#define POLLS_PER_FRAME 8

// ---- Chat history (newest at history_head - 1) ----

static char history[HISTORY_LINES][LINE_CHARS];
static int history_head;
static int history_count;
static bool history_dirty;

static void copy_text(char* out, u32 out_len, const char* in) {
    u32 i = 0;
    for (; i + 1 < out_len && in[i] != '\0'; i++) {
        out[i] = in[i];
    }
    out[i] = '\0';
}

// Writes `a` followed by `b` into `out`, truncating to fit. Mods have no sprintf.
static void join_text(char* out, u32 out_len, const char* a, const char* b) {
    copy_text(out, out_len, a);
    u32 used = 0;
    while (out[used] != '\0') {
        used++;
    }
    copy_text(out + used, out_len - used, b);
}

// Writes a non-negative number as decimal text.
static void number_text(char* out, u32 out_len, u32 value) {
    char digits[12];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && count < (int)sizeof(digits));
    u32 i = 0;
    for (; i + 1 < out_len && count > 0; i++) {
        out[i] = digits[--count];
    }
    out[i] = '\0';
}

static void history_push(const char* line) {
    copy_text(history[history_head], LINE_CHARS, line);
    history_head = (history_head + 1) % HISTORY_LINES;
    if (history_count < HISTORY_LINES) {
        history_count++;
    }
    history_dirty = true;
}

// Returns the line `age` entries back from the newest (0 = newest), or "" if there isn't one.
static const char* history_get(int age) {
    if (age >= history_count) {
        return "";
    }
    return history[(history_head - 1 - age + HISTORY_LINES) % HISTORY_LINES];
}

// ---- UI state ----

static RecompuiContext panel_context;
static RecompuiResource status_label;
static RecompuiResource coop_label;
static RecompuiResource server_input;
static RecompuiResource name_input;
static RecompuiResource password_input;
static RecompuiResource connect_button;
static RecompuiResource disconnect_button;
static RecompuiResource log_labels[LOG_LABELS];
static RecompuiResource roster_title_label;
static RecompuiResource roster_labels[CHAT_MAX_PLAYERS];
static RecompuiResource message_input;
static RecompuiResource send_button;
static RecompuiResource close_button;

static RecompuiContext overlay_context;
static RecompuiResource overlay_labels[OVERLAY_LABELS];

static bool panel_shown;
static bool overlay_shown;
static bool notice_pending; // A local notice (e.g. co-op) should pop up the overlay like a message.
static int overlay_frames_left;
static s32 shown_status = -1;
static u32 shown_roster_version;
static bool roster_drawn;

// UI callbacks run with their context already open; game hooks must open it themselves.
static bool in_panel_callback;

static void begin_panel_edit(void) {
    if (!in_panel_callback) {
        recompui_open_context(panel_context);
    }
}

static void end_panel_edit(void) {
    if (!in_panel_callback) {
        recompui_close_context(panel_context);
    }
}

// ---- Refreshing ----

static void refresh_roster(void) {
    s32 count = chat_roster_count();
    char text[LINE_CHARS];
    char number[12];

    number_text(number, sizeof(number), (u32)count);
    join_text(text, sizeof(text), "Online: ", number);
    recompui_set_text(roster_title_label, count > 0 ? text : "Online: not connected");

    for (int i = 0; i < CHAT_MAX_PLAYERS; i++) {
        if (i >= count || chat_roster_get(i, text, sizeof(text)) < 0) {
            text[0] = '\0';
        }
        recompui_set_text(roster_labels[i], text);
    }
}

static void refresh_panel(void) {
    s32 status = chat_status();
    u32 roster_version = chat_roster_version();
    bool roster_changed = !roster_drawn || roster_version != shown_roster_version;
    if (status == shown_status && !history_dirty && !roster_changed) {
        return;
    }

    begin_panel_edit();
    if (status != shown_status) {
        char text[200];
        char detail[160];
        switch (status) {
            case CHAT_CONNECTING:
                copy_text(text, sizeof(text), "Connecting...");
                break;
            case CHAT_CONNECTED:
                chat_get_name(detail, sizeof(detail));
                join_text(text, sizeof(text), "Connected as ", detail);
                break;
            case CHAT_ERROR:
                chat_get_error(detail, sizeof(detail));
                join_text(text, sizeof(text), "Error: ", detail);
                break;
            default:
                copy_text(text, sizeof(text), "Not connected");
                break;
        }
        recompui_set_text(status_label, text);
        shown_status = status;
    }
    if (history_dirty) {
        // The bottom label holds the newest line.
        for (int i = 0; i < LOG_LABELS; i++) {
            recompui_set_text(log_labels[i], history_get(LOG_LABELS - 1 - i));
        }
        history_dirty = false;
    }
    if (roster_changed) {
        refresh_roster();
        shown_roster_version = roster_version;
        roster_drawn = true;
    }
    end_panel_edit();
}

static void refresh_overlay(void) {
    recompui_open_context(overlay_context);
    for (int i = 0; i < OVERLAY_LABELS; i++) {
        recompui_set_text(overlay_labels[i], history_get(OVERLAY_LABELS - 1 - i));
    }
    recompui_close_context(overlay_context);
}

// Pulls new lines from the native library. Returns true if any arrived.
static bool poll_messages(void) {
    char line[LINE_CHARS];
    bool any = false;
    for (int i = 0; i < POLLS_PER_FRAME && chat_poll(line, sizeof(line)) >= 0; i++) {
        history_push(line);
        any = true;
    }
    return any;
}

void chat_ui_notify(const char* line) {
    history_push(line);
    notice_pending = true;
}

// ---- Actions ----

// Splits "host:port" (port optional) and connects.
static void connect_pressed(void) {
    char* server = recompui_get_input_text(server_input);
    char* name = recompui_get_input_text(name_input);
    char* password = recompui_get_input_text(password_input);

    char host[200];
    copy_text(host, sizeof(host), (server != NULL) ? server : "");
    u32 port = DEFAULT_PORT;
    char* colon = NULL;
    for (char* c = host; *c != '\0'; c++) {
        if (*c == ':') {
            colon = c;
        }
    }
    bool valid = host[0] != '\0';
    if (colon != NULL) {
        *colon = '\0';
        port = 0;
        for (char* c = colon + 1; *c != '\0'; c++) {
            if (*c < '0' || *c > '9' || port > 65535) {
                valid = false;
                break;
            }
            port = port * 10 + (u32)(*c - '0');
        }
        valid = valid && port > 0 && port <= 65535 && host[0] != '\0';
    }

    if (!valid) {
        history_push("* Enter the server as address:port, e.g. 192.168.1.20:47474");
    }
    else {
        const char* player = (name != NULL && name[0] != '\0') ? name : "Player";
        chat_connect(host, port, player, (password != NULL) ? password : "");
    }

    if (server != NULL) recomp_free(server);
    if (name != NULL) recomp_free(name);
    if (password != NULL) recomp_free(password);
}

static void send_pressed(void) {
    char* text = recompui_get_input_text(message_input);
    if (text == NULL) {
        return;
    }
    if (text[0] != '\0') {
        if (chat_send(text)) {
            recompui_set_input_text(message_input, "");
        }
        else {
            history_push("* Not connected, message not sent");
        }
    }
    recomp_free(text);
}

static void set_panel_shown(bool shown) {
    if (shown == panel_shown) {
        return;
    }
    if (shown) {
        // The panel shows the full log, so the overlay would only duplicate it.
        if (overlay_shown) {
            recompui_hide_context(overlay_context);
            overlay_shown = false;
        }
        // Co-op state only changes at file select, so refreshing it on open is enough.
        char coop_text[96];
        coop_describe(coop_text, sizeof(coop_text));
        begin_panel_edit();
        recompui_set_text(coop_label, coop_text);
        end_panel_edit();
        recompui_show_context(panel_context);
    }
    else {
        recompui_hide_context(panel_context);
    }
    panel_shown = shown;
}

static void panel_event(RecompuiResource resource, const RecompuiEventData* data, void* userdata) {
    if (data->type != UI_EVENT_CLICK) {
        return;
    }
    in_panel_callback = true;
    if (resource == connect_button) {
        connect_pressed();
    }
    else if (resource == disconnect_button) {
        chat_disconnect();
        history_push("* Disconnected");
    }
    else if (resource == send_button) {
        send_pressed();
    }
    else if (resource == close_button) {
        set_panel_shown(false);
    }
    // Clicks are also a chance to pick up messages, in case gameplay updates pause while the panel is open.
    poll_messages();
    refresh_panel();
    in_panel_callback = false;
}

// ---- Construction ----

static RecompuiResource create_row(RecompuiResource parent) {
    RecompuiResource row = recompui_create_element(panel_context, parent);
    recompui_set_display(row, DISPLAY_FLEX);
    recompui_set_flex_direction(row, FLEX_DIRECTION_ROW);
    recompui_set_align_items(row, ALIGN_ITEMS_CENTER);
    recompui_set_gap(row, 12.0f, UNIT_DP);
    return row;
}

static RecompuiResource create_input(RecompuiResource parent, bool password, const char* config_key, float grow) {
    RecompuiResource input = password ? recompui_create_passwordinput(panel_context, parent)
                                      : recompui_create_textinput(panel_context, parent);
    recompui_set_flex_grow(input, grow);
    recompui_set_flex_shrink(input, 1.0f);
    recompui_set_flex_basis(input, 0.0f, UNIT_DP);
    if (config_key != NULL) {
        char* value = recomp_get_config_string(config_key);
        if (value != NULL) {
            recompui_set_input_text(input, value);
            recomp_free_config_string(value);
        }
    }
    return input;
}

static RecompuiResource create_button(RecompuiResource parent, const char* text, RecompuiButtonStyle style) {
    RecompuiResource button = recompui_create_button(panel_context, parent, text, style);
    recompui_set_text_align(button, TEXT_ALIGN_CENTER);
    recompui_set_flex_shrink(button, 0.0f);
    recompui_register_callback(button, panel_event, NULL);
    return button;
}

static void create_panel(void) {
    RecompuiColor panel_color = { 8, 7, 13, 235 };
    RecompuiColor log_color = { 0, 0, 0, 120 };
    RecompuiColor border_color = { 255, 255, 255, 50 };

    panel_context = recompui_create_context();
    recompui_open_context(panel_context);

    RecompuiResource root = recompui_context_root(panel_context);
    recompui_set_position(root, POSITION_ABSOLUTE);
    recompui_set_top(root, 0, UNIT_DP);
    recompui_set_right(root, 0, UNIT_DP);
    recompui_set_bottom(root, 0, UNIT_DP);
    recompui_set_left(root, 0, UNIT_DP);
    recompui_set_width_auto(root);
    recompui_set_height_auto(root);
    recompui_set_display(root, DISPLAY_FLEX);
    recompui_set_justify_content(root, JUSTIFY_CONTENT_CENTER);
    recompui_set_align_items(root, ALIGN_ITEMS_CENTER);

    RecompuiResource panel = recompui_create_element(panel_context, root);
    recompui_set_display(panel, DISPLAY_FLEX);
    recompui_set_flex_direction(panel, FLEX_DIRECTION_COLUMN);
    recompui_set_width(panel, 1500.0f, UNIT_DP);
    recompui_set_max_width(panel, 92.0f, UNIT_PERCENT);
    recompui_set_padding(panel, 24.0f, UNIT_DP);
    recompui_set_gap(panel, 14.0f, UNIT_DP);
    recompui_set_border_width(panel, 1.1f, UNIT_DP);
    recompui_set_border_color(panel, &border_color);
    recompui_set_border_radius(panel, 16.0f, UNIT_DP);
    recompui_set_background_color(panel, &panel_color);

    RecompuiResource title_row = create_row(panel);
    recompui_set_justify_content(title_row, JUSTIFY_CONTENT_SPACE_BETWEEN);
    recompui_create_label(panel_context, title_row, "MMChat", LABELSTYLE_LARGE);
    coop_label = recompui_create_label(panel_context, title_row, "", LABELSTYLE_SMALL);
    status_label = recompui_create_label(panel_context, title_row, "Not connected", LABELSTYLE_NORMAL);

    RecompuiResource labels_row = create_row(panel);
    RecompuiResource server_caption = recompui_create_label(panel_context, labels_row, "Server (address:port)", LABELSTYLE_SMALL);
    RecompuiResource name_caption = recompui_create_label(panel_context, labels_row, "Your name", LABELSTYLE_SMALL);
    RecompuiResource password_caption = recompui_create_label(panel_context, labels_row, "Password", LABELSTYLE_SMALL);
    recompui_set_flex_grow(server_caption, 2.0f);
    recompui_set_flex_basis(server_caption, 0.0f, UNIT_DP);
    recompui_set_flex_grow(name_caption, 1.0f);
    recompui_set_flex_basis(name_caption, 0.0f, UNIT_DP);
    recompui_set_flex_grow(password_caption, 1.0f);
    recompui_set_flex_basis(password_caption, 0.0f, UNIT_DP);
    // Leaves room under the buttons so the captions line up with their inputs.
    RecompuiResource caption_spacer = recompui_create_element(panel_context, labels_row);
    recompui_set_width(caption_spacer, 360.0f, UNIT_DP);
    recompui_set_flex_shrink(caption_spacer, 0.0f);

    RecompuiResource connect_row = create_row(panel);
    server_input = create_input(connect_row, false, "server_address", 2.0f);
    name_input = create_input(connect_row, false, "player_name", 1.0f);
    password_input = create_input(connect_row, true, "server_password", 1.0f);
    connect_button = create_button(connect_row, "Connect", BUTTONSTYLE_PRIMARY);
    disconnect_button = create_button(connect_row, "Disconnect", BUTTONSTYLE_SECONDARY);

    // Message log on the left, who's online on the right.
    RecompuiResource middle_row = create_row(panel);
    recompui_set_align_items(middle_row, ALIGN_ITEMS_STRETCH);
    recompui_set_height(middle_row, 520.0f, UNIT_DP);

    RecompuiResource log = recompui_create_element(panel_context, middle_row);
    recompui_set_display(log, DISPLAY_FLEX);
    recompui_set_flex_direction(log, FLEX_DIRECTION_COLUMN);
    recompui_set_justify_content(log, JUSTIFY_CONTENT_FLEX_END);
    recompui_set_flex_grow(log, 1.0f);
    recompui_set_flex_basis(log, 0.0f, UNIT_DP);
    recompui_set_overflow(log, OVERFLOW_HIDDEN);
    recompui_set_padding(log, 12.0f, UNIT_DP);
    recompui_set_gap(log, 4.0f, UNIT_DP);
    recompui_set_border_radius(log, 8.0f, UNIT_DP);
    recompui_set_background_color(log, &log_color);
    for (int i = 0; i < LOG_LABELS; i++) {
        log_labels[i] = recompui_create_label(panel_context, log, "", LABELSTYLE_NORMAL);
    }

    RecompuiResource roster_box = recompui_create_element(panel_context, middle_row);
    recompui_set_display(roster_box, DISPLAY_FLEX);
    recompui_set_flex_direction(roster_box, FLEX_DIRECTION_COLUMN);
    recompui_set_width(roster_box, 400.0f, UNIT_DP);
    recompui_set_flex_shrink(roster_box, 0.0f);
    recompui_set_overflow(roster_box, OVERFLOW_HIDDEN);
    recompui_set_padding(roster_box, 12.0f, UNIT_DP);
    recompui_set_gap(roster_box, 4.0f, UNIT_DP);
    recompui_set_border_radius(roster_box, 8.0f, UNIT_DP);
    recompui_set_background_color(roster_box, &log_color);
    roster_title_label = recompui_create_label(panel_context, roster_box, "Online: not connected", LABELSTYLE_NORMAL);
    for (int i = 0; i < CHAT_MAX_PLAYERS; i++) {
        roster_labels[i] = recompui_create_label(panel_context, roster_box, "", LABELSTYLE_SMALL);
    }

    RecompuiResource message_row = create_row(panel);
    message_input = create_input(message_row, false, NULL, 1.0f);
    send_button = create_button(message_row, "Send", BUTTONSTYLE_PRIMARY);
    close_button = create_button(message_row, "Close", BUTTONSTYLE_SECONDARY);

    recompui_close_context(panel_context);
}

static void create_overlay(void) {
    RecompuiColor overlay_color = { 0, 0, 0, 150 };

    overlay_context = recompui_create_context();
    recompui_set_context_captures_input(overlay_context, 0);
    recompui_set_context_captures_mouse(overlay_context, 0);
    recompui_open_context(overlay_context);

    RecompuiResource root = recompui_context_root(overlay_context);
    recompui_set_position(root, POSITION_ABSOLUTE);
    recompui_set_left(root, 32.0f, UNIT_DP);
    recompui_set_bottom(root, 140.0f, UNIT_DP);
    recompui_set_width(root, 700.0f, UNIT_DP);
    recompui_set_height_auto(root);

    RecompuiResource box = recompui_create_element(overlay_context, root);
    recompui_set_display(box, DISPLAY_FLEX);
    recompui_set_flex_direction(box, FLEX_DIRECTION_COLUMN);
    recompui_set_padding(box, 10.0f, UNIT_DP);
    recompui_set_gap(box, 2.0f, UNIT_DP);
    recompui_set_border_radius(box, 8.0f, UNIT_DP);
    recompui_set_background_color(box, &overlay_color);
    for (int i = 0; i < OVERLAY_LABELS; i++) {
        overlay_labels[i] = recompui_create_label(overlay_context, box, "", LABELSTYLE_SMALL);
    }

    recompui_close_context(overlay_context);
}

RECOMP_CALLBACK("*", recomp_on_init)
void on_init() {
    create_panel();
    create_overlay();
    history_push("* Welcome to MMChat. Enter the host's address:port and press Connect.");
    recomp_printf("[mm_chat] loaded, hold L and press R in-game to open chat\n");
}

// Runs every gameplay frame: reports our location, opens the panel on L+R,
// pulls new messages, and fades the overlay.
RECOMP_HOOK("Play_UpdateMain") void on_play_update(PlayState* play) {
    // The title screen's attract demo also runs as gameplay, so only count real play.
    if (gSaveContext.gameMode == GAMEMODE_NORMAL) {
        chat_set_location(play->sceneId, CURRENT_DAY);
    }
    else {
        chat_set_location(CHAT_SCENE_NONE, 0);
    }

    Input* input = CONTROLLER1(&play->state);
    if (CHECK_BTN_ALL(input->cur.button, BTN_L) && CHECK_BTN_ALL(input->press.button, BTN_R)) {
        set_panel_shown(true);
    }

    coop_sync_update(play);

    bool new_messages = poll_messages() || notice_pending;
    notice_pending = false;

    if (panel_shown) {
        refresh_panel();
        return;
    }

    if (new_messages) {
        refresh_overlay();
        overlay_frames_left = OVERLAY_FRAMES;
        if (!overlay_shown) {
            recompui_show_context(overlay_context);
            overlay_shown = true;
        }
    }
    else if (overlay_shown && --overlay_frames_left <= 0) {
        recompui_hide_context(overlay_context);
        overlay_shown = false;
    }
}
