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
	int ofs;

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
	case '!':
		if (!isnumber(arg + 1))
		{
			result.payload.reason = "Not a valid integer";
			break;
		}

		ofs = atoi(arg + 1);

		if (arg[0] == '!')
		{
			ofs = EDICT_TO_PROG(EDICT_NUM(ofs));
		}

		result.payload.arg.value.i = ofs;
		result.payload.arg.kind = progsarg_int;
		result.success = true;
		break;
	case '@':
		result.payload.arg.value.s = arg + 1;
		result.payload.arg.kind = progsarg_string;
		result.success = true;
		break;
	case '$':
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
		result.payload.arg.value.efield.edict = NUM_FOR_EDICT(ed);
		result.payload.arg.kind = progsarg_field;
		result.success = true;
cleanup:
		free(tokens);
	}
	
	return result;
}

void PR_PrintArg(progsarg_t arg)
{
	Con_Printf("STUB\n");
}
