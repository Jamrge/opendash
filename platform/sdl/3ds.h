#pragma once
/*
 * 3ds.h - SDL3 desktop compatibility shim for libctru's <3ds.h>
 *
 * Types + system services stubbed/no-op'd, SDL3 window managed
 * by the gfx* layer. Input (hid) maps keyboard+mouse+gamepad to the 3DS
 * key masks (see shim_system.c for the exact mapping).
 *
 * Keyboard mapping (basic, phase 1):
 *   Space / Z -> KEY_A        X / Esc -> KEY_B      C -> KEY_X   V -> KEY_Y
 *   Q -> KEY_L   E -> KEY_R   1 -> KEY_ZL  2 -> KEY_ZR
 *   Enter -> KEY_START        Tab -> KEY_SELECT
 *   Arrow keys -> D-pad, WASD -> Circle Pad
 *   Left mouse button (over the bottom half of the window) -> KEY_TOUCH
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <math.h>
#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

/* newlib/libctru define __RAND_MAX; macOS libc only has RAND_MAX.
 * math_helpers.c uses __RAND_MAX, so map it for desktop builds. */
#ifndef __RAND_MAX
#define __RAND_MAX RAND_MAX
#endif

typedef uint8_t       u8;
typedef uint16_t      u16;
typedef uint32_t      u32;
typedef uint64_t      u64;
typedef int8_t        s8;
typedef int16_t       s16;
typedef int32_t       s32;
typedef int64_t       s64;
typedef float         f32;
typedef volatile u8   vu8;
typedef volatile u16  vu16;
typedef volatile u32  vu32;
typedef volatile u64  vu64;
typedef s32           Result;
typedef u32           Handle;
typedef void*         Thread;

#ifndef U64_MAX
#define U64_MAX UINT64_MAX
#endif

#define BIT(n) (1u << (n))
#define CUR_THREAD_HANDLE ((Handle)0xFFFF8000u)

/* Timing: SDL performance counter, exported as ms ticks.
 * libctru's CPU_TICKS_PER_MSEC is a *double* value
 * (os.h: #define CPU_TICKS_PER_MSEC (SYSCLOCK_ARM11 / 1000.0)); the float
 * type matters: main.c divides tick deltas by (CPU_TICKS_PER_MSEC * 1000)
 * in u64 arithmetic, which must promote to floating point or delta
 * truncates to 0 and the physics freeze. */
double gd_ticks_per_msec(void);
#define CPU_TICKS_PER_MSEC gd_ticks_per_msec()

/* HID shared memory: real layout is read by precise_input.c (phase 2).
 * NULL is unsafe there, so the shim backs it with a zeroed dummy buffer. */
extern vu32* hidSharedMem;

/* ------------------------------------------------------------------ */
/* Service calls                                                       */
/* ------------------------------------------------------------------ */

/* svc */
u64      svcGetSystemTick(void);
void     svcSleepThread(s64 ns);
void     svcBreak(u32 breakReason);
void     svcGetSystemInfo(s64* out, u32 infoType, s32 param);
Result   svcGetThreadPriority(s32* priority, Handle handle);

/* CITRA_TYPE / CITRA_VERSION come from the project (main.c) */

#define USERBREAK_PANIC 1
#define USERBREAK_ASSERT 2

/* apt */
bool aptMainLoop(void);
void aptSetHomeAllowed(bool allowed);

/* romfs */
Result romfsInit(void);
Result romfsExit(void);

/* soc (network stack init: no-op on desktop, curl uses OS sockets) */
Result socInit(void* socMem, size_t socMemSize);
Result socExit(void);

/* cfgu (system model detection) */
Result cfguInit(void);
Result cfguExit(void);
Result CFGU_GetSystemModel(u8* model);

enum
{
	CFG_MODEL_3DS    = 0,
	CFG_MODEL_3DSXL  = 1,
	CFG_MODEL_N3DS   = 2,
	CFG_MODEL_2DS    = 3,
	CFG_MODEL_N3DSXL = 4,
	CFG_MODEL_N2DSXL = 5,
};

/* os */
void  osSetSpeedupEnable(bool enable);

/* memalign: not in macOS libc, backed by posix_memalign */
void* memalign(size_t alignment, size_t size);

/* linearAlloc family: plain heap on desktop */
void* linearAlloc(size_t size);
void  linearFree(void* mem);
u32   linearSpaceFree(void);

/* envGetHeapSize / mallinfo: only referenced by the debug overlay */
u32 envGetHeapSize(void);

struct mallinfo
{
	int arena, ordblks, smblks, hblks, hblkhd;
	int usmblks, fsmblks, uordblks, fordblks, keepcost;
};
struct mallinfo mallinfo(void);

/* ------------------------------------------------------------------ */
/* gfx: owns the SDL2 window/renderer                                   */
/* ------------------------------------------------------------------ */

typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0 } gfx3dSide_t;  /* right eye removed with stereoscopic 3D */

#define GSP_SCREEN_WIDTH        240
#define GSP_SCREEN_HEIGHT_TOP   400
#define GSP_SCREEN_HEIGHT_TOP_2X 800
#define GSP_SCREEN_HEIGHT_BOTTOM 320

void gfxInitDefault(void);
void gfxExit(void);
void gfxSwapBuffers(void);
void gfxFlushBuffers(void);
void gfxSetDoubleBuffering(bool enable);
void gfxSetWide(bool enable);

void gspWaitForVBlank(void);

/* ------------------------------------------------------------------ */
/* hid: keyboard + mouse -> 3DS key mask                                */
/* ------------------------------------------------------------------ */

enum
{
	KEY_A      = BIT(0),
	KEY_B      = BIT(1),
	KEY_SELECT = BIT(2),
	KEY_START  = BIT(3),
	KEY_DRIGHT = BIT(4),
	KEY_DLEFT  = BIT(5),
	KEY_DUP    = BIT(6),
	KEY_DDOWN  = BIT(7),
	KEY_R      = BIT(8),
	KEY_L      = BIT(9),
	KEY_X      = BIT(10),
	KEY_Y      = BIT(11),
	KEY_ZL     = BIT(14),
	KEY_ZR     = BIT(15),
	KEY_TOUCH  = BIT(20),
	KEY_CSTICK_RIGHT = BIT(24),
	KEY_CSTICK_LEFT  = BIT(25),
	KEY_CSTICK_UP    = BIT(26),
	KEY_CSTICK_DOWN  = BIT(27),
	KEY_CPAD_RIGHT = BIT(28),
	KEY_CPAD_LEFT  = BIT(29),
	KEY_CPAD_UP    = BIT(30),
	KEY_CPAD_DOWN  = BIT(31),

	/* libctru semantics: D-pad | Circle Pad | C-stick combined */
	KEY_UP    = KEY_DUP    | KEY_CPAD_UP    | KEY_CSTICK_UP,
	KEY_DOWN  = KEY_DDOWN  | KEY_CPAD_DOWN  | KEY_CSTICK_DOWN,
	KEY_LEFT  = KEY_DLEFT  | KEY_CPAD_LEFT  | KEY_CSTICK_LEFT,
	KEY_RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT | KEY_CSTICK_RIGHT,
};

typedef struct { u16 px, py; }    touchPosition;
typedef struct { s16 dx, dy; }    circlePosition;

void hidScanInput(void);
u32  hidKeysDown(void);
bool is_debug_key_down(SDL_Scancode key);
u32  hidKeysHeld(void);
u32  hidKeysUp(void);
void hidTouchRead(touchPosition* pos);
void hidCircleRead(circlePosition* pos);

Result HIDUSER_GetSoundVolume(u8* volume);

/* ------------------------------------------------------------------ */
/* Threads + synchronization                                            */
/* ------------------------------------------------------------------ */

Thread threadCreate(void (*entry)(void*), void* arg, size_t stack_size,
                    int priority, int cpuid, bool detached);
void   threadJoin(Thread thread, u64 timeout_ns);
void   threadFree(Thread thread);

typedef int LightLock;   /* zero-initialized = unlocked (SDL spinlock) */
void LightLock_Init(LightLock* lock);
void LightLock_Lock(LightLock* lock);
void LightLock_Unlock(LightLock* lock);

typedef struct { void* sem; int reset; } LightEvent;

#define RESET_ONESHOT 0
#define RESET_STICKY  1

void LightEvent_Init(LightEvent* ev, int reset_type);
void LightEvent_Signal(LightEvent* ev);
void LightEvent_Wait(LightEvent* ev);
void LightEvent_Clear(LightEvent* ev);

/* ------------------------------------------------------------------ */
/* ndsp (audio): now backed by SDL3_mixer                              */
/* ------------------------------------------------------------------ */

extern MIX_Mixer* gd_mixer;
extern MIX_Track* gd_music_track;
void ensure_mixer(void);

void shim_set_top_screen_done_hook(void (*hook)(void));

/* Non-blocking text input */
bool gd_is_text_input_active(void);
const char *gd_text_input_get_current(void);
void gd_text_input_start(char *buf, int limit);
void gd_text_input_stop(void);

enum
{
	NDSP_WBUF_FREE    = 0,
	NDSP_WBUF_QUEUED  = 1,
	NDSP_WBUF_PLAYING = 2,
	NDSP_WBUF_DONE    = 3,
};

typedef struct ndspWaveBuf
{
	u32 status;
	union
	{
		const void* data_vaddr;
		const s16*  data_pcm16;
	};
	u32 nsamples;
	u32 offset;
	u8  looping;
} ndspWaveBuf;

#define NDSP_INTERP_TAIKO   0
#define NDSP_INTERP_NONE    1
#define NDSP_INTERP_LINEAR  2
#define NDSP_INTERP_POLYPHASE 3

#define NDSP_FORMAT_MONO_PCM16   0
#define NDSP_FORMAT_STEREO_PCM16 1
#define NDSP_FORMAT_ADPCM        2

#define NDSP_OUTPUT_MONO  0
#define NDSP_OUTPUT_STEREO 1

Result ndspInit(void);
void   ndspExit(void);
void   ndspSetOutputMode(int mode);
void   ndspSetCallback(void (*callback)(void*), void* user);
void   ndspChnReset(int channel);
void   ndspChnSetMix(int channel, const float* mix);
void   ndspChnSetRate(int channel, float rate);
void   ndspChnSetFormat(int channel, int format);
void   ndspChnSetInterp(int channel, int type);
void   ndspChnSetPaused(int channel, bool paused);
bool   ndspChnIsPaused(int channel);
void   ndspChnWaveBufAdd(int channel, ndspWaveBuf* buf);

void DSP_FlushDataCache(const void* data, size_t size);

/* ------------------------------------------------------------------ */
/* console output (error screens / debug)                               */
/* ------------------------------------------------------------------ */

typedef enum { debugDevice_CONSOLE = 0, debugDevice_SVC, debugDevice_FILE } debugDevice;

typedef struct PrintConsole { int _dummy; } PrintConsole;

PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console);
void          consoleDebugInit(debugDevice device);

/* ------------------------------------------------------------------ */
/* swkbd (3DS software keyboard): stubbed, returns "cancel"             */
/* ------------------------------------------------------------------ */

#define SWKBD_TYPE_NORMAL  0
#define SWKBD_TYPE_QWERTY  1
#define SWKBD_TYPE_NUMPAD  2
#define SWKBD_TYPE_WESTERN 3

#define SWKBD_PARENTAL  1
#define SWKBD_DARKEN_TOP_SCREEN BIT(0)

typedef enum
{
	SwkbdButtonNone   = -1,
	SwkbdButtonLeft   = 0,
	SwkbdButtonMiddle = 1,
	SwkbdButtonRight  = 2,
} SwkbdButton;

typedef struct { char _dummy[64]; } SwkbdState;

void swkbdInit(SwkbdState* swkbd, int type, int numButtons, int maxTextLen);
void swkbdSetHintText(SwkbdState* swkbd, const char* text);
void swkbdSetInitialText(SwkbdState* swkbd, const char* text);
void swkbdSetFeatures(SwkbdState* swkbd, u32 features);
SwkbdButton swkbdInputText(SwkbdState* swkbd, char* buf, size_t bufSize);

/* ------------------------------------------------------------------ */
/* Path translation                                                     */
/* ------------------------------------------------------------------ */
/*
 * The game reads assets from "romfs:/..." and stores user data in
 * "/3ds/gd3ds/...". fopen/mkdir are macro-redirected so project code
 * needs no edits:
 *   "romfs:/x/y" -> "romfs/x/y"   (relative to cwd: the repo's romfs/)
 *   "/3ds/x"     -> "sdl_fs/x"    (writable local folder)
 *   "sdmc:/3ds/x"-> "sdl_fs/x"
 */
FILE* gd3ds_fopen(const char* path, const char* mode);
int   gd3ds_mkdir(const char* path, mode_t mode);
int   gd3ds_access(const char* path, int mode);
void  gd_translate_path(const char* path, char* out, size_t outSize);
struct mpg123_handle_struct;   /* completed later by <mpg123.h> */
int   gd3ds_mpg_open(struct mpg123_handle_struct* mh, const char* path);

#define fopen  gd3ds_fopen
#define mkdir  gd3ds_mkdir
#define access gd3ds_access
#define mpg123_open gd3ds_mpg_open
