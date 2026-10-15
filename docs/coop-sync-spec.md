# Co-op sync spec (Level A: shared permanent progression)

Status: **draft for review**. Field names refer to `mm-decomp/include/z64save.h`; "survives reset" is
determined by `Sram_SaveEndOfCycle` in `mm-decomp/src/code/z_sram_NES.c`.

## Model

- Everyone keeps their **own clock, day and cycle**. Songs of Time, Double Time and Inverted Time are local.
- Only progression that **survives a Song of Time reset** is shared. Anything the reset wipes stays per player.
- Rupees, ammo and bottle contents are **per player** (decided).
- Co-op play uses a **separate save file** (`recomp_change_save_file`), so normal saves are never touched.
- The server keeps the merged shared state; a joining player receives it immediately.

## Receiving starts after the opening

Incoming progression is applied only once the player has learned the Song of Time. Before that, a new
player could receive e.g. the Deku Mask ahead of the Skull Kid's curse and break the intro. Their own
progress is still reported from the start.

## How syncing works

Each frame the mod compares the shared fields against the last snapshot. Local gains are sent to the
server; the server merges and broadcasts; incoming gains are written into the save and announced in chat
("Bob got the Bunny Hood"). Merge rules only ever **add** progression, so two players can't conflict and
nobody can lose anything through sync.

Merge rules used below:

| Rule | Meaning |
|---|---|
| **OR** | Bit flags: a bit set by anyone is set for everyone |
| **MAX** | Ordered levels: everyone gets the highest anyone has |
| **ADD** | Counters: each local increase is added to everyone's total (see heart pieces) |
| **Local** | Not shared |

## Shared fields

| Category | Field(s) | Rule | Notes |
|---|---|---|---|
| Masks | `inventory.items[24..46]` (mask slots) | OR (present / not present) | **Implemented** (key `0x1001`). Fierce Deity (slot 47) is excluded: the game can delete it on reset, and sharing would hand it straight back |
| Permanent items | `inventory.items[0..17]` except trade slots `SLOT_TRADE_DEED`, `SLOT_TRADE_KEY_MAMA`, `SLOT_TRADE_COUPLE` | OR (present / not present) | Bow, bombs, hookshot, lens, powder keg, pictograph box, Great Fairy sword… Ammo counts stay local |
| Bottles | number of slots in `SLOT_BOTTLE_1..6` that hold anything | MAX (count) | A gained bottle is given to others **empty**. Contents are local |
| Songs, remains, notebook, quest bits | `inventory.questItems` bits 0x00–0x18 except `QUEST_PICTOGRAPH` (0x19) | OR | Pictograph is cleared on reset; the heart-piece counter (0x1C+) has its own rule below |
| Upgrades | `inventory.upgrades` (quiver, bomb bag, wallet, …) | MAX per upgrade field | Wallet size is shared; rupees themselves stay local |
| Gilded Sword | sword equip value | MAX, only at `EQUIP_VALUE_SWORD_GILDED` | Razor Sword reverts on reset → local |
| Mirror Shield | shield equip value | MAX, only at `EQUIP_VALUE_SHIELD_MIRROR` | Hero's Shield can be eaten by Like Likes → local |
| Heart containers | `playerData.healthCapacity` | MAX | |
| Heart pieces | `questItems` heart-piece count (bits 0x1C+) | ADD | Works only because the pickup locations are also shared (below), so no piece is collected twice. 4 pieces roll into a container as usual |
| Magic, defense | `isMagicAcquired`, `isDoubleMagicAcquired`, `magicLevel`, `doubleDefense` | MAX | Great Fairy rewards |
| Owl statues | `playerData.owlActivationFlags` | OR | |
| Maps, compasses | `inventory.dungeonItems[i]` map and compass bits | OR | Boss key bit is cleared on reset → local |
| World map | `scenesVisible[]`, `regionsVisited`, `worldMapCloudVisibility` | OR | Tingle maps and explored regions |
| Persistent event flags | `weekEventReg[]` bits marked persistent in `sPersistentCycleWeekEventRegs` | OR | **Review needed**, see open questions |
| Persistent scene flags | chest / switch / collectible bits marked in `sPersistentCycleSceneFlags[scene]` | OR | Stored in `cycleSceneFlags[scene]`, **and** in `play->actorCtx.sceneFlags` for the scene you're in |

## Per player (not shared)

Rupees, ammo, bottle contents, current health and magic, small keys, boss keys, stray fairies, trade items,
Razor Sword, Hero's Shield, Skulltula tokens, non-persistent event and scene flags, time / day / cycle,
equipped C-buttons and form, minigame scores, Bombers' code, lottery codes, horse data, scarecrow song.

## Duplicate pickups

If Alice collects a heart piece and Bob receives it through sync, Bob could still walk to the same spot and
collect it again. Sharing the **persistent scene and event flags** marks the spot as taken for everyone.
This is why those flags are in scope even though they aren't items. Locations whose flags are *not*
persistent (rare for permanent rewards) would allow a second copy; those get found during playtesting.

## Live updates

- Inventory, quest items, upgrades and maps are read straight from the save, so they update immediately.
- Scene flags for the scene you're standing in must be written to `play->actorCtx.sceneFlags` as well,
  but already-spawned objects (an open chest in front of you) may not visually change until you re-enter.
- Getting an item through sync skips the "You got X!" cutscene; a chat notice replaces it.

## Open questions (to verify in code or in-game)

1. **Persistent `weekEventReg` bits**: some may mark story moments a player hasn't seen (e.g. an NPC
   conversation). Sharing them could skip scenes or confuse quests. Proposal: share only a reviewed
   allow-list — bombers' notebook entries plus flags that gate permanent item rewards — and grow it.
2. **Rewards gated by flags we don't share**: if Bob receives an item but the giver's flag isn't shared,
   Bob may be offered it again. Usually the game substitutes rupees, which is harmless; confirm case by case.
3. **Heart piece ADD timing**: a piece collected while two players are offline from each other must not be
   double counted on reconnect. The server needs per-player "pieces reported" totals, not just a sum.
4. **Bottles**: confirm giving an empty bottle into the first free bottle slot is safe mid-game.
5. **New co-op save**: start from a fresh file (everyone at the beginning) or from the host's existing save?

## Out of scope (Level A)

Shared clock or cycle resets, cycle-only progress, enemies / NPCs / actor sync, shared rupees or ammo.
