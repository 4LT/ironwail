#include "quakedef.h"
#include "pr_parse.h"

static int isnumber(const char *s) {
	char first_digit;

	if (s[0] == '-' || s[0] == '+')
	{
		first_digit = s[1];
	}
	else
	{
		first_digit = s[0];
	}

	return first_digit >= '0' && first_digit <= '9';
}

parseresult_t PR_ParseCmdArg(const char *arg)
{
	parseresult_t result;
	char *tokens, *rest;
	const char *tok;
	ddef_t *glob, *fielddef;
	edict_t *ed;
	float x, y, z;
    int i;

	result.success = false;

	switch(arg[0])
	{
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	case '-':
	case '+':
		if (!isnumber(arg))
		{
			result.payload.reason = "Not a valid float";
			break;
		}

		x = strtof(arg, &rest);

		if (rest[0] == '\0')
		{
			result.payload.arg.value.f = x;
			result.payload.arg.kind = progsarg_float;
			result.success = true;
			break;
		}

		while (isspace(rest[0])) rest++;

		if (!isnumber(rest))
		{
			result.payload.reason = "Not a valid vector";
			break;
		}

		y = strtof(rest, &rest);
		while (isspace(rest[0])) rest++;

		if (!isnumber(rest))
		{
			result.payload.reason = "Not a valid vector";
			break;
		}

		z = strtof(rest, &rest);
		result.payload.arg.value.v[0] = x;
		result.payload.arg.value.v[1] = y;
		result.payload.arg.value.v[2] = z;
		result.payload.arg.kind = progsarg_vector;
		result.success = true;
		break;
	case '#':
	case '$':
		if (!isnumber(arg + 1))
		{
			result.payload.reason = "Not a valid integer";
			break;
		}

        i = atoi(arg + 1);
		result.payload.arg.value.i = i;

		if (arg[0] == '#')
		{
            if (i < 0 || i > qcvm->num_edicts)
            {
                result.payload.reason = "Entity out of bounds";
                break;
            }
            else
            {
                result.payload.arg.kind = progsarg_entity;
            }
		}
		else
		{
            result.payload.arg.kind = progsarg_int;
		}

		result.success = true;
		break;
	case '@':
		result.payload.arg.value.s = arg + 1;
		result.payload.arg.kind = progsarg_string;
		result.success = true;
		break;
	case '%':
		result.payload.reason = "User vars unimplemented";
		break;
	default:
		tokens = strdup(arg);
		tok = strtok(tokens, ".");

		if (!tok)
		{
			result.payload.reason = "Bad token for global";
			goto cleanup;
		}

		glob = ED_FindGlobal(tok);

		if (!glob)
		{
			result.payload.reason = "Unrecognized global";
			goto cleanup;
		}

		tok = strtok(NULL, ".");
		
		if (!tok)
		{
			result.payload.arg.value.g = glob;
			result.payload.arg.kind = progsarg_global;
			result.success = true;
			goto cleanup;
		}

		ed = PROG_TO_EDICT(G_INT(glob->ofs));

		do
		{
			fielddef = ED_FindField(tok);

			if (!fielddef)
			{
				result.payload.reason = "Unrecognized field";
				goto cleanup;
			}

			tok = strtok(NULL, ".");

			if (tok)
			{

				if ((fielddef->type & ~DEF_SAVEGLOBAL) == ev_entity)
				{
					ed = PROG_TO_EDICT(E_INT(ed, fielddef->ofs));
				}
				else
				{
					result.payload.reason = "Can't access field of non-entity";
					goto cleanup;
				}
			}
		} while(tok);

		result.payload.arg.value.efield.fld = fielddef;
		result.payload.arg.value.efield.edict = ed;
		result.payload.arg.kind = progsarg_field;
		result.success = true;
cleanup:
		free(tokens);
	}
	
	return result;
}

static void SafePrintString(const char *s)
{
    if (s)
        Con_Printf("%s\n", s);
    else
        Con_Printf("(null string)\n");
}

static void PrintFunction(func_t func)
{
    if (func == 0)
        Con_Printf("Null function\n");

    Con_Printf(
        "Function %i%s\n",
        func,
        qcvm->functions[func].first_statement < 0 ? ", built-in" : ""
    );
}

static void PrintGlobal(ddef_t *g)
{
    unsigned short type = g->type & ~DEF_SAVEGLOBAL;

    switch (type)
    {
        case ev_string:
            SafePrintString(G_STRING(g->ofs));
            break;
        case ev_float:
            Con_Printf("%.3f\n", G_FLOAT(g->ofs));
            break;
        case ev_vector:
            Con_Printf("\'%.3f %.3f %.3f\'\n",
                G_FLOAT(g->ofs),
                G_FLOAT(g->ofs + 1),
                G_FLOAT(g->ofs + 2)
            );
            break;
        case ev_entity:
            ED_Print(G_EDICT(g->ofs));
            break;
        case ev_field:
            Con_Printf("Field %i\n", G_INT(g->ofs));
            break;
        case ev_function:
            PrintFunction(G_FUNCTION(g->ofs));
            break;

        default:
            Con_Printf("Unsupported type %i\n", (signed)type);
    }
}

static void PrintEntityField(efield_t ent_fld)
{
    float *vec;
    edict_t *ent = ent_fld.edict;
    ddef_t *fld = ent_fld.fld;
    unsigned short type = fld->type & ~DEF_SAVEGLOBAL;

    switch (type)
    {
        case ev_string:
            SafePrintString(E_STRING(ent, fld->ofs));
            break;
        case ev_float:
            Con_Printf("%.3f\n", E_FLOAT(ent, fld->ofs));
            break;
        case ev_vector:
            vec = E_VECTOR(ent, fld->ofs);
            Con_Printf("\'%.3f %.3f %.3f\'\n", vec[0], vec[1], vec[2]);
            break;
        case ev_entity:
            ED_Print(PROG_TO_EDICT(E_INT(ent, fld->ofs)));
            break;
        case ev_function:
            PrintFunction(E_INT(ent, fld->ofs));
            break;

        default:
            Con_Printf("Unsupported type %i\n", (signed)type);
    }
}

void PR_PrintArg(progsarg_t arg)
{
    float *vec;

    switch (arg.kind) {
        case progsarg_global:
            PrintGlobal(arg.value.g);
            break;
        case progsarg_field:
            PrintEntityField(arg.value.efield);
            break;
        case progsarg_string:
            SafePrintString(arg.value.s);
            break;
        case progsarg_float:
            Con_Printf("%.3f\n", arg.value.f);
            break;
        case progsarg_vector:
            vec = arg.value.v;
            Con_Printf("\'%.3f %.3f %.3f\'\n", vec[0], vec[1], vec[2]);
            break;
        case progsarg_int:
            Con_Printf("Integer %i\n", arg.value.i);
            break;
        case progsarg_entity:
            ED_Print(EDICT_NUM(arg.value.i));
            break;

        default:
            Con_Printf("Unsupported argument kind %i\n", arg.kind);
    }
}

