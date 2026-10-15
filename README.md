# MMChat

Text chat between friends for Majora's Mask: Recompiled (1.2.2+). One player hosts a small relay server; everyone's game connects to it. Hold L and press R in-game to open the chat window.

Player-facing instructions are in `FRIENDS_README.txt` (shipped as `README.txt` in the dist zip).

## Layout

- `src/` — mod code (MIPS), built against the decomp. `chat_ui.c` is the chat window and popup; `chat_native.h` declares the native imports.
- `native/` — native library loaded by the game next to the `.nrm`. `chat_client.c` is the portable network client (Winsock / BSD sockets, runs on a background thread); `chat_native.c` wraps it in the recomp ABI.
- `server/` — `mmchat_server.py` relay server and `mmchat_client.py` command-line test client (Python 3.8+, stdlib only).

## Building

```bash
git clone --recurse-submodules <repo-url>
make            # mod.elf + macOS dylib (universal)
make package    # also builds build/mm_chat.nrm
make install    # copies .nrm + dylib into the macOS mods folder
make windows    # Windows DLL via mingw-w64 (brew install mingw-w64)
make dist       # build/MMChat.zip for friends: both platforms + server
```

Requires Homebrew `llvm` and `lld` (Apple Clang can't target MIPS), `zip`, and `RecompModTool` from N64Recomp on `PATH`.

## Native library ABI

Each export has the signature `void f(uint8_t* rdram, recomp_context* ctx)`: arguments in `r4`-`r7`, return value in `r2`, game bytes at `(vaddr ^ 3) - 0xFFFFFFFF80000000`. The library must export `recomp_api_version = 1`, and every function must be listed under `native_libraries` in `mod.toml`.

## Co-op sync

Spec: `docs/coop-sync-spec.md`. `src/coop.c` switches to the co-op save file; `src/coop_sync.c` compares each shared value with the group's merged value (cached by the native client) every frame, reports local gains and applies missing ones. Keys carry their merge rule in bits 12-15 (1 = OR, 2 = MAX) so the server stays game-agnostic; never renumber keys, since they're stored in the server's `coop_state/<group>.json`. Incoming progression is applied only after the player has learned the Song of Time, and never while paused. The reward-flag and scene-flag rows are generated at startup from the game's own persistent-flag tables (`sPersistentCycleWeekEventRegs`, `sPersistentCycleSceneFlags`).

## Protocol (v3)

UTF-8 lines ending in `\n`, tab-separated fields, max 512 bytes from clients. See the docstring in `server/mmchat_server.py`. Bump `PROTOCOL_VERSION` in the server and `MMCHAT_PROTOCOL_VERSION` in `native/chat_client.h` together on incompatible changes.

## Presence

The mod reports `play->sceneId` and the current day every frame (scene -1 outside normal gameplay); the native client sends `WHERE` only on change and after each `WELCOME`. Scene names come from `native/scene_names.h`, generated from the decomp by `tools/gen_scene_names.py` (rerun it if the `mm-decomp` submodule is updated).
