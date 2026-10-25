#pragma once
#include "fn.h"
int permissions_applies(const fn_ctx *c);
void permissions_run(fn_ctx *c);
int permissions_message(MSG *msg);
void permissions_shutdown(void);
