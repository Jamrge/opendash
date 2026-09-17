#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wav_player.h"
#include "main.h"

SFXPool sfx_pool = {0};

void init_sfx_pool(void) {
    if (!gd_mixer) return;

    for (int i = 0; i < SFX_POOL_SIZE; i++) {
        sfx_pool.tracks[i] = MIX_CreateTrack(gd_mixer);
        if (sfx_pool.tracks[i]) {
            MIX_TagTrack(sfx_pool.tracks[i], "sfx");
        }
    }
    sfx_pool.next = 0;
}

bool load_wav(const char* path, SFX* sfx) {
    if (!gd_mixer) return false;

    char translated[1024];
    gd_translate_path(path, translated, sizeof(translated));

    sfx->audio = MIX_LoadAudio(gd_mixer, translated, true);
    return sfx->audio != NULL;
}

void play_sfx(SFX* sfx, int channel) {
    (void)channel;
    if (!sfx->audio || !gd_mixer) return;

    MIX_Track* track = sfx_pool.tracks[sfx_pool.next];

    if (track) {
        MIX_StopTrack(track, 0);
        MIX_SetTrackAudio(track, sfx->audio);
        MIX_PlayTrack(track, 0);
    }

    sfx_pool.next = (sfx_pool.next + 1) % SFX_POOL_SIZE;
}
