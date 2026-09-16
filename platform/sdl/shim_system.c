/*
 * shim_system.c - SDL3 backend for the libctru <3ds.h> surface.
 *
 * Real behavior: SDL window/renderer lifecycle, hid input mapping
 * (keyboard + mouse + basic gamepad), ROMFS/SDMC path translation,
 * threads/events over SDL.
 * No-ops: ndsp audio, svc/os/CFGU bits, soc network init.
 *
 * SDL3 notes (verified against installed SDL3 headers, 3.4.16):
 *   - SDL_Init / SDL_CreateWindowAndRenderer / SDL_PollEvent return bool.
 *   - SDL_GetMouseState fills FLOAT pointers.
 *   - SDL_GetKeyboardState returns const bool*.
 *   - gamepad API is SDL_Gamepad* (SDL_OpenGamepad/SDL_CloseGamepad/...).
 *   - SDL_sem is SDL_Semaphore (SDL_SignalSemaphore / SDL_WaitSemaphore /
 *     SDL_TryWaitSemaphore). SDL_AtomicLock is gone: LightLock is a CAS
 *     spinlock over SDL_AtomicInt.
 *   - SDL_QuitRequested is gone; quit events are tracked by the shim.
 */

#include <3ds.h>

#undef fopen
#undef mkdir

#include <SDL3/SDL.h>
#include "shim_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* SDL bootstrap (gfx window)                                           */
/* ------------------------------------------------------------------ */

SDL_Window*   gd_window    = NULL;
SDL_Renderer* gd_renderer  = NULL;
bool          gd_quit      = false;
bool          gd_sdl_ready = false;
static SDL_Gamepad* gd_controller = NULL;
static SDL_JoystickID       gd_controller_id = 0;

static void gd_open_first_gamepad(void)
{
	int count = 0;
	SDL_JoystickID* ids = SDL_GetGamepads(&count);
	if (ids)
	{
		for (int i = 0; i < count; ++i)
		{
			if (SDL_IsGamepad(ids[i]))
			{
				gd_controller = SDL_OpenGamepad(ids[i]);
				if (gd_controller)
				{
					gd_controller_id = ids[i];
					break;
				}
			}
		}
		SDL_free(ids);
	}
}

void gfxInitDefault(void)
{
	if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD))
	{
		fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
		exit(1);
	}

	/* window + renderer in one call (SDL3 form); logical letterboxed view */
	if (!SDL_CreateWindowAndRenderer("Opendash Prototype (SDL 3)",
	                                 400, 480, SDL_WINDOW_RESIZABLE,
	                                 &gd_window, &gd_renderer))
	{
		fprintf(stderr, "SDL_CreateWindowAndRenderer failed: %s\n", SDL_GetError());
		exit(1);
	}
	if (!SDL_SetRenderLogicalPresentation(gd_renderer, 400, 480,
	                                      SDL_LOGICAL_PRESENTATION_LETTERBOX))
	{
		fprintf(stderr, "SDL_SetRenderLogicalPresentation failed: %s\n", SDL_GetError());
	}
	gd_sdl_ready = true;
	gd_open_first_gamepad();
}

void gfxExit(void)
{
	if (!gd_sdl_ready) return;
	if (gd_controller) SDL_CloseGamepad(gd_controller);
	SDL_Quit();
	gd_controller    = NULL;
	gd_controller_id = 0;
	gd_sdl_ready     = false;
	gd_renderer      = NULL;
	gd_window        = NULL;
}

void gfxSwapBuffers(void)  { shim_present_frame(); }
void gfxFlushBuffers(void) {}
void gfxSetDoubleBuffering(bool enable) { (void)enable; }
void gfxSetWide(bool enable){ (void)enable; }
void gspWaitForVBlank(void) { SDL_Delay(16); }

/* ------------------------------------------------------------------ */
/* apt / svc / os / cfgu / soc / romfs: fixed values                    */
/* ------------------------------------------------------------------ */

bool aptMainLoop(void)
{
	if (!gd_sdl_ready) return !gd_quit;
	/* SDL3 dropped SDL_QuitRequested: poll pending events for SDL_EVENT_QUIT */
	SDL_Event ev;
	while (SDL_PollEvent(&ev))
	{
		if (ev.type == SDL_EVENT_QUIT) gd_quit = true;
	}
	return !gd_quit;
}

void aptSetHomeAllowed(bool allowed) { (void)allowed; }

double gd_ticks_per_msec(void)
{
	return (double)(SDL_GetPerformanceFrequency() / 1000u);
}

u64 svcGetSystemTick(void) { return (u64)SDL_GetPerformanceCounter(); }

void svcSleepThread(s64 ns) { SDL_Delay((Uint32)(ns / 1000000u)); }
void svcBreak(u32 breakReason) { (void)breakReason; }

void svcGetSystemInfo(s64* out, u32 infoType, s32 param)
{
	(void)infoType; (void)param;
	*out = 0;                    /* 0 => is_citra() reports false */
}

Result svcGetThreadPriority(s32* priority, Handle handle)
{
	(void)handle;
	if (priority) *priority = 0x30;
	return 0;
}

Result romfsInit(void) { return 0; }
Result romfsExit(void) { return 0; }

Result socInit(void* socMem, size_t memSize) { (void)socMem; (void)memSize; return 0; }
Result socExit(void) { return 0; }

Result cfguInit(void) { return 0; }
Result cfguExit(void) { return 0; }

Result CFGU_GetSystemModel(u8* model)
{
	if (model) *model = CFG_MODEL_N3DS;
	return 0;
}

void osSetSpeedupEnable(bool enable) { (void)enable; }

/* 3DS volume slider is 0..63; desktop reports max. */
Result HIDUSER_GetSoundVolume(u8* volume)
{
	if (volume) *volume = 63;
	return 0;
}

u32 envGetHeapSize(void) { return 32u << 20; }  /* dummy: debug overlay only */

struct mallinfo mallinfo(void)
{
	struct mallinfo mi;
	memset(&mi, 0, sizeof(mi));
	return mi;
}

/* ------------------------------------------------------------------ */
/* memalign / linearAlloc                                               */
/* ------------------------------------------------------------------ */

void* memalign(size_t alignment, size_t size)
{
	/* desktop: alignment is only required by the 3DS soc syscall, which is
	 * a no-op shim here; plain malloc keeps this portable (MinGW-w64's
	 * CRT has no guaranteed posix_memalign). */
	(void)alignment;
	return malloc(size);
}

void* linearAlloc(size_t size) { return malloc(size); }
void  linearFree(void* mem)    { free(mem); }
u32   linearSpaceFree(void)    { return 8u << 20; }  /* dummy debug value */

/* ------------------------------------------------------------------ */
/* HID: keyboard + mouse + basic gamepad -> 3DS key mask                */
/* ------------------------------------------------------------------ */

static u32 g_keys_held = 0;
static u32 g_keys_prev = 0;
static u32 g_keys_down = 0;
static u32 g_keys_up   = 0;
static touchPosition g_touch  = { 0, 0 };
static circlePosition g_circle = { 0, 0 };

/* Map the window to the 400x480 logical layout and read the mouse as
 * bottom-screen (320x240) coordinates. Touch only lives in the bottom
 * half (between x=40..360 in logical space). */
static bool scan_touch(float mx, float my)
{
	int w = 0, h = 0;
	SDL_GetWindowSize(gd_window, &w, &h);
	if (w <= 0 || h <= 0) return false;

	float scale   = (float)h / 480.0f;
	float content = 400.0f * scale;
	float off_x   = ((float)w - content) * 0.5f;
	float lx      = (mx - off_x) / scale;
	float ly      = my / scale;

	if (ly < 240.0f) return false;                /* upper half = top screen */

	float bx = (lx - 40.0f);                      /* bottom rect: x 40..360 */
	if (bx < 0.0f)    bx = 0.0f;
	if (bx >= 320.0f) bx = 319.0f;
	float by = (ly - 240.0f);
	if (by >= 240.0f) by = 239.0f;

	g_touch.px = (u16)bx;
	g_touch.py = (u16)by;
	return true;
}

static u32 scan_keys(const bool* keys)
{
	u32 k = 0;
	if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_Z]) k |= KEY_A;
	if (keys[SDL_SCANCODE_X]     || keys[SDL_SCANCODE_ESCAPE]) k |= KEY_B;
	if (keys[SDL_SCANCODE_C]) k |= KEY_X;
	if (keys[SDL_SCANCODE_V]) k |= KEY_Y;
	if (keys[SDL_SCANCODE_Q]) k |= KEY_L;
	if (keys[SDL_SCANCODE_E]) k |= KEY_R;
	if (keys[SDL_SCANCODE_1]) k |= KEY_ZL;
	if (keys[SDL_SCANCODE_2]) k |= KEY_ZR;
	if (keys[SDL_SCANCODE_RETURN] || keys[SDL_SCANCODE_KP_ENTER]) k |= KEY_START;
	if (keys[SDL_SCANCODE_TAB])   k |= KEY_SELECT;

	if (keys[SDL_SCANCODE_UP])    k |= KEY_DUP;
	if (keys[SDL_SCANCODE_DOWN])  k |= KEY_DDOWN;
	if (keys[SDL_SCANCODE_LEFT])  k |= KEY_DLEFT;
	if (keys[SDL_SCANCODE_RIGHT]) k |= KEY_DRIGHT;
	if (keys[SDL_SCANCODE_W]) k |= KEY_CPAD_UP;
	if (keys[SDL_SCANCODE_S]) k |= KEY_CPAD_DOWN;
	if (keys[SDL_SCANCODE_A]) k |= KEY_CPAD_LEFT;
	if (keys[SDL_SCANCODE_D]) k |= KEY_CPAD_RIGHT;

	float mx = 0, my = 0;
	Uint32 mbits = SDL_GetMouseState(&mx, &my);
	if ((mbits & SDL_BUTTON_LMASK) && scan_touch(mx, my)) k |= KEY_TOUCH;

	/* basic gamepad mapping (SDL_Gamepad API) */
	if (gd_controller)
	{
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_SOUTH))          k |= KEY_A;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_EAST))           k |= KEY_B;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_WEST))           k |= KEY_X;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_NORTH))          k |= KEY_Y;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))  k |= KEY_L;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER)) k |= KEY_R;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_START))          k |= KEY_START;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_BACK))           k |= KEY_SELECT;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_DPAD_UP))        k |= KEY_DUP;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_DPAD_DOWN))      k |= KEY_DDOWN;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_DPAD_LEFT))      k |= KEY_DLEFT;
		if (SDL_GetGamepadButton(gd_controller, SDL_GAMEPAD_BUTTON_DPAD_RIGHT))     k |= KEY_DRIGHT;

		Sint16 ax = SDL_GetGamepadAxis(gd_controller, SDL_GAMEPAD_AXIS_LEFTX);
		Sint16 ay = SDL_GetGamepadAxis(gd_controller, SDL_GAMEPAD_AXIS_LEFTY);
		g_circle.dx = (s16)(ax / 200);
		g_circle.dy = (s16)(-ay / 200);
		if (ax > 16000)       k |= KEY_CPAD_RIGHT;
		else if (ax < -16000) k |= KEY_CPAD_LEFT;
		if (ay < -16000)      k |= KEY_CPAD_UP;
		else if (ay > 16000)  k |= KEY_CPAD_DOWN;

		Sint16 tl = SDL_GetGamepadAxis(gd_controller, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
		Sint16 tr = SDL_GetGamepadAxis(gd_controller, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
		if (tl > 8000) k |= KEY_ZL;
		if (tr > 8000) k |= KEY_ZR;
	}
	else
	{
		g_circle.dx = 0; g_circle.dy = 0;   /* keyboard fallback for the circle pad */
		if (keys[SDL_SCANCODE_W])      g_circle.dy = 156;
		else if (keys[SDL_SCANCODE_S]) g_circle.dy = -156;
		if (keys[SDL_SCANCODE_A])      g_circle.dx = -156;
		else if (keys[SDL_SCANCODE_D]) g_circle.dx = 156;
	}

	return k;
}

static void gd_ring_push_pad(u32 keys, s16 dx, s16 dy);
static void gd_ring_push_touch(u16 px, u16 py, bool valid);
static void gd_ring_feed_event(const SDL_Event* ev, u64 scan_tick, u64 scan_ns);

void hidScanInput(void)
{
	if (gd_sdl_ready)
	{
		/* Opción B (240Hz input real): each scan drains the SDL event
		 * queue and writes ring samples into hidSharedMem with real
		 * timestamps, so precise_input.c sees a live sysmodule ring. */
		u64 scan_tick = SDL_GetPerformanceCounter();
		u64 scan_ns   = SDL_GetTicksNS();

		SDL_Event ev;
		while (SDL_PollEvent(&ev))
		{
			if (ev.type == SDL_EVENT_QUIT) { gd_quit = true; continue; }
			gd_ring_feed_event(&ev, scan_tick, scan_ns);
		}
		SDL_PumpEvents();                 /* refresh device state arrays */
		const bool* keys = SDL_GetKeyboardState(NULL);
		g_keys_prev = g_keys_held;
		g_keys_held = scan_keys(keys);

		/* end-of-scan checkpoint sample(s): full current state */
		gd_ring_push_pad(g_keys_held, g_circle.dx, g_circle.dy);
		float mx = 0, my = 0;
		u32 mbits = SDL_GetMouseState(&mx, &my);
		bool touch_now = (mbits & SDL_BUTTON_LMASK) && scan_touch(mx, my);
		gd_ring_push_touch(g_touch.px, g_touch.py, touch_now);
	}
	else
	{
		g_keys_prev = g_keys_held;
		g_keys_held = 0;
	}
	g_keys_down = g_keys_held & ~g_keys_prev;
	g_keys_up   = g_keys_prev & ~g_keys_held;
}

u32 hidKeysDown(void) { return g_keys_down; }
u32 hidKeysHeld(void) { return g_keys_held; }
u32 hidKeysUp(void)   { return g_keys_up; }

void hidTouchRead(touchPosition* pos)  { if (pos) *pos = g_touch; }
void hidCircleRead(circlePosition* pos){ if (pos) *pos = g_circle; }

/* HID shared memory ring (Opción B): emulates the 3DS HID sysmodule
 * shared-memory ring with live samples backed by real SDL3 events, so
 * precise_input.c works unchanged (the layout was verified against the
 * libctru.a disassembly for PAD and hid.h's documented 0xA8 touch base;
 * the touch ring offset (0xC8) matches the on-hardware offsets the
 * project already uses).
 *
 *   PAD   section: +0x00 u64 lap tick, +0x08 u64 prev lap tick,
 *                  +0x10 u32 sample counter (mono), +0x28 8 slots ×
 *                  { u32 keys, s16 cpad_dx, s16 cpad_dy, u32 attr }
 *   TOUCH section: +0xA8 u64, +0xB0 u64, +0xB8 u32 idx, +0xC8
 *                  8 slots × { u32 (py<<16)|px, u32 valid }
 * Laps: tick fields move every 8 samples written (the reader derives
 * sample_interval = (lap − prev_lap)/8; lap tick = SDL perf counter at
 * lap open). The sample index (-0x10 / +0xB8) is a monotonic counter;
 * slots are addressed as (idx & 7). */
static u32 gd_hid_shared_mem[128];
vu32* hidSharedMem = gd_hid_shared_mem;

#define GD_RING_SLOTS 8

static u32 gd_ring_pad_idx   = 0;
static u32 gd_ring_touch_idx = 0;
static u32 gd_ring_pad_state = 0;   /* running keys mask (event-applied) */
static u64 gd_last_sample_tick = 1;  /* last tick written to the ring (monotonic) */

/* advance a section's lap every 8 samples: prev = current, current = now */
static void gd_ring_lap_tick(u32* slot, u32 base_word)
{
	if ((*slot % GD_RING_SLOTS) == 0)
	{
		gd_hid_shared_mem[base_word + 2] = gd_hid_shared_mem[base_word + 0];
		gd_hid_shared_mem[base_word + 3] = gd_hid_shared_mem[base_word + 1];
		u64 now = (u64)SDL_GetPerformanceCounter();
		gd_hid_shared_mem[base_word + 0] = (u32)(now & 0xffffffffu);
		gd_hid_shared_mem[base_word + 1] = (u32)(now >> 32);
	}
}

/* one PAD sample: keys + circle pad + attr, slot (idx&7); idx++ mono */
static void gd_ring_push_pad(u32 keys, s16 dx, s16 dy)
{
	gd_ring_lap_tick(&gd_ring_pad_idx, 0);

	u32* slot = &gd_hid_shared_mem[(0x28 / 4) + (gd_ring_pad_idx % GD_RING_SLOTS) * 4];
	slot[0] = keys;
	slot[1] = (u32)(u16)dx;
	slot[2] = (u32)(u16)dy;
	slot[3] = 0;   /* sysmodule sample attribute */

	gd_hid_shared_mem[4] = gd_ring_pad_idx;   /* sample counter @0x10 */
	if (gd_ring_pad_idx < 0xffffffffu) gd_ring_pad_idx++;
}

/* one TOUCH sample: packed px/py + valid flag */
static void gd_ring_push_touch(u16 px, u16 py, bool valid)
{
	gd_ring_lap_tick(&gd_ring_touch_idx, 0xA8 / 4);

	u32* slot = &gd_hid_shared_mem[(0xC8 / 4) + (gd_ring_touch_idx % GD_RING_SLOTS) * 2];
	slot[0] = ((u32)py << 16) | (u32)px;
	slot[1] = valid ? 1u : 0u;

	gd_hid_shared_mem[0xA8 / 4 + 4] = gd_ring_touch_idx;   /* sample counter @0xB8 */
	if (gd_ring_touch_idx < 0xffffffffu) gd_ring_touch_idx++;
}

/* KEY_* bits contributed by a single scancode (mirrors scan_keys, so the
 * incremental ring writer and the state scanner agree on the mapping) */
static u32 gd_bits_for_scancode(SDL_Scancode sc)
{
	switch (sc)
	{
		case SDL_SCANCODE_SPACE: case SDL_SCANCODE_Z: return KEY_A;
		case SDL_SCANCODE_X:     case SDL_SCANCODE_ESCAPE: return KEY_B;
		case SDL_SCANCODE_C: return KEY_X;
		case SDL_SCANCODE_V: return KEY_Y;
		case SDL_SCANCODE_Q: return KEY_L;
		case SDL_SCANCODE_E: return KEY_R;
		case SDL_SCANCODE_1: return KEY_ZL;
		case SDL_SCANCODE_2: return KEY_ZR;
		case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return KEY_START;
		case SDL_SCANCODE_TAB: return KEY_SELECT;
		case SDL_SCANCODE_UP:    return KEY_DUP;
		case SDL_SCANCODE_DOWN:  return KEY_DDOWN;
		case SDL_SCANCODE_LEFT:  return KEY_DLEFT;
		case SDL_SCANCODE_RIGHT: return KEY_DRIGHT;
		case SDL_SCANCODE_W: return KEY_CPAD_UP;
		case SDL_SCANCODE_S: return KEY_CPAD_DOWN;
		case SDL_SCANCODE_A: return KEY_CPAD_LEFT;
		case SDL_SCANCODE_D: return KEY_CPAD_RIGHT;
		default: return 0;
	}
}

/* Feed one SDL event into the ring with a real per-event timestamp.
 * Events under the current scan get a clock inside (prev_scan..now):
 * tick = scan_tick - (scan_ns - ev.timestamp), converted from the ns
 * domain to the perf-counter domain, clamped monotonic. */
static void gd_ring_feed_event(const SDL_Event* ev, u64 scan_tick, u64 scan_ns)
{
	u64 ev_tick = scan_tick;   /* fallback: end of frame */
	if (ev->common.timestamp && ev->common.timestamp < scan_ns && scan_ns > scan_tick)
	{
		u64 age_ns = scan_ns - ev->common.timestamp;
		if (age_ns < scan_tick) ev_tick = scan_tick - age_ns;   /* ns ≈ ticks when freq=1e9 */
		if (ev_tick < gd_last_sample_tick + 1) ev_tick = gd_last_sample_tick + 1;
	}

	switch (ev->type)
	{
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
		{
			u32 bits = gd_bits_for_scancode(ev->key.scancode);
			if (bits == 0) break;
			if (ev->type == SDL_EVENT_KEY_DOWN) gd_ring_pad_state |= bits;
			else                                gd_ring_pad_state &= ~bits;
			gd_ring_push_pad(gd_ring_pad_state, 0, 0);
			gd_last_sample_tick = ev_tick;
			break;
		}

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		{
			bool inside = scan_touch(ev->button.x, ev->button.y);
			bool down = (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN)
			            && ev->button.button == SDL_BUTTON_LEFT;
			gd_ring_push_touch(g_touch.px, g_touch.py, down && inside);
			gd_last_sample_tick = ev_tick;
			break;
		}

		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
		{
			u32 bits = 0;
			switch (ev->gbutton.button)
			{
				case SDL_GAMEPAD_BUTTON_SOUTH: bits = KEY_A; break;
				case SDL_GAMEPAD_BUTTON_EAST:  bits = KEY_B; break;
				case SDL_GAMEPAD_BUTTON_WEST:  bits = KEY_X; break;
				case SDL_GAMEPAD_BUTTON_NORTH: bits = KEY_Y; break;
				case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  bits = KEY_L; break;
				case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: bits = KEY_R; break;
				case SDL_GAMEPAD_BUTTON_START:  bits = KEY_START; break;
				case SDL_GAMEPAD_BUTTON_BACK:   bits = KEY_SELECT; break;
				case SDL_GAMEPAD_BUTTON_DPAD_UP:    bits = KEY_DUP; break;
				case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  bits = KEY_DDOWN; break;
				case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  bits = KEY_DLEFT; break;
				case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: bits = KEY_DRIGHT; break;
				default: break;
			}
			if (bits == 0) break;
			if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) gd_ring_pad_state |= bits;
			else                                           gd_ring_pad_state &= ~bits;
			gd_ring_push_pad(gd_ring_pad_state, 0, 0);
			gd_last_sample_tick = ev_tick;
			break;
		}

		default:
			break;   /* unrelated event: no ring sample */
	}
}

/* ------------------------------------------------------------------ */
/* Threads + sync (SDL3: SDL_Thread / SDL_Semaphore / CAS spinlock)     */
/* ------------------------------------------------------------------ */

Thread threadCreate(void (*entry)(void*), void* arg, size_t stack_size,
                    int priority, int cpuid, bool detached)
{
	(void)stack_size; (void)priority; (void)cpuid; (void)detached;
	/* Always joinable under SDL; threadJoin frees the thread object. */
	union { void (*entry)(void*); SDL_ThreadFunction fn; } conv;
	conv.entry = entry;
	return (Thread)SDL_CreateThread(conv.fn, "gd3ds", arg);
}

void threadJoin(Thread thread, u64 timeout_ns)
{
	(void)timeout_ns;   /* libctru has a timeout; SDL waits indefinitely */
	if (thread) SDL_WaitThread((SDL_Thread*)thread, NULL);
}

void threadFree(Thread thread)
{
	/* Not used by the project today; best-effort detach if never joined. */
	if (thread) SDL_DetachThread((SDL_Thread*)thread);
}

/* LightLock: 3DS zero-initialized lock == SDL_AtomicInt (same int layout).
 * SDL3 has no SDL_AtomicLock spinlock anymore, so implement it with CAS. */
void LightLock_Init(LightLock* lock)   { *lock = 0; }

void LightLock_Lock(LightLock* lock)
{
	SDL_AtomicInt* atom = (SDL_AtomicInt*)lock;   /* layout-compatible: one int */
	while (!SDL_CompareAndSwapAtomicInt(atom, 0, 1))
		SDL_DelayNS(1000000);                     /* 1 us backoff while spinning */
}

void LightLock_Unlock(LightLock* lock)
{
	SDL_AtomicInt* atom = (SDL_AtomicInt*)lock;
	SDL_SetAtomicInt(atom, 0);
}

void LightEvent_Init(LightEvent* ev, int reset_type)
{
	ev->reset = reset_type;
	if (!ev->sem) ev->sem = SDL_CreateSemaphore(0);
}

void LightEvent_Signal(LightEvent* ev)
{
	if (ev->sem) SDL_SignalSemaphore((SDL_Semaphore*)ev->sem);
}

void LightEvent_Wait(LightEvent* ev)
{
	if (ev->sem) SDL_WaitSemaphore((SDL_Semaphore*)ev->sem);
}

void LightEvent_Clear(LightEvent* ev)
{
	while (ev->sem && SDL_TryWaitSemaphore((SDL_Semaphore*)ev->sem)) { /* drain */ }
}

/* ------------------------------------------------------------------ */
/* ndsp: no-op audio in this phase (pause state tracked for the player) */
/* ------------------------------------------------------------------ */

static bool g_channel_paused[24];

Result ndspInit(void) { return 0; }
void ndspExit(void) {}
void ndspSetOutputMode(int mode) { (void)mode; }
void ndspSetCallback(void (*callback)(void*), void* user) { (void)callback; (void)user; }
void ndspChnReset(int channel) { if (channel >= 0 && channel < 24) g_channel_paused[channel] = false; }
void ndspChnSetMix(int channel, const float* mix) { (void)channel; (void)mix; }
void ndspChnSetRate(int channel, float rate) { (void)channel; (void)rate; }
void ndspChnSetFormat(int channel, int format) { (void)channel; (void)format; }
void ndspChnSetInterp(int channel, int type) { (void)channel; (void)type; }
void ndspChnSetPaused(int channel, bool paused)
{
	if (channel >= 0 && channel < 24) g_channel_paused[channel] = paused;
}
bool ndspChnIsPaused(int channel)
{
	if (channel >= 0 && channel < 24) return g_channel_paused[channel];
	return false;
}
void ndspChnWaveBufAdd(int channel, ndspWaveBuf* buf)
{
	(void)channel;
	if (buf) buf->status = NDSP_WBUF_DONE;  /* consumed instantly: thread parks */
}

void DSP_FlushDataCache(const void* data, size_t size) { (void)data; (void)size; }

/* ------------------------------------------------------------------ */
/* console (error screens / debug output)                               */
/* ------------------------------------------------------------------ */

PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console)
{
	(void)screen;
	return console;
}

void consoleDebugInit(debugDevice device) { (void)device; }

/* ------------------------------------------------------------------ */
/* swkbd: stub                                                          */
/* ------------------------------------------------------------------ */

void swkbdInit(SwkbdState* swkbd, int type, int numButtons, int maxTextLen)
{
	memset(swkbd, 0, sizeof(*swkbd));
	(void)type; (void)numButtons; (void)maxTextLen;
}

void swkbdSetHintText(SwkbdState* swkbd, const char* text)   { (void)swkbd; (void)text; }
void swkbdSetInitialText(SwkbdState* swkbd, const char* text){ (void)swkbd; (void)text; }
void swkbdSetFeatures(SwkbdState* swkbd, u32 features)       { (void)swkbd; (void)features; }

SwkbdButton swkbdInputText(SwkbdState* swkbd, char* buf, size_t bufSize)
{
	(void)swkbd;
	if (buf && bufSize > 0) buf[0] = '\0';
	return SwkbdButtonLeft;  /* looks like "cancel" to the caller */
}

/* ------------------------------------------------------------------ */
/* Path translation (romfs:/, /3ds/, sdmc:/)                            */
/* ------------------------------------------------------------------ */

static void gd_translate_path(const char* path, char* out, size_t outSize)
{
	if ((strncmp(path, "romfs:/", 7)) == 0)
	{
		snprintf(out, outSize, "romfs/%s", path + 7);
	}
	else if ((strncmp(path, "sdmc:/", 6)) == 0)
	{
		snprintf(out, outSize, "sdl_fs/%s", path + 6);
	}
	else if ((strncmp(path, "/3ds/", 5)) == 0)
	{
		snprintf(out, outSize, "sdl_fs/%s", path + 5);
	}
	else
	{
		snprintf(out, outSize, "%s", path);
	}
}

FILE* gd3ds_fopen(const char* path, const char* mode)
{
	char translated[1024];
	gd_translate_path(path, translated, sizeof(translated));
	return fopen(translated, mode);
}

/* Create path (and its missing parents) after translation. */
#ifdef _WIN32
#include <direct.h>     /* _mkdir (MinGW has no 2-arg mkdir) */
static int gd_system_mkdir(const char* path, mode_t mode)
{
	(void)mode;
	return _mkdir(path);
}
#else
static int gd_system_mkdir(const char* path, mode_t mode)
{
	return mkdir(path, mode);
}
#endif

int gd3ds_mkdir(const char* path, mode_t mode)
{
	char translated[1024];
	gd_translate_path(path, translated, sizeof(translated));

	char tmp[1024];
	size_t len = strlen(translated);
	if (len >= sizeof(tmp)) return -1;
	memcpy(tmp, translated, len + 1);

	/* strip trailing separators */
	while (len > 1 && tmp[len - 1] == '/') tmp[--len] = '\0';

	for (char* p = tmp + 1; *p; ++p)
	{
		if (*p == '/')
		{
			*p = '\0';
			gd_system_mkdir(tmp, mode);
			*p = '/';
		}
	}
	return gd_system_mkdir(tmp, mode);
}
