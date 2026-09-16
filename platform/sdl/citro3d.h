#pragma once
/*
 * citro3d.h - SDL2 desktop compatibility shim (phase 1).
 * Types match the layouts the project touches directly:
 *   - C3D_Mtx.r[0].x (utils/gfx.c stereo shift readback)
 *   - GPU_ and GX_ constants passed to target/blending code
 * All functions are no-ops except target lifecycle, which wraps SDL
 * render targets; C3D_FrameEnd composites + presents.
 */

#include <3ds.h>

/* GPU enums (values compatible with libctru names, unused numerically) */
typedef enum { GPU_NEAREST = 0, GPU_LINEAR = 1 } GPU_TEXTURE_FILTER_PARAM;
typedef enum { GPU_SCISSOR_DISABLE = 0, GPU_SCISSOR_NORMAL = 1 } GPU_SCISSORMODE;

enum
{
	GPU_ZERO = 0,
	GPU_ONE,
	GPU_SRC_COLOR,
	GPU_ONE_MINUS_SRC_COLOR,
	GPU_SRC_ALPHA,
	GPU_ONE_MINUS_SRC_ALPHA,
	GPU_DST_COLOR,
	GPU_ONE_MINUS_DST_COLOR,
};

enum
{
	GPU_BLEND_ADD = 0,
	GPU_BLEND_SUBTRACT,
	GPU_BLEND_REVERSE_SUBTRACT,
	GPU_BLEND_MIN,
	GPU_BLEND_MAX,
};

enum
{
	GPU_PREVIOUS = 0,
	GPU_TEXTURE0 = 1,
	GPU_CONSTANT = 3,
};

enum
{
	GPU_MODULATE = 0,
	GPU_REPLACE,
	GPU_ADD,
};

enum
{
	GPU_RB_RGBA8 = 0,
	GPU_RB_DEPTH16 = 0,
};

/* GX transfer flags (only pattern matching matters in phase 1: stubs ignore) */
#define GX_TRANSFER_FLIP_VERT(x)   ((x) << 24)
#define GX_TRANSFER_OUT_TILED(x)   ((x) << 25)
#define GX_TRANSFER_RAW_COPY(x)    ((x) << 26)
#define GX_TRANSFER_IN_FORMAT(x)   ((x) << 15)
#define GX_TRANSFER_OUT_FORMAT(x)  ((x) << 18)
#define GX_TRANSFER_SCALING(x)     ((x) << 24)

#define GX_TRANSFER_FMT_RGBA8 0
#define GX_TRANSFER_FMT_RGB8  1

#define GX_TRANSFER_SCALE_NO 0
#define GX_TRANSFER_SCALE_X  1
#define GX_TRANSFER_SCALE_XY 2

typedef struct C3D_FVec
{
	float x, y, z, w;
} C3D_FVec;

/* Row access (gfx.c reads view scale via mtx->r[0].x) */
typedef union C3D_Mtx
{
	float m[16];
	C3D_FVec r[4];
} C3D_Mtx;

/* C3D_Tex: only used through pointers (img.tex, C2Di ctx->curTex) */
typedef struct C3D_Tex C3D_Tex;

/* Vendored c2d_internal.h embeds these by value; dummy layouts suffice */
typedef struct { u32 _flags; } C3D_AttrInfo;
typedef struct { u32 _flags; } C3D_BufInfo;
typedef struct { u32 _config; } C3D_ProcTex;
typedef struct { u32 _lut[32]; } C3D_ProcTexLut;
typedef struct { u32 _state[13]; } C3D_TexEnv;

typedef struct DVLB_s DVLB_s;
typedef struct { u32 _prog; } shaderProgram_s;

typedef struct C3D_RenderTarget_s C3D_RenderTarget;

bool C3D_Init(size_t commandBufferSize);
void C3D_Fini(void);
bool C3D_FrameBegin(u8 flags);
void C3D_FrameEnd(u8 flags);
#define C3D_FRAME_SYNCDRAW 0

C3D_RenderTarget* C3D_RenderTargetCreate(int width, int height, int colorFormat, int depthFormat);
void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32 transferFlags);
void C3D_RenderTargetDelete(C3D_RenderTarget* target);

void C3D_AlphaBlend(int colorOp, int alphaOp, int colorSrc, int colorDst, int alphaSrc, int alphaDst);
void C3D_TexSetFilter(C3D_Tex* tex, GPU_TEXTURE_FILTER_PARAM magFilter, GPU_TEXTURE_FILTER_PARAM minFilter);
void C3D_SetScissor(GPU_SCISSORMODE mode, int left, int top, int right, int bottom);

C3D_TexEnv* C3D_GetTexEnv(int index);
void C3D_TexEnvInit(C3D_TexEnv* env);
void C3D_TexEnvSrc(C3D_TexEnv* env, int mode, int src0, int src1, int src2);
void C3D_TexEnvFunc(C3D_TexEnv* env, int mode, int func);
#define C3D_Alpha 1
#define C3D_RGB   0

float C3D_GetProcessingTime(void);
float C3D_GetDrawingTime(void);
float C3D_GetCmdBufUsage(void);
#define C3D_DEFAULT_CMDBUF_SIZE 0x40000

/* Angles: project code feeds the result to sinf/cosf -> radians */
static inline float C3D_AngleFromDegrees(float degrees)
{
	return degrees * 3.14159265358979323846f / 180.0f;
}

/* libctru angle type (not needed yet) */
