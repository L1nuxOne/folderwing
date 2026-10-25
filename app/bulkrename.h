#pragma once
#include "fn.h"
int bulkrename_applies(const fn_ctx *c);
void bulkrename_run(fn_ctx *c);
int bulkrename_message(MSG *msg);
void bulkrename_shutdown(void);
