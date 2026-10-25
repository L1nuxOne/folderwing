#pragma once
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#define PANELP_DUPLICATE_OPEN 63u
#define PANELP_DUPLICATE_ACTION 165u
#define PANELP_DUPLICATE_STATE 166u
#define DUPLICATE_ROOTS 16u
typedef struct duplicate_open {uint64_t session;uint32_t count,wsl,as_root,reserved;uint32_t chars[DUPLICATE_ROOTS];} duplicate_open;
typedef struct duplicate_state {uint64_t session;uint32_t generation,state,groups,files;uint64_t members,duplicate_bytes,errors;} duplicate_state;
typedef struct duplicate_action {uint64_t session;uint32_t generation,root,action,reserved;} duplicate_action;
enum {DUPLICATE_LEFT=0,DUPLICATE_RIGHT,DUPLICATE_PREVIEW,DUPLICATE_COPY,DUPLICATE_MENU};
static inline int duplicate_relative_safe(const wchar_t *p){if(!p||!*p||*p==L'\\')return 0;while(*p){const wchar_t *e=wcschr(p,L'\\');size_t n=e?(size_t)(e-p):wcslen(p);if(!n||(n==1&&*p==L'.')||(n==2&&p[0]==L'.'&&p[1]==L'.'))return 0;for(size_t i=0;i<n;i++)if(p[i]==L':'||p[i]==L'/')return 0;if(!e)return 1;p=e+1;}return 0;}

/* Host policy update: uint32_t WSL enabled (0/1); future work only. */
#define PANELP_DUPLICATE_WSL_POLICY 66u
