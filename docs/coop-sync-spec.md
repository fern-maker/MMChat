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
| **ADD** | Counters: the server keeps each player's contribution and the total is their sum. A player with more than the start value + total reports the difference as new contribution; one with less receives it. Contributions are sent as absolute values, so a resend can't double count |
| **Local** | Not shared |

## Shared fields

| Category | Field(s) | Rule | Notes |
|---|---|---|---|
| Masks | `inventory.items[24..46]` (mask slots) | OR (present / not present) | **Implemented** (key `0x1001`). Fierce Deity (slot 47) is excluded: the game can delete it on reset, and sharing would hand it straight back |
| Permanent items | `inventory.items[0..17]` except trade slots `SLOT_TRADE_DEED`, `SLOT_TRADE_KEY_MAMA`, `SLOT_TRADE_COUPLE` | OR (present / not present) | **Implemented** (key `0x1008`, bit = slot). Ocarina and magic beans also left out. Ammo stays local: a received bow / bomb bag comes full, a keg with 1, sticks / nuts / bombchus empty. An item Takkuri stole counts as owned |
| Bottles | number of slots in `SLOT_BOTTLE_1..6` that hold anything, plus bottles Takkuri stole | ADD (count, max 6) | **Implemented** (key `0x3002`). Changed from MAX, which undercounts when players get *different* bottles. A gained bottle is given to others **empty**. Contents are local |
| Songs, remains, notebook, quest bits | `inventory.questItems` bits 0x00–0x18 except `QUEST_PICTOGRAPH` (0x19) | OR | **Implemented** (key `0x1002`; Song of Time excluded, cursor/unused bits 0x04-0x05 and 0x13-0x17 excluded). Pictograph is cleared on reset; the heart-piece counter (0x1C+) has its own rule below |
| Upgrades | `inventory.upgrades` (quiver, bomb bag, wallet, …) | MAX per upgrade field | **Implemented** (keys `0x2101`-`0x2103`: quiver, bomb bag, wallet). Wallet size is shared; rupees themselves stay local |
| Gilded Sword | sword equip value | OR | **Implemented** (key `0x1009` bit 0). Put on the human form's B button whatever form you're in. A stolen Gilded Sword counts as owned. Razor Sword reverts on reset → local |
| Mirror Shield | shield equip value | OR | **Implemented** (key `0x1009` bit 1). Hero's Shield can be eaten by Like Likes → local |
| Heart containers | `playerData.healthCapacity` | ADD | Part of the heart-pieces row below. Key `0x2104` (capacity, MAX, v0.5.0-v0.6.0) is retired: it undercounted when players collected *different* pieces |
| Heart pieces | `questItems` heart-piece count (bits 0x1C+) | ADD | **Implemented** (key `0x3001`, counted in quarter hearts: containers × 4 + pieces, from 12 to 80). Works only because the pickup locations and reward flags are also shared, so no piece is collected twice. 4 pieces roll into a container as usual |
| Magic, defense | `isMagicAcquired`, `isDoubleMagicAcquired`, `magicLevel`, `doubleDefense` | MAX | **Implemented** (key `0x1007`, applied like the Great Fairy: flag + meter fill target). Great Fairy rewards |
| Owl statues | `playerData.owlActivationFlags` | OR | **Implemented** (key `0x1003`).  |
| Maps, compasses | `inventory.dungeonItems[i]` map and compass bits | OR | **Implemented** (key `0x1004`). Boss key bit is cleared on reset → local |
| World map | `scenesVisible[]`, `regionsVisited`, `worldMapCloudVisibility` | OR | **Implemented** (keys `0x1005`, `0x1006`, `0x1010`-`0x1016`, applied silently). Tingle maps and explored regions |
| Persistent event flags | `weekEventReg[]` bits marked persistent in `sPersistentCycleWeekEventRegs` | OR | **Implemented** (keys `0x1100`+word, 18 used) for a reviewed allow-list only (below), intersected at startup with the game's persistent table |
| Persistent scene flags | chest / switch / collectible bits marked in `sPersistentCycleSceneFlags[scene]` | OR | **Implemented** (keys `0x1200` + scene×4 + field, 63 used; rows built from the game's table at startup). Read from / written to `play->actorCtx.sceneFlags` for the scene you're in, `cycleSceneFlags[scene]` otherwise. Includes dungeon shortcut switches. Applied silently |

## Shared `weekEventReg` flags

Shared only if listed here **and** persistent (the mod checks `sPersistentCycleWeekEventRegs` at startup):

- Every named `WEEKEVENTREG_RECEIVED_*` flag that survives a reset (29): heart-piece rewards, bottles
  (Kotake, aliens, beaver race, Goron race), bank and Oceanside wallets, shooting-gallery quivers, Great
  Spin Attack.
- Unnamed reward checks, each read in its actor's code before a reward is offered: `26_40` Koume's boat
  archery HP, `50_02` / `50_04` Anju's Grandmother's HPs, `57_08` Madame Aroma's Chateau Romani bottle,
  `73_08` toilet hand HP, `81_08` postbox HP, `87_04` / `87_08` swamp tourist guide pictograph HPs.
- `WEEKEVENTREG_TINGLE_MAP_BOUGHT_*` (6).
- Bombers' Notebook entries: all of `weekEventReg[66..71]` and `[72] & 0x7F` (55). Only the notebook
  reads these.

Not shared: `ENTERED_*` (first-visit cutscenes), Tatl hints (`31_*`, `87_10`-`87_80`, used by `Elf_Msg6`),
player-actor flags (`30_*`), story moments (`24_80` Goron child talked to, `51_04` Kafei's bell,
`53_40` / `93_08` turtle, `59_04` clock tower door, `86_80` boss warp, `92_80`, `93_04` moon), and
persistent flags no code reads (`03_80`, `15_20`, `24_02`, `31_20`, `38_20`, `72_80`).

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

1. ~~**Persistent `weekEventReg` bits**~~ (resolved: allow-list above): some may mark story moments a player hasn't seen (e.g. an NPC
   conversation). Sharing them could skip scenes or confuse quests. Proposal: share only a reviewed
   allow-list — bombers' notebook entries plus flags that gate permanent item rewards — and grow it.
2. **Rewards gated by flags we don't share**: if Bob receives an item but the giver's flag isn't shared,
   Bob may be offered it again. Usually the game substitutes rupees, which is harmless; confirm case by case.
3. ~~**Heart piece ADD timing**~~ (resolved: the server keeps per-player contributions, reported as absolute values; a piece is only double counted if two players collect the *same* one before the flag reaches the other, e.g. while the server is down): a piece collected while two players are offline from each other must not be
   double counted on reconnect. The server needs per-player "pieces reported" totals, not just a sum.
4. ~~**Bottles**~~ (resolved: an empty bottle written into the first free bottle slot mid-game worked in v0.7.0 testing).
5. **New co-op save**: start from a fresh file (everyone at the beginning) or from the host's existing save?

## Out of scope (Level A)

Shared clock or cycle resets, cycle-only progress, enemies / NPCs / actor sync, shared rupees or ammo.
