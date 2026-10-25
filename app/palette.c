/* The palette: see palette.h.
 *
 * Three modes share one overlay:
 *   Go to      folders worth jumping to (recent, tabs, pinned, drives),
 *              ranked by fuzzy match, then Everything's matches on the disk
 *   commands   every function the registry offers that applies right now
 *              (">" in Go to switches here, Backspace on empty goes back)
 *   search     Everything, an optional native WSL index, or a live folder scan
 *
 * Lifetime: the function that opened it holds the registry while it is
 * open; the window, the lists and the results are freed on close, so a
 * closed palette costs nothing. Each Everything query is a use of
 * search.everything, timed until its answer arrives (fn.search.everything),
 * and nothing waits on it: answers to older queries are dropped.
 *
 * The window is a child of the main window placed over the panes (they
 * clip siblings), so it moves with the window and never takes activation.
 * Clicking anywhere else in the app closes it. */
#include "palette.h"

#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "everything.h"
#include "fuzzy.h"
#include "shell.h"
#include "theme.h"
#include "renameutil.h"

enum { MODE_JUMP, MODE_CMDS, MODE_HERE };
enum { IT_CMD, IT_PLACE, IT_EV };
enum { EV_NONE, EV_WAIT, EV_DONE, EV_ABSENT, EV_FAILED, EV_TIMEOUT };
enum { BACKEND_NONE, BACKEND_EVERYTHING, BACKEND_LIVE, BACKEND_WSL_INDEX };

#define ROWS_MAX      12
#define EV_JUMP_MAX   100
#define EV_HERE_MAX   300
#define EV_ID0        0x4F500000u /* reply ids: EV_ID0 + query number */
#define EV_TIMEOUT_MS 3000
#define TIMER_EV      1
#define PM_CLOSE      (WM_APP + 40)
#define PM_SEARCH     (WM_APP + 41)
#define PM_EV_SENT    (WM_APP + 42)
#define TIMER_QUERY   2
#define TIMER_SEARCH  3
#define TIMER_INDEX   4
#define INDEX_BUTTON  1000
#define INDEX_ACTION  1100
#define TEXT_MAX      256

typedef struct live_search {
    HWND hwnd;HANDLE thread;CRITICAL_SECTION lock;volatile LONG stop;int done,queued;
    uint64_t notified;wchar_t *root;wchar_t query[TEXT_MAX];int allocation_error;
    ev_item hits[EV_HERE_MAX];uint32_t count;ev_list *list;fpe_search_stats stats;
    int query_job,force_live,backend;uint32_t action;fpe_index_status index;
} live_search;
static uint32_t sequence;

typedef struct item {
    uint8_t        kind, folder, src;
    char          *fn_id; /* owned ID of the command shown in this row */
    uint32_t       fn_revision;
    const wchar_t *path; /* places, Everything */
    const wchar_t *name; /* first column */
    const wchar_t *sub;  /* second column, dim */
    int            sub_len;
    wchar_t       *own[2];
    wchar_t        keys[32];
    int            score;
    uint32_t       order;
    uint32_t       hits[FUZZY_HITS_WORDS];  /* in name */
    uint32_t       shits[FUZZY_HITS_WORDS]; /* in sub */
} item;

static struct {
    HWND          hwnd, edit, back, index_button;
    WNDPROC       edit_orig;
    HBRUSH        edit_brush;
    const fn_def *held;
    Pane         *pane;
    int           mode, from_jump, setting;
    wchar_t       here[32768];

    item    *items;
    uint32_t nitems, items_cap;
    item    *evit;
    uint32_t nev;
    ev_list *ev;
    item   **shown;
    uint32_t nshown, shown_cap, ev_first;
    int      sel, top, hover, wheel;

    uint32_t ev_seq, ev_out; /* the latest query; queries not answered yet */
    int      ev_state;
    int      backend, coverage, probing, force_live, closing;
    live_search *scan;
    fpe_search_stats stats;
    fpe_index_status index;
    int want_query,pending_index,index_available;uint32_t index_action;uint64_t query_due;
    uint32_t ev_total;
    wchar_t  last_query[TEXT_MAX + MAX_PATH + 8];
    wchar_t  indexed_query[35072];

    uintptr_t published_values[9];uint32_t published_mask;
    wchar_t published_title[480];int published_title_valid;

    HDC     dc;
    HBITMAP bmp, old_bmp;
    int     bw, bh, w, h;
} PL;

static const fn_def *ev_def(void) { return fn_find("search.everything"); }

static int head_h(void) { return app_px(42); }
static int row_h(void) { return app_px(30); }
static int index_context(void){return PL.mode==MODE_HERE&&PL.index_available&&fpe_wsl_native(PL.here);}
static int foot_h(void) { return app_px(PL.mode==MODE_HERE?(index_context()?78:52):26); }

/* ---- items ---------------------------------------------------------------- */

static void free_items(void)
{
    for (uint32_t i = 0; i < PL.nitems; i++) {
        free(PL.items[i].fn_id);
        free(PL.items[i].own[0]);
        free(PL.items[i].own[1]);
    }
    PL.nitems = 0;
}

static item *new_item(void)
{
    if (PL.nitems == PL.items_cap) {
        uint32_t cap = PL.items_cap ? PL.items_cap * 2 : 64;
        item *v = realloc(PL.items, cap * sizeof *v);
        if (!v) return NULL;
        PL.items = v;
        PL.items_cap = cap;
    }
    item *it = &PL.items[PL.nitems];
    memset(it, 0, sizeof *it);
    it->order = PL.nitems++;
    it->sub_len = -1;
    return it;
}

static wchar_t *widen(const char *s)
{
    int n = s ? MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0) : 0;
    wchar_t *w = n > 0 ? malloc((size_t)n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static const wchar_t *last_segment(const wchar_t *path)
{
    size_t n = wcslen(path);
    while (n > 1 && path[n - 1] == L'\\') n--; /* "C:\" stays whole */
    for (size_t i = n; i-- > 0;)
        if (path[i] == L'\\' && i + 1 < n) return path + i + 1;
    return path;
}

static void load_commands(void)
{
    fn_ctx c;
    uint32_t revision = fn_revision();
    app_pane_ctx(&c, PL.pane, FN_FROM_PALETTE);
    for (uint32_t i = 0; i < fn_count(); i++) {
        const fn_def *d = fn_at(i);
        if (!fn_in_palette(d) || !fn_applies(d, &c)) continue;
        item *it = new_item();
        if (!it) break;
        it->kind = IT_CMD;
        it->fn_revision = revision;
        it->fn_id = _strdup(d->id);
        if (d->label && (it->own[0] = malloc(160 * sizeof(wchar_t)))) {
            d->label(&c, it->own[0], 160);
            it->own[0][159] = 0;
        } else if (!d->label) it->own[0] = _wcsdup(d->title);
        if (!it->fn_id || !it->own[0]) {
            free(it->fn_id); free(it->own[0]); PL.nitems--; break;
        }
        it->name = it->own[0];
        it->sub = it->own[1] = widen(d->about);
        fn_key_text(d, it->keys, 32);
    }
    app_ctx_free(&c);
    if (fn_revision() != revision) free_items();
}

static void add_place(void *user, const wchar_t *path, const wchar_t *label, int kind)
{
    const wchar_t *cur = app_pane_path(PL.pane);
    if (!path || !path[0] || (cur && !_wcsicmp(cur, path))) return;
    for (uint32_t i = 0; i < PL.nitems; i++)
        if (!_wcsicmp(PL.items[i].path, path)) return;
    item *it = new_item();
    if (!it) return;
    it->kind = IT_PLACE;
    it->src = (uint8_t)kind;
    it->folder = 1;
    it->path = it->own[0] = _wcsdup(path);
    it->own[1] = label ? _wcsdup(label) : NULL;
    if (!it->path) {
        PL.nitems--;
        return;
    }
    it->name = it->own[1] ? it->own[1] : last_segment(it->path);
    it->sub = it->path;
}

static void load_items(void)
{
    free_items();
    if (PL.mode == MODE_CMDS) load_commands();
    else if (PL.mode == MODE_JUMP) app_places(add_place, NULL);
}

/* ---- Everything ---------------------------------------------------------------- */

static void ev_drop(void)
{
    /* Published rows point into evit. Retire those rows before freeing the
     * backing storage: a refreshed query can wait on its worker or debounce. */
    PL.nshown=PL.ev_first;
    for(uint32_t i=0;i<PL.nev;i++){free(PL.evit[i].own[0]);free(PL.evit[i].own[1]);}
    free(PL.ev);
    free(PL.evit);
    PL.ev = NULL;
    PL.evit = NULL;
    PL.nev = 0;
    PL.ev_total = 0;
}

/* Every query still waiting is over (answered, superseded or given up). */
static void ev_settle(void)
{
    while (PL.ev_out) {
        PL.ev_out--;
        fn_end(ev_def());
    }
    if (PL.hwnd) KillTimer(PL.hwnd, TIMER_EV);
}

static void refilter(int keep_sel);
static void publish_state(void);

static int by_rank(const void *a, const void *b)
{
    const item *x = a, *y = b;
    if (x->score != y->score) return x->score > y->score ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

static void take_results(ev_list *l)
{
    ev_drop();
    if (!l) {
        PL.ev_state = EV_FAILED;
        refilter(1);
        return;
    }
    PL.ev = l;
    PL.ev_total = l->total;
    PL.ev_state = EV_DONE;
    PL.evit = calloc(l->n ? l->n : 1, sizeof *PL.evit);
    if (PL.evit) {
        /* Everything sorts by name; rank by how well the name matches instead
         * (Everything syntax such as ext:pdf matches nothing here and keeps its order) */
        wchar_t t[TEXT_MAX];
        GetWindowTextW(PL.edit, t, TEXT_MAX);
        for (uint32_t i = 0; i < l->n; i++) {
            const ev_item *e = &l->items[i];
            item *it = &PL.evit[PL.nev];
            it->kind = IT_EV;
            it->folder = e->folder;
            it->order = PL.nev++;
            it->path = e->path;
            it->name = e->path + e->name;
            it->sub = e->path;
            it->sub_len = e->name > 1 ? (int)e->name - 1 : (int)e->name; /* the folder, without its last "\" */
            if((PL.backend==BACKEND_LIVE||PL.backend==BACKEND_WSL_INDEX)&&fpe_wsl_native(PL.here)){
                it->own[0]=malloc(1600*sizeof(wchar_t));
                if(it->own[0]){rename_display(it->name,1,it->own[0],1600);it->name=it->own[0];}
            }
            it->score = fuzzy_score(t, it->name, NULL);
            if (it->score >= 0 && it->folder && PL.mode == MODE_JUMP) it->score += 6;
        }
        qsort(PL.evit, PL.nev, sizeof *PL.evit, by_rank);
    }
    refilter(1);
}

/* A worker never touches the palette or its lists. One bounded mailbox, one
 * worker per overlay; superseding a query cancels it before starting another. */
static int live_hit(void *user,const wchar_t *path,const fpe_entry *entry,const fpe_search_stats *stats)
{
    live_search *s=user;if(InterlockedCompareExchange(&s->stop,0,0))return 0;
    if(path){
        wchar_t *copy=_wcsdup(path);if(!copy){s->allocation_error=1;return 0;}
        ev_item *it=&s->hits[s->count++];it->path=copy;it->folder=fpe_entry_is_dir(entry);
        const wchar_t *name=wcsrchr(copy,L'\\');it->name=name?(uint32_t)(name+1-copy):0;
    }
    uint64_t now=GetTickCount64();
    if(now-s->notified>=100){s->notified=now;EnterCriticalSection(&s->lock);s->stats=*stats;
        if(!s->queued)s->queued=PostMessageW(s->hwnd,PM_SEARCH,0,0)!=0;LeaveCriticalSection(&s->lock);}
    return !InterlockedCompareExchange(&s->stop,0,0);
}
static int index_keep(void *user,const fpe_index_status *index)
{(void)index;live_search *s=user;return !InterlockedCompareExchange(&s->stop,0,0);}
static DWORD WINAPI live_worker(void *user)
{
    live_search *s=user;fpe_search_stats stats={0};ev_list *list=NULL;
    if(!s->query_job){
        fpe_search_index_control(s->root,s->action,index_keep,s,&s->index);goto finished;
    }
    int indexed=0;
    if(!s->force_live&&fpe_wsl_native(s->root)&&s->index.state!=FPE_INDEX_PAUSED){
        fpe_index_status *attempt=malloc(sizeof *attempt);
        if(attempt){
            fpe_err err=fpe_search_folder_indexed(s->root,s->query,live_hit,s,&stats,attempt);
            indexed=err==0;if(err!=ERROR_NOT_SUPPORTED)s->index=*attempt;free(attempt);
        }
    }
    s->backend=indexed?BACKEND_WSL_INDEX:BACKEND_LIVE;
    if(!indexed&&!InterlockedCompareExchange(&s->stop,0,0)){
        for(uint32_t i=0;i<s->count;i++)free(s->hits[i].path);s->count=0;s->allocation_error=0;
        fpe_search_folder(s->root,s->query,live_hit,s,&stats);
    }
    if(s->allocation_error){stats.errors++;stats.first_error=ERROR_NOT_ENOUGH_MEMORY;}
    size_t chars=0;for(uint32_t i=0;i<s->count;i++)chars+=wcslen(s->hits[i].path)+1;
    list=malloc(sizeof *list+s->count*sizeof(ev_item)+chars*sizeof(wchar_t));
    if(list){list->n=list->total=s->count;wchar_t *text=(wchar_t*)(list->items+s->count);
        for(uint32_t i=0;i<s->count;i++){list->items[i]=s->hits[i];list->items[i].path=text;size_t n=wcslen(s->hits[i].path)+1;memcpy(text,s->hits[i].path,n*sizeof *text);text+=n;}}
    else{stats.errors++;if(!stats.first_error)stats.first_error=ERROR_NOT_ENOUGH_MEMORY;}
    for(uint32_t i=0;i<s->count;i++)free(s->hits[i].path);
finished:
    EnterCriticalSection(&s->lock);s->stats=stats;s->list=list;s->done=1;
    if(!s->queued)s->queued=PostMessageW(s->hwnd,PM_SEARCH,0,0)!=0;LeaveCriticalSection(&s->lock);return 0;
}
static void cancel_live(void)
{
    if(PL.scan){InterlockedExchange(&PL.scan->stop,1);CancelSynchronousIo(PL.scan->thread);}
}
static void start_worker(int query_job,uint32_t action)
{
    if(query_job){PL.backend=BACKEND_LIVE;PL.probing=0;PL.ev_state=EV_WAIT;PL.want_query=0;}
    if(PL.scan){publish_state();return;}
    live_search *s=calloc(1,sizeof *s);if(!s){PL.ev_state=EV_FAILED;publish_state();return;}
    s->root=_wcsdup(PL.here);wcscpy(s->query,PL.last_query);s->hwnd=PL.hwnd;s->query_job=query_job;s->force_live=PL.force_live;s->action=action;s->index=PL.index;InitializeCriticalSection(&s->lock);
    if(s->root)s->thread=CreateThread(NULL,0,live_worker,s,CREATE_SUSPENDED,NULL);
    if(!s->thread){free(s->root);DeleteCriticalSection(&s->lock);free(s);PL.ev_state=EV_FAILED;publish_state();return;}
    PL.scan=s;SetTimer(PL.hwnd,TIMER_SEARCH,100,NULL);ResumeThread(s->thread);publish_state();
}
static void start_live(void){start_worker(1,FPE_INDEX_STATUS);}
static void index_action(uint32_t action)
{
    if(!index_context()||action<FPE_INDEX_ENABLE||action>FPE_INDEX_REMOVE)return;
    PL.pending_index=1;PL.index_action=action;PL.want_query=!!PL.last_query[0];cancel_live();
    if(!PL.scan){PL.pending_index=0;start_worker(0,action);}
}
/* Sending IPC on a worker keeps a hung/elevated Everything out of UI dispatch.
 * Reply data still arrives at the palette through its normal WM_COPYDATA. */
static DWORD WINAPI send_worker(void *user)
{
    struct send {HWND hwnd;uint32_t id,max;wchar_t text[];} *s=user;
    int rc=ev_query(s->hwnd,EV_ID0+s->id,s->text,s->max);
    PostMessageW(s->hwnd,PM_EV_SENT,s->id,(LPARAM)rc);free(s);return 0;
}
static void send_query(const wchar_t *q,uint32_t max)
{
    if(fn_begin(ev_def())){start_live();return;}
    PL.ev_out++;PL.ev_state=EV_WAIT;PL.ev_seq=++sequence;
    struct send {HWND hwnd;uint32_t id,max;wchar_t text[];} *s=malloc(sizeof *s+(wcslen(q)+1)*sizeof(wchar_t));
    HANDLE thread=NULL;if(s){s->hwnd=PL.hwnd;s->id=PL.ev_seq;s->max=max;wcscpy(s->text,q);thread=CreateThread(NULL,0,send_worker,s,0,NULL);}
    if(thread){CloseHandle(thread);SetTimer(PL.hwnd,TIMER_EV,EV_TIMEOUT_MS,NULL);}
    else{free(s);ev_settle();if(PL.mode==MODE_HERE)start_live();else PL.ev_state=EV_FAILED;}
    publish_state();
}
static void dispatch_query(void)
{
    if(PL.closing||!PL.last_query[0])return;
    uint64_t now=GetTickCount64();if(now<PL.query_due){SetTimer(PL.hwnd,TIMER_QUERY,(UINT)(PL.query_due-now),NULL);return;}
    if(PL.scan){PL.want_query=1;return;}
    PL.want_query=0;
    if(PL.mode==MODE_JUMP){send_query(PL.last_query,EV_JUMP_MAX);return;}
    wchar_t normalized[2304];
    if(PL.force_live||fpe_wsl_native(PL.here)||!fn_active(ev_def())||!ev_window()||
       fpe_search_index_query(PL.last_query,normalized,2304)||PL.coverage<0){start_live();return;}
    if(!PL.coverage){
        if(!ev_folder_query(PL.here,PL.indexed_query,35072)){start_live();return;}
        PL.backend=BACKEND_EVERYTHING;PL.probing=1;send_query(PL.indexed_query,1);return;
    }
    size_t n=wcslen(PL.here);
    _snwprintf(PL.indexed_query,35072,L"path:nowildcards:\"%ls%ls\" %ls",PL.here,n&&PL.here[n-1]==L'\\'?L"":L"\\",normalized);
    PL.indexed_query[35071]=0;PL.backend=BACKEND_EVERYTHING;PL.probing=0;send_query(PL.indexed_query,EV_HERE_MAX);
}
static void ev_answer(const COPYDATASTRUCT *cds)
{
    if(cds->dwData!=EV_ID0+PL.ev_seq||!PL.ev_out||PL.closing)return;
    ev_settle();ev_list *list=ev_parse(cds->lpData,cds->cbData);
    if(PL.mode==MODE_HERE&&PL.probing){
        PL.probing=0;int covered=0;
        if(list)for(uint32_t i=0;i<list->n;i++)if(list->items[i].folder&&fpe_path_equal(list->items[i].path,PL.here))covered=1;
        free(list);PL.coverage=covered?1:-1;dispatch_query();
    }else if(!list&&PL.mode==MODE_HERE)start_live();else take_results(list);
    InvalidateRect(PL.hwnd,NULL,FALSE);
}
static void query(const wchar_t *text)
{
    int want=PL.mode==MODE_HERE?!!text[0]:PL.mode==MODE_JUMP&&wcslen(text)>=2&&fn_active(ev_def());
    if(!wcscmp(text,PL.last_query)&&want)return;
    PL.ev_seq=++sequence;ev_settle();if(PL.scan&&PL.scan->query_job)cancel_live();ev_drop();PL.stats=(fpe_search_stats){0};PL.probing=0;PL.want_query=want;
    PL.last_query[0]=0;PL.ev_state=EV_NONE;PL.backend=BACKEND_NONE;KillTimer(PL.hwnd,TIMER_QUERY);
    if(want){wcscpy(PL.last_query,text);PL.query_due=GetTickCount64()+160;
        if(PL.mode==MODE_HERE&&fpe_search_validate(text)){PL.ev_state=EV_FAILED;PL.want_query=0;}
        else{PL.ev_state=EV_WAIT;SetTimer(PL.hwnd,TIMER_QUERY,160,NULL);}}
    refilter(0);
}

/* ---- ranking --------------------------------------------------------------------- */

static int by_score(const void *a, const void *b) { return by_rank(*(item *const *)a, *(item *const *)b); }

static int place_bonus(const item *it)
{
    switch (it->src) {
    case APP_PLACE_RECENT: return it->order < 12 ? 12 - (int)it->order : 0;
    case APP_PLACE_PIN: return 8;
    case APP_PLACE_TAB: return 6;
    default: return 3;
    }
}

static int score_local(item *it, const wchar_t *t)
{
    memset(it->shits, 0, sizeof it->shits);
    if (!t[0]) {
        memset(it->hits, 0, sizeof it->hits);
        return 0;
    }
    int s = fuzzy_score(t, it->name, it->hits);
    if (it->kind == IT_CMD) {
        wchar_t id[64];
        if (MultiByteToWideChar(CP_UTF8, 0, it->fn_id, -1, id, 64)) {
            int si = fuzzy_score(t, id, NULL) - 10;
            if (si > s) {
                memset(it->hits, 0, sizeof it->hits);
                s = si;
            }
        }
        return s;
    }
    if (s >= 0) s += 20; /* the folder's own name beats a match in its parents */
    int sp = fuzzy_score(t, it->path, it->shits);
    if (sp > s) {
        memset(it->hits, 0, sizeof it->hits);
        s = sp;
    } else {
        memset(it->shits, 0, sizeof it->shits);
    }
    return s < 0 ? -1 : s + place_bonus(it);
}

static void place_window(void);
static void set_mode(int mode, const wchar_t *text);

static void refilter(int keep_sel)
{
    wchar_t t[TEXT_MAX];
    GetWindowTextW(PL.edit, t, TEXT_MAX);
    uint32_t need = PL.nitems + PL.nev;
    if (need > PL.shown_cap) {
        item **v = realloc(PL.shown, need * sizeof *v);
        if (!v) return;
        PL.shown = v;
        PL.shown_cap = need;
    }
    PL.nshown = 0;
    for (uint32_t i = 0; i < PL.nitems; i++) {
        item *it = &PL.items[i];
        it->score = score_local(it, t);
        if (it->score >= 0) PL.shown[PL.nshown++] = it;
    }
    if (t[0]) qsort(PL.shown, PL.nshown, sizeof *PL.shown, by_score);
    PL.ev_first = PL.nshown;
    for (uint32_t i = 0; i < PL.nev; i++) {
        item *it = &PL.evit[i];
        int dup = 0;
        for (uint32_t k = 0; k < PL.ev_first && !dup; k++) dup = !_wcsicmp(PL.shown[k]->path, it->path);
        if (dup) continue;
        if (fuzzy_score(t, it->name, it->hits) < 0) memset(it->hits, 0, sizeof it->hits);
        PL.shown[PL.nshown++] = it;
    }
    if (!keep_sel) PL.sel = PL.top = 0;
    if (PL.sel >= (int)PL.nshown) PL.sel = PL.nshown ? (int)PL.nshown - 1 : 0;
    if (PL.top > PL.sel) PL.top = PL.sel;
    place_window();
    publish_state();
    InvalidateRect(PL.hwnd, NULL, FALSE);
}

static void on_text(void)
{
    wchar_t t[TEXT_MAX];
    GetWindowTextW(PL.edit, t, TEXT_MAX);
    if (PL.mode == MODE_JUMP && t[0] == L'>') {
        set_mode(MODE_CMDS, t + 1);
        return;
    }
    refilter(0);
    query(t);
}

/* ---- window -------------------------------------------------------------------- */

static int visible_rows(void) { return PL.nshown < 1 ? 1 : PL.nshown > ROWS_MAX ? ROWS_MAX : (int)PL.nshown; }

static void place_window(void)
{
    if (!PL.hwnd) return;
    RECT rc;
    GetClientRect(app_window(), &rc);
    int w = app_px(720);
    if (w > rc.right - app_px(40)) w = rc.right - app_px(40);
    if (w < app_px(300)) w = app_px(300);
    int h = head_h() + visible_rows() * row_h() + foot_h() + 2;
    int x = (rc.right - w) / 2, y = app_px(44);
    if (y + h > rc.bottom) y = rc.bottom - h > 0 ? rc.bottom - h : 0;
    SetWindowPos(PL.hwnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
}

static const wchar_t *mode_label(wchar_t *buf, size_t cap)
{
    if (PL.mode == MODE_CMDS) return L"Run";
    if (PL.mode == MODE_JUMP) return L"Go to";
    _snwprintf(buf, cap, L"Search in %ls", last_segment(PL.here));
    buf[cap - 1] = 0;
    return buf;
}

static int label_width(HDC dc)
{
    wchar_t buf[MAX_PATH + 16];
    const wchar_t *s = mode_label(buf, MAX_PATH + 16);
    SIZE sz = { 0 };
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    int cap = PL.w * 2 / 5;
    return (sz.cx < cap ? sz.cx : cap) + app_px(14);
}

static void place_edit(void)
{
    HDC dc = GetDC(PL.hwnd);
    HGDIOBJ old = SelectObject(dc, app_font());
    int x = app_px(14) + label_width(dc);
    SelectObject(dc, old);
    ReleaseDC(PL.hwnd, dc);
    int eh = app_px(20);
    int ix=index_context(),button=app_px(78);
    if(ix&&!PL.index_button){
        PL.index_button=CreateWindowExW(0,L"BUTTON",L"Index...",WS_CHILD|BS_OWNERDRAW,0,0,0,0,PL.hwnd,(HMENU)(UINT_PTR)INDEX_BUTTON,GetModuleHandleW(NULL),NULL);
        if(PL.index_button)SendMessageW(PL.index_button,WM_SETFONT,(WPARAM)app_font(),FALSE);
    }
    MoveWindow(PL.edit, x, (head_h() - eh) / 2, PL.w - x - app_px(18)-(ix?button:0), eh, TRUE);
    if(PL.index_button){ShowWindow(PL.index_button,ix?SW_SHOWNA:SW_HIDE);MoveWindow(PL.index_button,PL.w-button-app_px(8),app_px(7),button-app_px(6),head_h()-app_px(14),TRUE);}
}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = { x, y, x + w, y + h };
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, GetStockObject(DC_BRUSH));
}

static void text(HDC dc, const wchar_t *s, int len, int x, int y, int right, int h, COLORREF c, UINT fmt)
{
    if (right <= x) return;
    RECT r = { x, y, right, y + h };
    SetTextColor(dc, c);
    DrawTextW(dc, s, len, &r, fmt | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}

/* Text with its matched characters in the accent colour; returns where it ends. */
static int text_hits(HDC dc, const wchar_t *s, int len, int x, int y, int right, int h, COLORREF c, const uint32_t *hits)
{
    int any = 0;
    for (int i = 0; i < FUZZY_HITS_WORDS && !any; i++) any = hits[i] != 0;
    SIZE sz;
    if (!any) {
        GetTextExtentPoint32W(dc, s, len, &sz);
        text(dc, s, len, x, y, right, h, c, DT_END_ELLIPSIS);
        return x + sz.cx < right ? x + sz.cx : right;
    }
    for (int i = 0; i < len && x < right;) {
        int hit = fuzzy_hit(hits, (size_t)i), j = i + 1;
        while (j < len && fuzzy_hit(hits, (size_t)j) == hit) j++;
        GetTextExtentPoint32W(dc, s + i, j - i, &sz);
        text(dc, s + i, j - i, x, y, right, h, hit ? C_ACCENT : c, j == len ? DT_END_ELLIPSIS : 0);
        x += sz.cx;
        i = j;
    }
    return x < right ? x : right;
}

static void draw_icon(HDC dc, const item *it, int x, int y, int h)
{
    app_draw_icon(dc, it->path, it->folder, it->kind == IT_PLACE ? it->src : -1, x, y, h);
}

static const wchar_t *place_tag(const item *it)
{
    if (it->kind == IT_EV) return NULL;
    switch (it->src) {
    case APP_PLACE_RECENT: return L"recent";
    case APP_PLACE_TAB: return L"tab";
    case APP_PLACE_PIN: return L"pinned";
    default: return NULL;
    }
}

static void paint_row(HDC dc, const item *it, int y, int selected, int hovered)
{
    int rh = row_h(), pad = app_px(14), right = PL.w - pad;
    if (selected) fill(dc, 1, y, PL.w - 2, rh, C_SEL);
    else if (hovered) fill(dc, 1, y, PL.w - 2, rh, C_SEL_IDLE);
    int x = pad;
    if (it->kind != IT_CMD) {
        draw_icon(dc, it, x, y, rh);
        x += app_px(26);
    }
    const wchar_t *tag = it->kind == IT_CMD ? (it->keys[0] ? it->keys : NULL) : place_tag(it);
    if (tag) {
        SIZE sz;
        GetTextExtentPoint32W(dc, tag, (int)wcslen(tag), &sz);
        text(dc, tag, -1, right - sz.cx, y, right, rh, C_DIM, 0);
        right -= sz.cx + app_px(16);
    }
    int name_right = x + (right - x) * 3 / 5;
    int end = text_hits(dc, it->name, (int)wcslen(it->name), x, y, name_right, rh, C_TEXT, it->hits);
    if (!it->sub) return;
    int sl = it->sub_len >= 0 ? it->sub_len : (int)wcslen(it->sub);
    int sx = end + app_px(14);
    int any = 0;
    for (int i = 0; i < FUZZY_HITS_WORDS && !any; i++) any = it->shits[i] != 0;
    if (any) text_hits(dc, it->sub, sl, sx, y, right, rh, C_DIM, it->shits);
    else text(dc, it->sub, sl, sx, y, right, rh, C_DIM, it->kind == IT_CMD ? DT_END_ELLIPSIS : DT_PATH_ELLIPSIS);
}

static void status_text(wchar_t *buf, size_t cap)
{
    buf[0] = 0;
    if (PL.mode == MODE_CMDS) return;
    if(PL.mode==MODE_HERE){
        if(PL.ev_state==EV_FAILED){_snwprintf(buf,cap,L"Invalid query or search failed. Use words, *.ext, ext:, file:, folder:, size:, dm:");}
        else if(PL.backend==BACKEND_LIVE||PL.backend==BACKEND_WSL_INDEX){
            if(PL.stats.errors&&!PL.stats.scanned){
                _snwprintf(buf,cap,L"Live search unavailable (error %u). Check folder access and, for WSL, the installed distro / helper setting.",PL.stats.first_error);buf[cap-1]=0;return;
            }
            _snwprintf(buf,cap,L"%ls: %ls%u matches; %u checked%ls%ls%ls",PL.backend==BACKEND_WSL_INDEX?L"WSL index":L"Live",PL.ev_state==EV_WAIT?L"searching; ":L"",PL.ev_state==EV_WAIT?PL.stats.matched:PL.ev_total,PL.stats.scanned,
                PL.stats.limited?L"; limit reached":L"",PL.stats.errors?L"; partial / access errors":L"",PL.stats.skipped?L"; links / mounts skipped":L"");
        }else if(PL.ev_state==EV_WAIT)_snwprintf(buf,cap,PL.probing?L"Checking Everything coverage...":L"Searching...");
        else if(PL.ev_state==EV_DONE)_snwprintf(buf,cap,L"Everything: %u matches%ls",PL.ev_total,PL.ev&&PL.ev->n<PL.ev_total?L" (top shown)":L"");
        else _snwprintf(buf,cap,L"Words, *.ext, ext:, file:, folder:, size:, dm:   Ctrl+Shift+F live");
        buf[cap-1]=0;return;
    }
    if (!fn_active(ev_def())) {
        if (PL.mode == MODE_JUMP) _snwprintf(buf, cap, L"Everything search is off");
        return;
    }
    switch (PL.ev_state) {
    case EV_WAIT: _snwprintf(buf, cap, L"Everything: searching..."); break;
    case EV_DONE: _snwprintf(buf, cap, L"Everything: %u match%ls%ls", PL.ev_total, PL.ev_total == 1 ? L"" : L"es",
                             PL.ev && PL.ev->n < PL.ev_total ? L" (top shown)" : L""); break;
    case EV_ABSENT: _snwprintf(buf, cap, L"Everything is not running"); break;
    case EV_FAILED: _snwprintf(buf, cap, L"Everything did not take the query"); break;
    case EV_TIMEOUT: _snwprintf(buf, cap, L"Everything did not answer"); break;
    default:
        if (PL.mode == MODE_JUMP) _snwprintf(buf, cap, L"Type 2+ letters to search the disk too");
        break;
    }
    buf[cap - 1] = 0;
}

static void index_text(wchar_t *buf,size_t cap)
{
    const wchar_t *state=PL.index.state==FPE_INDEX_CURRENT?L"Current":PL.index.state==FPE_INDEX_SCANNING?L"Scanning":
        PL.index.state==FPE_INDEX_STALE?L"Updating":PL.index.state==FPE_INDEX_PAUSED?L"Paused":PL.index.state==FPE_INDEX_ERROR?L"Unavailable / incomplete":L"Off";
    if(PL.scan&&!PL.scan->query_job&&PL.scan->action!=FPE_INDEX_STATUS)_snwprintf(buf,cap,L"WSL index: applying change...");
    else if(PL.index.state==FPE_INDEX_ERROR)_snwprintf(buf,cap,L"WSL index: %ls (error %u) · live search available",state,PL.index.error);
    else if(!PL.index.enabled)_snwprintf(buf,cap,L"WSL index: Off · Index... to select this folder");
    else if(PL.index.state==FPE_INDEX_SCANNING)_snwprintf(buf,cap,L"WSL index: Scanning · %llu checked · live while indexing",(unsigned long long)PL.index.scanned);
    else _snwprintf(buf,cap,L"WSL index: %ls · %llu entries · %ls",state,(unsigned long long)PL.index.entries,PL.index.root);
    buf[cap-1]=0;
}
/* Publish model transitions independently of painting. A completed snapshot
 * exposes its counts/backend before its Done flag, and repeats no unchanged
 * native writes. Remember only successful publications so failures retry. */
static int publish_value(unsigned index,const wchar_t *name,uintptr_t value)
{
    uint32_t bit=1u<<index;
    if((PL.published_mask&bit)&&PL.published_values[index]==value)return 1;
    if(!SetPropW(PL.hwnd,name,(HANDLE)value))return 0;
    PL.published_values[index]=value;PL.published_mask|=bit;return 1;
}
static void publish_state(void)
{
    if(!PL.hwnd)return;
    static const wchar_t *names[]={L"Search.Backend",L"Search.State",L"Search.Count",L"Search.Limited",L"Search.Errors",L"Index.State",L"Index.Enabled",L"Index.Entries",L"Index.Error"};
    uintptr_t values[]={(uintptr_t)PL.backend,(uintptr_t)PL.ev_state,PL.nshown,PL.stats.limited,PL.stats.errors,(uintptr_t)PL.index.state,(uintptr_t)PL.index.enabled,(uintptr_t)PL.index.entries,PL.index.error};
    if(PL.ev_state!=EV_DONE)publish_value(1,names[1],values[1]);
    int ready=1;for(unsigned i=0;i<9;i++)if(i!=1)ready&=publish_value(i,names[i],values[i]);
    wchar_t status[128],title[480];status_text(status,128);
    if(index_context()){wchar_t index[320];index_text(index,320);_snwprintf(title,480,L"%ls | %ls",status,index);title[479]=0;}
    else wcscpy(title,status);
    if(!PL.published_title_valid||wcscmp(PL.published_title,title)){
        if(SetWindowTextW(PL.hwnd,title)){wcscpy(PL.published_title,title);PL.published_title_valid=1;}
    }
    if(PL.ev_state==EV_DONE&&ready)publish_value(1,names[1],values[1]);
}
static void index_menu(void)
{
    if(!index_context())return;HMENU menu=CreatePopupMenu();if(!menu)return;
    if(!PL.index.enabled){
        AppendMenuW(menu,MF_STRING,INDEX_ACTION+FPE_INDEX_ENABLE,L"Index this folder and subfolders");
        if(PL.index.state==FPE_INDEX_ERROR&&PL.index.root[0]){
            AppendMenuW(menu,MF_STRING,INDEX_ACTION+FPE_INDEX_REBUILD,L"Rebuild saved index");
            AppendMenuW(menu,MF_STRING,INDEX_ACTION+FPE_INDEX_REMOVE,L"Remove saved index");
        }
    }
    else{
        wchar_t scope[640]=L"Index: ";size_t pos=7;
        for(size_t i=0;PL.index.root[i]&&pos+3<640;i++){scope[pos++]=PL.index.root[i];if(PL.index.root[i]==L'&')scope[pos++]=L'&';}scope[pos]=0;
        AppendMenuW(menu,MF_STRING|MF_GRAYED,0,scope);
        AppendMenuW(menu,MF_SEPARATOR,0,NULL);
        AppendMenuW(menu,MF_STRING,INDEX_ACTION+(PL.index.state==FPE_INDEX_PAUSED?FPE_INDEX_RESUME:FPE_INDEX_PAUSE),PL.index.state==FPE_INDEX_PAUSED?L"Resume indexing":L"Pause indexing");
        AppendMenuW(menu,MF_STRING,INDEX_ACTION+FPE_INDEX_REBUILD,L"Rescan / rebuild index");
        AppendMenuW(menu,MF_STRING,INDEX_ACTION+FPE_INDEX_REMOVE,L"Remove this folder's index");
    }
    RECT r;GetWindowRect(PL.index_button,&r);POINT point={r.left,r.bottom};UINT chosen=app_menu_track(menu,PL.hwnd,point);DestroyMenu(menu);
    if(chosen>=INDEX_ACTION+FPE_INDEX_ENABLE&&chosen<=INDEX_ACTION+FPE_INDEX_REMOVE)index_action(chosen-INDEX_ACTION);
}

static const wchar_t *hint_text(void)
{
    if (PL.mode == MODE_CMDS) return L"Enter run   Esc close";
    const item *it = PL.nshown && PL.sel < (int)PL.nshown ? PL.shown[PL.sel] : NULL;
    if (it && !it->folder) return L"Enter show   Ctrl+Enter new tab   Shift+Enter open";
    if (PL.mode == MODE_JUMP) return L"Enter go   Ctrl+Enter new tab   Shift+Enter other pane   > commands";
    return L"Enter go   Ctrl+Enter new tab   Shift+Enter other pane";
}

static void paint(HDC dc)
{
    int w = PL.w, h = PL.h, hh = head_h(), rh = row_h(), pad = app_px(14);
    HGDIOBJ old_font = SelectObject(dc, app_font());
    fill(dc, 0, 0, w, h, C_POPUP);

    wchar_t lb[MAX_PATH + 16];
    int lw = label_width(dc);
    text(dc, mode_label(lb, MAX_PATH + 16), -1, pad, 0, pad + lw - app_px(10), hh, C_ACCENT, DT_END_ELLIPSIS);
    fill(dc, pad + lw - app_px(6), (hh - app_px(28)) / 2, w - pad - lw - app_px(6), app_px(28), C_EDIT);
    fill(dc, 1, hh - 1, w - 2, 1, C_BORDER);

    int rows = visible_rows();
    if (!PL.nshown) {
        const wchar_t *msg = PL.mode == MODE_CMDS ? L"No command matches"
                             : PL.mode == MODE_HERE && GetWindowTextLengthW(PL.edit) == 0
                                 ? L"Type to search this folder and its subfolders"
                             : PL.ev_state == EV_WAIT ? L"Searching..."
                             : PL.ev_state==EV_FAILED?L"Check the query: words, wildcards, ext:, file:, folder:, size:, dm:"
                             : PL.stats.errors?L"Search incomplete — some locations could not be read"
                                                      : L"Nothing matches";
        text(dc, msg, -1, pad, hh, w - pad, rh, C_DIM, DT_END_ELLIPSIS);
    }
    for (int r = 0; r < rows && PL.top + r < (int)PL.nshown; r++) {
        int i = PL.top + r, y = hh + r * rh;
        if (i == (int)PL.ev_first && i > 0) fill(dc, pad, y, w - 2 * pad, 1, C_BORDER); /* Everything's below */
        paint_row(dc, PL.shown[i], y, i == PL.sel, i == PL.hover);
    }

    int fy = hh + rows * rh;
    fill(dc, 1, fy, w - 2, 1, C_BORDER);
    wchar_t st[128],ix[320];
    status_text(st, 128);
    if(index_context())index_text(ix,320);
    publish_state(); /* retry native publication failures without depending on paint for progress */
    const wchar_t *hint = hint_text();
    SIZE sz;
    GetTextExtentPoint32W(dc, hint, (int)wcslen(hint), &sz);
    int hx = w - pad - sz.cx;
    if(PL.mode==MODE_HERE){
        text(dc,st,-1,pad,fy,w-pad,app_px(26),PL.ev_state>=EV_ABSENT||PL.stats.errors?C_ERROR:C_DIM,DT_END_ELLIPSIS);
        int hint_y=fy+app_px(26);
        if(index_context()){text(dc,ix,-1,pad,hint_y,w-pad,app_px(26),PL.index.state==FPE_INDEX_ERROR?C_ERROR:C_DIM,DT_END_ELLIPSIS);hint_y+=app_px(26);}
        text(dc,hint,-1,pad,hint_y,w-pad,app_px(26),C_DIM,DT_END_ELLIPSIS);
    }else{
        text(dc, st, -1, pad, fy, hx - app_px(12), foot_h(), PL.ev_state >= EV_ABSENT ? C_ERROR : C_DIM, DT_END_ELLIPSIS);
        text(dc, hint, -1, hx > pad ? hx : pad, fy, w - pad, foot_h(), C_DIM, DT_END_ELLIPSIS);
    }

    SetDCBrushColor(dc, C_BORDER);
    RECT all = { 0, 0, w, h };
    FrameRect(dc, &all, GetStockObject(DC_BRUSH));
    SelectObject(dc, old_font);
}

static void ensure_visible(void)
{
    int rows = visible_rows();
    if (PL.sel < PL.top) PL.top = PL.sel;
    if (PL.sel >= PL.top + rows) PL.top = PL.sel - rows + 1;
    if (PL.top < 0) PL.top = 0;
}

static void move_sel(int delta)
{
    if (!PL.nshown) return;
    int s = PL.sel + delta;
    if (s < 0) s = 0;
    if (s >= (int)PL.nshown) s = (int)PL.nshown - 1;
    PL.sel = s;
    ensure_visible();
    InvalidateRect(PL.hwnd, NULL, FALSE);
}

static void close_palette(int restore_focus)
{
    if (!PL.hwnd && !PL.held) return;
    HWND h = PL.hwnd, back = PL.back;
    const fn_def *held = PL.held;
    PL.ev_seq=++sequence;
    ev_settle();
    if (restore_focus && back && IsWindow(back) && IsWindowVisible(back)) {
        HWND f = GetFocus();
        if (f == PL.edit || f == h) SetFocus(back);
    }
    if(PL.scan){PL.closing=1;cancel_live();ShowWindow(h,SW_HIDE);KillTimer(h,TIMER_QUERY);KillTimer(h,TIMER_INDEX);return;}
    PL.hwnd = NULL; /* before destroying: focus changes during it must not close twice */
    if (h) DestroyWindow(h);
    if (PL.dc) {
        SelectObject(PL.dc, PL.old_bmp);
        DeleteObject(PL.bmp);
        DeleteDC(PL.dc);
    }
    if (PL.edit_brush) DeleteObject(PL.edit_brush);
    free_items();
    free(PL.items);
    free(PL.shown);
    ev_drop();
    memset(&PL, 0, sizeof PL);
    if (held) fn_release(held);
}

static void activate(int i, int ctrl, int shift)
{
    if (i < 0 || i >= (int)PL.nshown) return;
    const item *it = PL.shown[i];
    Pane *p = PL.pane;
    if (it->kind == IT_CMD) {
        uint32_t revision = it->fn_revision;
        char *id = _strdup(it->fn_id);
        close_palette(1);
        if (!id) return;
        if (fn_revision() != revision) { free(id); return; }
        fn_ctx c;
        app_pane_ctx(&c, p, FN_FROM_PALETTE);
        fn_run_id_at_revision(id, revision, &c);
        app_ctx_free(&c);
        free(id);
        return;
    }
    wchar_t *path = _wcsdup(it->path);
    int folder = it->folder;
    close_palette(1);
    if (!path) return;
    if (folder) {
        fn_ctx c;
        app_pane_ctx(&c, p, FN_FROM_PALETTE);
        int dual = (c.state & FN_S_DUAL) != 0;
        app_ctx_free(&c);
        if (ctrl) app_tab_new(p, path, 1);
        else app_navigate(shift && dual ? app_other_pane(p) : p, path);
    } else if (shift) {
        if ((INT_PTR)ShellExecuteW(app_window(), NULL, path, NULL, NULL, SW_SHOWNORMAL) <= 32)
            app_status(p, 1, L"Could not open %ls", path);
    } else {
        if (ctrl) {
            wchar_t *slash = wcsrchr(path, L'\\');
            if (slash) {
                wchar_t keep = slash[1];
                slash[1] = 0;
                app_tab_new(p, path, 1);
                slash[1] = keep;
            }
        }
        app_reveal(p, path);
    }
    free(path);
}

static LRESULT CALLBACK edit_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    WNDPROC orig = PL.edit_orig;
    switch (m) {
    case WM_KEYDOWN: {
        int ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        switch (w) {
        case VK_UP: move_sel(-1); return 0;
        case VK_DOWN: move_sel(1); return 0;
        case VK_PRIOR: move_sel(-(ROWS_MAX - 1)); return 0;
        case VK_NEXT: move_sel(ROWS_MAX - 1); return 0;
        case VK_RETURN: activate(PL.sel, ctrl, shift); return 0;
        case VK_ESCAPE: close_palette(1); return 0;
        case VK_TAB: return 0;
        case VK_BACK:
            if (PL.mode == MODE_CMDS && PL.from_jump && GetWindowTextLengthW(h) == 0) {
                set_mode(MODE_JUMP, L"");
                return 0;
            }
            break;
        case 'P':
            if (ctrl) {
                set_mode(shift ? MODE_CMDS : MODE_JUMP, L"");
                return 0;
            }
            break;
        case 'F':
            if(ctrl&&shift&&PL.mode==MODE_HERE){PL.force_live=1;PL.last_query[0]=0;on_text();return 0;}
            break;
        default: break;
        }
        break;
    }
    case WM_CHAR:
        /* keep Ctrl+A/C/V/X/Z and Backspace; drop the rest (they would beep or insert boxes) */
        if (w == 127 || (w < 32 && w != 1 && w != 3 && w != 8 && w != 22 && w != 24 && w != 26)) return 0;
        break;
    case WM_KILLFOCUS:
        /* a click elsewhere in the window closes; switching apps does not */
        if (w && PL.hwnd && (HWND)w != PL.hwnd && IsChild(app_window(), (HWND)w)) PostMessageW(PL.hwnd, PM_CLOSE, 0, 0);
        break;
    case WM_MOUSEWHEEL:
        return SendMessageW(PL.hwnd, m, w, l);
    }
    return CallWindowProcW(orig, h, m, w, l);
}

static int row_at(int y)
{
    int r = (y - head_h()) / row_h();
    if (y < head_h() || r >= visible_rows()) return -1;
    r += PL.top;
    return r < (int)PL.nshown ? r : -1;
}

static LRESULT CALLBACK palette_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SIZE:
        PL.w = LOWORD(l);
        PL.h = HIWORD(l);
        if (PL.edit) place_edit();
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        int64_t t = fpe_prof_begin();
        if (!PL.dc || PL.bw != PL.w || PL.bh != PL.h) {
            if (!PL.dc) PL.dc = CreateCompatibleDC(dc);
            if (PL.bmp) {
                SelectObject(PL.dc, PL.old_bmp);
                DeleteObject(PL.bmp);
            }
            PL.bmp = CreateCompatibleBitmap(dc, PL.w > 0 ? PL.w : 1, PL.h > 0 ? PL.h : 1);
            PL.old_bmp = SelectObject(PL.dc, PL.bmp);
            SetBkMode(PL.dc, TRANSPARENT);
            PL.bw = PL.w;
            PL.bh = PL.h;
        }
        paint(PL.dc);
        BitBlt(dc, 0, 0, PL.w, PL.h, PL.dc, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        FPE_PROF_END("palette.paint", t);
        return 0;
    }

    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)w, C_TEXT);
        SetBkColor((HDC)w, C_EDIT);
        return (LRESULT)PL.edit_brush;

    case WM_COMMAND:
        if(LOWORD(w)==INDEX_BUTTON){index_menu();return 0;}
        if(LOWORD(w)>=INDEX_ACTION+FPE_INDEX_ENABLE&&LOWORD(w)<=INDEX_ACTION+FPE_INDEX_REMOVE){index_action(LOWORD(w)-INDEX_ACTION);return 0;}
        if ((HWND)l == PL.edit && HIWORD(w) == EN_CHANGE && !PL.setting) {
            int64_t t = fpe_prof_begin();
            on_text();
            FPE_PROF_END("palette.filter", t);
        }
        return 0;

    case WM_DRAWITEM:{
        const DRAWITEMSTRUCT *d=(const DRAWITEMSTRUCT*)l;
        if(d&&d->CtlID==INDEX_BUTTON){
            SetDCBrushColor(d->hDC,C_EDIT);FillRect(d->hDC,&d->rcItem,GetStockObject(DC_BRUSH));
            SetDCBrushColor(d->hDC,C_BORDER);FrameRect(d->hDC,&d->rcItem,GetStockObject(DC_BRUSH));
            HGDIOBJ old=SelectObject(d->hDC,app_font());SetBkMode(d->hDC,TRANSPARENT);SetTextColor(d->hDC,C_TEXT);
            RECT r=d->rcItem;DrawTextW(d->hDC,L"Index...",-1,&r,DT_SINGLELINE|DT_CENTER|DT_VCENTER);
            if(d->itemState&ODS_FOCUS){InflateRect(&r,-3,-3);DrawFocusRect(d->hDC,&r);}SelectObject(d->hDC,old);return TRUE;
        }break;
    }

    case WM_COPYDATA:
        if (PL.hwnd) ev_answer((const COPYDATASTRUCT *)l);
        return TRUE;

    case WM_TIMER:
        if(w==TIMER_QUERY){KillTimer(h,w);dispatch_query();publish_state();InvalidateRect(h,NULL,FALSE);return 0;}
        if(w==TIMER_SEARCH){PostMessageW(h,PM_SEARCH,0,0);return 0;}
        if(w==TIMER_INDEX){if(!PL.closing&&index_context()&&!PL.scan&&!PL.want_query&&PL.index.enabled&&PL.index.state!=FPE_INDEX_PAUSED)start_worker(0,FPE_INDEX_STATUS);return 0;}
        if (w == TIMER_EV && PL.ev_out) {
            PL.ev_seq=++sequence;
            ev_settle();
            if(PL.mode==MODE_HERE)start_live();else PL.ev_state = EV_TIMEOUT;
            InvalidateRect(h, NULL, FALSE);
        }
        KillTimer(h, w);publish_state();
        return 0;

    case PM_EV_SENT:
        if((uint32_t)w==PL.ev_seq&&PL.ev_out&&(int)l){
            ev_settle();if(PL.mode==MODE_HERE)start_live();else PL.ev_state=(int)l==-1?EV_ABSENT:EV_FAILED;
            InvalidateRect(h,NULL,FALSE);
        }publish_state();return 0;
    case PM_SEARCH:{
        live_search *s=PL.scan;if(!s)return 0;
        EnterCriticalSection(&s->lock);if(s->query_job&&!InterlockedCompareExchange(&s->stop,0,0))PL.stats=s->stats;s->queued=0;int done=s->done;LeaveCriticalSection(&s->lock);
        if(done){
            int stale=InterlockedCompareExchange(&s->stop,0,0)!=0;ev_list *list=s->list;
            int query_job=s->query_job,backend=s->backend;
            int refresh=(!query_job&&s->index.state==FPE_INDEX_CURRENT&&(PL.index.state!=FPE_INDEX_CURRENT||PL.index.updated!=s->index.updated))||
                (PL.backend==BACKEND_WSL_INDEX&&s->index.state!=FPE_INDEX_CURRENT);
            if(!stale&&index_context())PL.index=s->index;
            CloseHandle(s->thread);DeleteCriticalSection(&s->lock);free(s->root);free(s);PL.scan=NULL;KillTimer(h,TIMER_SEARCH);
            if(PL.closing){free(list);close_palette(0);return 0;}
            if(stale){free(list);PL.stats=(fpe_search_stats){0};}
            else if(query_job){PL.backend=backend;take_results(list);}
            else free(list);
            if(PL.pending_index){PL.pending_index=0;start_worker(0,PL.index_action);}
            else if(PL.last_query[0]&&PL.ev_state!=EV_FAILED&&(PL.want_query||stale||refresh)){
                ev_drop();PL.stats=(fpe_search_stats){0};PL.ev_state=EV_WAIT;refilter(1);dispatch_query();
            }
        }
        publish_state();InvalidateRect(h,NULL,FALSE);return 0;
    }

    case WM_MOUSEMOVE: {
        int r = row_at(GET_Y_LPARAM(l));
        if (r != PL.hover) {
            PL.hover = r;
            InvalidateRect(h, NULL, FALSE);
            TRACKMOUSEEVENT tme = { sizeof tme, TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        PL.hover = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;

    case WM_LBUTTONDOWN: {
        int r = row_at(GET_Y_LPARAM(l));
        if (r >= 0) {
            PL.sel = r;
            activate(r, (w & MK_CONTROL) != 0, (w & MK_SHIFT) != 0);
        } else {
            SetFocus(PL.edit);
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        PL.wheel += GET_WHEEL_DELTA_WPARAM(w);
        int rows = PL.wheel / WHEEL_DELTA * 3;
        PL.wheel %= WHEEL_DELTA;
        int max_top = (int)PL.nshown - visible_rows();
        PL.top -= rows;
        if (PL.top > max_top) PL.top = max_top;
        if (PL.top < 0) PL.top = 0;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }

    case PM_CLOSE:
        close_palette(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---- opening ------------------------------------------------------------------------ */

static void set_mode(int mode, const wchar_t *text)
{
    if (!PL.hwnd) return;
    if (mode == MODE_CMDS && PL.mode == MODE_JUMP) PL.from_jump = 1;
    PL.mode = mode;
    PL.index_available=mode==MODE_HERE&&fpe_wsl_native(PL.here)&&fpe_wsl_available(PL.here);
    memset(&PL.index,0,sizeof PL.index);PL.want_query=PL.pending_index=0;
    KillTimer(PL.hwnd,TIMER_INDEX);if(index_context())SetTimer(PL.hwnd,TIMER_INDEX,750,NULL);
    PL.ev_seq=++sequence;cancel_live();PL.coverage=PL.probing=PL.force_live=0;PL.stats=(fpe_search_stats){0};
    ev_settle();
    ev_drop();
    PL.ev_state = EV_NONE;
    PL.last_query[0] = 0;
    load_items();
    PL.setting = 1;
    SetWindowTextW(PL.edit, text);
    int len = (int)wcslen(text);
    SendMessageW(PL.edit, EM_SETSEL, (WPARAM)len, (LPARAM)len);
    PL.setting = 0;
    place_edit();
    refilter(0);
    query(text);
    if(index_context()&&!PL.scan&&!text[0])start_worker(0,FPE_INDEX_STATUS);
}

static void open_palette(fn_ctx *c, int mode, const char *self_id)
{
    if (PL.hwnd) { /* the same key again: switch modes in place */
        if(PL.closing){PL.closing=0;ShowWindow(PL.hwnd,SW_SHOWNA);PL.back=GetFocus();SetFocus(PL.edit);}
        if (mode == MODE_HERE && c->dir) {
            _snwprintf(PL.here, 32768, L"%ls", c->dir);
            PL.here[32767] = 0;
        }
        set_mode(mode, L"");
        return;
    }
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc = { sizeof wc };
        wc.lpfnWndProc = palette_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"OpenPilotPalette";
        registered = RegisterClassExW(&wc) != 0;
    }
    const fn_def *self = fn_find(self_id);
    if (!registered || fn_hold(self)) return;
    PL.held = self;
    PL.pane = c->pane ? (Pane *)c->pane : app_active_pane();
    PL.back = GetFocus();
    PL.mode = mode;
    PL.hover = -1;
    if (c->dir) {
        _snwprintf(PL.here, 32768, L"%ls", c->dir);
        PL.here[32767] = 0;
    }
    PL.index_available=mode==MODE_HERE&&fpe_wsl_native(PL.here)&&fpe_wsl_available(PL.here);
    PL.edit_brush = CreateSolidBrush(C_EDIT);
    PL.hwnd = CreateWindowExW(0, L"OpenPilotPalette", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                              app_window(), NULL, inst, NULL);
    PL.edit = PL.hwnd ? CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, PL.hwnd, NULL,
                                        inst, NULL)
                      : NULL;
    if (!PL.edit) {
        close_palette(0);
        return;
    }
    /* answers from an Everything running elevated still get through */
    ChangeWindowMessageFilterEx(PL.hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
    SendMessageW(PL.edit, WM_SETFONT, (WPARAM)app_font(), FALSE);
    SendMessageW(PL.edit, EM_SETLIMITTEXT, TEXT_MAX - 1, 0);
    PL.edit_orig = (WNDPROC)SetWindowLongPtrW(PL.edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    load_items();
    refilter(0);
    place_window();
    ShowWindow(PL.hwnd, SW_SHOWNA);
    if(index_context()){SetTimer(PL.hwnd,TIMER_INDEX,750,NULL);start_worker(0,FPE_INDEX_STATUS);}
    if (GetForegroundWindow() == app_window()) SetFocus(PL.edit);
    else PL.back = NULL;
}

int palette_shown(void) { return PL.hwnd != NULL; }

void palette_layout(void)
{
    if (PL.hwnd) place_window();
}

void palette_redraw(void)
{
    if (PL.hwnd) InvalidateRect(PL.hwnd, NULL, FALSE);
}

void palette_close(void) { close_palette(0); }

/* ---- functions ------------------------------------------------------------------------ */

static void run_jump(fn_ctx *c) { open_palette(c, MODE_JUMP, "palette.jump"); }
static void run_commands(fn_ctx *c) { open_palette(c, MODE_CMDS, "palette.commands"); }
static void run_search(fn_ctx *c) { open_palette(c, MODE_HERE, "palette.search"); }
static void run_live_search(fn_ctx *c){open_palette(c,MODE_HERE,"palette.search_live");PL.force_live=1;}
static void run_index_manager(fn_ctx *c){open_palette(c,MODE_HERE,"search.wsl_index");}
static int index_applies(const fn_ctx *c){return c->dir&&fpe_wsl_native(c->dir)&&fpe_wsl_available(c->dir);}
static int search_applies(const fn_ctx *c) { return c->dir!=NULL; }
static const fn_opt ev_options[]={
    {"instance","","Named Everything instance; empty tries the standard instance and 1.5a",NULL},{NULL}};
static void configure_everything(void)
{
    const fn_def *d=fn_find("search.everything");const char *name=fn_option(d,"instance");wchar_t wide[64];size_t n=strlen(name);
    if(n>=64){fn_config_error(d,"instance exceeds 63 characters");ev_set_instance(L"");return;}
    for(size_t i=0;i<n;i++)if(!((name[i]>='a'&&name[i]<='z')||(name[i]>='A'&&name[i]<='Z')||(name[i]>='0'&&name[i]<='9')||strchr("._-",name[i]))){fn_config_error(d,"instance accepts letters, digits, dot, underscore and hyphen");ev_set_instance(L"");return;}
    for(size_t i=0;i<=n;i++)wide[i]=(unsigned char)name[i];ev_set_instance(wide);
}

static const fn_def palette_functions[] = {
    { .id = "palette.jump", .title = L"Go to folder", .about = "Jump to a recent, open, pinned or any folder (Everything)",
      .life = FN_COLD, .where = (FN_ANYWHERE|FN_ARCHIVE), .keys = "Ctrl+P, Ctrl+G", .run = run_jump },
    { .id = "palette.commands", .title = L"Command palette", .about = "Run any function by name",
      .life = FN_COLD, .flags = FN_HIDDEN, .where = (FN_ANYWHERE|FN_ARCHIVE), .keys = "Ctrl+Shift+P, F1", .run = run_commands },
    { .id = "palette.search", .title = L"Search this folder", .about = "Search with Everything when indexed; otherwise scan this folder and subfolders",
      .life = FN_COLD, .where = FN_ANYWHERE, .keys = "Ctrl+F, Ctrl+E", .applies = search_applies, .run = run_search },
    { .id="palette.search_live",.title=L"Search this folder live",.about="Scan current filenames without using an index; links and mount boundaries are not followed",
      .life=FN_COLD,.where=FN_ANYWHERE,.keys="Ctrl+Shift+F",.applies=search_applies,.run=run_live_search },
    { .id="search.wsl_index",.title=L"Manage WSL search index",.about="Use Index... to select this folder, pause, resume, rescan or remove its index",
      .life=FN_COLD,.where=FN_ANYWHERE,.applies=index_applies,.run=run_index_manager },
    { .id = "search.everything", .title = L"Everything search", .life = FN_COLD,
      .opts=ev_options,.configure=configure_everything,
      .about = "Windows disk-wide results from a running Everything (voidtools)" },
};

void palette_register(void)
{
    fn_register(palette_functions, sizeof palette_functions / sizeof palette_functions[0]);
}
