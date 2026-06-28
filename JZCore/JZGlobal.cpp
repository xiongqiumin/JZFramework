#include <stdlib.h>
#include "JZGlobal.h"
#include "JZLog.h"

void jzassert_log(const char *file, int line, const char *message)
{
    LOG_FMT_F("%s(%d): %s",file,line,message);
}