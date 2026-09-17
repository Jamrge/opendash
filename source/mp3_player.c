#include <3ds.h>
#include <stdatomic.h>
#include "mp3_player.h"
#include "level_loading.h"
#include "math_helpers.h"
#include <stdio.h>
#include <string.h>
#include "main.h"
#include <unistd.h>
#include "menus/settings.h"
#include "state.h"

#define MUSIC_CHANNEL 0

MIX_Mixer* gd_mixer = NULL;
MIX_Track* gd_music_track = NULL;
static MIX_Audio* current_music = NULL;

static volatile bool quit = false;
static volatile bool looping = false;
static volatile bool paused = false;

static _Atomic float amplitude_atomic = 0.0f;

static float current_music_rate = 0;
static int   current_music_channels = 0;

static float calculate_amplitude(float power);
static float calculate_power(int16_t *samples, size_t frames, int channels);

static void gd_amplitude_callback(void* userdata, MIX_Track* track,
                                   const SDL_AudioSpec* spec, float* pcm, int samples)
{
    (void)userdata; (void)track;

    int ch = spec->channels;
    if (ch <= 0) ch = 2;
    int frames = samples / ch;
    if (frames <= 0) return;

    int16_t buf[2048];
    int copy = frames * ch;
    if (copy > 2048) copy = 2048;

    for (int i = 0; i < copy; i++) {
        float v = pcm[i] * 32767.f;
        if (v > 32767.f) v = 32767.f;
        if (v < -32768.f) v = -32768.f;
        buf[i] = (int16_t)v;
    }

    float rms = calculate_power(buf, frames, ch);
    atomic_store_explicit(&amplitude_atomic, calculate_amplitude(rms), memory_order_relaxed);
}

static void gd_track_stopped(void* userdata, MIX_Track* track)
{
    (void)userdata; (void)track;
    /* Music ended naturally — let the game detect via amplitude decay */
}

void ensure_mixer(void)
{
    if (gd_mixer) return;

    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;

    MIX_Init();

    gd_mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
    if (!gd_mixer) return;

    gd_music_track = MIX_CreateTrack(gd_mixer);
    if (gd_music_track) {
        MIX_SetTrackStoppedCallback(gd_music_track, gd_track_stopped, NULL);
        MIX_SetTrackRawCallback(gd_music_track, gd_amplitude_callback, NULL);
    }
}

void audio_init(void) {
    ensure_mixer();
}

void audio_exit(void) {
    /* SDL3_mixer manages its own resources; we just clear our references */
}

u32 decode_mp3(void* buffer) {
    (void)buffer;
    return 0;
}

float calculate_amplitude(float power) {
    if (state.practice_mode && !settingsState.practiceMusicSync) return 0.f;
    if (game_state == STATE_MAIN_MENU) return 0.5f;

    static float prev = 0.0f;
    static float pulse = 0.0f;
    static float avg_delta = 0.0f;
    static float prev_power = 0.0f;

    float delta = power - prev;
    float abs_delta = fabsf(delta);

    avg_delta = avg_delta + ((abs_delta - avg_delta) * 0.1f);

    float rms_delta = power - prev_power;
    float abs_rms_delta = fabsf(rms_delta);
    float rms_thresh = avg_delta * POWER_THRESH_MULTIPLIER;

    if (abs_rms_delta > rms_thresh && rms_delta > 0.f) {
        pulse = AMP_MAX;
    }

    pulse *= AMP_I_DECAY;

    if (pulse > AMP_MAX) pulse = AMP_MAX;
    if (pulse < AMP_MIN) pulse = AMP_MIN;

    prev = power;
    prev_power = power;

    return pulse;
}

float calculate_power(int16_t *samples, size_t frames, int channels) {
    uint64_t sum_squares = 0;
    uint32_t count = 0;

    for (size_t i = 0; i < frames; i++) {
        int16_t mono;

        if (channels == 2) {
            int16_t left  = samples[i*2];
            int16_t right = samples[i*2 + 1];
            mono = (left + right) / 2;
        } else {
            mono = samples[i];
        }

        sum_squares += mono * mono;
        count++;
    }

    if (count == 0) return 0.f;

    return ((float)sum_squares / count) / (32768.f * 32768.f);
}

int play_mp3(char *path, bool loop, float seek) {
    if (access(path, F_OK) != 0) return 0;

    ensure_mixer();
    if (!gd_mixer) return 0;

    stop_mp3();

    char translated[1024];
    gd_translate_path(path, translated, sizeof(translated));

    current_music = MIX_LoadAudio(gd_mixer, translated, false);
    if (!current_music) return 0;

    SDL_AudioSpec fmt;
    if (MIX_GetAudioFormat(current_music, &fmt)) {
        current_music_rate = (float)fmt.freq;
        current_music_channels = fmt.channels;
    } else {
        current_music_rate = 44100;
        current_music_channels = 2;
    }

    quit = false;
    looping = loop;
    paused = false;

    if (!gd_music_track) {
        gd_music_track = MIX_CreateTrack(gd_mixer);
        if (gd_music_track)
            MIX_SetTrackStoppedCallback(gd_music_track, gd_track_stopped, NULL);
    }

    MIX_SetTrackAudio(gd_music_track, current_music);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);

    if (seek > 0 && current_music_rate > 0) {
        Sint64 start_frame = (Sint64)(seek * current_music_rate);
        SDL_SetNumberProperty(props, MIX_PROP_PLAY_START_FRAME_NUMBER, start_frame);
    }

    MIX_PlayTrack(gd_music_track, props);
    SDL_DestroyProperties(props);

    if (loop && seek > 0) {
        MIX_SetTrackLoops(gd_music_track, -1);
    }

    return 1;
}

void seek_mp3(float time) {
    if (time < 0) time = 0;
    if (!gd_music_track || !current_music) return;

    if (current_music_rate <= 0.f) return;

    Sint64 frame = (Sint64)(time * current_music_rate);
    MIX_SetTrackPlaybackPosition(gd_music_track, frame);
}

void toggle_playback_mp3(void) {
    if (!gd_music_track) return;
    if (MIX_TrackPaused(gd_music_track)) {
        MIX_ResumeTrack(gd_music_track);
        paused = false;
    } else {
        MIX_PauseTrack(gd_music_track);
        paused = true;
    }
}

void pause_playback_mp3(void) {
    if (!gd_music_track) return;
    MIX_PauseTrack(gd_music_track);
    paused = true;
}

void unpause_playback_mp3(void) {
    if (!gd_music_track) return;
    MIX_ResumeTrack(gd_music_track);
    paused = false;
}

void fade_to_amplitude(float new_amplitude) {
    atomic_store_explicit(&amplitude_atomic, calculate_amplitude(new_amplitude), memory_order_relaxed);
}

float get_amplitude(void) {
    return atomic_load_explicit(&amplitude_atomic, memory_order_relaxed);
}

int play_mp3_buf(void *buf, size_t sz, bool loop, float seek) {
    if (!buf || sz == 0) return 0;

    ensure_mixer();
    if (!gd_mixer) return 0;

    stop_mp3();

    /* Load directly from memory — predecode=true decodes everything before returning */
    SDL_IOStream *io = SDL_IOFromConstMem(buf, sz);
    if (!io) {
        free(buf);
        return 0;
    }
    current_music = MIX_LoadAudio_IO(gd_mixer, io, true, true);
    free(buf);

    if (!current_music) return 0;

    SDL_AudioSpec fmt;
    if (MIX_GetAudioFormat(current_music, &fmt)) {
        current_music_rate = (float)fmt.freq;
        current_music_channels = fmt.channels;
    } else {
        current_music_rate = 44100;
        current_music_channels = 2;
    }

    quit = false;
    looping = loop;
    paused = false;

    if (!gd_music_track) {
        gd_music_track = MIX_CreateTrack(gd_mixer);
        if (gd_music_track)
            MIX_SetTrackStoppedCallback(gd_music_track, gd_track_stopped, NULL);
    }

    MIX_SetTrackAudio(gd_music_track, current_music);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, MIX_PROP_PLAY_LOOPS_NUMBER, loop ? -1 : 0);

    if (seek > 0 && current_music_rate > 0) {
        Sint64 start_frame = (Sint64)(seek * current_music_rate);
        SDL_SetNumberProperty(props, MIX_PROP_PLAY_START_FRAME_NUMBER, start_frame);
    }

    MIX_PlayTrack(gd_music_track, props);
    SDL_DestroyProperties(props);

    return 1;
}

void stop_mp3(void) {
    if (gd_music_track) {
        MIX_StopTrack(gd_music_track, 0);
    }

    if (current_music) {
        MIX_DestroyAudio(current_music);
        current_music = NULL;
    }

    quit = true;
    atomic_store_explicit(&amplitude_atomic, 0.0f, memory_order_relaxed);
}
