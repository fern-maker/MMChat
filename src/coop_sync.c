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
//
// Each shared value is a row in `categories` below: a key, how to read it from the save, and how to
// write a merged value back.

#define SAVE_INFO (gSaveContext.save.saveInfo)

// ---- Keys (never renumber: they are stored in the server's group files) ----

#define KEY_MASKS SYNC_KEY(SYNC_OP_OR, 0x001)
#define KEY_QUEST SYNC_KEY(SYNC_OP_OR, 0x002)
#define KEY_OWLS SYNC_KEY(SYNC_OP_OR, 0x003)
#define KEY_DUNGEON_MAPS SYNC_KEY(SYNC_OP_OR, 0x004)
#define KEY_REGIONS SYNC_KEY(SYNC_OP_OR, 0x005)
#define KEY_CLOUDS SYNC_KEY(SYNC_OP_OR, 0x006)
#define KEY_ABILITIES SYNC_KEY(SYNC_OP_OR, 0x007)
#define KEY_SCENES_VISIBLE(i) SYNC_KEY(SYNC_OP_OR, 0x010 + (i)) // 0x010-0x016
#define KEY_QUIVER SYNC_KEY(SYNC_OP_MAX, 0x101)
#define KEY_BOMB_BAG SYNC_KEY(SYNC_OP_MAX, 0x102)
#define KEY_WALLET SYNC_KEY(SYNC_OP_MAX, 0x103)
#define KEY_HEART_CAPACITY SYNC_KEY(SYNC_OP_MAX, 0x104)

// ---- Masks ----

// Mask slots 0x18-0x2E in slot order; bit i is slot 0x18 + i. Fierce Deity (slot 0x2F) is left out: the
// game can delete it on a cycle reset, and sharing would hand it straight back.
#define FIRST_MASK_SLOT SLOT_MASK_POSTMAN
#define SHARED_MASK_COUNT 23

static const u8 mask_items[SHARED_MASK_COUNT] = {
    ITEM_MASK_POSTMAN, ITEM_MASK_ALL_NIGHT, ITEM_MASK_BLAST, ITEM_MASK_STONE, ITEM_MASK_GREAT_FAIRY,
    ITEM_MASK_DEKU, ITEM_MASK_KEATON, ITEM_MASK_BREMEN, ITEM_MASK_BUNNY, ITEM_MASK_DON_GERO,
    ITEM_MASK_SCENTS, ITEM_MASK_GORON, ITEM_MASK_ROMANI, ITEM_MASK_CIRCUS_LEADER, ITEM_MASK_KAFEIS_MASK,
    ITEM_MASK_COUPLE, ITEM_MASK_TRUTH, ITEM_MASK_ZORA, ITEM_MASK_KAMARO, ITEM_MASK_GIBDO,
    ITEM_MASK_GARO, ITEM_MASK_CAPTAIN, ITEM_MASK_GIANT,
};

static const char* const mask_names[32] = {
    "the Postman's Hat", "the All-Night Mask", "the Blast Mask", "the Stone Mask", "the Great Fairy's Mask",
    "the Deku Mask", "the Keaton Mask", "the Bremen Mask", "the Bunny Hood", "Don Gero's Mask",
    "the Mask of Scents", "the Goron Mask", "Romani's Mask", "the Circus Leader's Mask", "Kafei's Mask",
    "the Couple's Mask", "the Mask of Truth", "the Zora Mask", "Kamaro's Mask", "the Gibdo Mask",
    "Garo's Mask", "the Captain's Hat", "the Giant's Mask",
};

static u32 read_masks(void) {
    u32 bits = 0;
    for (int i = 0; i < SHARED_MASK_COUNT; i++) {
        if (SAVE_INFO.inventory.items[FIRST_MASK_SLOT + i] != ITEM_NONE) {
            bits |= 1u << i;
        }
    }
    return bits;
}

static void apply_masks(PlayState* play, u32 missing) {
    for (int i = 0; i < SHARED_MASK_COUNT; i++) {
        if (missing & (1u << i)) {
            SAVE_INFO.inventory.items[FIRST_MASK_SLOT + i] = mask_items[i];
        }
    }
}

// ---- Quest status: remains, songs, notebook ----

// Song of Time is left out: it marks the end of the opening (see the receive gate below) and must be
// learned in-game. Bits 0x04-0x05 and 0x13-0x17 are cursor positions or unused, and the pictograph
// (0x19) is cleared every cycle.
#define QUEST_BIT(q) (1u << (q))
#define SHARED_QUEST_BITS                                                                                   \
    (QUEST_BIT(QUEST_REMAINS_ODOLWA) | QUEST_BIT(QUEST_REMAINS_GOHT) | QUEST_BIT(QUEST_REMAINS_GYORG) |     \
     QUEST_BIT(QUEST_REMAINS_TWINMOLD) | QUEST_BIT(QUEST_SONG_SONATA) | QUEST_BIT(QUEST_SONG_LULLABY) |     \
     QUEST_BIT(QUEST_SONG_BOSSA_NOVA) | QUEST_BIT(QUEST_SONG_ELEGY) | QUEST_BIT(QUEST_SONG_OATH) |         \
     QUEST_BIT(QUEST_SONG_SARIA) | QUEST_BIT(QUEST_SONG_HEALING) | QUEST_BIT(QUEST_SONG_EPONA) |           \
     QUEST_BIT(QUEST_SONG_SOARING) | QUEST_BIT(QUEST_SONG_STORMS) | QUEST_BIT(QUEST_SONG_SUN) |            \
     QUEST_BIT(QUEST_BOMBERS_NOTEBOOK) | QUEST_BIT(QUEST_SONG_LULLABY_INTRO))

static const char* const quest_names[32] = {
    [QUEST_REMAINS_ODOLWA] = "Odolwa's Remains",
    [QUEST_REMAINS_GOHT] = "Goht's Remains",
    [QUEST_REMAINS_GYORG] = "Gyorg's Remains",
    [QUEST_REMAINS_TWINMOLD] = "Twinmold's Remains",
    [QUEST_SONG_SONATA] = "the Sonata of Awakening",
    [QUEST_SONG_LULLABY] = "the Goron Lullaby",
    [QUEST_SONG_BOSSA_NOVA] = "the New Wave Bossa Nova",
    [QUEST_SONG_ELEGY] = "the Elegy of Emptiness",
    [QUEST_SONG_OATH] = "the Oath to Order",
    [QUEST_SONG_SARIA] = "Saria's Song",
    [QUEST_SONG_HEALING] = "the Song of Healing",
    [QUEST_SONG_EPONA] = "Epona's Song",
    [QUEST_SONG_SOARING] = "the Song of Soaring",
    [QUEST_SONG_STORMS] = "the Song of Storms",
    [QUEST_SONG_SUN] = "the Sun's Song",
    [QUEST_BOMBERS_NOTEBOOK] = "the Bombers' Notebook",
    [QUEST_SONG_LULLABY_INTRO] = "the Lullaby Intro",
};

static u32 read_quest(void) {
    return SAVE_INFO.inventory.questItems;
}

static void apply_quest(PlayState* play, u32 missing) {
    SAVE_INFO.inventory.questItems |= missing;
}

// ---- Owl statues ----

static u32 read_owls(void) {
    return SAVE_INFO.playerData.owlActivationFlags;
}

static void apply_owls(PlayState* play, u32 missing) {
    SAVE_INFO.playerData.owlActivationFlags |= (u16)missing;
}

// ---- Dungeon maps and compasses ----

// Bit 2*i is dungeon i's compass, bit 2*i+1 its map. Boss keys reset every cycle, so they stay local.
#define DUNGEON_ITEM_SLOTS 10

static u32 read_dungeon_maps(void) {
    u32 bits = 0;
    for (int i = 0; i < DUNGEON_ITEM_SLOTS; i++) {
        u8 items = SAVE_INFO.inventory.dungeonItems[i];
        if (items & (1 << DUNGEON_COMPASS)) {
            bits |= 1u << (2 * i);
        }
        if (items & (1 << DUNGEON_MAP)) {
            bits |= 1u << (2 * i + 1);
        }
    }
    return bits;
}

static void apply_dungeon_maps(PlayState* play, u32 missing) {
    for (int i = 0; i < DUNGEON_ITEM_SLOTS; i++) {
        if (missing & (1u << (2 * i))) {
            SAVE_INFO.inventory.dungeonItems[i] |= 1 << DUNGEON_COMPASS;
        }
        if (missing & (1u << (2 * i + 1))) {
            SAVE_INFO.inventory.dungeonItems[i] |= 1 << DUNGEON_MAP;
        }
    }
}

// ---- World map (Tingle maps, explored regions, cleared clouds) ----

static u32 read_regions(void) {
    return SAVE_INFO.regionsVisited;
}

static void apply_regions(PlayState* play, u32 missing) {
    SAVE_INFO.regionsVisited |= missing;
}

static u32 read_clouds(void) {
    return SAVE_INFO.worldMapCloudVisibility;
}

static void apply_clouds(PlayState* play, u32 missing) {
    SAVE_INFO.worldMapCloudVisibility |= missing;
}

#define SCENES_VISIBLE_FUNCS(i)                                     \
    static u32 read_scenes_visible_##i(void) {                      \
        return SAVE_INFO.scenesVisible[i];                          \
    }                                                               \
    static void apply_scenes_visible_##i(PlayState* play, u32 missing) { \
        SAVE_INFO.scenesVisible[i] |= missing;                      \
    }

SCENES_VISIBLE_FUNCS(0)
SCENES_VISIBLE_FUNCS(1)
SCENES_VISIBLE_FUNCS(2)
SCENES_VISIBLE_FUNCS(3)
SCENES_VISIBLE_FUNCS(4)
SCENES_VISIBLE_FUNCS(5)
SCENES_VISIBLE_FUNCS(6)

// ---- Great Fairy rewards: magic, double magic, double defense ----

#define ABILITY_MAGIC (1u << 0)
#define ABILITY_DOUBLE_MAGIC (1u << 1)
#define ABILITY_DOUBLE_DEFENSE (1u << 2)

static const char* const ability_names[32] = {
    "the magic meter",
    "double magic",
    "double defense",
};

static u32 read_abilities(void) {
    u32 bits = 0;
    if (SAVE_INFO.playerData.isMagicAcquired) {
        bits |= ABILITY_MAGIC;
    }
    if (SAVE_INFO.playerData.isDoubleMagicAcquired) {
        bits |= ABILITY_DOUBLE_MAGIC;
    }
    if (SAVE_INFO.playerData.doubleDefense) {
        bits |= ABILITY_DOUBLE_DEFENSE;
    }
    return bits;
}

// Mirrors the Great Fairy: set the flag and a fill target; the HUD then grows the meter on its own.
static void apply_abilities(PlayState* play, u32 missing) {
    if (missing & (ABILITY_MAGIC | ABILITY_DOUBLE_MAGIC)) {
        SAVE_INFO.playerData.isMagicAcquired = true;
        if (missing & ABILITY_DOUBLE_MAGIC) {
            SAVE_INFO.playerData.isDoubleMagicAcquired = true;
        }
        SAVE_INFO.playerData.magicLevel = 0;
        gSaveContext.magicFillTarget =
            SAVE_INFO.playerData.isDoubleMagicAcquired ? MAGIC_DOUBLE_METER : MAGIC_NORMAL_METER;
    }
    if (missing & ABILITY_DOUBLE_DEFENSE) {
        SAVE_INFO.playerData.doubleDefense = true;
    }
}

// ---- Upgrades and heart containers (MAX) ----

static u32 read_quiver(void) {
    return GET_CUR_UPG_VALUE(UPG_QUIVER);
}

static void apply_quiver(PlayState* play, u32 value) {
    Inventory_ChangeUpgrade(UPG_QUIVER, value);
}

static u32 read_bomb_bag(void) {
    return GET_CUR_UPG_VALUE(UPG_BOMB_BAG);
}

static void apply_bomb_bag(PlayState* play, u32 value) {
    Inventory_ChangeUpgrade(UPG_BOMB_BAG, value);
}

static u32 read_wallet(void) {
    return GET_CUR_UPG_VALUE(UPG_WALLET);
}

static void apply_wallet(PlayState* play, u32 value) {
    Inventory_ChangeUpgrade(UPG_WALLET, value);
}

static u32 read_heart_capacity(void) {
    return (u32)SAVE_INFO.playerData.healthCapacity;
}

// Like a heart container: raise the maximum and fill the new hearts.
static void apply_heart_capacity(PlayState* play, u32 value) {
    s16 gained = (s16)value - SAVE_INFO.playerData.healthCapacity;
    SAVE_INFO.playerData.healthCapacity = (s16)value;
    SAVE_INFO.playerData.health += gained;
}

// ---- Category table ----

typedef struct {
    u32 key;
    u32 shared_bits; // OR keys: which bits are shared. MAX keys: 0.
    u32 max_value;   // MAX keys: the largest valid value, so a bad server value can't corrupt the save.
    u32 (*read)(void);
    // OR keys get the missing bits; MAX keys get the new value.
    void (*apply)(PlayState* play, u32 value);
    const char* const* bit_names; // OR keys: per-bit notice text; NULL entries fall back to `label`.
    const char* label;
} SyncCategory;

static const SyncCategory categories[] = {
    { KEY_MASKS, (1u << SHARED_MASK_COUNT) - 1, 0, read_masks, apply_masks, mask_names, "a mask" },
    { KEY_QUEST, SHARED_QUEST_BITS, 0, read_quest, apply_quest, quest_names, "quest progress" },
    { KEY_OWLS, 0xFFFF, 0, read_owls, apply_owls, NULL, "an owl statue" },
    { KEY_DUNGEON_MAPS, (1u << (2 * DUNGEON_ITEM_SLOTS)) - 1, 0, read_dungeon_maps, apply_dungeon_maps, NULL,
      "a dungeon map or compass" },
    { KEY_REGIONS, 0xFFFFFFFF, 0, read_regions, apply_regions, NULL, NULL },
    { KEY_CLOUDS, 0xFFFFFFFF, 0, read_clouds, apply_clouds, NULL, NULL },
    { KEY_SCENES_VISIBLE(0), 0xFFFFFFFF, 0, read_scenes_visible_0, apply_scenes_visible_0, NULL, NULL },
    { KEY_SCENES_VISIBLE(1), 0xFFFFFFFF, 0, read_scenes_visible_1, apply_scenes_visible_1, NULL, NULL },
    { KEY_SCENES_VISIBLE(2), 0xFFFFFFFF, 0, read_scenes_visible_2, apply_scenes_visible_2, NULL, NULL },
    { KEY_SCENES_VISIBLE(3), 0xFFFFFFFF, 0, read_scenes_visible_3, apply_scenes_visible_3, NULL, NULL },
    { KEY_SCENES_VISIBLE(4), 0xFFFFFFFF, 0, read_scenes_visible_4, apply_scenes_visible_4, NULL, NULL },
    { KEY_SCENES_VISIBLE(5), 0xFFFFFFFF, 0, read_scenes_visible_5, apply_scenes_visible_5, NULL, NULL },
    { KEY_SCENES_VISIBLE(6), 0xFFFFFFFF, 0, read_scenes_visible_6, apply_scenes_visible_6, NULL, NULL },
    { KEY_ABILITIES, ABILITY_MAGIC | ABILITY_DOUBLE_MAGIC | ABILITY_DOUBLE_DEFENSE, 0, read_abilities,
      apply_abilities, ability_names, NULL },
    { KEY_QUIVER, 0, 3, read_quiver, apply_quiver, NULL, "a bigger quiver" },
    { KEY_BOMB_BAG, 0, 3, read_bomb_bag, apply_bomb_bag, NULL, "a bigger bomb bag" },
    { KEY_WALLET, 0, 2, read_wallet, apply_wallet, NULL, "a bigger wallet" },
    { KEY_HEART_CAPACITY, 0, 20 * 0x10, read_heart_capacity, apply_heart_capacity, NULL, "a heart container" },
};

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

// Notices are collected per frame so catching up on lots of progress (e.g. joining a group) gives one
// summary line instead of a flood.
#define MAX_FRAME_NOTICES 4

static char frame_notices[MAX_FRAME_NOTICES][160];
static int frame_notice_count;

static void queue_notice(u32 key, const char* thing) {
    if (thing == NULL) {
        return; // Map exploration etc.: applied silently.
    }
    if (frame_notice_count < MAX_FRAME_NOTICES) {
        char who[64];
        char* line = frame_notices[frame_notice_count];
        chat_sync_get_who(key, who, sizeof(who));
        copy_text(line, 160, "* Co-op: ");
        if (who[0] != '\0') {
            append_text(line, 160, who);
            append_text(line, 160, " shared ");
        }
        else {
            append_text(line, 160, "received ");
        }
        append_text(line, 160, thing);
    }
    frame_notice_count++;
}

static void flush_notices(void) {
    if (frame_notice_count > MAX_FRAME_NOTICES) {
        chat_ui_notify("* Co-op: caught up on progress from your group");
    }
    else {
        for (int i = 0; i < frame_notice_count; i++) {
            chat_ui_notify(frame_notices[i]);
        }
    }
    frame_notice_count = 0;
}

// ---- Sync loop ----

static void sync_category(PlayState* play, const SyncCategory* cat, bool can_receive) {
    u32 local = cat->read();
    u32 merged = chat_sync_get(cat->key);

    if (cat->shared_bits != 0) {
        local &= cat->shared_bits;
        if (local & ~merged) {
            chat_sync_report(cat->key, local);
        }
        u32 missing = merged & ~local & cat->shared_bits;
        if (!can_receive || missing == 0) {
            return;
        }
        cat->apply(play, missing);
        for (int bit = 0; bit < 32; bit++) {
            if (missing & (1u << bit)) {
                const char* name = cat->bit_names != NULL ? cat->bit_names[bit] : NULL;
                queue_notice(cat->key, name != NULL ? name : cat->label);
                if (name == NULL) {
                    break; // One generic notice per category is enough.
                }
            }
        }
    }
    else {
        if (local > merged) {
            chat_sync_report(cat->key, local);
        }
        if (!can_receive || merged <= local || merged > cat->max_value) {
            return;
        }
        cat->apply(play, merged);
        queue_notice(cat->key, cat->label);
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

    for (u32 i = 0; i < ARRAY_COUNT(categories); i++) {
        sync_category(play, &categories[i], can_receive);
    }
    flush_notices();
}
