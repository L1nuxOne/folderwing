/* F07: production terminal selection, libvterm and Windows clipboard. A guard
 * page immediately follows the production allocation; clipboard faults are
 * deterministic wrappers, not hooks in the shipped terminal. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
static void *arena, *guarded;
static size_t arena_size, last_bytes;
static int heap_fail, fault, outstanding, heap_calls;
/* Clipboard history/synchronization readers can hold this global resource
 * briefly. Bound harness acquisition; the shipped function has no retry. */
static BOOL available_open(HWND h)
{
    uint64_t until=GetTickCount64()+500;
    do{if(OpenClipboard(h))return TRUE;Sleep(1);}while(GetTickCount64()<until);
    return FALSE;
}
static void *copy_malloc(size_t n)
{
    heap_calls++;last_bytes=n;if(heap_fail)return NULL;
    SYSTEM_INFO si;GetSystemInfo(&si);size_t page=si.dwPageSize;
    if(!n||n>SIZE_MAX-page*2||arena)return NULL;
    arena_size=((n+page-1)/page+1)*page;
    arena=VirtualAlloc(NULL,arena_size,MEM_RESERVE,PAGE_NOACCESS);
    if(!arena)return NULL;
    if(!VirtualAlloc(arena,arena_size-page,MEM_COMMIT,PAGE_READWRITE)){VirtualFree(arena,0,MEM_RELEASE);arena=NULL;return NULL;}
    guarded=(char *)arena+arena_size-page-n;return guarded;
}
static void copy_free(void *p)
{
    if(!p)return;
    if(p!=guarded){fputs("unexpected production allocation\n",stderr);abort();}
    VirtualFree(arena,0,MEM_RELEASE);arena=guarded=NULL;
}
static HGLOBAL WINAPI copy_global_alloc(UINT flags,SIZE_T n)
{if(fault==1)return NULL;HGLOBAL g=GlobalAlloc(flags,n);if(g)outstanding++;return g;}
static LPVOID WINAPI copy_global_lock(HGLOBAL g){return fault==2?NULL:GlobalLock(g);}
static BOOL WINAPI copy_open(HWND h){return fault==3?FALSE:available_open(h);}
static BOOL WINAPI copy_empty(void){return fault==4?FALSE:EmptyClipboard();}
static HANDLE WINAPI copy_set(UINT format,HANDLE g)
{if(fault==5)return NULL;HANDLE result=SetClipboardData(format,g);if(result)outstanding--;return result;}
static HGLOBAL WINAPI copy_global_free(HGLOBAL g){HGLOBAL result=GlobalFree(g);if(!result)outstanding--;return result;}
#define malloc copy_malloc
#define free copy_free
#define GlobalAlloc copy_global_alloc
#define GlobalLock copy_global_lock
#define OpenClipboard copy_open
#define EmptyClipboard copy_empty
#define SetClipboardData copy_set
#define GlobalFree copy_global_free
#define wWinMain terminal_entry_unused
#ifndef TERMINAL_SOURCE
#define TERMINAL_SOURCE "../app/fpterm.c"
#endif
#include TERMINAL_SOURCE
#undef malloc
#undef free
#undef GlobalAlloc
#undef GlobalLock
#undef OpenClipboard
#undef EmptyClipboard
#undef SetClipboardData
#undef GlobalFree
#undef wWinMain
static int passed,failed;
#define VERIFY(label,cond) do{if(cond){passed++;printf("  ok    %s\n",label);}else{failed++;printf("  FAIL  %s (line %d)\n",label,__LINE__);}fflush(stdout);}while(0)
static HWND owner;
static sess fixture;
static void select_range(int64_t a,int ac,int64_t b,int bc)
{A.sel_s=&fixture;A.sel_on=1;A.sel_al=a;A.sel_ac=ac;A.sel_bl=b;A.sel_bc=bc;}
static void fixture_begin(int rows,int cols)
{
    memset(&A,0,sizeof A);memset(&fixture,0,sizeof fixture);
    A.hwnd=owner;A.dpi=96;A.cw=8;A.chh=16;A.h=600;A.w=1000;A.ntabs=1;A.tabs[0]=&fixture;
    fixture.sb_cap=rows;fixture.sb_n=rows;fixture.sb=calloc((size_t)rows,sizeof *fixture.sb);
    for(int y=0;y<rows;y++){fixture.sb[y]=calloc(1,sizeof(sline)+(size_t)cols*sizeof(tcell));fixture.sb[y]->n=cols;}
    select_range(0,0,rows-1,cols);fault=heap_fail=heap_calls=0;last_bytes=0;
}
static void fixture_end(void)
{for(int i=0;i<fixture.sb_n;i++)free(fixture.sb[i]);free(fixture.sb);if(fixture.vt)vterm_free(fixture.vt);VERIFY("temporary copy allocations and untransferred handles are released",!arena&&!outstanding);}
static int seed_clipboard(const wchar_t *text)
{
    size_t bytes=(wcslen(text)+1)*sizeof *text;HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,bytes);if(!g)return 0;
    void *p=GlobalLock(g);if(!p){GlobalFree(g);return 0;}memcpy(p,text,bytes);GlobalUnlock(g);
    if(!available_open(owner)){GlobalFree(g);return 0;}BOOL ok=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,g)!=NULL;
    CloseClipboard();if(!ok)GlobalFree(g);return ok;
}
static int clipboard_equals(const wchar_t *text,size_t n)
{
    int same=0;if(!available_open(owner))return 0;HGLOBAL g=GetClipboardData(CF_UNICODETEXT);const wchar_t *p=g?GlobalLock(g):NULL;
    if(p){same=GlobalSize(g)>=(n+1)*sizeof *p&&!memcmp(p,text,n*sizeof *p)&&p[n]==0;GlobalUnlock(g);}CloseClipboard();return same;
}
static size_t scalar(wchar_t *p,uint32_t c)
{if(c<0x10000){p[0]=(wchar_t)c;return 1;}c-=0x10000;p[0]=(wchar_t)(0xd800+(c>>10));p[1]=(wchar_t)(0xdc00+(c&1023));return 2;}
static void test_terminal_selection_utf16(void)
{
    puts("Terminal selection: exact supplementary UTF-16, guard boundary, all copy shortcuts and real libvterm");
    const int counts[]={0,1,259,260,261,300,511,512};wchar_t expected[1030];
    for(unsigned j=0;j<sizeof counts/sizeof *counts;j++){
        int n=counts[j];fixture_begin(1,n);size_t k=0;
        for(int i=0;i<n;i++){uint32_t ch=0x1d400+(unsigned)(i%26);fixture.sb[0]->c[i].ch=ch;k+=scalar(expected+k,ch);}expected[k]=0;
        VERIFY("boundary fixture seeds the clipboard",seed_clipboard(L"previous"));
        copy_selection();VERIFY("single-width supplementary cells copy exact surrogate pairs",clipboard_equals(expected,k));
        fixture_end();
    }
    fixture_begin(1,8);uint32_t cells[]={L'A',0,0x03b2,0x1f642|AT_WIDE,AT_SKIP,L'Z',L' ',L' '};
    for(int i=0;i<8;i++)fixture.sb[0]->c[i].ch=cells[i];
    const wchar_t mixed[]={L'A',L' ',0x03b2,0xd83d,0xde42,L'Z',0};
    key('C',PANELP_M_CTRL|PANELP_M_SHIFT);VERIFY("Ctrl+Shift+C keeps the selection and skips wide continuation cells",A.sel_on&&clipboard_equals(mixed,6));
    key('C',PANELP_M_CTRL);VERIFY("Ctrl+C copies before clearing selection",!A.sel_on&&clipboard_equals(mixed,6));
    select_range(0,0,0,8);panelp_input click={.msg=WM_RBUTTONDOWN,.x=20,.y=100};input(&click);
    VERIFY("right click copies before clearing selection",!A.sel_on&&clipboard_equals(mixed,6));fixture_end();
    fixture_begin(0,0);fixture.rows=2;fixture.cols=512;fixture.vt=vterm_new(2,512);VERIFY("real terminal parser initializes",fixture.vt!=NULL);
    if(fixture.vt){vterm_set_utf8(fixture.vt,1);fixture.scr=vterm_obtain_screen(fixture.vt);vterm_screen_reset(fixture.scr,1);
        char bytes[1200];size_t k=0;for(int i=0;i<300;i++){memcpy(bytes+i*4,"\xf0\x9d\x90\x80",4);k+=scalar(expected+k,0x1d400);}expected[k]=0;
        VERIFY("real libvterm accepts 300 supplementary characters",vterm_input_write(fixture.vt,bytes,sizeof bytes)==sizeof bytes);
        select_range(0,0,0,300);key('C',PANELP_M_CTRL|PANELP_M_SHIFT);
        VERIFY("production live-screen conversion and keyboard copy retain all 300 characters",clipboard_equals(expected,k));
        A.w=5000;panelp_input drag={.msg=WM_LBUTTONDOWN,.x=pad_x(),.y=tabbar_h()+S(2)+A.chh/2};input(&drag);
        drag.msg=WM_MOUSEMOVE;drag.x+=300*A.cw;input(&drag);drag.msg=WM_LBUTTONUP;input(&drag);
        VERIFY("production mouse selection retains the complete 300-cell range",A.sel_on&&!A.selecting&&A.sel_ac==0&&A.sel_bc==300);
        key('C',PANELP_M_CTRL|PANELP_M_SHIFT);VERIFY("mouse-selected supplementary output reaches the real Unicode clipboard",clipboard_equals(expected,k));
    }fixture_end();
}
static void test_terminal_selection_ranges(void)
{
    puts("Terminal selection: multiline CRLF, reversed endpoints and dropped scrollback");
    fixture_begin(3,512);wchar_t expected[3100];size_t k=0;
    for(int y=0;y<3;y++){for(int i=0;i<512;i++){fixture.sb[y]->c[i].ch=0x1d400+(unsigned)y;k+=scalar(expected+k,0x1d400+(unsigned)y);}if(y<2){expected[k++]=L'\r';expected[k++]=L'\n';}}
    expected[k]=0;copy_selection();VERIFY("full-width multiline supplementary selection preserves CRLF and final NUL",clipboard_equals(expected,k));
    select_range(2,512,0,0);copy_selection();VERIFY("reversed selection has identical output",clipboard_equals(expected,k));fixture_end();
    fixture_begin(3,4);const wchar_t *lines[]={L"a   ",L"    ",L" b  "};for(int y=0;y<3;y++)for(int c=0;c<4;c++)fixture.sb[y]->c[c].ch=lines[y][c];
    copy_selection();VERIFY("trimming preserves interior blank lines and leading spaces",clipboard_equals(L"a\r\n\r\n b",7));
    fixture.dropped=100;select_range(INT64_MIN,0,INT64_MAX,512);copy_selection();VERIFY("extreme stale endpoints clip to retained lines without overflow",clipboard_equals(L"a\r\n\r\n b",7)&&last_bytes<10000);
    VERIFY("stale selection seed is available",seed_clipboard(L"untouched"));select_range(-9,0,99,4);int calls=heap_calls;copy_selection();VERIFY("fully dropped selection neither allocates nor replaces clipboard",calls==heap_calls&&clipboard_equals(L"untouched",9));
    fixture.dropped=INT64_MAX-2;select_range(INT64_MAX-2,0,INT64_MAX,4);copy_selection();VERIFY("last retained INT64_MAX line does not wrap iteration",clipboard_equals(L"a\r\n\r\n b",7));
    fixture.dropped=INT64_MAX;calls=heap_calls;copy_selection();VERIFY("invalid retained range is rejected before arithmetic",calls==heap_calls);
    fixture.dropped=0;select_range(0,-3,0,1);copy_selection();VERIFY("negative starting column cannot read before a line",clipboard_equals(L"a",1));fixture_end();
}
static void test_terminal_clipboard_failures(void)
{
    puts("Terminal clipboard: allocation, lock, open and publication failures");fixture_begin(1,1);fixture.sb[0]->c[0].ch='x';
    for(int mode=0;mode<=5;mode++){
        VERIFY("failure fixture has previous clipboard text",seed_clipboard(L"keep"));heap_fail=mode==0;fault=mode;copy_selection();
        VERIFY("failure releases temporary allocation and clipboard handle",!arena&&!outstanding);
        if(mode<5)VERIFY("preparation/open/empty failure preserves the previous clipboard",clipboard_equals(L"keep",4));
        else VERIFY("failed SetClipboardData does not claim transferred ownership",outstanding==0);
        heap_fail=fault=0;
    }
    copy_selection();VERIFY("successful publication transfers a complete Unicode object",clipboard_equals(L"x",1));fixture_end();
}
int wmain(void)
{
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    owner=CreateWindowExW(0,L"STATIC",L"OpenPilot terminal clipboard regression",0,0,0,0,0,HWND_MESSAGE,NULL,GetModuleHandleW(NULL),NULL);
    VERIFY("private clipboard owner window is available",owner!=NULL);if(!owner)return 1;
    test_terminal_selection_utf16();test_terminal_selection_ranges();test_terminal_clipboard_failures();DestroyWindow(owner);
    printf("\n%d passed, %d failed\n",passed,failed);return failed?1:0;
}
