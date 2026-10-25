/* Actual Windows coordinator and native helper; private per-helper mount namespaces. */
#include "../app/duplicates.h"
#include <stdio.h>
static int passed,failed;
static wchar_t parent[32768],child[32768],deep[32768],ordinary[32768],alias[32768];
#define CHECK(label,c) do { if(c){passed++;printf("ok %s\n",label);}else{failed++;printf("FAIL %d %s\n",__LINE__,label);} } while(0)
static duplicate_result scan(const wchar_t *const *roots,uint32_t n){duplicate_result r;duplicates_scan(roots,n,1,0,NULL,NULL,NULL,&r);printf("mount result state=%u error=%lu count=%u groups=%u members=%llu bytes=%llu aliases=%llu skipped=%llu entries=%u\n",r.state,(unsigned long)r.error,r.count,r.groups,(unsigned long long)r.members,(unsigned long long)r.duplicate_bytes,(unsigned long long)r.aliases,(unsigned long long)r.skipped,r.entries);return r;}
static void test_duplicate_mount_explicit_roots_and_aliases(void){
 const wchar_t *one[]={parent};duplicate_result r=scan(one,1);CHECK("parent excludes actual same-device bind mount",r.state==DUP_COMPLETE&&!r.errors&&r.count==2&&!r.groups&&r.aliases==1&&r.skipped==1);duplicates_free(&r);
 const wchar_t *only[]={child};r=scan(only,1);CHECK("explicit mounted root is independently readable and byte verified",r.state==DUP_COMPLETE&&!r.errors&&r.count==2&&r.groups==1&&r.members==2&&r.duplicate_bytes==11);duplicates_free(&r);
 const wchar_t *both[]={parent,child};r=scan(both,2);CHECK("explicit mounted root is not suppressed by parent scope",r.state==DUP_COMPLETE&&!r.errors&&r.count==4&&r.groups==1&&r.members==3&&r.duplicate_bytes==22&&r.aliases==1&&r.skipped==1);unsigned mask=0;for(uint32_t i=0;i<r.count;i++)if(r.files[i].group)mask|=1u<<r.files[i].root;CHECK("verified group preserves original root indices",mask==3);duplicates_free(&r);
 const wchar_t *reverse[]={child,parent,alias};r=scan(reverse,3);CHECK("child-first request and alternate UNC spelling preserve exact totals",r.state==DUP_COMPLETE&&!r.errors&&r.count==4&&r.groups==1&&r.members==3&&r.duplicate_bytes==22&&r.aliases==1&&r.skipped==1);mask=0;for(uint32_t i=0;i<r.count;i++)if(r.files[i].group)mask|=1u<<r.files[i].root;CHECK("scheduling retains caller indices and deduplicates UNC alias",mask==3);duplicates_free(&r);
}
static void test_duplicate_mount_deep_and_ordinary_overlap(void){
 const wchar_t *below[]={parent,deep};duplicate_result r=scan(below,2);CHECK("explicit path below excluded mount is still scanned",r.state==DUP_COMPLETE&&!r.errors&&r.count==3&&r.groups==1&&r.members==2&&r.duplicate_bytes==11&&r.aliases==1&&r.skipped==1);duplicates_free(&r);
 const wchar_t *base[]={parent};r=scan(base,1);uint32_t entries=r.entries;duplicates_free(&r);const wchar_t *overlap[]={ordinary,parent};r=scan(overlap,2);CHECK("completed ordinary child coverage is scanned once",r.state==DUP_COMPLETE&&!r.errors&&r.count==2&&!r.groups&&r.entries==entries&&r.aliases==1&&r.skipped==1);duplicates_free(&r);
}
int wmain(int argc,wchar_t **argv){setvbuf(stdout,NULL,_IONBF,0);if(argc!=2)return 2;wcsncpy(parent,argv[1],32767);swprintf(child,32768,L"%ls\\mounted",parent);swprintf(deep,32768,L"%ls\\nested",child);swprintf(ordinary,32768,L"%ls\\ordinary",parent);if(wcsncmp(parent,L"\\\\wsl.localhost\\",16))return 2;swprintf(alias,32768,L"\\\\wsl$\\%ls\\mounted\\",parent+16);test_duplicate_mount_explicit_roots_and_aliases();test_duplicate_mount_deep_and_ordinary_overlap();printf("%d passed, %d failed; 2 live mount selectors\n",passed,failed);return failed?1:0;}
