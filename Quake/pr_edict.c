/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2009 John Fitzgibbons and others
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
// sv_edict.c -- entity dictionary

#include "quakedef.h"

qboolean	pr_alpha_supported; //johnfitz
int			pr_effects_mask; // only enable 2021 rerelease quad/penta dlights when applicable

globalvars_t	*pr_global_struct;

int		type_size[8] = {
	1,					// ev_void
	1,	// sizeof(string_t) / 4		// ev_string
	1,					// ev_float
	3,					// ev_vector
	1,					// ev_entity
	1,					// ev_field
	1,	// sizeof(func_t) / 4		// ev_function
	1	// sizeof(void *) / 4		// ev_pointer
};

static ddef_t	*ED_FieldAtOfs (int ofs);
static qboolean	ED_ParseEpair (void *base, ddef_t *key, const char *s);

cvar_t	nomonsters = {"nomonsters", "0", CVAR_NONE};
cvar_t	gamecfg = {"gamecfg", "0", CVAR_NONE};
cvar_t	scratch1 = {"scratch1", "0", CVAR_NONE};
cvar_t	scratch2 = {"scratch2", "0", CVAR_NONE};
cvar_t	scratch3 = {"scratch3", "0", CVAR_NONE};
cvar_t	scratch4 = {"scratch4", "0", CVAR_NONE};
cvar_t	savedgamecfg = {"savedgamecfg", "0", CVAR_ARCHIVE};
cvar_t	saved1 = {"saved1", "0", CVAR_ARCHIVE};
cvar_t	saved2 = {"saved2", "0", CVAR_ARCHIVE};
cvar_t	saved3 = {"saved3", "0", CVAR_ARCHIVE};
cvar_t	saved4 = {"saved4", "0", CVAR_ARCHIVE};

/*
=================
PR_HashInit
=================
*/
static void PR_HashInit (prhashtable_t *table, int capacity, const char *name)
{
	capacity *= 2; // 50% load factor
	table->capacity = capacity;
	table->strings = (const char **) Hunk_AllocName (sizeof(*table->strings) * capacity, name);
	table->indices = (int         *) Hunk_AllocName (sizeof(*table->indices) * capacity, name);
}

/*
=================
PR_HashGet
=================
*/
static int PR_HashGet (prhashtable_t *table, const char *key)
{
	unsigned pos = COM_HashString (key) % table->capacity, end = pos;

	do
	{
		const char *s = table->strings[pos];
		if (!s)
			return -1;
		if (0 == strcmp(s, key))
			return table->indices[pos];

		++pos;
		if (pos == table->capacity)
			pos = 0;
	}
	while (pos != end);

	return -1;
}

/*
=================
PR_HashAdd
=================
*/
static void PR_HashAdd (prhashtable_t *table, int skey, int value)
{
	const char *name = PR_GetString (skey);
	unsigned pos = COM_HashString (name) % table->capacity, end = pos;

	do
	{
		if (!table->strings[pos])
		{
			table->strings[pos] = name;
			table->indices[pos] = value;
			return;
		}

		++pos;
		if (pos == table->capacity)
			pos = 0;
	}
	while (pos != end);

	Sys_Error ("PR_HashAdd failed");
}

/*
===============
PR_InitHashTables
===============
*/
static void PR_InitHashTables (void)
{
	int i;

	PR_HashInit (&qcvm->ht_fields, qcvm->progs->numfielddefs, "ht_fields");
	for (i = 0; i < qcvm->progs->numfielddefs; i++)
		PR_HashAdd (&qcvm->ht_fields, qcvm->fielddefs[i].s_name, i);

	PR_HashInit (&qcvm->ht_functions, qcvm->progs->numfunctions, "ht_functions");
	for (i = 0; i < qcvm->progs->numfunctions; i++)
		PR_HashAdd (&qcvm->ht_functions, qcvm->functions[i].s_name, i);

	PR_HashInit (&qcvm->ht_globals, qcvm->progs->numglobaldefs, "ht_globals");
	for (i = 0; i < qcvm->progs->numglobaldefs; i++)
		PR_HashAdd (&qcvm->ht_globals, qcvm->globaldefs[i].s_name, i);
}

/*
=================
ED_AddToFreeList
=================
*/
static void ED_AddToFreeList (edict_t *ed)
{
	ed->free = true;
	if ((byte *)ed <= (byte *)qcvm->edicts + q_max (svs.maxclients, 1) * qcvm->edict_size)
		return;
	if (ed->freechain.prev)
		RemoveLink (&ed->freechain);
	InsertLinkBefore (&ed->freechain, &qcvm->free_edicts);
}

/*
=================
ED_RemoveFromFreeList
=================
*/
static void ED_RemoveFromFreeList (edict_t *ed)
{
	ed->free = false;
	if (ed->freechain.prev)
	{
		RemoveLink (&ed->freechain);
		ed->freechain.prev = ed->freechain.next = NULL;
	}
}

/*
=================
ED_ClearEdict

Sets everything to NULL
=================
*/
void ED_ClearEdict (edict_t *e)
{
	memset (&e->v, 0, qcvm->progs->entityfields * 4);
	ED_RemoveFromFreeList (e);
}

/*
=================
ED_Alloc

Either finds a free edict, or allocates a new one.
Try to avoid reusing an entity that was recently freed, because it
can cause the client to think the entity morphed into something else
instead of being removed and recreated, which can cause interpolated
angles and bad trails.
=================
*/
edict_t *ED_Alloc (void)
{
	edict_t		*e;

	if (qcvm->free_edicts.next != &qcvm->free_edicts)
	{
		e = STRUCT_FROM_LINK (qcvm->free_edicts.next, edict_t, freechain);
		if (!e->free)
			Host_Error ("ED_Alloc: free list entity still in use");
		// the first couple seconds of server time can involve a lot of
		// freeing and allocating, so relax the replacement policy
		if (e->freetime < 2 || qcvm->time - e->freetime > 0.5)
		{
			ED_ClearEdict (e);
			return e;
		}
	}

	if (qcvm->num_edicts == qcvm->max_edicts) //johnfitz -- use sv.max_edicts instead of MAX_EDICTS
		Host_Error ("ED_Alloc: no free edicts (max_edicts is %i)", qcvm->max_edicts);

	e = EDICT_NUM(qcvm->num_edicts++);
	memset(e, 0, qcvm->edict_size); // ericw -- switched sv.edicts to malloc(), so we are accessing uninitialized memory and must fully zero it, not just ED_ClearEdict

	return e;
}

/*
=================
ED_Free

Marks the edict as free
FIXME: walk all entities and NULL out references to this entity
=================
*/
void ED_Free (edict_t *ed)
{
	SV_UnlinkEdict (ed);		// unlink from world bsp
	ED_AddToFreeList (ed);

	ed->v.model = 0;
	ed->v.takedamage = 0;
	ed->v.modelindex = 0;
	ed->v.colormap = 0;
	ed->v.skin = 0;
	ed->v.frame = 0;
	VectorCopy (vec3_origin, ed->v.origin);
	VectorCopy (vec3_origin, ed->v.angles);
	ed->v.nextthink = -1;
	ed->v.solid = 0;
	ed->alpha = ENTALPHA_DEFAULT; //johnfitz -- reset alpha for next entity

	ed->freetime = qcvm->time;
}

//===========================================================================

/*
============
ED_GlobalAtOfs
============
*/
static ddef_t *ED_GlobalAtOfs (int ofs)
{
	ddef_t		*def;
	int			i;

	for (i = 0; i < qcvm->progs->numglobaldefs; i++)
	{
		def = &qcvm->globaldefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

/*
============
ED_FieldAtOfs
============
*/
static ddef_t *ED_FieldAtOfs (int ofs)
{
	ddef_t		*def;
	int			i;

	for (i = 0; i < qcvm->progs->numfielddefs; i++)
	{
		def = &qcvm->fielddefs[i];
		if (def->ofs == ofs)
			return def;
	}
	return NULL;
}

/*
============
ED_FindField
============
*/
static ddef_t *ED_FindField (const char *name)
{
	ddef_t		*def;
	int			i;

	if (qcvm->ht_fields.capacity > 0)
	{
		int index = PR_HashGet (&qcvm->ht_fields, name);
		if (index < 0)
			return NULL;
		return qcvm->fielddefs + index;
	}

	for (i = 0; i < qcvm->progs->numfielddefs; i++)
	{
		def = &qcvm->fielddefs[i];
		if ( !strcmp(PR_GetString(def->s_name), name) )
			return def;
	}
	return NULL;
}


/*
============
ED_FindGlobal
============
*/
static ddef_t *ED_FindGlobal (const char *name)
{
	ddef_t		*def;
	int			i;

	if (qcvm->ht_globals.capacity > 0)
	{
		int index = PR_HashGet (&qcvm->ht_globals, name);
		if (index < 0)
			return NULL;
		return qcvm->globaldefs + index;
	}

	for (i = 0; i < qcvm->progs->numglobaldefs; i++)
	{
		def = &qcvm->globaldefs[i];
		if ( !strcmp(PR_GetString(def->s_name), name) )
			return def;
	}
	return NULL;
}


/*
============
ED_FindFunction
============
*/
static dfunction_t *ED_FindFunction (const char *fn_name)
{
	dfunction_t		*func;
	int				i;

	if (qcvm->ht_functions.capacity > 0)
	{
		int index = PR_HashGet (&qcvm->ht_functions, fn_name);
		if (index < 0)
			return NULL;
		return qcvm->functions + index;
	}

	for (i = 0; i < qcvm->progs->numfunctions; i++)
	{
		func = &qcvm->functions[i];
		if ( !strcmp(PR_GetString(func->s_name), fn_name) )
			return func;
	}
	return NULL;
}

/*
============
GetEdictFieldValue
============
*/
eval_t *GetEdictFieldValue(edict_t *ed, const char *field)
{
	ddef_t *def;

	def = ED_FindField (field);
	if (!def)
		return NULL;

	return (eval_t *)((char *)&ed->v + def->ofs*4);
}


/*
============
PR_ValueString
(etype_t type, eval_t *val)

Returns a string describing *data in a type specific manner
=============
*/
static const char *PR_ValueString (int type, eval_t *val)
{
	static char	line[512];
	ddef_t		*def;
	dfunction_t	*f;

	type &= ~DEF_SAVEGLOBAL;

	switch (type)
	{
	case ev_string:
		q_snprintf (line, sizeof(line), "%s", PR_GetString(val->string));
		break;
	case ev_entity:
		q_snprintf (line, sizeof(line), "entity %i", NUM_FOR_EDICT(PROG_TO_EDICT(val->edict)) );
		break;
	case ev_function:
		f = qcvm->functions + val->function;
		q_snprintf (line, sizeof(line), "%s()", PR_GetString(f->s_name));
		break;
	case ev_field:
		def = ED_FieldAtOfs ( val->_int );
		q_snprintf (line, sizeof(line), ".%s", PR_GetString(def->s_name));
		break;
	case ev_void:
		q_snprintf (line, sizeof(line), "void");
		break;
	case ev_float:
		q_snprintf (line, sizeof(line), "%5.1f", val->_float);
		break;
	case ev_vector:
		q_snprintf (line, sizeof(line), "'%5.1f %5.1f %5.1f'", val->vector[0], val->vector[1], val->vector[2]);
		break;
	case ev_pointer:
		q_snprintf (line, sizeof(line), "pointer");
		break;
	default:
		q_snprintf (line, sizeof(line), "bad type %i", type);
		break;
	}

	return line;
}

/*
============
PR_UglyValueString
(etype_t type, eval_t *val)

Returns a string describing *data in a type specific manner
Easier to parse than PR_ValueString
=============
*/
static const char *PR_UglyValueString (int type, eval_t *val)
{
	static char	line[1024];
	ddef_t		*def;
	dfunction_t	*f;

	type &= ~DEF_SAVEGLOBAL;

	switch (type)
	{
	case ev_string:
		q_snprintf (line, sizeof(line), "%s", PR_GetString(val->string));
		break;
	case ev_entity:
		q_snprintf (line, sizeof(line), "%i", NUM_FOR_EDICT(PROG_TO_EDICT(val->edict)));
		break;
	case ev_function:
		f = qcvm->functions + val->function;
		q_snprintf (line, sizeof(line), "%s", PR_GetString(f->s_name));
		break;
	case ev_field:
		def = ED_FieldAtOfs ( val->_int );
		q_snprintf (line, sizeof(line), "%s", PR_GetString(def->s_name));
		break;
	case ev_void:
		q_snprintf (line, sizeof(line), "void");
		break;
	case ev_float:
		q_snprintf (line, sizeof(line), "%f", val->_float);
		break;
	case ev_vector:
		q_snprintf (line, sizeof(line), "%f %f %f", val->vector[0], val->vector[1], val->vector[2]);
		break;
	default:
		q_snprintf (line, sizeof(line), "bad type %i", type);
		break;
	}

	return line;
}

/*
============
PR_GlobalString

Returns a string with a description and the contents of a global,
padded to 20 field width
============
*/
const char *PR_GlobalString (int ofs)
{
	static char	line[512];
	const char	*s;
	int		i;
	ddef_t		*def;
	void		*val;

	val = (void *)&qcvm->globals[ofs];
	def = ED_GlobalAtOfs(ofs);
	if (!def)
		q_snprintf (line, sizeof(line), "%i(?)", ofs);
	else
	{
		s = PR_ValueString (def->type, (eval_t *)val);
		q_snprintf (line, sizeof(line), "%i(%s)%s", ofs, PR_GetString(def->s_name), s);
	}

	i = strlen(line);
	for ( ; i < 20; i++)
		strcat (line, " ");
	strcat (line, " ");

	return line;
}

const char *PR_GlobalStringNoContents (int ofs)
{
	static char	line[512];
	int		i;
	ddef_t		*def;

	def = ED_GlobalAtOfs(ofs);
	if (!def)
		q_snprintf (line, sizeof(line), "%i(?)", ofs);
	else
		q_snprintf (line, sizeof(line), "%i(%s)", ofs, PR_GetString(def->s_name));

	i = strlen(line);
	for ( ; i < 20; i++)
		strcat (line, " ");
	strcat (line, " ");

	return line;
}


/*
=============
ED_Print

For debugging
=============
*/
void ED_Print (edict_t *ed)
{
	ddef_t	*d;
	int		*v;
	int		i, j, l;
	const char	*name;
	int		type;

	if (ed->free)
	{
		Con_Printf ("FREE\n");
		return;
	}

	Con_SafePrintf("\nEDICT %i:\n", NUM_FOR_EDICT(ed)); //johnfitz -- was Con_Printf
	for (i = 1; i < qcvm->progs->numfielddefs; i++)
	{
		d = &qcvm->fielddefs[i];
		name = PR_GetString(d->s_name);
		l = strlen (name);
		if (l > 1 && name[l - 2] == '_')
			continue;	// skip _x, _y, _z vars

		v = (int *)((char *)&ed->v + d->ofs*4);

	// if the value is still all 0, skip the field
		type = d->type & ~DEF_SAVEGLOBAL;

		for (j = 0; j < type_size[type]; j++)
		{
			if (v[j])
				break;
		}
		if (j == type_size[type])
			continue;

		Con_SafePrintf ("%s", name); //johnfitz -- was Con_Printf
		while (l++ < 15)
			Con_SafePrintf (" "); //johnfitz -- was Con_Printf

		Con_SafePrintf ("%s\n", PR_ValueString(d->type, (eval_t *)v)); //johnfitz -- was Con_Printf
	}
}

/*
=============
ED_Write

For savegames
=============
*/
void ED_Write (FILE *f, edict_t *ed)
{
	ddef_t	*d;
	int		*v;
	int		i, j;
	const char	*name;
	int		type;

	fprintf (f, "{\n");

	if (ed->free)
	{
		fprintf (f, "}\n");
		return;
	}

	for (i = 1; i < qcvm->progs->numfielddefs; i++)
	{
		d = &qcvm->fielddefs[i];
		name = PR_GetString(d->s_name);
		j = strlen (name);
		if (j > 1 && name[j - 2] == '_')
			continue;	// skip _x, _y, _z vars

		v = (int *)((char *)&ed->v + d->ofs*4);

	// if the value is still all 0, skip the field
		type = d->type & ~DEF_SAVEGLOBAL;
		for (j = 0; j < type_size[type]; j++)
		{
			if (v[j])
				break;
		}
		if (j == type_size[type])
			continue;

		fprintf (f, "\"%s\" ", name);
		fprintf (f, "\"%s\"\n", PR_UglyValueString(d->type, (eval_t *)v));
	}

	//johnfitz -- save entity alpha manually when progs.dat doesn't know about alpha
	if (!pr_alpha_supported && ed->alpha != ENTALPHA_DEFAULT)
		fprintf (f, "\"alpha\" \"%f\"\n", ENTALPHA_TOSAVE(ed->alpha));
	//johnfitz

	fprintf (f, "}\n");
}

void ED_PrintNum (int ent)
{
	ED_Print (EDICT_NUM(ent));
}

/*
=============
ED_PrintEdicts

For debugging, prints all the entities in the current server
=============
*/
void ED_PrintEdicts (void)
{
	int		i;

	if (!sv.active)
		return;

	PR_SwitchQCVM(&sv.qcvm);
	Con_Printf ("%i entities\n", qcvm->num_edicts);
	for (i = 0; i < qcvm->num_edicts; i++)
		ED_PrintNum (i);
	PR_SwitchQCVM(NULL);
}

/*
=============
ED_PrintEdict_f

For debugging, prints a single edicy
=============
*/
static void ED_PrintEdict_f (void)
{
	int		i;

	if (!sv.active)
		return;

	i = Q_atoi (Cmd_Argv(1));
	PR_SwitchQCVM(&sv.qcvm);
	if (i < 0 || i >= qcvm->num_edicts)
	{
		Con_Printf("Bad edict number\n");
		return;
	}
	ED_PrintNum (i);
	PR_SwitchQCVM(NULL);
}

/*
=============
ED_Count

For debugging
=============
*/
static void ED_Count (void)
{
	edict_t	*ent;
	int	i, active, models, solid, step;

	if (!sv.active)
		return;

	PR_SwitchQCVM(&sv.qcvm);
	active = models = solid = step = 0;
	for (i = 0; i < qcvm->num_edicts; i++)
	{
		ent = EDICT_NUM(i);
		if (ent->free)
			continue;
		active++;
		if (ent->v.solid)
			solid++;
		if (ent->v.model)
			models++;
		if (ent->v.movetype == MOVETYPE_STEP)
			step++;
	}

	Con_Printf ("num_edicts:%3i\n", qcvm->num_edicts);
	Con_Printf ("active    :%3i\n", active);
	Con_Printf ("view      :%3i\n", models);
	Con_Printf ("touch     :%3i\n", solid);
	Con_Printf ("step      :%3i\n", step);
	PR_SwitchQCVM(NULL);
}


/*
==============================================================================

ARCHIVING GLOBALS

FIXME: need to tag constants, doesn't really work
==============================================================================
*/

/*
=============
ED_WriteGlobals
=============
*/
void ED_WriteGlobals (FILE *f)
{
	ddef_t		*def;
	int			i;
	const char		*name;
	int			type;

	fprintf (f, "{\n");
	for (i = 0; i < qcvm->progs->numglobaldefs; i++)
	{
		def = &qcvm->globaldefs[i];
		type = def->type;
		if ( !(def->type & DEF_SAVEGLOBAL) )
			continue;
		type &= ~DEF_SAVEGLOBAL;

		if (type != ev_string && type != ev_float && type != ev_entity)
			continue;

		name = PR_GetString(def->s_name);
		fprintf (f, "\"%s\" ", name);
		fprintf (f, "\"%s\"\n", PR_UglyValueString(type, (eval_t *)&qcvm->globals[def->ofs]));
	}
	fprintf (f, "}\n");
}

/*
=============
ED_ParseGlobals
=============
*/
const char *ED_ParseGlobals (const char *data)
{
	char	keyname[64];
	ddef_t	*key;

	while (1)
	{
	// parse key
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			Host_Error ("ED_ParseEntity: EOF without closing brace");

		q_strlcpy (keyname, com_token, sizeof(keyname));

	// parse value
		data = COM_Parse (data);
		if (!data)
			Host_Error ("ED_ParseEntity: EOF without closing brace");

		if (com_token[0] == '}')
			Host_Error ("ED_ParseEntity: closing brace without data");

		key = ED_FindGlobal (keyname);
		if (!key)
		{
			Con_Printf ("'%s' is not a global\n", keyname);
			continue;
		}

		if (!ED_ParseEpair ((void *)qcvm->globals, key, com_token))
			Host_Error ("ED_ParseGlobals: parse error");
	}
	return data;
}

//============================================================================


/*
=============
ED_NewString
=============
*/
static string_t ED_NewString (const char *string)
{
	char	*new_p;
	int		i, l;
	string_t	num;

	l = strlen(string) + 1;
	num = PR_AllocString (l, &new_p);

	for (i = 0; i < l; i++)
	{
		if (string[i] == '\\' && i < l-1)
		{
			i++;
			if (string[i] == 'n')
				*new_p++ = '\n';
			else
				*new_p++ = '\\';
		}
		else
			*new_p++ = string[i];
	}

	return num;
}


/*
=============
ED_ParseEval

Can parse either fields or globals
returns false if error
=============
*/
static qboolean ED_ParseEpair (void *base, ddef_t *key, const char *s)
{
	int		i;
	char	string[128];
	ddef_t	*def;
	char	*v, *w;
	char	*end;
	void	*d;
	dfunction_t	*func;

	d = (void *)((int *)base + key->ofs);

	switch (key->type & ~DEF_SAVEGLOBAL)
	{
	case ev_string:
		*(string_t *)d = ED_NewString(s);
		break;

	case ev_float:
		*(float *)d = atof (s);
		break;

	case ev_vector:
		q_strlcpy (string, s, sizeof(string));
		end = (char *)string + strlen(string);
		v = string;
		w = string;

		for (i = 0; i < 3 && (w <= end); i++) // ericw -- added (w <= end) check
		{
		// set v to the next space (or 0 byte), and change that char to a 0 byte
			while (*v && *v != ' ')
				v++;
			*v = 0;
			((float *)d)[i] = atof (w);
			w = v = v+1;
		}
		// ericw -- fill remaining elements to 0 in case we hit the end of string
		// before reading 3 floats.
		if (i < 3)
		{
			Con_DWarning ("Avoided reading garbage for \"%s\" \"%s\"\n", PR_GetString(key->s_name), s);
			for (; i < 3; i++)
				((float *)d)[i] = 0.0f;
		}
		break;

	case ev_entity:
		*(int *)d = EDICT_TO_PROG(EDICT_NUM(atoi (s)));
		break;

	case ev_field:
		def = ED_FindField (s);
		if (!def)
		{
			//johnfitz -- HACK -- suppress error becuase fog/sky fields might not be mentioned in defs.qc
			if (strncmp(s, "sky", 3) && strcmp(s, "fog"))
				Con_DPrintf ("Can't find field %s\n", s);
			return false;
		}
		*(int *)d = G_INT(def->ofs);
		break;

	case ev_function:
		func = ED_FindFunction (s);
		if (!func)
		{
			Con_Printf ("Can't find function %s\n", s);
			return false;
		}
		*(func_t *)d = func - qcvm->functions;
		break;

	default:
		break;
	}
	return true;
}

/*
====================
ED_ParseEdict

Parses an edict out of the given string, returning the new position
ed should be a properly initialized empty edict.
Used for initial level load and for savegames.
====================
*/
const char *ED_ParseEdict (const char *data, edict_t *ent)
{
	ddef_t		*key;
	char		keyname[256];
	qboolean	anglehack, init;
	int		n;

	init = false;

	// clear it
	if (ent != qcvm->edicts)	// hack
		memset (&ent->v, 0, qcvm->progs->entityfields * 4);

	// go through all the dictionary pairs
	while (1)
	{
		// parse key
		data = COM_Parse (data);
		if (com_token[0] == '}')
			break;
		if (!data)
			Host_Error ("ED_ParseEntity: EOF without closing brace");

		// anglehack is to allow QuakeEd to write single scalar angles
		// and allow them to be turned into vectors. (FIXME...)
		if (!strcmp(com_token, "angle"))
		{
			strcpy (com_token, "angles");
			anglehack = true;
		}
		else
			anglehack = false;

		// FIXME: change light to _light to get rid of this hack
		if (!strcmp(com_token, "light"))
			strcpy (com_token, "light_lev");	// hack for single light def

		q_strlcpy (keyname, com_token, sizeof(keyname));

		// another hack to fix keynames with trailing spaces
		n = strlen(keyname);
		while (n && keyname[n-1] == ' ')
		{
			keyname[n-1] = 0;
			n--;
		}

		// parse value
		data = COM_Parse (data);
		if (!data)
			Host_Error ("ED_ParseEntity: EOF without closing brace");

		if (com_token[0] == '}')
			Host_Error ("ED_ParseEntity: closing brace without data");

		init = true;

		// keynames with a leading underscore are used for utility comments,
		// and are immediately discarded by quake
		if (keyname[0] == '_')
			continue;

		//johnfitz -- hack to support .alpha even when progs.dat doesn't know about it
		if (!strcmp(keyname, "alpha"))
			ent->alpha = ENTALPHA_ENCODE(Q_atof(com_token));
		//johnfitz

		key = ED_FindField (keyname);
		if (!key)
		{
			//johnfitz -- HACK -- suppress error becuase fog/sky/alpha fields might not be mentioned in defs.qc
			if (strncmp(keyname, "sky", 3) && strcmp(keyname, "fog") && strcmp(keyname, "alpha"))
				Con_DPrintf ("\"%s\" is not a field\n", keyname); //johnfitz -- was Con_Printf
			continue;
		}

		if (anglehack)
		{
			char	temp[32];
			strcpy (temp, com_token);
			sprintf (com_token, "0 %s 0", temp);
		}

		if (!ED_ParseEpair ((void *)&ent->v, key, com_token))
			Host_Error ("ED_ParseEdict: parse error");
	}

	if (!init)
		ED_AddToFreeList (ent);

	return data;
}


/*
================
ED_LoadFromFile

The entities are directly placed in the array, rather than allocated with
ED_Alloc, because otherwise an error loading the map would have entity
number references out of order.

Creates a server's entity / program execution context by
parsing textual entity definitions out of an ent file.

Used for both fresh maps and savegame loads.  A fresh map would also need
to call ED_CallSpawnFunctions () to let the objects initialize themselves.
================
*/
void ED_LoadFromFile (const char *data)
{
	const char	*classname;
	dfunction_t	*func;
	edict_t		*ent = NULL;
	int		inhibit = 0;

	pr_global_struct->time = qcvm->time;

	// parse ents
	while (1)
	{
		// parse the opening brace
		data = COM_Parse (data);
		if (!data)
			break;
		if (com_token[0] != '{')
			Host_Error ("ED_LoadFromFile: found %s when expecting {",com_token);

		if (!ent)
			ent = EDICT_NUM(0);
		else
			ent = ED_Alloc ();
		data = ED_ParseEdict (data, ent);

		// remove things from different skill levels or deathmatch
		if (deathmatch.value)
		{
			if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH))
			{
				ED_Free (ent);
				inhibit++;
				continue;
			}
		}
		else if ((current_skill == 0 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_EASY))
				|| (current_skill == 1 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_MEDIUM))
				|| (current_skill >= 2 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_HARD)) )
		{
			ED_Free (ent);
			inhibit++;
			continue;
		}

//
// immediately call spawn function
//
		if (!ent->v.classname)
		{
			Con_SafePrintf ("No classname for:\n"); //johnfitz -- was Con_Printf
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

		classname = PR_GetString (ent->v.classname);
		if (sv.nomonsters && !Q_strncmp (classname, "monster_", 8))
		{
			ED_Free (ent);
			inhibit++;
			continue;
		}

	// look for the spawn function
		func = ED_FindFunction (classname);

		if (!func)
		{
			Con_SafePrintf ("No spawn function for:\n"); //johnfitz -- was Con_Printf
			ED_Print (ent);
			ED_Free (ent);
			continue;
		}

		SV_ReserveSignonSpace (512);

		pr_global_struct->self = EDICT_TO_PROG(ent);
		PR_ExecuteProgram (func - qcvm->functions);
	}

	Con_DPrintf ("%i entities inhibited\n", inhibit);
}


qcvm_t *qcvm;
globalvars_t	*pr_global_struct;
void PR_SwitchQCVM(qcvm_t *nvm)
{
	if (qcvm && nvm)
		Sys_Error("PR_SwitchQCVM: A qcvm was already active");
	qcvm = nvm;
	if (qcvm)
		pr_global_struct = (globalvars_t*)qcvm->globals;
	else
		pr_global_struct = NULL;
}

void PR_ClearProgs(qcvm_t *vm)
{
	qcvm_t *oldvm = qcvm;
	if (!vm->progs)
		return;	//wasn't loaded.
	qcvm = NULL;
	PR_SwitchQCVM(vm);

	if (qcvm->knownstrings)
		Z_Free ((void *)qcvm->knownstrings);
	free(qcvm->edicts); // ericw -- sv.edicts switched to use malloc()
	if (qcvm->fielddefs != (ddef_t *)((byte *)qcvm->progs + qcvm->progs->ofs_fielddefs))
		free(qcvm->fielddefs);
	memset(qcvm, 0, sizeof(*qcvm));

	qcvm = NULL;
	PR_SwitchQCVM(oldvm);
}

/*
===============
PR_InitBuiltins
===============
*/
static void PR_InitBuiltins (void)
{
	dfunction_t	*func;
	const char	*name;
	int			i, j;

	// add base builtins
	memcpy (qcvm->builtins, pr_basebuiltins, pr_numbasebuiltins * sizeof (pr_basebuiltins[0]));

	// fill all other entries with PF_Fixme
	for (i = pr_numbasebuiltins; i < MAX_BUILTINS; i++)
		qcvm->builtins[i] = pr_basebuiltins[0];

	// fill top slots (excluding the very last one) with extension builtins
	for (j = 0; j < pr_numextbuiltins; j++)
	{
		extbuiltin_t *ext = &pr_extbuiltins[j];
		ext->number = MAX_BUILTINS - 1 - pr_numextbuiltins + j;
		qcvm->builtins[ext->number] = ext->func;
	}

	qcvm->numbuiltins = MAX_BUILTINS;

	// remap progs functions with id 0
	for (i = 0; i < qcvm->progs->numfunctions; i++)
	{
		func = &qcvm->functions[i];
		if (func->first_statement || func->parm_start || func->locals)
			continue;

		name = PR_GetString (func->s_name);
		for (j = 0; j < pr_numextbuiltins; j++)
		{
			extbuiltin_t *ext = &pr_extbuiltins[j];
			if (!strcmp (name, ext->name))
			{
				func->first_statement = -ext->number;
				break;
			}
		}
	}
}

/*
===============
PR_HasGlobal
===============
*/
static qboolean PR_HasGlobal (const char *name, float value)
{
	ddef_t *g = ED_FindGlobal (name);
	return g && (g->type & ~DEF_SAVEGLOBAL) == ev_float && G_FLOAT (g->ofs) == value;
}


/*
===============
PR_FindSupportedEffects

Checks for the presence of Quake 2021 release effects flags and returns a mask
with the correspondings bits either on or off depending on the result, in order
to avoid conflicts (e.g. Arcane Dimensions uses bit 32 for its explosions)
===============
*/
static int PR_FindSupportedEffects (void)
{
	qboolean isqex = 
		PR_HasGlobal ("EF_QUADLIGHT", EF_QEX_QUADLIGHT) &&
		(PR_HasGlobal ("EF_PENTLIGHT", EF_QEX_PENTALIGHT) || PR_HasGlobal ("EF_PENTALIGHT", EF_QEX_PENTALIGHT))
	;
	return isqex ? -1 : -1 & ~(EF_QEX_QUADLIGHT|EF_QEX_PENTALIGHT|EF_QEX_CANDLELIGHT);
}


/*
===============
PR_PatchRereleaseBuiltins

Quake 2021 release update 1 adds bprint/sprint/centerprint builtins with new id's
(see https://steamcommunity.com/games/2310/announcements/detail/2943653788150871156)
This function patches them back to use the old indices
===============
*/
static void PR_PatchRereleaseBuiltins (void)
{
	dfunction_t *f;
	if ((f = ED_FindFunction ("centerprint")) != NULL && f->first_statement == -90)
		f->first_statement = -73;
	if ((f = ED_FindFunction ("bprint")) != NULL && f->first_statement == -91)
		f->first_statement = -23;
	if ((f = ED_FindFunction ("sprint")) != NULL && f->first_statement == -92)
		f->first_statement = -24;
}


/*
===============
PR_LoadProgs
===============
*/
void PR_LoadProgs (void)
{
	int			i;

	CRC_Init (&qcvm->crc);

	qcvm->progs = (dprograms_t *)COM_LoadHunkFile ("progs.dat", NULL);
	if (!qcvm->progs)
		Host_Error ("PR_LoadProgs: couldn't load progs.dat");
	Con_DPrintf ("Programs occupy %iK.\n", com_filesize/1024);

	for (i = 0; i < com_filesize; i++)
		CRC_ProcessByte (&qcvm->crc, ((byte *)qcvm->progs)[i]);

	// byte swap the header
	for (i = 0; i < (int) sizeof(*qcvm->progs) / 4; i++)
		((int *)qcvm->progs)[i] = LittleLong ( ((int *)qcvm->progs)[i] );

	if (qcvm->progs->version != PROG_VERSION)
		Host_Error ("progs.dat has wrong version number (%i should be %i)", qcvm->progs->version, PROG_VERSION);
	if (qcvm->progs->crc != PROGHEADER_CRC)
		Host_Error ("progs.dat system vars have been modified, progdefs.h is out of date");

	qcvm->functions = (dfunction_t *)((byte *)qcvm->progs + qcvm->progs->ofs_functions);
	qcvm->strings = (char *)qcvm->progs + qcvm->progs->ofs_strings;
	if (qcvm->progs->ofs_strings + qcvm->progs->numstrings >= com_filesize)
		Host_Error ("progs.dat strings go past end of file\n");

	// initialize the strings
	qcvm->numknownstrings = 0;
	qcvm->maxknownstrings = 0;
	qcvm->stringssize = qcvm->progs->numstrings;
	if (qcvm->knownstrings)
		Z_Free ((void *)qcvm->knownstrings);
	qcvm->knownstrings = NULL;
	qcvm->firstfreeknownstring = NULL;
	PR_SetEngineString("");

	qcvm->globaldefs = (ddef_t *)((byte *)qcvm->progs + qcvm->progs->ofs_globaldefs);
	qcvm->fielddefs = (ddef_t *)((byte *)qcvm->progs + qcvm->progs->ofs_fielddefs);
	qcvm->statements = (dstatement_t *)((byte *)qcvm->progs + qcvm->progs->ofs_statements);

	qcvm->globals = (float *)((byte *)qcvm->progs + qcvm->progs->ofs_globals);
	pr_global_struct = (globalvars_t*)qcvm->globals;

	// byte swap the lumps
	for (i = 0; i < qcvm->progs->numstatements; i++)
	{
		qcvm->statements[i].op = LittleShort(qcvm->statements[i].op);
		qcvm->statements[i].a = LittleShort(qcvm->statements[i].a);
		qcvm->statements[i].b = LittleShort(qcvm->statements[i].b);
		qcvm->statements[i].c = LittleShort(qcvm->statements[i].c);
	}

	for (i = 0; i < qcvm->progs->numfunctions; i++)
	{
		qcvm->functions[i].first_statement = LittleLong (qcvm->functions[i].first_statement);
		qcvm->functions[i].parm_start = LittleLong (qcvm->functions[i].parm_start);
		qcvm->functions[i].s_name = LittleLong (qcvm->functions[i].s_name);
		qcvm->functions[i].s_file = LittleLong (qcvm->functions[i].s_file);
		qcvm->functions[i].numparms = LittleLong (qcvm->functions[i].numparms);
		qcvm->functions[i].locals = LittleLong (qcvm->functions[i].locals);
	}

	for (i = 0; i < qcvm->progs->numglobaldefs; i++)
	{
		qcvm->globaldefs[i].type = LittleShort (qcvm->globaldefs[i].type);
		qcvm->globaldefs[i].ofs = LittleShort (qcvm->globaldefs[i].ofs);
		qcvm->globaldefs[i].s_name = LittleLong (qcvm->globaldefs[i].s_name);
	}

	pr_alpha_supported = false; //johnfitz

	for (i = 0; i < qcvm->progs->numfielddefs; i++)
	{
		qcvm->fielddefs[i].type = LittleShort (qcvm->fielddefs[i].type);
		if (qcvm->fielddefs[i].type & DEF_SAVEGLOBAL)
			Host_Error ("PR_LoadProgs: pr_fielddefs[i].type & DEF_SAVEGLOBAL");
		qcvm->fielddefs[i].ofs = LittleShort (qcvm->fielddefs[i].ofs);
		qcvm->fielddefs[i].s_name = LittleLong (qcvm->fielddefs[i].s_name);

		//johnfitz -- detect alpha support in progs.dat
		if (!strcmp(qcvm->strings + qcvm->fielddefs[i].s_name,"alpha"))
			pr_alpha_supported = true;
		//johnfitz
	}

	for (i = 0; i < qcvm->progs->numglobals; i++)
		((int *)qcvm->globals)[i] = LittleLong (((int *)qcvm->globals)[i]);

	qcvm->edict_size = qcvm->progs->entityfields * 4 + sizeof(edict_t) - sizeof(entvars_t);
	// round off to next highest whole word address (esp for Alpha)
	// this ensures that pointers in the engine data area are always
	// properly aligned
	qcvm->edict_size += sizeof(void *) - 1;
	qcvm->edict_size &= ~(sizeof(void *) - 1);

	PR_InitHashTables ();
	PR_InitBuiltins ();
	PR_PatchRereleaseBuiltins ();

	pr_effects_mask = PR_FindSupportedEffects ();
}

/*
===============
ED_Nomonsters_f
===============
*/
static void ED_Nomonsters_f (cvar_t *cvar)
{
	if (cvar->value)
		Con_Warning ("\"%s\" can break gameplay.\n", cvar->name);
}


/*
===============
PR_Init
===============
*/
void PR_Init (void)
{
	Cmd_AddCommand ("edict", ED_PrintEdict_f);
	Cmd_AddCommand ("edicts", ED_PrintEdicts);
	Cmd_AddCommand ("edictcount", ED_Count);
	Cmd_AddCommand ("profile", PR_Profile_f);
	Cvar_RegisterVariable (&nomonsters);
	Cvar_SetCallback (&nomonsters, ED_Nomonsters_f);
	Cvar_RegisterVariable (&gamecfg);
	Cvar_RegisterVariable (&scratch1);
	Cvar_RegisterVariable (&scratch2);
	Cvar_RegisterVariable (&scratch3);
	Cvar_RegisterVariable (&scratch4);
	Cvar_RegisterVariable (&savedgamecfg);
	Cvar_RegisterVariable (&saved1);
	Cvar_RegisterVariable (&saved2);
	Cvar_RegisterVariable (&saved3);
	Cvar_RegisterVariable (&saved4);
}


edict_t *EDICT_NUM(int n)
{
	if (n < 0 || n >= qcvm->max_edicts)
		Host_Error ("EDICT_NUM: bad number %i", n);
	return (edict_t *)((byte *)qcvm->edicts + (n)*qcvm->edict_size);
}

int NUM_FOR_EDICT(edict_t *e)
{
	int		b;

	b = (byte *)e - (byte *)qcvm->edicts;
	b = b / qcvm->edict_size;

	if (b < 0 || b >= qcvm->num_edicts)
		Host_Error ("NUM_FOR_EDICT: bad pointer");
	return b;
}

//===========================================================================


#define	PR_STRING_ALLOCSLOTS	256

static int PR_AllocStringSlot (void)
{
	ptrdiff_t i;

	if (qcvm->firstfreeknownstring)
	{
		i = qcvm->firstfreeknownstring - qcvm->knownstrings;
		if (i < 0 || i >= qcvm->maxknownstrings)
			Sys_Error ("PR_AllocStringSlot failed: invalid free list index %" SDL_PRIs64 "/%i\n", (int64_t)i, qcvm->maxknownstrings);
		qcvm->firstfreeknownstring = (const char **) *qcvm->firstfreeknownstring;
	}
	else
	{
		i = qcvm->numknownstrings++;
		if (i >= qcvm->maxknownstrings)
		{
			qcvm->maxknownstrings += PR_STRING_ALLOCSLOTS;
			Con_DPrintf2 ("PR_AllocStringSlot: realloc'ing for %d slots\n", qcvm->maxknownstrings);
			qcvm->knownstrings = (const char **) Z_Realloc ((void *)qcvm->knownstrings, qcvm->maxknownstrings * sizeof(char *));
		}
	}

	return (int)i;
}

static qboolean PR_IsValidString (const char *p)
{
	if (!p)
		return false;
	// pointers inside the knownstrings array make up the free list and shouldn't be treated as actual strings
	if (p >= (const char *) qcvm->knownstrings && p < (const char *) (qcvm->knownstrings + qcvm->maxknownstrings))
		return false;
	return true;
}

const char *PR_GetString (int num)
{
	if (num >= 0 && num < qcvm->stringssize)
		return qcvm->strings + num;
	else if (num < 0 && num >= -qcvm->numknownstrings)
	{
		if (!qcvm->knownstrings[-1 - num])
		{
			Host_Error ("PR_GetString: attempt to get a non-existant string %d\n", num);
			return "";
		}
		return qcvm->knownstrings[-1 - num];
	}
	else
	{
		Host_Error("PR_GetString: invalid string offset %d\n", num);
		return "";
	}
}

int PR_SetEngineString (const char *s)
{
	int		i;

	if (!s)
		return 0;
#if 0	/* can't: sv.model_precache & sv.sound_precache points to pr_strings */
	if (s >= pr_strings && s <= pr_strings + pr_stringssize)
		Host_Error("PR_SetEngineString: \"%s\" in pr_strings area\n", s);
#else
	if (s >= qcvm->strings && s <= qcvm->strings + qcvm->stringssize - 2)
		return (int)(s - qcvm->strings);
#endif
	for (i = 0; i < qcvm->numknownstrings; i++)
	{
		if (qcvm->knownstrings[i] == s)
			return -1 - i;
	}
	// new unknown engine string
	//Con_DPrintf ("PR_SetEngineString: new engine string %p\n", s);
	i = PR_AllocStringSlot ();
	qcvm->knownstrings[i] = s;
	return -1 - i;
}

int PR_AllocString (int size, char **ptr)
{
	int		i;

	if (!size)
		return 0;
	i = PR_AllocStringSlot ();
	qcvm->knownstrings[i] = (char *)Hunk_AllocName(size, "string");
	if (ptr)
		*ptr = (char *) qcvm->knownstrings[i];
	return -1 - i;
}

