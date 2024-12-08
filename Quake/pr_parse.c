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
	char *rest;
	float x, y, z;

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
		result.payload.reason = "Integer literals unimplemented";
		break;
	case '@':
		result.payload.reason = "String literals unimplemented";
		break;
	case '$':
		result.payload.reason = "User vars unimplemented";
		break;
	default:
		result.payload.reason = "Globals/fields unimplemented";
	}
	
	return result;
}

void PR_PrintArg(progsarg_t arg)
{
	Con_Printf("STUB\n");
}
