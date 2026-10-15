#ifndef __COOP_H__
#define __COOP_H__

#include "global.h"

typedef enum {
    COOP_OFF,          // Normal saves in use.
    COOP_ACTIVE,       // The co-op save file is in use; progression may sync.
    COOP_BLOCKED,      // Co-op was requested but another mod that switches save files is enabled.
} CoopState;

CoopState coop_get_state(void);

// The active co-op save name (also the sync group name), or "" when co-op isn't active.
const char* coop_get_save_name(void);

// Runs every gameplay frame from the Play_UpdateMain hook: reports and applies shared progression.
void coop_sync_update(PlayState* play);

// Writes a one-line description of the co-op state for the UI.
void coop_describe(char* out, u32 out_len);

#endif
