#pragma once
/*
 * citro2d.h - SDL2 desktop compatibility shim (phase 1).
 *
 * Struct layouts guarded by direct field access in project code:
 *   - C2D_Image.tex / C2D_Image.subtex
 *   - Tex3DS_SubTexture.{left,top,right,bottom,width,height}
 *   - C2D_Sprite.image (embedded by value in Sprite/SpriteObject/UI structs)
 *
 * Drawing entry points exist but are silent no-ops in phase 1.
 * Scene/Target/View handled over SDL render targets.
 * NOTE: C2D_CreateScreenTargetExt and C2D_DrawTriangleUV are declared by
 * the project itself (utils/gfx.h) and are NOT part of the shim.
 */

#include <3ds.h>
#include "citro3d.h"

typedef struct Tex3DS_SubTexture_s
{
	float left, top, right, bottom;
	u16   width, height;
} Tex3DS_SubTexture;

/* tex3ds helper used by trail.c */
static inline bool Tex3DS_SubTextureRotated(const Tex3DS_SubTexture* subtex)
{
	return subtex->bottom < subtex->top;
}

typedef struct C2D_Image
{
	C3D_Tex* tex;
	const Tex3DS_SubTexture* subtex;
} C2D_Image;

typedef struct { u32 color; float blend; } C2D_Tint;
typedef struct { C2D_Tint corners[4]; } C2D_ImageTint;

typedef struct { float x, y; } C2D_Vector;

typedef struct C2D_DrawParams
{
	C2D_Vector pos;
	C2D_Vector wh;
	C2D_Vector center;
	float angle;
	float depth;
} C2D_DrawParams;

typedef struct C2D_SpriteParams
{
	C2D_Vector pos;
	C2D_Vector center;
	C2D_Vector scale;
	float angle;
	float depth;
} C2D_SpriteParams;

typedef struct C2D_Sprite
{
	C2D_Image image;
	C2D_SpriteParams params;
} C2D_Sprite;

/* used through pointers only (vendored C2D_Font_s struct) */
typedef struct CFNT_s CFNT_s;

typedef struct C2D_SpriteSheet_s
{
	u32 numPages;               /* atlas pages (SDL textures: shim-private) */
	u32 count;                  /* number of sprite entries = t3s file lines */
	C3D_Tex** pageTexs;         /* numPages textures, per atlas page */
	C2D_Image* images;          /* count entries: tex + subtex per index */
	char name[32];              /* sheet basename (shim-private, debug logs) */
} C2D_SpriteSheet_s;

typedef C2D_SpriteSheet_s* C2D_SpriteSheet;

enum
{
	C2D_TintNone   = 0,
	C2D_TintMult   = 1,
	C2D_TintGrowth = 2,
};

/* lifecycle */
bool C2D_Init(u32 maxObjects);
void C2D_Fini(void);
void C2D_Prepare(void);
void C2D_Flush(void);

/* scene / target */
bool C2D_SceneBegin(C3D_RenderTarget* target);
void C2D_TargetClear(C3D_RenderTarget* target, u32 color);

/* view (camera) state */
void C2D_ViewReset(void);
void C2D_ViewScale(float scaleX, float scaleY);
void C2D_ViewTranslate(float translateX, float translateY);
void C2D_ViewSave(C3D_Mtx* mtx);
void C2D_ViewRestore(const C3D_Mtx* mtx);

/* color helpers */
u32  C2D_Color32(u8 r, u8 g, u8 b, u8 a);
u32  C2D_Color32f(float r, float g, float b, float a);

/* tint */
void C2D_PlainImageTint(C2D_ImageTint* tint, u32 color, float blend);
void C2D_SetTintMode(int mode);

/* fade overlay */
bool C2D_Fade(u32 color);   /* citro2d real: returns bool (base.h:316) */

/* sprite sheets: phase 1 returns a valid handle with a dummy image */
C2D_SpriteSheet C2D_SpriteSheetLoad(const char* filename);
void            C2D_SpriteSheetFree(C2D_SpriteSheet sheet);
C2D_Image       C2D_SpriteSheetGetImage(C2D_SpriteSheet sheet, u32 index);

/* sprites */
bool C2D_SpriteFromSheet(C2D_Sprite* sprite, C2D_SpriteSheet sheet, u32 index);
bool C2D_SpriteFromImage(C2D_Sprite* sprite, C2D_Image image);
bool C2D_SpriteSetPos(C2D_Sprite* sprite, float x, float y);
bool C2D_SpriteSetCenter(C2D_Sprite* sprite, float x, float y);
bool C2D_SpriteSetScale(C2D_Sprite* sprite, float x, float y);
bool C2D_SpriteSetRotation(C2D_Sprite* sprite, float angle);
bool C2D_SpriteSetRotationDegrees(C2D_Sprite* sprite, float angle);
bool C2D_SpriteRotate(C2D_Sprite* sprite, float angle);

/* draws: silent no-ops in phase 1 (layouts kept in shim for later) */
bool C2D_DrawSprite(const C2D_Sprite* sprite);
bool C2D_DrawSpriteTinted(const C2D_Sprite* sprite, const C2D_ImageTint* tint);
bool C2D_DrawImageAtRotated(C2D_Image image, float x, float y, float depth, float angle,
                            const C2D_ImageTint* tint, float scaleX, float scaleY);
bool C2D_DrawLine(float x1, float y1, u32 clr1, float x2, float y2, u32 clr2, float thickness, u32 clr3);
bool C2D_DrawTriangle(float x1, float y1, u32 clr1, float x2, float y2, u32 clr2,
                      float x3, float y3, u32 clr3, float depth);
bool C2D_DrawRectSolid(float x, float y, float z, float w, float h, u32 color);
bool C2D_DrawRectangle(float x, float y, float z, float w, float h,
                       u32 c1, u32 c2, u32 c3, u32 c4);
