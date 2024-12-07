#include "pr_parse.h"

parseresult_t PR_ParseCmdArg(const char *arg)
{
	parseresult_t result;
	result.payload.reason = "Stub implementation";
	result.success = false;
	return result;
}

void PR_PrintArg(progsarg_t arg)
{
	Con_Printf("STUB\n");
}
