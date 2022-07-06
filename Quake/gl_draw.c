/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

// draw.c -- 2d drawing

#include "quakedef.h"

//extern unsigned char d_15to8table[65536]; //johnfitz -- never used

cvar_t		scr_conalpha = {"scr_conalpha", "0.5", CVAR_ARCHIVE}; //johnfitz

qpic_t		*draw_disc;
qpic_t		*draw_backtile;

gltexture_t *char_texture; //johnfitz
qpic_t		*pic_ovr, *pic_ins; //johnfitz -- new cursor handling
qpic_t		*pic_nul; //johnfitz -- for missing gfx, don't crash

//johnfitz -- new pics
byte pic_ovr_data[8][8] =
{
	{255,255,255,255,255,255,255,255},
	{255, 15, 15, 15, 15, 15, 15,255},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255, 15, 15, 15, 15, 15, 15,  2},
	{255,255,  2,  2,  2,  2,  2,  2},
};

byte pic_ins_data[9][8] =
{
	{ 15, 15,255,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{ 15, 15,  2,255,255,255,255,255},
	{255,  2,  2,255,255,255,255,255},
};

byte pic_nul_data[8][8] =
{
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{252,252,252,252,  0,  0,  0,  0},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
	{  0,  0,  0,  0,252,252,252,252},
};

byte pic_stipple_data[8][8] =
{
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
	{255,  0,  0,  0,255,  0,  0,  0},
	{  0,  0,255,  0,  0,  0,255,  0},
};

byte pic_crosshair_data[8][8] =
{
	{255,255,255,255,255,255,255,255},
	{255,255,255,  8,  9,255,255,255},
	{255,255,255,  6,  8,  2,255,255},
	{255,  6,  8,  8,  6,  8,  8,255},
	{255,255,  2,  8,  8,  2,  2,  2},
	{255,255,255,  7,  8,  2,255,255},
	{255,255,255,255,  2,  2,255,255},
	{255,255,255,255,255,255,255,255},
};
//johnfitz

typedef struct
{
	gltexture_t *gltexture;
	float		sl, tl, sh, th;
} glpic_t;

typedef struct guivertex_t {
	float		pos[2];
	float		uv[2];
	GLubyte		color[4];
} guivertex_t;

#define MAX_BATCH_QUADS 2048

static int numbatchquads = 0;
static guivertex_t batchverts[4 * MAX_BATCH_QUADS];
static GLushort batchindices[6 * MAX_BATCH_QUADS];

glcanvas_t glcanvas;

//==============================================================================
//
//  PIC CACHING
//
//==============================================================================

typedef struct cachepic_s
{
	char		name[MAX_QPATH];
	qpic_t		pic;
	byte		padding[32];	// for appended glpic
} cachepic_t;

#define	MAX_CACHED_PICS		512	//Spike -- increased to avoid csqc issues.
cachepic_t	menu_cachepics[MAX_CACHED_PICS];
int			menu_numcachepics;

byte		menuplyr_pixels[4096];

//  scrap allocation
//  Allocate all the little status bar obejcts into a single texture
//  to crutch up stupid hardware / drivers

#define	MAX_SCRAPS		2
#define	BLOCK_WIDTH		256
#define	BLOCK_HEIGHT	256
#define	SCRAP_PADDING	1

int			scrap_allocated[MAX_SCRAPS][BLOCK_WIDTH];
byte		scrap_texels[MAX_SCRAPS][BLOCK_WIDTH*BLOCK_HEIGHT]; //johnfitz -- removed *4 after BLOCK_HEIGHT
qboolean	scrap_dirty;
gltexture_t	*scrap_textures[MAX_SCRAPS]; //johnfitz
gltexture_t	*winquakemenubg;


/*
================
Scrap_AllocBlock

returns an index into scrap_texnums[] and the position inside it
================
*/
int Scrap_AllocBlock (int w, int h, int *x, int *y)
{
	int		i, j;
	int		best, best2;
	int		texnum;

	for (texnum=0 ; texnum<MAX_SCRAPS ; texnum++)
	{
		best = BLOCK_HEIGHT;

		for (i=0 ; i<BLOCK_WIDTH-w ; i++)
		{
			best2 = 0;

			for (j=0 ; j<w ; j++)
			{
				if (scrap_allocated[texnum][i+j] >= best)
					break;
				if (scrap_allocated[texnum][i+j] > best2)
					best2 = scrap_allocated[texnum][i+j];
			}
			if (j == w)
			{	// this is a valid spot
				*x = i;
				*y = best = best2;
			}
		}

		if (best + h > BLOCK_HEIGHT)
			continue;

		for (i=0 ; i<w ; i++)
			scrap_allocated[texnum][*x + i] = best + h;

		return texnum;
	}

	Sys_Error ("Scrap_AllocBlock: full"); //johnfitz -- correct function name
	return 0; //johnfitz -- shut up compiler
}

/*
================
Scrap_Upload -- johnfitz -- now uses TexMgr
================
*/
void Scrap_Upload (void)
{
	char name[8];
	int	i;

	for (i=0; i<MAX_SCRAPS; i++)
	{
		sprintf (name, "scrap%i", i);
		scrap_textures[i] = TexMgr_LoadImage (NULL, name, BLOCK_WIDTH, BLOCK_HEIGHT, SRC_INDEXED, scrap_texels[i],
			"", (src_offset_t)scrap_texels[i], TEXPREF_ALPHA | TEXPREF_OVERWRITE | TEXPREF_NOPICMIP);
	}

	scrap_dirty = false;
}

/*
================
Draw_PicFromWad
================
*/
qpic_t *Draw_PicFromWad2 (const char *name, unsigned int texflags)
{
	int i;
	cachepic_t *pic;
	qpic_t	*p;
	glpic_t	gl;
	src_offset_t offset; //johnfitz
	lumpinfo_t *info;

	//Spike -- added cachepic stuff here, to avoid glitches if the function is called multiple times with the same image.
	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
	{
		if (!strcmp (name, pic->name))
			return &pic->pic;
	}
	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");

	p = (qpic_t *) W_GetLumpName (name, &info);
	if (!p)
	{
		Con_SafePrintf ("W_GetLumpName: %s not found\n", name);
		return pic_nul; //johnfitz
	}
	if (info->type != TYP_QPIC) {Con_SafePrintf ("Draw_PicFromWad: lump \"%s\" is not a qpic\n", name); return pic_nul;}
	if ((size_t)info->size < sizeof(int)*2) {Con_SafePrintf ("Draw_PicFromWad: pic \"%s\" is too small for its qpic header (%u bytes)\n", name, info->size); return pic_nul;}
	if ((size_t)info->size < sizeof(int)*2+p->width*p->height) {Con_SafePrintf ("Draw_PicFromWad: pic \"%s\" truncated (%u*%u requires %u at least bytes)\n", name, p->width,p->height, 8+p->width*p->height); return pic_nul;}
	if ((size_t)info->size > sizeof(int)*2+p->width*p->height) Con_DPrintf ("Draw_PicFromWad: pic \"%s\" over-sized (%u*%u requires only %u bytes)\n", name, p->width,p->height, 8+p->width*p->height);

	// load little ones into the scrap
	if (p->width < 64 && p->height < 64)
	{
		int		x, y;
		int		i, j, k;
		int		texnum;

		texnum = Scrap_AllocBlock (p->width + SCRAP_PADDING, p->height + SCRAP_PADDING, &x, &y);
		scrap_dirty = true;
		k = 0;
		for (i=0 ; i<p->height ; i++)
		{
			for (j=0 ; j<p->width ; j++, k++)
				scrap_texels[texnum][(y+i)*BLOCK_WIDTH + x + j] = p->data[k];
		}
		gl.gltexture = scrap_textures[texnum]; //johnfitz -- changed to an array
		//johnfitz -- no longer go from 0.01 to 0.99
		gl.sl = x/(float)BLOCK_WIDTH;
		gl.sh = (x+p->width)/(float)BLOCK_WIDTH;
		gl.tl = y/(float)BLOCK_WIDTH;
		gl.th = (y+p->height)/(float)BLOCK_WIDTH;
	}
	else
	{
		char texturename[64]; //johnfitz
		q_snprintf (texturename, sizeof(texturename), "%s:%s", WADFILENAME, name); //johnfitz

		offset = (src_offset_t)p - (src_offset_t)wad_base + sizeof(int)*2; //johnfitz

		gl.gltexture = TexMgr_LoadImage (NULL, texturename, p->width, p->height, SRC_INDEXED, p->data, WADFILENAME,
										  offset, texflags); //johnfitz -- TexMgr
		gl.sl = 0;
		gl.sh = (texflags&TEXPREF_PAD)?(float)p->width/(float)TexMgr_PadConditional(p->width):1; //johnfitz
		gl.tl = 0;
		gl.th = (texflags&TEXPREF_PAD)?(float)p->height/(float)TexMgr_PadConditional(p->height):1; //johnfitz
	}

	menu_numcachepics++;
	strcpy (pic->name, name);
	pic->pic = *p;
	memcpy (pic->pic.data, &gl, sizeof(glpic_t));

	return &pic->pic;
}

qpic_t *Draw_PicFromWad (const char *name)
{
	return Draw_PicFromWad2 (name, TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP);
}

/*
================
Draw_CachePic
================
*/
qpic_t	*Draw_TryCachePic (const char *path, unsigned int texflags)
{
	cachepic_t	*pic;
	int			i;
	qpic_t		*dat;
	glpic_t		gl;

	for (pic=menu_cachepics, i=0 ; i<menu_numcachepics ; pic++, i++)
	{
		if (!strcmp (path, pic->name))
			return &pic->pic;
	}
	if (menu_numcachepics == MAX_CACHED_PICS)
		Sys_Error ("menu_numcachepics == MAX_CACHED_PICS");
	menu_numcachepics++;
	strcpy (pic->name, path);

//
// load the pic from disk
//
	dat = (qpic_t *)COM_LoadTempFile (path, NULL);
	if (!dat)
		return NULL;
	SwapPic (dat);

	// HACK HACK HACK --- we need to keep the bytes for
	// the translatable player picture just for the menu
	// configuration dialog
	if (!strcmp (path, "gfx/menuplyr.lmp"))
		memcpy (menuplyr_pixels, dat->data, dat->width*dat->height);

	pic->pic.width = dat->width;
	pic->pic.height = dat->height;

	gl.gltexture = TexMgr_LoadImage (NULL, path, dat->width, dat->height, SRC_INDEXED, dat->data, path,
									  sizeof(int)*2, texflags); //johnfitz -- TexMgr
	gl.sl = 0;
	gl.sh = (float)dat->width/(float)TexMgr_PadConditional(dat->width); //johnfitz
	gl.tl = 0;
	gl.th = (float)dat->height/(float)TexMgr_PadConditional(dat->height); //johnfitz
	memcpy (pic->pic.data, &gl, sizeof(glpic_t));

	return &pic->pic;
}

qpic_t	*Draw_CachePic (const char *path)
{
	qpic_t *pic = Draw_TryCachePic(path, TEXPREF_ALPHA | TEXPREF_PAD | TEXPREF_NOPICMIP | TEXPREF_CLAMP);
	if (!pic)
		Sys_Error ("Draw_CachePic: failed to load %s", path);
	return pic;
}

/*
================
Draw_MakePic -- johnfitz -- generate pics from internal data
================
*/
qpic_t *Draw_MakePic (const char *name, int width, int height, byte *data)
{
	int flags = TEXPREF_NEAREST | TEXPREF_ALPHA | TEXPREF_PERSIST | TEXPREF_NOPICMIP | TEXPREF_PAD;
	qpic_t		*pic;
	glpic_t		gl;

	pic = (qpic_t *) Hunk_Alloc (sizeof(qpic_t) - 4 + sizeof (glpic_t));
	pic->width = width;
	pic->height = height;

	gl.gltexture = TexMgr_LoadImage (NULL, name, width, height, SRC_INDEXED, data, "", (src_offset_t)data, flags);
	gl.sl = 0;
	gl.sh = (float)width/(float)TexMgr_PadConditional(width);
	gl.tl = 0;
	gl.th = (float)height/(float)TexMgr_PadConditional(height);
	memcpy (pic->data, &gl, sizeof(glpic_t));

	return pic;
}

//==============================================================================
//
//  INIT
//
//==============================================================================

/*
===============
Draw_LoadPics -- johnfitz
===============
*/
void Draw_LoadPics (void)
{
	lumpinfo_t	*info;
	byte		*data;
	src_offset_t	offset;

	data = (byte *) W_GetLumpName ("conchars", &info);
	if (!data) Sys_Error ("Draw_LoadPics: couldn't load conchars");
	offset = (src_offset_t)data - (src_offset_t)wad_base;
	char_texture = TexMgr_LoadImage (NULL, WADFILENAME":conchars", 128, 128, SRC_INDEXED, data,
		WADFILENAME, offset, TEXPREF_ALPHA | TEXPREF_NEAREST | TEXPREF_NOPICMIP | TEXPREF_CONCHARS);

	draw_disc = Draw_PicFromWad ("disc");
	draw_backtile = Draw_PicFromWad ("backtile");
}

/*
===============
Draw_NewGame -- johnfitz
===============
*/
void Draw_NewGame (void)
{
	cachepic_t	*pic;
	int			i;

	// empty scrap and reallocate gltextures
	memset(scrap_allocated, 0, sizeof(scrap_allocated));
	memset(scrap_texels, 255, sizeof(scrap_texels));

	Scrap_Upload (); //creates 2 empty gltextures

	// empty lmp cache
	for (pic = menu_cachepics, i = 0; i < menu_numcachepics; pic++, i++)
		pic->name[0] = 0;
	menu_numcachepics = 0;

	// reload wad pics
	W_LoadWadFile (); //johnfitz -- filename is now hard-coded for honesty
	Draw_LoadPics ();
	SCR_LoadPics ();
	Sbar_LoadPics ();
	PR_ReloadPics (false);
}

/*
===============
Draw_CreateWinQuakeMenuBgTex
===============
*/
static void Draw_CreateWinQuakeMenuBgTex (void)
{
	static unsigned winquakemenubg_pixels[4*2] =
	{
		0x00ffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu,
		0xffffffffu, 0xffffffffu, 0x00ffffffu, 0xffffffffu,
	};

	winquakemenubg = TexMgr_LoadImage (NULL, "winquakemenubg", 4, 2, SRC_RGBA,
		(byte*)winquakemenubg_pixels, "", (src_offset_t)winquakemenubg_pixels,
		TEXPREF_ALPHA | TEXPREF_NEAREST | TEXPREF_PERSIST | TEXPREF_NOPICMIP
	);
}

/*
===============
Draw_Init -- johnfitz -- rewritten
===============
*/
void Draw_Init (void)
{
	int i;

	Cvar_RegisterVariable (&scr_conalpha);

	// init quad indices
	for (i = 0; i < MAX_BATCH_QUADS; i++)
	{
		batchindices[i*6 + 0] = i*4 + 0;
		batchindices[i*6 + 1] = i*4 + 1;
		batchindices[i*6 + 2] = i*4 + 2;
		batchindices[i*6 + 3] = i*4 + 0;
		batchindices[i*6 + 4] = i*4 + 2;
		batchindices[i*6 + 5] = i*4 + 3;
	}

	// clear scrap and allocate gltextures
	memset(scrap_allocated, 0, sizeof(scrap_allocated));
	memset(scrap_texels, 255, sizeof(scrap_texels));

	Scrap_Upload (); //creates 2 empty textures

	// create internal pics
	pic_ins = Draw_MakePic ("ins", 8, 9, &pic_ins_data[0][0]);
	pic_ovr = Draw_MakePic ("ovr", 8, 8, &pic_ovr_data[0][0]);
	pic_nul = Draw_MakePic ("nul", 8, 8, &pic_nul_data[0][0]);

	Draw_CreateWinQuakeMenuBgTex ();

	// load game pics
	Draw_LoadPics ();
}

//==============================================================================
//
//  2D DRAWING
//
//==============================================================================

/*
================
Draw_Flush
================
*/
void Draw_Flush (void)
{
	GLuint buf;
	GLbyte *ofs;

	if (!numbatchquads)
		return;

	GL_UseProgram (glprogs.gui);
	GL_SetState (glcanvas.blendmode | GLS_NO_ZTEST | GLS_NO_ZWRITE | GLS_CULL_NONE | GLS_ATTRIBS(3));
	GL_Bind (GL_TEXTURE0, glcanvas.texture);

	GL_Upload (GL_ARRAY_BUFFER, batchverts, sizeof(batchverts[0]) * 4 * numbatchquads, &buf, &ofs);
	GL_BindBuffer (GL_ARRAY_BUFFER, buf);
	GL_VertexAttribPointerFunc (0, 2, GL_FLOAT, GL_FALSE, sizeof(batchverts[0]), ofs + offsetof(guivertex_t, pos));
	GL_VertexAttribPointerFunc (1, 2, GL_FLOAT, GL_FALSE, sizeof(batchverts[0]), ofs + offsetof(guivertex_t, uv));
	GL_VertexAttribPointerFunc (2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(batchverts[0]), ofs + offsetof(guivertex_t, color));

	GL_Upload (GL_ELEMENT_ARRAY_BUFFER, batchindices, sizeof(batchindices[0]) * 6 * numbatchquads, &buf, &ofs);
	GL_BindBuffer (GL_ELEMENT_ARRAY_BUFFER, buf);
	glDrawElements (GL_TRIANGLES, numbatchquads * 6, GL_UNSIGNED_SHORT, ofs);

	numbatchquads = 0;
}

/*
================
Draw_SetTexture
================
*/
static void Draw_SetTexture (gltexture_t *tex)
{
	if (tex == glcanvas.texture)
		return;
	Draw_Flush ();
	glcanvas.texture = tex;
}

/*
================
Draw_SetBlending
================
*/
static void Draw_SetBlending (unsigned blend)
{
	if (blend == glcanvas.blendmode)
		return;
	Draw_Flush ();
	glcanvas.blendmode = blend;
}

/*
================
GL_SetCanvasColor
================
*/
void GL_SetCanvasColor (float r, float g, float b, float a)
{
	glcanvas.color[0] = (int) CLAMP(0.f, r * 255.f + 0.5f, 255.f);
	glcanvas.color[1] = (int) CLAMP(0.f, g * 255.f + 0.5f, 255.f);
	glcanvas.color[2] = (int) CLAMP(0.f, b * 255.f + 0.5f, 255.f);
	glcanvas.color[3] = (int) CLAMP(0.f, a * 255.f + 0.5f, 255.f);
}

/*
================
Draw_AllocQuad
================
*/
static guivertex_t* Draw_AllocQuad (void)
{
	if (numbatchquads == MAX_BATCH_QUADS)
		Draw_Flush ();
	return batchverts + 4 * numbatchquads++;
}

/*
================
Draw_SetVertex
================
*/
static void Draw_SetVertex (guivertex_t *v, float x, float y, float s, float t)
{
	v->pos[0] = x * glcanvas.scale[0] + glcanvas.offset[0];
	v->pos[1] = y * glcanvas.scale[1] + glcanvas.offset[1];
	v->uv[0] = s;
	v->uv[1] = t;
	memcpy (v->color, glcanvas.color, 4 * sizeof(GLubyte));
}

/*
================
Draw_CharacterQuadEx -- johnfitz -- seperate function to spit out verts
================
*/
void Draw_CharacterQuadEx (float x, float y, float dimx, float dimy, char num)
{
	int				row, col;
	float			frow, fcol, fsize;
	guivertex_t		*verts;

	row = num>>4;
	col = num&15;

	frow = row*0.0625;
	fcol = col*0.0625;
	fsize = 0.0625;

	verts = Draw_AllocQuad ();
	Draw_SetVertex (verts++, x,      y,      fcol,         frow);
	Draw_SetVertex (verts++, x+dimx, y,      fcol + fsize, frow);
	Draw_SetVertex (verts++, x+dimx, y+dimy, fcol + fsize, frow + fsize);
	Draw_SetVertex (verts++, x,      y+dimy, fcol,         frow + fsize);
}

/*
================
Draw_CharacterQuad
================
*/
void Draw_CharacterQuad (int x, int y, char num)
{
	Draw_CharacterQuadEx (x, y, 8, 8, num);
}

/*
================
Draw_CharacterEx
================
*/
void Draw_CharacterEx (float x, float y, float dimx, float dimy, int num)
{
	if (y <= -dimy)
		return;			// totally off screen

	num &= 255;

	if (num == 32)
		return; //don't waste verts on spaces

	Draw_SetTexture (char_texture);
	Draw_CharacterQuadEx (x, y, dimx, dimy, (char) num);
}

/*
================
Draw_Character
================
*/
void Draw_Character (int x, int y, int num)
{
	Draw_CharacterEx (x, y, 8, 8, (char) num);
}

/*
================
Draw_StringEx
================
*/
void Draw_StringEx (int x, int y, int dim, const char *str)
{
	if (y <= -dim)
		return;			// totally off screen

	Draw_SetTexture (char_texture);

	while (*str)
	{
		if (*str != 32) //don't waste verts on spaces
			Draw_CharacterQuadEx (x, y, dim, dim, *str);
		str++;
		x += dim;
	}
}

/*
================
Draw_String
================
*/
void Draw_String (int x, int y, const char *str)
{
	Draw_StringEx (x, y, 8, str);
}

/*
=============
Draw_Pic -- johnfitz -- modified
=============
*/
void Draw_Pic (int x, int y, qpic_t *pic)
{
	glpic_t		*gl;
	guivertex_t	*verts;

	if (!pic)
		return;

	if (scrap_dirty)
		Scrap_Upload ();
	gl = (glpic_t *)pic->data;
	Draw_SetTexture (gl->gltexture);

	verts = Draw_AllocQuad ();
	Draw_SetVertex (verts++, x,            y,             gl->sl, gl->tl);
	Draw_SetVertex (verts++, x+pic->width, y,             gl->sh, gl->tl);
	Draw_SetVertex (verts++, x+pic->width, y+pic->height, gl->sh, gl->th);
	Draw_SetVertex (verts++, x,            y+pic->height, gl->sl, gl->th);
}

/*
=============
Draw_SubPic
=============
*/
void Draw_SubPic (float x, float y, float w, float h, qpic_t *pic, float s1, float t1, float s2, float t2, const float *rgb, float alpha)
{
	glpic_t		*gl;
	guivertex_t	*verts;

	if (!pic || alpha <= 0.0f)
		return;

	s2 += s1;
	t2 += t1;

	if (scrap_dirty)
		Scrap_Upload ();
	gl = (glpic_t *)pic->data;
	Draw_SetTexture (gl->gltexture);

	if (rgb)
		GL_SetCanvasColor (rgb[0], rgb[1], rgb[2], alpha);
	else
		GL_SetCanvasColor (1.f, 1.f, 1.f, alpha);

	verts = Draw_AllocQuad ();
	Draw_SetVertex (verts++, x,   y,   LERP (gl->sl, gl->sh, s1), LERP (gl->tl, gl->th, t1));
	Draw_SetVertex (verts++, x+w, y,   LERP (gl->sl, gl->sh, s2), LERP (gl->tl, gl->th, t1));
	Draw_SetVertex (verts++, x+w, y+h, LERP (gl->sl, gl->sh, s2), LERP (gl->tl, gl->th, t2));
	Draw_SetVertex (verts++, x,   y+h, LERP (gl->sl, gl->sh, s1), LERP (gl->tl, gl->th, t2));

	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);
}

/*
=============
Draw_TransPicTranslate -- johnfitz -- rewritten to use texmgr to do translation

Only used for the player color selection menu
=============
*/
void Draw_TransPicTranslate (int x, int y, qpic_t *pic, int top, int bottom)
{
	static int oldtop = -2;
	static int oldbottom = -2;

	if (top != oldtop || bottom != oldbottom)
	{
		glpic_t *p = (glpic_t *)pic->data;
		gltexture_t *glt = p->gltexture;
		oldtop = top;
		oldbottom = bottom;
		TexMgr_ReloadImage (glt, top, bottom);
	}
	Draw_Pic (x, y, pic);
}

/*
================
Draw_ConsoleBackground -- johnfitz -- rewritten
================
*/
void Draw_ConsoleBackground (void)
{
	qpic_t *pic;
	float alpha;

	pic = Draw_CachePic ("gfx/conback.lmp");
	pic->width = vid.conwidth;
	pic->height = vid.conheight;

	alpha = (con_forcedup) ? 1.0f : scr_conalpha.value;

	GL_SetCanvas (CANVAS_CONSOLE); //in case this is called from weird places

	if (alpha > 0.0f)
	{
		GL_SetCanvasColor (1.f, 1.f, 1.f, alpha);
		Draw_Pic (0, 0, pic);
		GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);
	}
}


/*
=============
Draw_TileClear

This repeats a 64*64 tile graphic to fill the screen around a sized down
refresh window.
=============
*/
void Draw_TileClear (int x, int y, int w, int h)
{
	glpic_t	*gl;
	guivertex_t *verts;

	gl = (glpic_t *)draw_backtile->data;

	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);

	Draw_SetTexture (gl->gltexture);

	verts = Draw_AllocQuad ();
	Draw_SetVertex (verts++, x,   y,   x/64.0,     y/64.0);
	Draw_SetVertex (verts++, x+w, y,   (x+w)/64.0, y/64.0);
	Draw_SetVertex (verts++, x+w, y+h, (x+w)/64.0, (y+h)/64.0);
	Draw_SetVertex (verts++, x,   y+h, x/64.0,     (y+h)/64.0);
}

/*
=============
Draw_FillEx

Fills a box of pixels with a single color
=============
*/
void Draw_FillEx (float x, float y, float w, float h, const float *rgb, float alpha)
{
	guivertex_t *verts;
	
	GL_SetCanvasColor (rgb[0], rgb[1], rgb[2], alpha); //johnfitz -- added alpha
	Draw_SetTexture (whitetexture);

	verts = Draw_AllocQuad ();
	Draw_SetVertex (verts++, x,   y,   0.f, 0.f);
	Draw_SetVertex (verts++, x+w, y,   0.f, 0.f);
	Draw_SetVertex (verts++, x+w, y+h, 0.f, 0.f);
	Draw_SetVertex (verts++, x,   y+h, 0.f, 0.f);

	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);
}

void Draw_Fill (int x, int y, int w, int h, int c, float alpha) //johnfitz -- added alpha
{
	byte *pal = (byte *)d_8to24table; //johnfitz -- use d_8to24table instead of host_basepal
	float rgb[3];

	rgb[0] = pal[c*4+0] * (1.f/255.f);
	rgb[1] = pal[c*4+1] * (1.f/255.f);
	rgb[2] = pal[c*4+2] * (1.f/255.f);

	Draw_FillEx (x, y, w, h, rgb, alpha);
}

/*
================
Draw_FadeScreen -- johnfitz -- revised
================
*/
void Draw_FadeScreen (void)
{
	guivertex_t *verts;
	float smax = 0.f, tmax = 0.f, s;

	GL_SetCanvas (CANVAS_DEFAULT);
	if (softemu >= SOFTEMU_BANDED)
	{
		Draw_SetTexture (whitetexture);
		/* first pass */
		Draw_SetBlending (GLS_BLEND_MULTIPLY);
		GL_SetCanvasColor (0.56f, 0.43f, 0.13f, 1.f);
		verts = Draw_AllocQuad ();
		Draw_SetVertex (verts++, 0,       0,        0.f,  0.f);
		Draw_SetVertex (verts++, glwidth, 0,        smax, 0.f);
		Draw_SetVertex (verts++, glwidth, glheight, smax, tmax);
		Draw_SetVertex (verts++, 0,       glheight, 0.f,  tmax);
		/* second pass */
		Draw_SetBlending (GLS_BLEND_ALPHA);
		GL_SetCanvasColor (0.095f, 0.08f, 0.045f, 0.6f);
	}
	else if (softemu == SOFTEMU_COARSE)
	{
		s = q_min((float)glwidth / 320.0, (float)glheight / 200.0);
		s = CLAMP (1.0, scr_menuscale.value, s);
		s = floor (s);
		smax = glwidth / (winquakemenubg->width * s);
		tmax = glheight / (winquakemenubg->height * s);
		Draw_SetTexture (winquakemenubg);
		Draw_SetBlending (GLS_BLEND_ALPHA);
		GL_SetCanvasColor (0.f, 0.f, 0.f, 1.f);
	}
	else
	{
		Draw_SetTexture (whitetexture);
		Draw_SetBlending (GLS_BLEND_ALPHA);
		GL_SetCanvasColor (0.f, 0.f, 0.f, 0.5f);
	}

	verts = Draw_AllocQuad ();
	Draw_SetVertex (verts++, 0,       0,        0.f,  0.f);
	Draw_SetVertex (verts++, glwidth, 0,        smax, 0.f);
	Draw_SetVertex (verts++, glwidth, glheight, smax, tmax);
	Draw_SetVertex (verts++, 0,       glheight, 0.f,  tmax);

	Draw_SetBlending (GLS_BLEND_ALPHA);
	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);

	Sbar_Changed();
}

/*
================
Draw_SetTransform
================
*/
static void Draw_SetTransform (float left, float right, float bottom, float top)
{
	float tx = -(right + left) / (right - left);
	float ty = -(top + bottom) / (top - bottom);

	glcanvas.left = left;
	glcanvas.right = right;
	glcanvas.bottom = bottom;
	glcanvas.top = top;
	glcanvas.scale[0] = 2.0f / (right - left);
	glcanvas.scale[1] = 2.0f / (top - bottom);
	glcanvas.offset[0] = tx;
	glcanvas.offset[1] = ty;
}

/*
================
Draw_GetMenuTransform
================
*/
void Draw_GetMenuTransform (vrect_t *bounds, vrect_t *viewport)
{
	float s;
	s = q_min((float)glwidth / 320.0f, (float)glheight / 200.0f);
	s = CLAMP (1.0f, scr_menuscale.value, s);
	// ericw -- doubled width to 640 to accommodate long keybindings
	bounds->x = 0;
	bounds->y = 0;
	bounds->width = 640;
	bounds->height = 200;
	viewport->x = glx + (glwidth - 320*s) / 2;
	viewport->y = gly + (glheight - 200*s) / 2;
	viewport->width = 640*s;
	viewport->height = 200*s;
}

/*
================
GL_SetCanvas -- johnfitz -- support various canvas types
================
*/
void GL_SetCanvas (canvastype newcanvas)
{
	extern vrect_t scr_vrect;
	vrect_t bounds, viewport;
	float s;
	int lines;

	if (newcanvas == glcanvas.type)
		return;

	Draw_Flush ();
	glcanvas.type = newcanvas;
	glcanvas.texture = NULL;

	switch(newcanvas)
	{
	case CANVAS_DEFAULT:
		Draw_SetTransform (0, glwidth, glheight, 0);
		glViewport (glx, gly, glwidth, glheight);
		break;
	case CANVAS_CONSOLE:
		lines = vid.conheight - (scr_con_current * vid.conheight / glheight);
		Draw_SetTransform (0, vid.conwidth, vid.conheight + lines, lines);
		glViewport (glx, gly, glwidth, glheight);
		break;
	case CANVAS_MENU:
		Draw_GetMenuTransform (&bounds, &viewport);
		Draw_SetTransform (bounds.x, bounds.x+bounds.width, bounds.y+bounds.height, bounds.y);
		glViewport (viewport.x, viewport.y, viewport.width, viewport.height);
		break;
	case CANVAS_CSQC:
		s = CLAMP (1.0, scr_sbarscale.value, (float)glwidth / 320.0);
		Draw_SetTransform (0, glwidth/s, glheight/s, 0);
		glViewport (glx, gly, glwidth, glheight);
		break;
	case CANVAS_SBAR:
		s = CLAMP (1.0f, scr_sbarscale.value, (float)glwidth / 320.0f);
		if (cl.gametype == GAME_DEATHMATCH && scr_hudstyle.value < 1)
		{
			Draw_SetTransform (0, glwidth / s, 48, 0);
			glViewport (glx, gly, glwidth, 48*s);
		}
		else
		{
			Draw_SetTransform (0, 320, 48, 0);
			glViewport (glx + (glwidth - 320*s) / 2, gly, 320*s, 48*s);
		}
		break;
	case CANVAS_SBAR2:
		s = q_min (glwidth / 400.0, glheight / 225.0);
		s = CLAMP (1.0, scr_sbarscale.value, s);
		Draw_SetTransform (0, glwidth/s, glheight/s, 0);
		glViewport (glx, gly, glwidth, glheight);
		break;
	case CANVAS_CROSSHAIR: //0,0 is center of viewport
		s = CLAMP (1.0f, scr_crosshairscale.value, 10.0f);
		Draw_SetTransform (scr_vrect.width/-2/s, scr_vrect.width/2/s, scr_vrect.height/2/s, scr_vrect.height/-2/s);
		glViewport (scr_vrect.x, glheight - scr_vrect.y - scr_vrect.height, scr_vrect.width & ~1, scr_vrect.height & ~1);
		break;
	case CANVAS_BOTTOMLEFT: //used by devstats
		s = (float)glwidth/vid.conwidth; //use console scale
		Draw_SetTransform (0, 320, 200, 0);
		glViewport (glx, gly, 320*s, 200*s);
		break;
	case CANVAS_BOTTOMRIGHT: //used by fps/clock
		s = (float)glwidth/vid.conwidth; //use console scale
		Draw_SetTransform (0, 320, 200, 0);
		glViewport (glx+glwidth-320*s, gly, 320*s, 200*s);
		break;
	case CANVAS_TOPRIGHT: //used by disc
		s = (float)glwidth/vid.conwidth; //use console scale
		Draw_SetTransform (0, 320, 200, 0);
		glViewport (glx+glwidth-320*s, gly+glheight-200*s, 320*s, 200*s);
		break;
	default:
		Sys_Error ("GL_SetCanvas: bad canvas type");
	}
}

/*
================
GL_Set2D
================
*/
void GL_Set2D (void)
{
	glcanvas.type = CANVAS_INVALID;
	glcanvas.texture = NULL;
	glcanvas.blendmode = GLS_BLEND_ALPHA;
	GL_SetCanvas (CANVAS_DEFAULT);
	GL_SetCanvasColor (1.f, 1.f, 1.f, 1.f);
}
