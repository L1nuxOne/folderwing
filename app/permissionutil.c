#include "permissionutil.h"
int permission_octal(const wchar_t *text,uint32_t *out)
{
    if(!text||!out)return 0;size_t n=wcslen(text);if(n<3||n>4)return 0;
    uint32_t value=0;for(size_t i=0;i<n;i++){if(text[i]<L'0'||text[i]>L'7')return 0;value=value*8+text[i]-L'0';}
    *out=value;return 1;
}
int permission_bit_state(const uint32_t *modes,size_t count,uint32_t bit)
{
    if(!modes||!count)return PERMISSION_KEEP;int first=(modes[0]&bit)!=0;
    for(size_t i=1;i<count;i++)if(((modes[i]&bit)!=0)!=first)return PERMISSION_KEEP;
    return first;
}
int permission_patch_action(const permission_patch *p,uint32_t bit)
{
    return (p->mask&bit)?((p->value&bit)?PERMISSION_SET:PERMISSION_CLEAR):PERMISSION_KEEP;
}
void permission_patch_choose(permission_patch *p,uint32_t bit,int action)
{
    bit&=07777;if(action==PERMISSION_KEEP){p->mask&=~bit;p->value&=~bit;}
    else{p->mask|=bit;if(action==PERMISSION_SET)p->value|=bit;else p->value&=~bit;}
}
void permission_patch_cycle(permission_patch *p,uint32_t bit,int original)
{
    int action=permission_patch_action(p,bit);
    if(action==PERMISSION_KEEP)action=original==PERMISSION_SET?PERMISSION_CLEAR:PERMISSION_SET;
    else if(original==PERMISSION_KEEP)action=action==PERMISSION_SET?PERMISSION_CLEAR:PERMISSION_KEEP;
    else action=action!=original?PERMISSION_KEEP:!original;
    permission_patch_choose(p,bit,action);
}
uint32_t permission_patch_mode(const permission_patch *p,uint32_t original)
{
    uint32_t mask=p->mask&07777;return (original&~mask)|(p->value&mask);
}
