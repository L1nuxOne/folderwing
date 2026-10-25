#pragma once
#include "fn.h"
int foldersize_applies(const fn_ctx *c);
void foldersize_run(fn_ctx *c);
int foldersize_message(MSG *msg);
void foldersize_shutdown(void);
