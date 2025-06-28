#ifndef PR_PARSE_H
#define PR_PARSE_H

#include "progs.h"

typedef enum
{
	// variables
	progsarg_global,
	progsarg_field,
	progsarg_uservar,

	// literals
	progsarg_string,
	progsarg_float,
	progsarg_vector,
	progsarg_int,
	progsarg_entity
} progsargkind_t;

typedef struct efield_s {
	edict_t *edict;
	ddef_t *fld;
} efield_t;

typedef struct progsarg_s
{
	progsargkind_t kind;
	union {
		const char *s;
		int i;
		float f;
		float v[3];
		ddef_t *g;
		efield_t efield;
	} value;
} progsarg_t;

typedef struct parseresult_s
{
	qboolean success;
	union {
		progsarg_t arg;
		const char *reason;
	} payload;
} parseresult_t;

parseresult_t PR_ParseCmdArg(const char *arg);
void PR_PrintArg(progsarg_t arg);

#endif
