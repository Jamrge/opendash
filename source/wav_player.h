#pragma once
#include <3ds.h>

#define SFX_POOL_SIZE 8

typedef struct {
    MIX_Audio* audio;
} SFX;

typedef struct {
    MIX_Track* tracks[SFX_POOL_SIZE];
    int next;
} SFXPool;

bool load_wav(const char* path, SFX* sfx);
void init_sfx_pool(void);
void play_sfx(SFX* sfx, int channel);
