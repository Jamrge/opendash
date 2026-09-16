#pragma once
/* Shared state between the SDL3 shim .c files (internal, not for project code) */

#ifndef SHIM_INTERNAL_H
#define SHIM_INTERNAL_H

#include <SDL3/SDL.h>
#include <stdbool.h>

extern SDL_Window*   gd_window;
extern SDL_Renderer* gd_renderer;
extern bool          gd_quit;
extern bool          gd_sdl_ready;

typedef struct shimRenderTarget
{
	SDL_Texture* tex;
	int   screen;   /* gfxScreen_t value */
	int   side;     /* gfx3dSide_t value */
	int   w, h;     /* display-space size */
} shimRenderTarget;

shimRenderTarget* shim_get_target_tex(void* target);
bool shim_begin_frame(void);
void shim_present_frame(void);

#endif
