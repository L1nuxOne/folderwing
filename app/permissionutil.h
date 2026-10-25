#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#define PERMISSION_MAX_ITEMS 256u
enum { PERMISSION_KEEP=-1, PERMISSION_CLEAR=0, PERMISSION_SET=1 };
typedef struct permission_patch {uint32_t mask,value;} permission_patch;
int permission_octal(const wchar_t *text,uint32_t *out);
int permission_bit_state(const uint32_t *modes,size_t count,uint32_t bit);
int permission_patch_action(const permission_patch *p,uint32_t bit);
void permission_patch_choose(permission_patch *p,uint32_t bit,int action);
/* Mixed bits cycle keep/set/clear; uniform bits toggle away/back to original. */
void permission_patch_cycle(permission_patch *p,uint32_t bit,int original);
uint32_t permission_patch_mode(const permission_patch *p,uint32_t original);
