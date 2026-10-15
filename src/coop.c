#include "modding.h"
#include "global.h"
#include "recomputils.h"
#include "recompconfig.h"
#include "recompui.h"
#include "coop.h"

// Co-op play uses its own save file, saves/mm_chat/coop_<name>.bin, so sync can never touch normal saves.
//
// The switch happens right before file select starts: FileSelect_Init reads every save slot, so swapping
// the backing file first means the game simply sees the co-op file's slots. Switching during gameplay is
// never done.

#define SAVE_NAME_CHARS 32

// recomp_change_save_file paths are relative to saves/<mod id>/; this is the game's normal save file.
#define NORMAL_SAVE_PATH "../mm.n64.us.1.0"

static CoopState state = COOP_OFF;
static char active_name[SAVE_NAME_CHARS + 1];
static bool conflicting_mod_enabled;

static RecompuiContext banner_context;
static RecompuiResource banner_title;
static RecompuiResource banner_detail;

static void copy_text(char* out, u32 out_len, const char* in) {
    u32 i = 0;
    for (; i + 1 < out_len && in[i] != '\0'; i++) {
        out[i] = in[i];
    }
    out[i] = '\0';
}

static void append_text(char* out, u32 out_len, const char* in) {
    u32 used = 0;
    while (out[used] != '\0') {
        used++;
    }
    copy_text(out + used, out_len - used, in);
}

// Keeps only characters that are safe in a file name on every platform.
static void sanitize_save_name(char* out, u32 out_len, const char* in) {
    u32 used = 0;
    for (u32 i = 0; in != NULL && in[i] != '\0' && used + 1 < out_len; i++) {
        char c = in[i];
        bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (safe) {
            out[used++] = c;
        }
    }
    out[used] = '\0';
    if (used == 0) {
        copy_text(out, out_len, "default");
    }
}

CoopState coop_get_state(void) {
    return state;
}

void coop_describe(char* out, u32 out_len) {
    switch (state) {
        case COOP_ACTIVE:
            copy_text(out, out_len, "Co-op save: ");
            append_text(out, out_len, active_name);
            break;
        case COOP_BLOCKED:
            copy_text(out, out_len, "Co-op off: disable Ultimate 6 Save Slots first");
            break;
        default:
            copy_text(out, out_len, "Co-op off (normal saves)");
            break;
    }
}

static void update_banner(void) {
    char title[96];
    const char* detail;
    switch (state) {
        case COOP_ACTIVE:
            copy_text(title, sizeof(title), "Co-op save: ");
            append_text(title, sizeof(title), active_name);
            detail = "Progression is shared with your group.";
            break;
        case COOP_BLOCKED:
            copy_text(title, sizeof(title), "Co-op unavailable");
            detail = "Disable Ultimate 6 Save Slots to use co-op. Showing normal saves.";
            break;
        default:
            title[0] = '\0';
            detail = "";
            break;
    }
    recompui_open_context(banner_context);
    recompui_set_text(banner_title, title);
    recompui_set_text(banner_detail, detail);
    recompui_close_context(banner_context);
}

// Decides which save file file select should show, based on the mod's config.
static void apply_config(void) {
    bool want_coop = recomp_get_config_u32("coop_mode") == 1;
    char wanted_name[SAVE_NAME_CHARS + 1];
    char* configured = recomp_get_config_string("coop_save_name");
    sanitize_save_name(wanted_name, sizeof(wanted_name), configured);
    if (configured != NULL) {
        recomp_free_config_string(configured);
    }

    if (want_coop && conflicting_mod_enabled) {
        // Leave whatever file the other mod chose; never fight it over the save file.
        state = COOP_BLOCKED;
        return;
    }

    if (want_coop) {
        bool same_file = state == COOP_ACTIVE;
        for (u32 i = 0; same_file && i <= SAVE_NAME_CHARS; i++) {
            same_file = active_name[i] == wanted_name[i];
            if (wanted_name[i] == '\0') {
                break;
            }
        }
        if (!same_file) {
            char path[SAVE_NAME_CHARS + 8];
            copy_text(path, sizeof(path), "coop_");
            append_text(path, sizeof(path), wanted_name);
            recomp_change_save_file(path);
            copy_text(active_name, sizeof(active_name), wanted_name);
            recomp_printf("[mm_chat] switched to co-op save %s\n", path);
        }
        state = COOP_ACTIVE;
    }
    else {
        if (state == COOP_ACTIVE) {
            recomp_change_save_file(NORMAL_SAVE_PATH);
            recomp_printf("[mm_chat] switched back to normal saves\n");
        }
        active_name[0] = '\0';
        state = COOP_OFF;
    }
}

RECOMP_HOOK("FileSelect_Init") void coop_on_file_select_init(GameState* thisx) {
    apply_config();
    update_banner();
    if (state != COOP_OFF) {
        recompui_show_context(banner_context);
    }
}

RECOMP_HOOK("FileSelect_Destroy") void coop_on_file_select_destroy(GameState* thisx) {
    recompui_hide_context(banner_context);
}

RECOMP_CALLBACK("*", recomp_on_init)
void coop_on_init() {
    conflicting_mod_enabled = recomp_is_dependency_met("mm_ultimate_6_save_slots") == DEPENDENCY_STATUS_FOUND ||
                              recomp_is_dependency_met("mm_ultimate_6_save_slots_cn") == DEPENDENCY_STATUS_FOUND;

    RecompuiColor banner_color = { 0, 0, 0, 170 };
    RecompuiColor text_color = { 255, 230, 140, 255 };

    banner_context = recompui_create_context();
    recompui_set_context_captures_input(banner_context, 0);
    recompui_set_context_captures_mouse(banner_context, 0);
    recompui_open_context(banner_context);

    // A fixed-width card in the top-right corner, clear of the file select menu.
    RecompuiResource root = recompui_context_root(banner_context);
    recompui_set_position(root, POSITION_ABSOLUTE);
    recompui_set_top(root, 32.0f, UNIT_DP);
    recompui_set_right(root, 32.0f, UNIT_DP);
    recompui_set_width(root, 420.0f, UNIT_DP);
    recompui_set_height_auto(root);

    RecompuiResource box = recompui_create_element(banner_context, root);
    recompui_set_display(box, DISPLAY_FLEX);
    recompui_set_flex_direction(box, FLEX_DIRECTION_COLUMN);
    recompui_set_gap(box, 4.0f, UNIT_DP);
    recompui_set_padding(box, 12.0f, UNIT_DP);
    recompui_set_border_radius(box, 8.0f, UNIT_DP);
    recompui_set_background_color(box, &banner_color);
    banner_title = recompui_create_label(banner_context, box, "", LABELSTYLE_NORMAL);
    recompui_set_color(banner_title, &text_color);
    banner_detail = recompui_create_label(banner_context, box, "", LABELSTYLE_SMALL);

    recompui_close_context(banner_context);
}
