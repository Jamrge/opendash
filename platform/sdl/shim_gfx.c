/*
 * shim_gfx.c - SDL3 backend for citro2d/citro3d.
 *
 * Real rendering (from phase 2, now on SDL3):
 *   - C2D_SpriteSheetLoad reads the converter output (platform/sdl/tools/
 *     t3s_to_atlas.py -> assets_build/<name>.atlas) and creates one SDL
 *     texture per atlas page. Entry i == .t3s line i == the index the game
 *     hardcodes in C2D_SpriteFromSheet(sheet, i).
 *   - C2D_DrawSprite(Tinted) / C2D_DrawImageAtRotated render with
 *     SDL_RenderTextureRotated honoring C2D_SpriteParams (pos/center/scale/
 *     angle), negative scale as flips and the C2D_ImageTint color (mods).
 *   - C2D_DrawRectSolid / C2D_DrawRectangle fill rects on the active target.
 *   - The view state (C2D_View*) transforms positions AND sizes:
 *       out = S*p + o;  ViewScale multiplies into S, ViewTranslate adds S*d.
 *
 * Still stubbed (same scope as before): custom triangles/quad path (text
 * glyph fallback + 9-slice), fade overlay.
 * (Scissor/clip is real now: C3D_SetScissor maps onto SDL_SetRenderClipRect.
 *  Additive blending is real too: C3D_AlphaBlend maps the two PICA200
 *  states the game uses onto SDL blend modes stamped per draw.)
 *
 * SDL3 notes (verified against installed SDL3 headers, 3.4.16):
 *   - SDL_RenderCopyExF is gone: use SDL_RenderTextureRotated
 *     (float src/dst rects, double angle, SDL_FlipMode).
 *   - SDL_RenderCopy -> SDL_RenderTexture (float rects).
 *   - SDL_RenderFillRectF -> SDL_RenderFillRect (always float).
 *   - SDL_RenderSetLogicalSize -> SDL_SetRenderLogicalPresentation
 *     (done in shim_system.c together with window creation).
 *   - Everything render-related returns bool now.
 */

#include <3ds.h>
#include <citro2d.h>
#include <citro3d.h>
#include "shim_internal.h"

#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "c2d_internal.h"   /* vendored C2Di_Context / C2Di_Quad types */

/* ------------------------------------------------------------------ */
/* Internal types                                                       */
/* ------------------------------------------------------------------ */

/* C3D_Tex: real struct wrapping an SDL texture (header stays SDL-free) */
struct C3D_Tex
{
	SDL_Texture* sdl;
};

struct C3D_RenderTarget_s
{
	shimRenderTarget base;   /* must stay first (shim_get_target_tex cast) */
};

/* atlas container (see t3s_to_atlas.py header): little-endian */
#define ATLAS_MAGIC   0x41533354u  /* bytes 'T','3','S','A' */
#define ATLAS_VERSION 1u

static u32 rd_u32(FILE* f)
{
	unsigned char b[4];
	if (fread(b, 1, 4, f) != 4) return 0;
	return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

static u16 rd_u16(FILE* f)
{
	unsigned char b[2];
	if (fread(b, 1, 2, f) != 2) return 0;
	return (u16)(b[0] | (b[1] << 8));
}

static bool file_readable(const char* path)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fclose(f);
	return true;
}

static C3D_RenderTarget* g_targets[4] = { 0 }; /* indexed by gfx screen value */

/* ------------------------------------------------------------------ */
/* Render targets + frame composite                                     */
/* ------------------------------------------------------------------ */

bool C3D_Init(size_t commandBufferSize) { (void)commandBufferSize; return true; }
void C3D_Fini(void) {}

bool C3D_FrameBegin(u8 flags) { (void)flags; return shim_begin_frame(); }

void C3D_FrameEnd(u8 flags)
{
	(void)flags;
	shim_present_frame();
}

/* width/height arrive transposed from the 3DS layout (see project's
 * C2D_CreateScreenTargetExt); display size is the swapped pair. */
C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, int colorFormat, int depthFormat)
{
	(void)colorFormat; (void)depthFormat;
	if (!gd_renderer) return NULL;

	C3D_RenderTarget* target = (C3D_RenderTarget*)malloc(sizeof(*target));
	if (!target) return NULL;

	target->base.w = height;   /* swap: display width  = buffer height */
	target->base.h = width;    /* swap: display height = buffer width  */
	target->base.tex = SDL_CreateTexture(gd_renderer, SDL_PIXELFORMAT_ABGR8888,
	                                     SDL_TEXTUREACCESS_TARGET,
	                                     target->base.w, target->base.h);
	target->base.screen = 0;
	target->base.side   = 0;
	if (!target->base.tex)
	{
		free(target);
		return NULL;
	}
	SDL_SetTextureBlendMode(target->base.tex, SDL_BLENDMODE_NONE);  /* direct screen copy, alpha-independent */
	return target;
}

void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags)
{
	if (target)
	{
		target->base.screen = (int)screen;
		target->base.side   = (int)side;
	}
	(void)transferFlags;
}

void C3D_RenderTargetDelete(C3D_RenderTarget* target)
{
	if (!target) return;
	/* audit fix: targets registered in g_targets[] must be unregistered
	 * here, or shim_present_frame later copies a FREED SDL texture
	 * (Metal EXC_BAD_ACCESS: MTLResourceListAddResource with garbage). */
	for (int i = 0; i < 4; ++i)
	{
		if (g_targets[i] == target) g_targets[i] = NULL;
	}
	if (target->base.tex) SDL_DestroyTexture(target->base.tex);
	free(target);
}

bool shim_begin_frame(void)
{
	return true;
}

void shim_present_frame(void)
{
	if (!gd_renderer) return;

	SDL_SetRenderTarget(gd_renderer, NULL);
	SDL_SetRenderDrawColor(gd_renderer, 0, 0, 0, 255);
	SDL_RenderClear(gd_renderer);

	/* composite: draw each screen texture into the 400x480 logical window */
	for (int i = 0; i < 4; ++i)
	{
		C3D_RenderTarget* t = g_targets[i];
		if (!t || !t->base.tex) continue;

		SDL_FRect dst;
		if (t->base.screen == GFX_TOP)
		{
			dst.x = 0; dst.y = 0; dst.w = 400; dst.h = 240;
		}
		else /* GFX_BOTTOM */
		{
			dst.x = 40; dst.y = 240; dst.w = 320; dst.h = 240;
		}
		SDL_RenderTexture(gd_renderer, t->base.tex, NULL, &dst);
	}

	SDL_RenderPresent(gd_renderer);

	/* crude ~60 fps cap (menu loops would otherwise spin) */
	static Uint64 last = 0;
	Uint64 now = SDL_GetTicks();
	if (now - last < 15) SDL_Delay((Uint32)(15 - (now - last)));
	last = SDL_GetTicks();
}

shimRenderTarget* shim_get_target_tex(void* target)
{
	return target ? &((C3D_RenderTarget*)target)->base : NULL;
}

/* ------------------------------------------------------------------ */
/* C2D lifecycle / scene / target                                       */
/* ------------------------------------------------------------------ */

bool C2D_Init(u32 maxObjects) { (void)maxObjects; return true; }
void C2D_Fini(void) {}

/* upstream C2D_Prepare arms the vertex batch context (C2DiF_Active +
 * buffer sizes) so C2Di_CheckBufSpace passes for the frame; C2D_Flush
 * dispatches pending vertices and resets the batch counters. */
static void gd_tri_flush(void);

void C2D_Prepare(void)
{
	C2Di_Context* ctx = C2Di_GetContext();
	ctx->vtxBufSize = 4096;   /* per-frame vertex budget (plenty for trails) */
	ctx->idxBufSize = 4096;
	ctx->vtxBufPos  = 0;
	ctx->idxBufPos  = 0;
	ctx->flags |= C2DiF_Active;
}

void C2D_Flush(void)
{
	gd_tri_flush();
	C2Di_Context* ctx = C2Di_GetContext();
	ctx->vtxBufPos = 0;
	ctx->idxBufPos = 0;
}

bool C2D_SceneBegin(C3D_RenderTarget* target)
{
	if (!gd_renderer) return false;

	if (target)
	{
		if ((int)target->base.screen >= 0 && (int)target->base.screen < 4)
			g_targets[target->base.screen] = target;
		SDL_SetTextureBlendMode(target->base.tex, SDL_BLENDMODE_NONE);  /* direct screen copy, alpha-independent */
		SDL_SetRenderTarget(gd_renderer, target->base.tex);
		/* [DEBUG SPAWN] log L3 (target activo por escena) */
		fprintf(stderr, "[DEBUG SPAWN] SceneBegin screen=%d(%s) tex=%p\n",
		        (int)target->base.screen,
		        (int)target->base.screen == GFX_TOP ? "TOP" : "BOTTOM",
		        (void*)target->base.tex);
	}
	else
	{
		SDL_SetRenderTarget(gd_renderer, NULL);
	}
	return true;
}

void C2D_TargetClear(C3D_RenderTarget* target, u32 color)
{
	if (!gd_renderer) return;
	if (target)
	{
		SDL_SetRenderTarget(gd_renderer, target->base.tex);
	}

	u8 a = (u8)((color >> 24) & 0xff);
	u8 b = (u8)((color >> 16) & 0xff);
	u8 g = (u8)((color >>  8) & 0xff);
	u8 r = (u8)(color & 0xff);
	SDL_SetRenderDrawColor(gd_renderer, r, g, b, a);
	SDL_RenderClear(gd_renderer);
}

/* ------------------------------------------------------------------ */
/* View (camera) state                                                  */
/*                                                                     */
/* Representation: out = S*p + o, per axis. citro2d semantics:          */
/*   ViewScale(s)    -> scale INTO the world: S *= s, o stays           */
/*   ViewTranslate(d)-> applied in local (pre-scale) space: o += S*d    */
/* Gameplay check: Scale(0.75) then Translate(0,-10) then drawing an     */
/* area-space y=280 ground lands at 0.75*280 - 7.5 = 202.5 px, and the   */
/* closing Translate(0,+10)/Scale(4/3) restores identity exactly.        */
/* ------------------------------------------------------------------ */

static float g_view[4] = { 1.0f, 1.0f, 0.0f, 0.0f }; /* Sx, Sy, ox, oy */

/* active PICA200 blend state (set by C3D_AlphaBlend, consumed by every
 * draw path: textures, UV geometry, rects) */
static SDL_BlendMode gd_active_blend = SDL_BLENDMODE_BLEND;

void C2D_ViewReset(void)
{
	g_view[0] = 1.0f; g_view[1] = 1.0f; g_view[2] = 0.0f; g_view[3] = 0.0f;
}

void C2D_ViewScale(float scaleX, float scaleY)
{
	g_view[0] *= scaleX;
	g_view[1] *= scaleY;
}

void C2D_ViewTranslate(float translateX, float translateY)
{
	g_view[2] += g_view[0] * translateX;
	g_view[3] += g_view[1] * translateY;
}

/* round-trip storage: gfx.c reads view scale back via mtx->r[0].x */
void C2D_ViewSave(C3D_Mtx* mtx)
{
	if (!mtx) return;
	mtx->r[0].x = g_view[0];
	mtx->r[0].y = g_view[1];
	mtx->r[1].x = g_view[2];
	mtx->r[1].y = g_view[3];
	mtx->r[2].x = 0; mtx->r[2].y = 0; mtx->r[2].z = 0; mtx->r[2].w = 0;
	mtx->r[3].x = 0; mtx->r[3].y = 0; mtx->r[3].z = 0; mtx->r[3].w = 0;
}

void C2D_ViewRestore(const C3D_Mtx* mtx)
{
	if (!mtx) return;
	g_view[0] = mtx->r[0].x;
	g_view[1] = mtx->r[0].y;
	g_view[2] = mtx->r[1].x;
	g_view[3] = mtx->r[1].y;
}

/* ------------------------------------------------------------------ */
/* C3D misc stubs                                                       */
/* ------------------------------------------------------------------ */

void C3D_AlphaBlend(int colorOp, int alphaOp, int colorSrc, int colorDst, int alphaSrc, int alphaDst)
{
	/* Map the PICA200 blend state the game sets (only two combinations are
	 * ever used; see graphics.c change_blending + main.c frame setup):
	 *   alpha/normal: ADD,ADD, SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ONE, ZERO
	 *                 -> dstRGB = src*srcA + dst*(1-srcA)  = SDL_BLENDMODE_BLEND
	 *   additive:     ADD,ADD, SRC_ALPHA, ONE,             ONE, ZERO
	 *                 -> dstRGB = src*srcA + dst           = SDL_BLENDMODE_ADD
	 * UV geometry (SDL_RenderGeometry) and textures honor the per-texture
	 * blend mode; rects honor the renderer draw blend mode, so the active
	 * mode is stamped at draw time in gd_render_sprite / gd_tri_flush /
	 * C2D_DrawRectSolid. */
	(void)colorOp; (void)alphaOp;
	if (colorDst == GPU_ONE && alphaDst == GPU_ZERO && alphaSrc == GPU_ONE)
	{
		gd_active_blend = SDL_BLENDMODE_ADD;
	}
	else
	{
		gd_active_blend = SDL_BLENDMODE_BLEND;
	}
}

void C3D_TexSetFilter(C3D_Tex* tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter)
{
	(void)tex; (void)magFilter; (void)minFilter;
}

void C3D_SetScissor(GPU_SCISSORMODE mode, int left, int top, int right, int bottom)
{
	/* Real scissor implementation (was a phase-2 stub).
	 *
	 * The 3DS framebuffer is transposed; the game (utils/gfx.c set_scissor)
	 * converts a bottom-screen display rect (x,y,w,h) into GPU form as:
	 *   left   = 240 - (y + h)   // Y min, flipped
	 *   right  = 240 - y         // Y max, flipped
	 *   top    = 320 - (x + w)   // X min, flipped (transposed axis)
	 *   bottom = 320 - x         // X max, flipped
	 * (only caller: ui_list draws its panel on the bottom screen).
	 *
	 * SDL3 clip rects (SDL_SetRenderClipRect, verified against installed
	 * SDL3 headers: each render target has its own clip rect) are plain
	 * top-left display rects in the CURRENT target's pixel space, so the
	 * inverse mapping uses the current target's display size.
	 */
	if (!gd_renderer) return;

	if (mode == GPU_SCISSOR_DISABLE)
	{
		SDL_SetRenderClipRect(gd_renderer, NULL);
		return;
	}

	/* locate the current target for its display size */
	int screenW = 0, screenH = 0;
	SDL_Texture* cur = SDL_GetRenderTarget(gd_renderer);
	for (int i = 0; i < 4; ++i)
	{
		if (g_targets[i] && g_targets[i]->base.tex == cur)
		{
			screenW = g_targets[i]->base.w;
			screenH = g_targets[i]->base.h;
			break;
		}
	}
	if (screenW <= 0 || screenH <= 0)
	{
		SDL_SetRenderClipRect(gd_renderer, NULL);  /* window target: no scissor semantics */
		return;
	}

	SDL_Rect clip;
	clip.x = screenW - bottom;              /* display X min  (= original x) */
	clip.y = screenH - right;               /* display Y min (= original y)  */
	clip.w = (screenW - top)    - clip.x;   /* display width  (= original w) */
	clip.h = (screenH - left)   - clip.y;   /* display height (= original h) */

	if (clip.w < 0 || clip.h < 0) clip.w = 0; /* degenerate: keep SDL happy */
	if (clip.h < 0) clip.h = 0;

	/* temp debug (compare against the ui_list panel rect) */
	fprintf(stderr, "[shim][scissor] mode=%d gpu=(l%d t%d r%d b%d) -> clip=(%d,%d %dx%d)\n",
	        (int)mode, left, top, right, bottom, clip.x, clip.y, clip.w, clip.h);

	SDL_SetRenderClipRect(gd_renderer, &clip);
}

static C3D_TexEnv g_tex_envs[6];

C3D_TexEnv* C3D_GetTexEnv(int index)
{
	if (index < 0 || index >= 6) index = 0;
	return &g_tex_envs[index];
}

void C3D_TexEnvInit(C3D_TexEnv* env)          { if (env) memset(env, 0, sizeof(*env)); }
void C3D_TexEnvSrc(C3D_TexEnv* env, int mode, int src0, int src1, int src2)
{
	(void)env; (void)mode; (void)src0; (void)src1; (void)src2;
}
void C3D_TexEnvFunc(C3D_TexEnv* env, int mode, int func) { (void)env; (void)mode; (void)func; }

float C3D_GetProcessingTime(void) { return 0.0f; }
float C3D_GetDrawingTime(void)    { return 0.0f; }
float C3D_GetCmdBufUsage(void)    { return 0.0f; }

/* ------------------------------------------------------------------ */
/* Sprite sheets: .atlas container loader                               */
/* ------------------------------------------------------------------ */

u32 C2D_Color32(u8 r, u8 g, u8 b, u8 a)
{
	return ((u32)(a & 0xff) << 24) | ((u32)(b & 0xff) << 16) |
	       ((u32)(g & 0xff) << 8) | (u32)(r & 0xff);
}

u32 C2D_Color32f(float r, float g, float b, float a)
{
	return C2D_Color32((u8)(r * 255.0f), (u8)(g * 255.0f), (u8)(b * 255.0f), (u8)(a * 255.0f));
}

void C2D_PlainImageTint(C2D_ImageTint* tint, u32 color, float blend)
{
	if (!tint) return;
	for (int i = 0; i < 4; ++i)
	{
		tint->corners[i].color = color;
		tint->corners[i].blend = blend;
	}
}

void C2D_SetTintMode(int mode) { (void)mode; }
/* citro2d real C2D_Fade (c2d/base.h:316): per-fragment blend toward the
 * fade color, applied to every draw until the next C2D_Fade. The shim
 * replicates it by stamping the fade factor onto each draw's colors
 * (sprites via ColorMod, UV geometry via vertex colors, rects via
 * DrawColor) - the game calls draw_fade() at frame start so every draw
 * of the frame gets the factor, same order as the upstream uniform. */
static float gd_fade_r = 0.0f, gd_fade_g = 0.0f, gd_fade_b = 0.0f, gd_fade_a = 0.0f;

static inline float gd_fade_color_factor(float c)
{
	return (1.0f - gd_fade_a) + gd_fade_a * c;
}

bool C2D_Fade(u32 color)
{
	gd_fade_r = (float)(color & 0xff) / 255.0f;
	gd_fade_g = (float)((color >> 8) & 0xff) / 255.0f;
	gd_fade_b = (float)((color >> 16) & 0xff) / 255.0f;
	gd_fade_a = (float)((color >> 24) & 0xff) / 255.0f;
	return true;
}

/* Build a minimal fallback sheet (1x1 transparent) so a missing atlas
 * doesn't crash main.c's svcBreak(USERBREAK_PANIC) path. */
static C2D_SpriteSheet gd_make_fallback_sheet(void)
{
	C2D_SpriteSheet sheet = (C2D_SpriteSheet)calloc(1, sizeof(C2D_SpriteSheet_s));
	if (!sheet) return NULL;

	sheet->numPages = 1;
	sheet->count    = 1;
	snprintf(sheet->name, sizeof(sheet->name), "<fallback>");
	sheet->pageTexs = (C3D_Tex**)calloc(1, sizeof(C3D_Tex*));
	sheet->images   = (C2D_Image*)calloc(1, sizeof(C2D_Image));
	if (!sheet->pageTexs || !sheet->images)
	{
		C2D_SpriteSheetFree(sheet);
		return NULL;
	}
	sheet->images[0].tex = (C3D_Tex*)calloc(1, sizeof(C3D_Tex));
	sheet->pageTexs[0] = sheet->images[0].tex;

	Tex3DS_SubTexture* sub = (Tex3DS_SubTexture*)calloc(1, sizeof(Tex3DS_SubTexture));
	sub->left = 0; sub->top = 0; sub->right = 0; sub->bottom = 0;
	sub->width = 0; sub->height = 0;
	sheet->images[0].subtex = sub;

	if (gd_renderer && sheet->images[0].tex)
	{
		sheet->images[0].tex->sdl = SDL_CreateTexture(gd_renderer,
			SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
		if (sheet->images[0].tex->sdl)
		{
			u32 px = 0; /* transparent pixel */
			SDL_UpdateTexture(sheet->images[0].tex->sdl, NULL, &px, 4);
		}
	}
	return sheet;
}

C2D_SpriteSheet C2D_SpriteSheetLoad(const char* filename)
{
	if (!filename) return gd_make_fallback_sheet();

	/* basename without extension: "romfs:/gfx/sprites.t3x" -> "sprites" */
	const char* sep = strrchr(filename, '/');
	const char* base = sep ? sep + 1 : filename;
	char name[128];
	snprintf(name, sizeof(name), "%s", base);
	char* dot = strrchr(name, '.');
	if (dot) *dot = '\0';
	/* candidates: cwd-relative first, then exe-relative */
	/* SDL3: SDL_GetBasePath returns a cached internal string (see
	 * SDL_filesystem.h, 3.4.16); it must NOT be freed with SDL_free. */
	const char* bp = SDL_GetBasePath();
	char cand[2][1024];
	snprintf(cand[0], sizeof(cand[0]), "platform/sdl/assets_build/%s.atlas", name);
	snprintf(cand[1], sizeof(cand[1]), "%s../platform/sdl/assets_build/%s.atlas",
	         bp ? bp : "./", name);

	const char* chosen = NULL;
	if (file_readable(cand[0])) chosen = cand[0];
	else if (file_readable(cand[1])) chosen = cand[1];

	if (!chosen)
	{
		fprintf(stderr, "[shim] atlas not found for '%s' (run the t3s converter)\n",
		        filename);
		return gd_make_fallback_sheet();
	}

	FILE* f = fopen(chosen, "rb");
	if (!f) return gd_make_fallback_sheet();

	u32 magic   = rd_u32(f);
	u32 version = rd_u32(f);
	u32 numP    = rd_u32(f);
	u32 nent    = rd_u32(f);
	if (magic != ATLAS_MAGIC || version != ATLAS_VERSION || numP == 0 ||
	    numP > 16 || nent == 0)
	{
		fprintf(stderr, "[shim] bad atlas '%s'\n", chosen);
		fclose(f);
		return gd_make_fallback_sheet();
	}

	int pageW[16], pageH[16];
	for (u32 p = 0; p < numP; ++p)
	{
		pageW[p] = (int)rd_u32(f);
		pageH[p] = (int)rd_u32(f);
		/* audit fix: reject non-positive page dims found in corrupt files */
		if (pageW[p] <= 0 || pageH[p] <= 0)
		{
			fprintf(stderr, "[shim] atlas '%s': bad page dims\n", chosen);
			fclose(f);
			return gd_make_fallback_sheet();
		}
	}

	struct AtlEntry { u32 page; s32 x, y, w, h; };
	struct AtlEntry* ents = (struct AtlEntry*)calloc(nent, sizeof(*ents));
	Tex3DS_SubTexture* subtexs = (Tex3DS_SubTexture*)calloc(nent, sizeof(Tex3DS_SubTexture));
	C2D_SpriteSheet sheet = (C2D_SpriteSheet)calloc(1, sizeof(C2D_SpriteSheet_s));
	if (!ents || !subtexs || !sheet)
	{
		free(ents); free(subtexs); free(sheet);
		fclose(f);
		return gd_make_fallback_sheet();
	}

	for (u32 i = 0; i < nent; ++i)
	{
		ents[i].page = rd_u16(f);
		ents[i].x = (s32)rd_u16(f);
		ents[i].y = (s32)rd_u16(f);
		ents[i].w = (s32)rd_u16(f);
		ents[i].h = (s32)rd_u16(f);
	}

	sheet->numPages = numP;
	sheet->count    = nent;
	snprintf(sheet->name, sizeof(sheet->name), "%s", name);
	sheet->pageTexs = (C3D_Tex**)calloc(numP, sizeof(C3D_Tex*));
	sheet->images   = (C2D_Image*)calloc(nent, sizeof(C2D_Image));
	if (!sheet->pageTexs || !sheet->images)
	{
		free(ents); free(subtexs); C2D_SpriteSheetFree(sheet);
		fclose(f);
		return gd_make_fallback_sheet();
	}

	for (u32 i = 0; i < nent; ++i)
	{
		subtexs[i].left   = (float)ents[i].x;
		subtexs[i].top    = (float)ents[i].y;
		subtexs[i].right  = (float)(ents[i].x + ents[i].w);
		subtexs[i].bottom = (float)(ents[i].y + ents[i].h);
		subtexs[i].width  = (u16)ents[i].w;
		subtexs[i].height = (u16)ents[i].h;
		sheet->images[i].subtex = &subtexs[i];
	}

	/* one texture per page; pages' pixel data is concatenated in order.
	 * audit fix: a page whose pixel data fails to load used to leave an
	 * UNINITIALIZED SDL streaming texture bound to sprites; sampling that
	 * on an SDL3/Metal renderer is undefined and matches the EXC_BAD_ACCESS
	 * in MTLResourceListAddResource. Skip binding such textures. */
	for (u32 p = 0; p < numP; ++p)
	{
		sheet->pageTexs[p] = (C3D_Tex*)calloc(1, sizeof(C3D_Tex));
		if (!sheet->pageTexs[p]) continue;
		if (!gd_renderer) continue;

		SDL_Texture* tex = SDL_CreateTexture(gd_renderer, SDL_PIXELFORMAT_ABGR8888,
		                                     SDL_TEXTUREACCESS_STREAMING,
		                                     pageW[p], pageH[p]);
		if (!tex) continue;

		size_t sz = (size_t)pageW[p] * (size_t)pageH[p] * 4;
		void* pixels = malloc(sz);
		if (pixels && fread(pixels, 1, sz, f) == sz)
		{
			SDL_UpdateTexture(tex, NULL, pixels, pageW[p] * 4);
			SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
			sheet->pageTexs[p]->sdl = tex;      /* bound only when fully loaded */
		}
		else
		{
			fprintf(stderr, "[shim] atlas '%s': short pixel data on page %u\n",
			        chosen, (unsigned)p);
			SDL_DestroyTexture(tex);
		}
		free(pixels);
	}

	for (u32 i = 0; i < nent; ++i)
	{
		u32 pg = ents[i].page;
		if (pg >= numP) pg = 0;
		sheet->images[i].tex = sheet->pageTexs[pg];
	}

	free(ents);
	fclose(f);
	return sheet;
}

void C2D_SpriteSheetFree(C2D_SpriteSheet sheet)
{
	if (!sheet) return;
	for (u32 p = 0; p < sheet->numPages; ++p)
	{
		if (!sheet->pageTexs[p]) continue;
		if (sheet->pageTexs[p]->sdl) SDL_DestroyTexture(sheet->pageTexs[p]->sdl);
		free(sheet->pageTexs[p]);
	}
	free(sheet->pageTexs);
	if (sheet->images)
	{
		/* subtex block is one allocation sized to count */
		free((void*)sheet->images[0].subtex);
		free(sheet->images);
	}
	free(sheet);
}

C2D_Image C2D_SpriteSheetGetImage(C2D_SpriteSheet sheet, u32 index)
{
	if (sheet && index < sheet->count) return sheet->images[index];
	/* [DEBUG SPAWN] log #8c (índice fuera de rango: causa de invisibilidad muta) */
	fprintf(stderr, "[DEBUG SPAWN] GetImage OOR: sheet=%s count=%u idx=%u -> imagen vacia\n",
	        sheet ? sheet->name : "(null sheet)", sheet ? (unsigned)sheet->count : 0u,
	        (unsigned)index);
	static C2D_Image empty = { NULL, NULL };
	return empty;
}

bool C2D_SpriteFromSheet(C2D_Sprite* sprite, C2D_SpriteSheet sheet, u32 index)
{
	if (!sprite) return false;
	sprite->image = C2D_SpriteSheetGetImage(sheet, index);
	sprite->params.pos.x = 0; sprite->params.pos.y = 0;
	/* citro2d real default (c2d/sprite.h:28): anchor = top-left (0,0).
	 * Sprites that want to be centered call C2D_SpriteSetCenter explicitly. */
	sprite->params.center.x = 0.0f; sprite->params.center.y = 0.0f;
	sprite->params.scale.x = 1; sprite->params.scale.y = 1;
	sprite->params.angle = 0;
	sprite->params.depth = 0;
	return sprite->image.tex != NULL;
}

bool C2D_SpriteFromImage(C2D_Sprite* sprite, C2D_Image image)
{
	if (!sprite) return false;
	sprite->image = image;
	sprite->params.pos.x = 0; sprite->params.pos.y = 0;
	/* citro2d real default (c2d/sprite.h:28): anchor = top-left (0,0) */
	sprite->params.center.x = 0.0f; sprite->params.center.y = 0.0f;
	sprite->params.scale.x = 1; sprite->params.scale.y = 1;
	sprite->params.angle = 0;
	sprite->params.depth = 0;
	return sprite->image.tex != NULL;
}

bool C2D_SpriteSetPos(C2D_Sprite* sprite, float x, float y)
{
	sprite->params.pos.x = x;
	sprite->params.pos.y = y;
	return true;
}

bool C2D_SpriteSetCenter(C2D_Sprite* sprite, float x, float y)
{
	sprite->params.center.x = x;
	sprite->params.center.y = y;
	return true;
}

bool C2D_SpriteSetScale(C2D_Sprite* sprite, float x, float y)
{
	sprite->params.scale.x = x;
	sprite->params.scale.y = y;
	return true;
}

bool C2D_SpriteSetRotation(C2D_Sprite* sprite, float angle)
{
	sprite->params.angle = angle;
	return true;
}

bool C2D_SpriteSetRotationDegrees(C2D_Sprite* sprite, float angle)
{
	return C2D_SpriteSetRotation(sprite, C3D_AngleFromDegrees(angle));
}

bool C2D_SpriteRotate(C2D_Sprite* sprite, float angle)
{
	sprite->params.angle += angle;
	return true;
}

/* ------------------------------------------------------------------ */
/* Drawing                                                              */
/* ------------------------------------------------------------------ */

static void gd_apply_view(float x, float y, float* outX, float* outY)
{
	*outX = g_view[0] * x + g_view[2];
	*outY = g_view[1] * y + g_view[3];
}

/* Shared sprite blit honoring the citro2d params + tint color. */
static bool gd_render_sprite(C3D_Tex* tex, const Tex3DS_SubTexture* subtex,
                             float px, float py, float cx, float cy,
                             float sx, float sy, float angleRad,
                             u32 tintColor)
{
	if (!gd_renderer || !tex || !tex->sdl || !subtex) return false;
	if (!subtex->width || !subtex->height) return false;

	SDL_FlipMode flip = SDL_FLIP_NONE;
	if (sx < 0.0f) { flip = (SDL_FlipMode)(flip | SDL_FLIP_HORIZONTAL); sx = -sx; }
	if (sy < 0.0f) { flip = (SDL_FlipMode)(flip | SDL_FLIP_VERTICAL);   sy = -sy; }

	float dw = (float)subtex->width  * sx * g_view[0];
	float dh = (float)subtex->height * sy * g_view[1];
	if (dw <= 0.0f || dh <= 0.0f) return false;

	float vx, vy;
	gd_apply_view(px, py, &vx, &vy);

	SDL_FRect src;
	src.x = subtex->left;
	src.y = subtex->top;
	src.w = (float)subtex->width;
	src.h = (float)subtex->height;

	SDL_FRect dst;
	dst.x = vx - (cx * dw);
	dst.y = vy - (cy * dh);
	dst.w = dw;
	dst.h = dh;

	SDL_FPoint center;
	center.x = cx * dw;
	center.y = cy * dh;

	/* [DEBUG SPAWN] log L2 (blit: target/subtex/dst/view) */
	static int gameplay_dump = 0;
	bool in_gameplay = (g_view[0] != 1.0f || g_view[1] != 1.0f ||
	                    g_view[2] != 0.0f || g_view[3] != 0.0f); /* view del gameplay (Scale 0.75) */
	if (in_gameplay && gameplay_dump++ < 400)
	{
		const char* tgt = "WINDOW";
		SDL_Texture* cur = SDL_GetRenderTarget(gd_renderer);
		for (int i = 0; i < 4; ++i)
		{
			if (g_targets[i] && g_targets[i]->base.tex == cur)
			{
				tgt = (i == GFX_TOP) ? "TOP" : "BOTTOM";
				break;
			}
		}
		fprintf(stderr,
		        "[DEBUG SPAWN] render: tgt=%s tex=%s subtex[%.0f..%.0f x %.0f..%.0f] dst=(%.1f,%.1f %.1fx%.1f) center=(%.1f,%.1f) view=(%.2f,%.2f,%.1f,%.1f) tint=0x%08x\n",
		        tgt, tex->sdl ? "OK" : "NULL",
		        subtex->left, subtex->right, subtex->top, subtex->bottom,
		        dst.x, dst.y, dst.w, dst.h, center.x, center.y,
		        g_view[0], g_view[1], g_view[2], g_view[3], tintColor);
	}

	SDL_SetTextureColorMod(tex->sdl,
	                       (Uint8)((tintColor >>  0) & 0xff) * gd_fade_color_factor(gd_fade_r),
	                       (Uint8)((tintColor >>  8) & 0xff) * gd_fade_color_factor(gd_fade_g),
	                       (Uint8)((tintColor >> 16) & 0xff) * gd_fade_color_factor(gd_fade_b));
	SDL_SetTextureAlphaMod(tex->sdl, (Uint8)((tintColor >> 24) & 0xff));
	SDL_SetTextureBlendMode(tex->sdl, gd_active_blend);   /* change_blending state */

	return SDL_RenderTextureRotated(gd_renderer, tex->sdl, &src, &dst,
	                                (double)(angleRad * 180.0f /
	                                         3.14159265358979323846f),
	                                &center, flip);
}

bool C2D_DrawSprite(const C2D_Sprite* sprite)
{
	if (!sprite) return false;
	u32 tint = 0xFFFFFFFF;
	return gd_render_sprite(sprite->image.tex, sprite->image.subtex,
	                        sprite->params.pos.x, sprite->params.pos.y,
	                        sprite->params.center.x, sprite->params.center.y,
	                        sprite->params.scale.x, sprite->params.scale.y,
	                        sprite->params.angle, tint);
}

bool C2D_DrawSpriteTinted(const C2D_Sprite* sprite, const C2D_ImageTint* tint)
{
	if (!sprite) return false;
	u32 tint_c = tint ? tint->corners[0].color : 0xFFFFFFFF;
	return gd_render_sprite(sprite->image.tex, sprite->image.subtex,
	                        sprite->params.pos.x, sprite->params.pos.y,
	                        sprite->params.center.x, sprite->params.center.y,
	                        sprite->params.scale.x, sprite->params.scale.y,
	                        sprite->params.angle, tint_c);
}

bool C2D_DrawImageAtRotated(C2D_Image image, float x, float y, float depth, float angle,
                            const C2D_ImageTint* tint, float scaleX, float scaleY)
{
	(void)depth;
	u32 tint_c = tint ? tint->corners[0].color : 0xFFFFFFFF;
	return gd_render_sprite(image.tex, image.subtex,
	                        x, y, 0.5f, 0.5f, scaleX, scaleY, angle, tint_c);
}

bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 color)
{
	(void)z;
	if (!gd_renderer) return false;
	SDL_FRect r = { x, y, w, h };
	Uint8 a = (Uint8)((color >> 24) & 0xff);
	Uint8 b = (Uint8)((color >> 16) & 0xff);
	Uint8 g = (Uint8)((color >>  8) & 0xff);
	Uint8 r8 = (Uint8)(color & 0xff);
	SDL_SetRenderDrawBlendMode(gd_renderer, gd_active_blend);  /* change_blending state */
	SDL_SetRenderDrawColor(gd_renderer,
	                       (Uint8)(r8 * gd_fade_color_factor(gd_fade_r)),
	                       (Uint8)(g  * gd_fade_color_factor(gd_fade_g)),
	                       (Uint8)(b  * gd_fade_color_factor(gd_fade_b)),
	                       a);
	return SDL_RenderFillRect(gd_renderer, &r);
}

bool C2D_DrawRectangle(float x, float y, float z, float w, float h,
                       u32 c1, u32 c2, u32 c3, u32 c4)
{
	/* citro2d real (c2d/base.h:430): corner order is clr0=top-left,
	 * clr1=top-right, clr2=bottom-left, clr3=bottom-right. Rendered as
	 * a colored quad; SDL_RenderGeometry interpolates color across the
	 * 2 triangles automatically (verified: SDL3 headers SDL_render.h
	 * SDL_Vertex{position, color, tex_coord}). Same fade + blend mode
	 * treatment as the other draw paths. */
	(void)z;
	if (!gd_renderer) return false;
	if (w <= 0 || h <= 0) return false;

	SDL_Vertex verts[4];
	const float xs[2] = { x, x + w };
	const float ys[2] = { y, y + h };
	const u32 clrs[4] = { c1, c2, c3, c4 };   /* TL, TR, BL, BR */
	int vi = 0;
	for (int cy = 0; cy < 2; ++cy)
	{
		for (int cx = 0; cx < 2; ++cx, ++vi)
		{
			u32 c = clrs[vi];
			verts[vi].position.x = xs[cx];
			verts[vi].position.y = ys[cy];
			verts[vi].color.r = (float)(c & 0xff) / 255.0f * gd_fade_color_factor(gd_fade_r);
			verts[vi].color.g = (float)((c >> 8) & 0xff) / 255.0f * gd_fade_color_factor(gd_fade_g);
			verts[vi].color.b = (float)((c >> 16) & 0xff) / 255.0f * gd_fade_color_factor(gd_fade_b);
			verts[vi].color.a = (float)((c >> 24) & 0xff) / 255.0f;
			verts[vi].tex_coord.x = 0.0f;
			verts[vi].tex_coord.y = 0.0f;
		}
	}
	const int idx[6] = { 0, 1, 2, 2, 1, 3 };  /* TL,TR,BL / BL,TR,BR: same winding as upstream (ADD ops) */
	SDL_SetRenderDrawBlendMode(gd_renderer, gd_active_blend);
	return SDL_RenderGeometry(gd_renderer, NULL, verts, 4, idx, 6);
}

/* remaining primitive path still stubbed (scope) */
bool C2D_DrawLine(float x1, float y1, u32 clr1, float x2, float y2, u32 clr2, float thickness, u32 clr3)
{
	(void)x1; (void)y1; (void)clr1; (void)x2; (void)y2; (void)clr2; (void)thickness; (void)clr3;
	return true;
}

bool C2D_DrawTriangle(float x1, float y1, u32 clr1, float x2, float y2, u32 clr2,
                      float x3, float y3, u32 clr3, float depth)
{
	(void)x1; (void)y1; (void)clr1; (void)x2; (void)y2; (void)clr2; (void)x3; (void)y3; (void)clr3; (void)depth;
	return true;
}

/* ------------------------------------------------------------------ */
/* C2Di internals: REAL vertex path backing the project's custom        */
/* C2D_DrawTriangleUV (utils/gfx.c:372). citro2d upstream has no        */
/* DrawTriangleUV: it feeds C2Di_AppendVtx/AppendTri, whose vertices    */
/* are transformed by projMtx*mdlvMtx - i.e. the SAME view transform    */
/* as sprites (C2D_ViewScale/Translate), so gd_apply_view applies.      */
/*                                                                      */
/* Semantics replicated from the vendored internals flow:               */
/*   game call order: C2Di_SetTex(img.tex) -> C2Di_Update ->            */
/*   C2Di_AppendTri -> C2Di_AppendVtx x3. The texture comes EXPLICITLY  */
/*   from the passed C2D_Image (ctx->curTex), not from a sheet global.  */
/*   Per-vertex u32 color is ABGR (a<<24) like C2D_Color32; SDL3's      */
/*   SDL_RenderGeometry modulates texture color per vertex exactly      */
/*   like C2D_TintMult.                                                 */
/*   ptx/pty (ProcTex) are constants (0,1) in every game call; the      */
/*   upstream text-ProcTex path is unused by the game -> ignored.       */
/* ------------------------------------------------------------------ */

C2Di_Context __C2Di_Context = { 0 };

typedef struct
{
	SDL_FPoint pos;
	SDL_FColor col;
	SDL_FPoint uv;
} GdTriVtx;

static GdTriVtx gd_tri_vtx[3];
static int gd_tri_n = 0;

static void gd_tri_flush(void)
{
	/* the pending triangle is flushed as soon as its 3rd vertex lands */
	if (!gd_renderer || gd_tri_n < 3) return;

	C2Di_Context* ctx = C2Di_GetContext();
	if (!ctx->curTex || !ctx->curTex->sdl)
	{
		gd_tri_n = 0;   /* curTex unbound (fallback sheet): drop silently */
		return;
	}

	SDL_Vertex verts[3];
	for (int i = 0; i < 3; ++i)
	{
		verts[i].position  = gd_tri_vtx[i].pos;
		verts[i].color     = gd_tri_vtx[i].col;
		verts[i].tex_coord = gd_tri_vtx[i].uv;
	}
	SDL_SetTextureBlendMode(ctx->curTex->sdl, gd_active_blend);  /* change_blending state */
	SDL_RenderGeometry(gd_renderer, ctx->curTex->sdl, verts, 3, NULL, 0);
	gd_tri_n = 0;
}

void C2Di_Update(void) {}

void C2Di_AppendTri(void)
{
	C2Di_Context* ctx = C2Di_GetContext();
	/* upstream appends 3 indices pointing at the vertices appended right
	 * after this call; the immediate shim renders at the 3rd vertex */
	if (ctx->idxBufPos + 3 <= ctx->idxBufSize) ctx->idxBufPos += 3;
	else gd_tri_n = 0;
}

void C2Di_CalcQuad(C2Di_Quad* quad, const C2D_DrawParams* params) { (void)quad; (void)params; }

void C2Di_AppendVtx(float x, float y, float z, float u, float v, float ptx, float pty, u32 color)
{
	C2Di_Context* ctx = C2Di_GetContext();
	if (ctx->vtxBufPos < ctx->vtxBufSize) ctx->vtxBufPos++;
	if (gd_tri_n >= 3) return;

	/* SAME view transform as gd_render_sprite (citro2d applies mdlvMtx) */
	SDL_FPoint vp;
	gd_apply_view(x, y, &vp.x, &vp.y);

	SDL_FColor col;
	col.r = (float)(color & 0xff) / 255.0f * gd_fade_color_factor(gd_fade_r);
	col.g = (float)((color >> 8) & 0xff) / 255.0f * gd_fade_color_factor(gd_fade_g);
	col.b = (float)((color >> 16) & 0xff) / 255.0f * gd_fade_color_factor(gd_fade_b);
	col.a = (float)((color >> 24) & 0xff) / 255.0f;

	gd_tri_vtx[gd_tri_n].pos = vp;
	gd_tri_vtx[gd_tri_n].col = col;
	gd_tri_vtx[gd_tri_n].uv.x = u;
	gd_tri_vtx[gd_tri_n].uv.y = v;
	gd_tri_n++;

	if (gd_tri_n == 3) gd_tri_flush();

	(void)z; (void)ptx; (void)pty;   /* ProcTex constants, unused */
}

void C2Di_FlushVtxBuf(void)
{
	gd_tri_flush();
}
