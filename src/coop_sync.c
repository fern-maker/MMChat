#include "modding.h"
#include "global.h"
#include "recomputils.h"
#include "chat_native.h"
#include "chat_ui.h"
#include "coop.h"

// Co-op sync of shared progression (docs/coop-sync-spec.md).
//
// Every frame each shared value is compared with the group's merged value held by the native library:
// - anything we have that the group doesn't is reported (and reaches the server now or on reconnect);
// - anything the group has that we don't is written into our save, with a chat notice.
// Merge rules only ever add progression, so this can't take anything away.

// ---- Keys (never renumber: they are stored in the server's group files) ----

#define KEY_MASKS SYNC_KEY(SYNC_OP_OR, 0x001)

// ---- Masks ----

// Mask slots 0x18-0x2E in slot order; bit i of KEY_MASKS is slot 0x18 + i. Fierce Deity (slot 0x2F) is
// left out: the game can delete it on a cycle reset, and sharing would hand it straight back.
#define FIRST_MASK_SLOT SLOT_MASK_POSTMAN
#define SHARED_MASK_COUNT 23

static const u8 mask_items[SHARED_MASK_COUNT] = {
    ITEM_MASK_POSTMAN, ITEM_MASK_ALL_NIGHT, ITEM_MASK_BLAST, ITEM_MASK_STONE, ITEM_MASK_GREAT_FAIRY,
    ITEM_MASK_DEKU, ITEM_MASK_KEATON, ITEM_MASK_BREMEN, ITEM_MASK_BUNNY, ITEM_MASK_DON_GERO,
    ITEM_MASK_SCENTS, ITEM_MASK_GORON, ITEM_MASK_ROMANI, ITEM_MASK_CIRCUS_LEADER, ITEM_MASK_KAFEIS_MASK,
    ITEM_MASK_COUPLE, ITEM_MASK_TRUTH, ITEM_MASK_ZORA, ITEM_MASK_KAMARO, ITEM_MASK_GIBDO,
    ITEM_MASK_GARO, ITEM_MASK_CAPTAIN, ITEM_MASK_GIANT,
};

static const char* const mask_names[SHARED_MASK_COUNT] = {
    "Postman's Hat", "All-Night Mask", "Blast Mask", "Stone Mask", "Great Fairy's Mask",
    "Deku Mask", "Keaton Mask", "Bremen Mask", "Bunny Hood", "Don Gero's Mask",
    "Mask of Scents", "Goron Mask", "Romani's Mask", "Circus Leader's Mask", "Kafei's Mask",
    "Couple's Mask", "Mask of Truth", "Zora Mask", "Kamaro's Mask", "Gibdo Mask",
    "Garo's Mask", "Captain's Hat", "Giant's Mask",
};

#define SHARED_MASK_BITS ((1u << SHARED_MASK_COUNT) - 1)

static u32 read_masks(void) {
    u32 bits = 0;
    for (int i = 0; i < SHARED_MASK_COUNT; i++) {
        if (gSaveContext.save.saveInfo.inventory.items[FIRST_MASK_SLOT + i] != ITEM_NONE) {
            bits |= 1u << i;
        }
    }
    return bits;
}

// ---- Notices ----

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

// "* Co-op: Bob shared the Bunny Hood", or without a name when it came from the group's saved state.
static void notify_received(u32 key, const char* thing) {
    char who[64];
    char line[200];
    chat_sync_get_who(key, who, sizeof(who));
    copy_text(line, sizeof(line), "* Co-op: ");
    if (who[0] != '\0') {
        append_text(line, sizeof(line), who);
        append_text(line, sizeof(line), " shared ");
    }
    else {
        append_text(line, sizeof(line), "received ");
    }
    append_text(line, sizeof(line), thing);
    chat_ui_notify(line);
}

// ---- Per-category sync ----

static void sync_masks(bool can_receive) {
    u32 local = read_masks();
    u32 merged = chat_sync_get(KEY_MASKS);

    if (local & ~merged) {
        chat_sync_report(KEY_MASKS, local);
    }

    u32 missing = merged & ~local & SHARED_MASK_BITS;
    if (!can_receive || missing == 0) {
        return;
    }

    int received = 0;
    for (int i = 0; i < SHARED_MASK_COUNT; i++) {
        if (missing & (1u << i)) {
            gSaveContext.save.saveInfo.inventory.items[FIRST_MASK_SLOT + i] = mask_items[i];
            received++;
        }
    }
    // Catching up on many masks at once (e.g. joining a group) gets one line instead of a flood.
    if (received <= 3) {
        for (int i = 0; i < SHARED_MASK_COUNT; i++) {
            if (missing & (1u << i)) {
                char thing[64];
                copy_text(thing, sizeof(thing), "the ");
                append_text(thing, sizeof(thing), mask_names[i]);
                notify_received(KEY_MASKS, thing);
            }
        }
    }
    else {
        chat_ui_notify("* Co-op: received several masks from your group");
    }
}

void coop_sync_update(PlayState* play) {
    if (coop_get_state() != COOP_ACTIVE) {
        chat_coop_set_group("");
        return;
    }
    chat_coop_set_group(coop_get_save_name());

    // The title screen's attract demo also runs gameplay; only real play has a co-op save loaded.
    if (gSaveContext.gameMode != GAMEMODE_NORMAL) {
        return;
    }

    // Shared progression only arrives after the opening (learning the Song of Time), so a new player's
    // intro can't be broken by, say, receiving the Deku Mask before the curse. Our own progress is still
    // reported from the start.
    bool can_receive = CHECK_QUEST_ITEM(QUEST_SONG_TIME) != 0;

    sync_masks(can_receive);
}
