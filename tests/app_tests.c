#include "../src/namecodec.h"
/* Demo app helpers and bounded Settings input with private hidden windows. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#include "../app/everything.h"
#include "../app/imageutil.h"
#include "../app/imagebrowse.h"
#include "../app/pathentry.h"
#include "../app/linkdialog.h"
#include "../app/permissionutil.h"
#include "../app/renameutil.h"
#include "../app/thumbutil.h"
#include "../app/textutil.h"
#include "../app/imageproto.h"
#include "../app/fn.h"
#include "../app/fuzzy.h"
#include "../app/pdfpos.h"
#include "../app/pdfproto.h"
#include "../app/pdftext.h"
#include "../app/relaycmd.h"
#include "../app/relayedit.h"
#include "../app/session.h"
#include "../app/pathwire.h"
#include "../app/folderview.h"
#include "../app/shell.h"
#include "../app/termutil.h"
#include "../app/operations.h"
#include "../app/panelproto.h"
#include "../app/typeicons.h"

static int g_failed, g_passed;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (cond) g_passed++;                                                    \
        else { g_failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

#define ALL (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK)

static DWORD effect1(const wchar_t *src, const wchar_t *dest, DWORD keys, DWORD allowed)
{
    const wchar_t *v[1] = { src };
    return shell_drop_effect(v, 1, dest, keys, allowed);
}

static void test_symlink_names_and_endpoints(void)
{
    printf("Linux symlinks: exact names, byte limits and reachable endpoints\n");
    wchar_t mapped[260],long_name[260];
    CHECK(linkdialog_name(L"file β ",mapped)&&!wcscmp(mapped,L"file β "));
    CHECK(linkdialog_name(L"back\\slash:star*",mapped)&&!wcscmp(mapped,L"back\uf05cslash\uf03astar\uf02a"));
    CHECK(linkdialog_name(L".hidden",mapped));
    CHECK(!linkdialog_name(L"",mapped));CHECK(!linkdialog_name(L".",mapped));CHECK(!linkdialog_name(L"..",mapped));
    CHECK(!linkdialog_name(L"folder/file",mapped));CHECK(!linkdialog_name(NULL,mapped));
    for(int i=0;i<255;i++)long_name[i]=L'a';long_name[255]=0;CHECK(linkdialog_name(long_name,mapped));
    long_name[255]=L'a';long_name[256]=0;CHECK(!linkdialog_name(long_name,mapped));
    for(int i=0;i<128;i++)long_name[i]=L'β';long_name[128]=0;CHECK(!linkdialog_name(long_name,mapped));
    long_name[127]=0;CHECK(linkdialog_name(long_name,mapped));
    CHECK(linkdialog_name(L"\uf041",mapped)&&mapped[0]==0xf041);
    CHECK(linkdialog_name(L"raw\xdcff",mapped)&&mapped[3]==0xdcff);
    CHECK(!linkdialog_name(L"bad\xd800",mapped));
    const wchar_t *dest=L"\\\\wsl.localhost\\Ubuntu\\tmp\\destination";
    fpe_wsl_configure(NULL);
    CHECK(fpe_wsl_symlink_compatible(L"\\\\wsl$\\ubuntu\\home\\Foo",dest));
    CHECK(fpe_wsl_symlink_compatible(L"C:\\from\\file.dat",dest));
    CHECK(!fpe_wsl_symlink_compatible(L"\\\\wsl.localhost\\Debian\\home\\file",dest));
    CHECK(!fpe_wsl_symlink_compatible(L"\\\\server\\share\\file",dest));
    CHECK(!fpe_wsl_symlink_compatible(L"C:relative",dest));
    CHECK(!fpe_wsl_symlink_compatible(L"C:\\",dest));
    CHECK(!fpe_wsl_symlink_compatible(L"\\\\wsl.localhost\\Ubuntu",dest));
    CHECK(!fpe_wsl_symlink_compatible(L"\\\\wsl.localhost\\Ubuntu\\",dest));
    CHECK(!fpe_wsl_symlink_compatible(L"C:\\file",L"D:\\out"));
    CHECK(!fpe_wsl_symlink_compatible(NULL,dest));
    CHECK(!fpe_wsl_symlink_compatible(L"C:\\file",NULL));
    fpe_wsl_config off={0};fpe_wsl_configure(&off);
    CHECK(!fpe_wsl_symlink_compatible(L"C:\\file",dest));fpe_wsl_configure(NULL);
}

static void test_permission_patches(void)
{
    printf("permission patches: mixed intent, per-item preservation and octal bounds\n");
    uint32_t modes[]={0104750,0041770,0100640};permission_patch p={0};uint32_t value=0;
    CHECK(permission_bit_state(modes,3,0400)==PERMISSION_SET);
    CHECK(permission_bit_state(modes,3,0001)==PERMISSION_CLEAR);
    CHECK(permission_bit_state(modes,3,0020)==PERMISSION_KEEP);
    CHECK(permission_bit_state(NULL,0,0400)==PERMISSION_KEEP);
    permission_patch_cycle(&p,0020,PERMISSION_KEEP);
    CHECK(permission_patch_mode(&p,modes[0])==0104770);
    CHECK(permission_patch_mode(&p,modes[1])==0041770);
    CHECK(permission_patch_mode(&p,modes[2])==0100660);
    permission_patch_cycle(&p,0020,PERMISSION_KEEP);
    CHECK(permission_patch_mode(&p,modes[0])==0104750);
    CHECK(permission_patch_mode(&p,modes[1])==0041750);
    CHECK(permission_patch_mode(&p,modes[2])==0100640);
    permission_patch_cycle(&p,0020,PERMISSION_KEEP);
    CHECK(p.mask==0);CHECK(permission_patch_mode(&p,modes[1])==modes[1]);
    permission_patch_cycle(&p,0400,PERMISSION_SET);
    CHECK(permission_patch_mode(&p,modes[0])==0104350);
    permission_patch_cycle(&p,0400,PERMISSION_SET);CHECK(p.mask==0);
    permission_patch_cycle(&p,0001,PERMISSION_CLEAR);
    CHECK(permission_patch_mode(&p,modes[1])==0041771);
    permission_patch_cycle(&p,0001,PERMISSION_CLEAR);CHECK(p.mask==0);
    CHECK(permission_octal(L"640",&value)&&value==0640);
    CHECK(permission_octal(L"0000",&value)&&value==0);
    CHECK(permission_octal(L"7777",&value)&&value==07777);
    CHECK(!permission_octal(L"789",&value));CHECK(!permission_octal(L"10000",&value));
    CHECK(!permission_octal(L"",&value));CHECK(!permission_octal(L"64",&value));
    CHECK(!permission_octal(L" 640",&value));CHECK(!permission_octal(L"-640",&value));
    p=(permission_patch){07777,0755};CHECK(permission_patch_mode(&p,0104750)==0100755);
    permission_patch_choose(&p,04000,PERMISSION_KEEP);CHECK(permission_patch_mode(&p,0104750)==0104755);
    /* Every choice is checked across all permission combinations and both
     * file types: no unchosen/special/type bit may change. */
    int preserved=1;
    for(uint32_t mode=0;mode<=07777;mode++)for(int type=0;type<2;type++){
        uint32_t original=mode|(type?0040000:0100000);p=(permission_patch){0};
        permission_patch_choose(&p,0020,PERMISSION_SET);permission_patch_choose(&p,0002,PERMISSION_CLEAR);
        uint32_t after=permission_patch_mode(&p,original);
        if((after&~0022u)!=(original&~0022u)||!(after&0020)||(after&0002))preserved=0;
    }
    CHECK(preserved);
}

static void test_right_drop_choices(void)
{
    printf("right drops: legal choices and drag-source button protocol\n");
    const wchar_t *paths[]={L"C:\\from\\a.txt"};
    CHECK(shell_drop_choices(paths,1,L"C:\\to",ALL)==ALL);
    CHECK(shell_drop_choices(paths,1,L"D:\\to",DROPEFFECT_COPY)==DROPEFFECT_COPY);
    CHECK(shell_drop_choices(paths,1,L"C:\\from",ALL)==(DROPEFFECT_COPY|DROPEFFECT_LINK));
    CHECK(shell_drop_choices(paths,1,L"C:\\from",DROPEFFECT_MOVE)==0);
    CHECK(shell_drop_choices(paths,1,L"C:\\to",DROPEFFECT_LINK)==DROPEFFECT_LINK);
    const wchar_t *folder[]={L"C:\\from"};
    CHECK(shell_drop_choices(folder,1,L"C:\\from\\child",ALL)==0);
    CHECK(shell_drop_choices(folder,1,L"C:\\from",ALL)==0);
    CHECK(shell_drop_choices(paths,0,L"C:\\to",ALL)==0);
    CHECK(shell_drop_choices(paths,1,NULL,ALL)==0);
    CHECK(shell_drag_continue(0,MK_RBUTTON,1)==S_OK);
    CHECK(shell_drag_continue(0,0,1)==DRAGDROP_S_DROP);
    CHECK(shell_drag_continue(1,MK_RBUTTON,1)==DRAGDROP_S_CANCEL);
    CHECK(shell_drag_continue(0,MK_LBUTTON|MK_RBUTTON,1)==DRAGDROP_S_CANCEL);
    CHECK(shell_drag_continue(0,MK_LBUTTON,0)==S_OK);
    CHECK(shell_drag_continue(0,0,0)==DRAGDROP_S_DROP);
    CHECK(shell_drag_continue(0,MK_RBUTTON,0)==DRAGDROP_S_CANCEL);
}

static void test_drop_effect(void)
{
    printf("drop effect rules\n");
    /* Explorer's default: move on the same volume, copy across volumes */
    CHECK(effect1(L"C:\\a\\f.txt", L"C:\\b", 0, ALL) == DROPEFFECT_MOVE);
    CHECK(effect1(L"C:\\a\\f.txt", L"D:\\b", 0, ALL) == DROPEFFECT_COPY);
    CHECK(effect1(L"c:\\a\\f.txt", L"C:\\b", 0, ALL) == DROPEFFECT_MOVE);
    CHECK(effect1(L"\\\\srv\\share\\f.txt", L"\\\\srv\\share\\x", 0, ALL) == DROPEFFECT_MOVE);
    CHECK(effect1(L"\\\\srv\\share\\f.txt", L"\\\\srv\\other\\x", 0, ALL) == DROPEFFECT_COPY);
    CHECK(effect1(L"\\\\srv\\share\\f.txt", L"C:\\x", 0, ALL) == DROPEFFECT_COPY);

    /* modifiers win */
    CHECK(effect1(L"C:\\a\\f.txt", L"C:\\b", MK_CONTROL, ALL) == DROPEFFECT_COPY);
    CHECK(effect1(L"C:\\a\\f.txt", L"D:\\b", MK_SHIFT, ALL) == DROPEFFECT_MOVE);

    /* the source decides what is allowed */
    CHECK(effect1(L"C:\\a\\f.txt", L"C:\\b", 0, DROPEFFECT_COPY) == DROPEFFECT_COPY);
    CHECK(effect1(L"C:\\a\\f.txt", L"D:\\b", 0, DROPEFFECT_MOVE) == DROPEFFECT_MOVE);
    CHECK(effect1(L"C:\\a\\f.txt", L"D:\\b", 0, DROPEFFECT_LINK) == DROPEFFECT_NONE);

    /* nonsense drops are refused */
    CHECK(effect1(L"C:\\a", L"C:\\a", 0, ALL) == DROPEFFECT_NONE);          /* onto itself */
    CHECK(effect1(L"C:\\a", L"C:\\a\\sub", 0, ALL) == DROPEFFECT_NONE);     /* into itself */
    CHECK(effect1(L"C:\\a", L"C:\\A\\SUB\\deeper", MK_CONTROL, ALL) == DROPEFFECT_NONE);
    CHECK(effect1(L"C:\\a\\f.txt", L"C:\\a", 0, ALL) == DROPEFFECT_NONE);   /* already there */
    CHECK(effect1(L"C:\\a\\f.txt", L"C:\\a\\", 0, ALL) == DROPEFFECT_NONE);
    CHECK(effect1(L"C:\\f.txt", L"C:\\", 0, ALL) == DROPEFFECT_NONE);       /* root */
    CHECK(effect1(L"C:\\a\\f.txt", L"C:\\a", MK_CONTROL, ALL) == DROPEFFECT_COPY); /* Ctrl duplicates */
    CHECK(effect1(L"C:\\ab\\f.txt", L"C:\\a", 0, ALL) == DROPEFFECT_MOVE);  /* prefix, not parent */
    CHECK(effect1(L"C:\\a", L"C:\\ab", 0, ALL) == DROPEFFECT_MOVE);         /* sibling, not child */
    CHECK(effect1(L"C:\\a\\f.txt", NULL, 0, ALL) == DROPEFFECT_NONE);

    /* several items: one bad apple refuses the drop; mixed volumes copy */
    const wchar_t *two[2] = { L"C:\\a\\x", L"C:\\b\\y" };
    CHECK(shell_drop_effect(two, 2, L"C:\\c", 0, ALL) == DROPEFFECT_MOVE);
    CHECK(shell_drop_effect(two, 2, L"C:\\b\\y\\z", 0, ALL) == DROPEFFECT_NONE);
    CHECK(shell_drop_effect(two, 2, L"C:\\a", 0, ALL) == DROPEFFECT_MOVE); /* only some are already there */
    const wchar_t *mixed[2] = { L"C:\\a\\x", L"D:\\b\\y" };
    CHECK(shell_drop_effect(mixed, 2, L"C:\\c", 0, ALL) == DROPEFFECT_COPY);
    CHECK(shell_drop_effect(two, 0, L"C:\\c", 0, ALL) == DROPEFFECT_NONE);
}

/* ---- function registry ---------------------------------------------------- */

static uint64_t t_now;
static uint32_t t_armed; /* last fn_host.arm request, 0 = cancelled */
static int t_ran[8], t_started[8], t_stopped[8], t_ticks, t_order[16], t_norder;
static uint64_t host_now(void) { return t_now; }
static void host_arm(uint32_t ms) { t_armed = ms; }

static void run0(fn_ctx *c) { t_ran[0]++; }
static void run1(fn_ctx *c) { t_ran[1]++; }
static void run2(fn_ctx *c) { t_ran[2]++; }
static void run3(fn_ctx *c) { t_ran[3]++; }
static int start_svc(void) { t_started[0]++; t_order[t_norder++] = 1; return 0; }
static void stop_svc(void) { t_stopped[0]++; t_order[t_norder++] = -1; }
static int start_dep(void) { t_started[1]++; t_order[t_norder++] = 2; return 0; }
static void stop_dep(void) { t_stopped[1]++; t_order[t_norder++] = -2; }
static int start_cold(void) { t_started[2]++; return 0; }
static void stop_cold(void) { t_stopped[2]++; }
static int start_warm(void) { t_started[3]++; return 0; }
static void stop_warm(void) { t_stopped[3]++; }
static int start_fail(void) { return -1; }
static void tick_warm(void) { t_ticks++; }
static int only_two(const fn_ctx *c) { return c->n == 2; }

static const fn_opt opts[] = { { "program", "auto", "which one" }, { NULL } };

static const fn_def defs[] = {
    { .id = "svc.a", .life = FN_HOT, .start = start_svc, .stop = stop_svc },
    { .id = "svc.b", .life = FN_HOT, .flags = FN_DEFERRED, .needs = "svc.a", .start = start_dep, .stop = stop_dep },
    { .id = "t.dual", .title = L"Dual", .life = FN_COLD, .where = FN_PANE, .requires = FN_S_DUAL, .keys = "F5",
      .run = run0, .menus = FN_MENU_ITEMS, .group = 1 },
    { .id = "t.any", .title = L"Any", .life = FN_COLD, .where = FN_ANYWHERE, .keys = "F5, Ctrl+R", .verbs = "open explore",
      .run = run1, .start = start_cold, .stop = stop_cold, .menus = FN_MENU_ITEMS | FN_MENU_FOLDER, .group = 0 },
    { .id = "t.two", .title = L"Two", .life = FN_COLD, .where = FN_PANE, .keys = "Ctrl+Shift+N", .applies = only_two,
      .run = run2, .menus = FN_MENU_ITEMS, .group = 1, .opts = opts },
    { .id = "t.needs", .title = L"Needs", .life = FN_COLD, .where = FN_PANE, .keys = "F9", .needs = "svc.b",
      .run = run3, .menus = FN_MENU_ITEMS, .group = 2 },
    { .id = "t.warm", .title = L"Warm", .life = FN_WARM, .where = FN_PANE, .idle_ms = 1000, .keys = "F11",
      .start = start_warm, .stop = stop_warm, .tick = tick_warm, .run = run0 },
    { .id = "t.fail", .title = L"Fail", .life = FN_COLD, .where = FN_PANE, .keys = "F3", .start = start_fail, .run = run0,
      .menus = FN_MENU_FOLDER | FN_MENU_OURS, .group = 3 },
    { .id = "t.off", .title = L"Off", .life = FN_COLD, .flags = FN_OPTIONAL, .where = FN_PANE, .keys = "F4", .run = run0 },
};

static void registry_fresh(const char *settings)
{
    fn_reset();
    memset(t_ran, 0, sizeof t_ran);
    memset(t_started, 0, sizeof t_started);
    memset(t_stopped, 0, sizeof t_stopped);
    t_ticks = t_norder = 0;
    t_now = 1000;
    t_armed = 0;
    fn_host h = { host_now, host_arm, NULL };
    fn_init(&h);
    fn_register(defs, sizeof defs / sizeof defs[0]);
    fn_settings(settings, settings ? strlen(settings) : 0);
}

static fn_ctx pane(uint32_t state, uint32_t n)
{
    fn_ctx c = { 0 };
    c.where = FN_PANE;
    c.state = state;
    c.n = n;
    return c;
}

static int key_is(const char *s, uint8_t vk, uint8_t mods)
{
    fn_binding k;
    return !fn_parse_key(s, &k) && k.vk == vk && k.mods == mods;
}

static int format_is(const char *s, const wchar_t *want)
{
    fn_binding k;
    wchar_t buf[48];
    if (fn_parse_key(s, &k)) return 0;
    fn_format_key(k, buf, 48);
    return !wcscmp(buf, want);
}

static void test_keys(void)
{
    printf("key names\n");
    fn_binding k;
    CHECK(key_is("F2", VK_F2, 0));
    CHECK(key_is("f12", VK_F12, 0));
    CHECK(key_is("Ctrl+Shift+N", 'N', FN_CTRL | FN_SHIFT));
    CHECK(key_is("shift + ctrl + n", 'N', FN_CTRL | FN_SHIFT));
    CHECK(key_is("Alt+Enter", VK_RETURN, FN_ALT));
    CHECK(key_is("Alt+Left", VK_LEFT, FN_ALT));
    CHECK(key_is("Ctrl+1", '1', FN_CTRL));
    CHECK(key_is("Shift+Delete", VK_DELETE, FN_SHIFT));
    CHECK(key_is("Del", VK_DELETE, 0));
    CHECK(key_is("PageDown", VK_NEXT, 0));
    CHECK(key_is("Ctrl+Plus", VK_OEM_PLUS, FN_CTRL));
    CHECK(!fn_parse_key("Ctrl+\\", &k) && k.mods == FN_CTRL); /* the key depends on the layout */
    CHECK(fn_parse_key("Ctrl+Foo", &k) != 0);
    CHECK(fn_parse_key("Hyper+A", &k) != 0);
    CHECK(fn_parse_key("", &k) != 0);
    CHECK(fn_parse_key("F25", &k) != 0);
    CHECK(format_is("ctrl+shift+n", L"Ctrl+Shift+N"));
    CHECK(format_is("alt+return", L"Alt+Enter"));
    CHECK(format_is("F7", L"F7"));
    CHECK(format_is("shift+tab", L"Shift+Tab"));
}

static void test_registry_dispatch(void)
{
    printf("registry: keys, conditions, menus, verbs\n");
    registry_fresh(NULL);
    fn_ctx single = pane(FN_S_ITEMS | FN_S_ONE, 1), dual = pane(FN_S_ITEMS | FN_S_DUAL, 2);

    /* one key, two functions: the first that applies wins */
    CHECK(fn_key(VK_F5, 0, &dual) == 1 && t_ran[0] == 1 && t_ran[1] == 0);
    CHECK(fn_key(VK_F5, 0, &single) == 1 && t_ran[0] == 1 && t_ran[1] == 1);
    CHECK(fn_key('R', FN_CTRL, &single) == 1 && t_ran[1] == 2);
    CHECK(fn_key('R', 0, &single) == 0);                      /* modifiers must match exactly */
    CHECK(fn_key('N', FN_CTRL | FN_SHIFT, &single) == 0);     /* applies() says no */
    CHECK(fn_key('N', FN_CTRL | FN_SHIFT, &dual) == 1 && t_ran[2] == 1);
    fn_ctx tree = { .where = FN_TREE, .state = FN_S_ITEMS | FN_S_DUAL, .n = 1 };
    CHECK(fn_key(VK_F5, 0, &tree) == 1 && t_ran[0] == 1);     /* t.dual is pane-only: t.any runs */
    CHECK(fn_key(VK_F4, 0, &single) == 0);                    /* optional: off unless switched on */

    /* menus: only what applies, grouped, with the key as a hint */
    HMENU m = CreatePopupMenu();
    CHECK(fn_menu(m, FN_MENU_ITEMS, &dual) == 4);             /* any | dual, two | needs */
    CHECK(GetMenuItemCount(m) == 6);                          /* plus two separators */
    wchar_t text[64];
    GetMenuStringW(m, 0, text, 64, MF_BYPOSITION);
    CHECK(!wcscmp(text, L"Any\tF5"));
    CHECK(fn_menu_run(GetMenuItemID(m, 2), &dual) == 1 && t_ran[0] == 2);
    DestroyMenu(m);
    m = CreatePopupMenu();
    CHECK(fn_menu(m, FN_MENU_ITEMS, &single) == 2);           /* any, needs */
    DestroyMenu(m);
    m = CreatePopupMenu();
    CHECK(fn_menu(m, FN_MENU_FOLDER, &single) == 1);
    DestroyMenu(m);
    m = CreatePopupMenu();
    CHECK(fn_menu(m, FN_MENU_FOLDER | FN_MENU_OURS, &single) == 2); /* plus t.fail: only in our own menu */
    DestroyMenu(m);

    char verbs[64];
    fn_verbs(&single, verbs, sizeof verbs);
    CHECK(!strcmp(verbs, "open explore"));
    CHECK(fn_verb("explore", &single) == 1 && t_ran[1] == 4);
    CHECK(fn_verb("delete", &single) == 0);
    CHECK(fn_run_id("no.such", &single) == 0);
    fn_shutdown();
}

static void test_registry_settings(void)
{
    printf("registry: settings\n");
    registry_fresh("; comment\n"
                   "[functions]\n t.dual = off\n t.off = on\n"
                   "[keys]\n t.any = F6\n t.two = none\n"
                   "[menu]\n t.needs = off\n"
                   "[t.two]\n program = cmd\n");
    CHECK(fn_settings_errors()[0] == 0);
    fn_ctx dual = pane(FN_S_ITEMS | FN_S_DUAL, 2);
    CHECK(!fn_active(fn_find("t.dual")));
    CHECK(fn_key(VK_F5, 0, &dual) == 0);                      /* t.dual off, t.any moved to F6 */
    CHECK(fn_key(VK_F6, 0, &dual) == 1 && t_ran[1] == 1);
    CHECK(fn_key('R', FN_CTRL, &dual) == 0);                  /* rebinding replaces the defaults */
    CHECK(fn_key('N', FN_CTRL | FN_SHIFT, &dual) == 0);       /* unbound */
    CHECK(fn_key(VK_F4, 0, &dual) == 1);                      /* optional, switched on */
    HMENU m = CreatePopupMenu();
    CHECK(fn_menu(m, FN_MENU_ITEMS, &dual) == 2);             /* any, two: needs is hidden from menus */
    DestroyMenu(m);
    CHECK(fn_key(VK_F9, 0, &dual) == 1);                      /* ...but its key still works */
    CHECK(!strcmp(fn_option(fn_find("t.two"), "program"), "cmd"));
    wchar_t k[32];
    CHECK(fn_key_text(fn_find("t.any"), k, 32) && !wcscmp(k, L"F6"));

    /* switching off a service switches off what needs it */
    registry_fresh("[functions]\nsvc.a = off\n");
    CHECK(!fn_active(fn_find("svc.b")) && !fn_active(fn_find("t.needs")));
    fn_ctx one = pane(FN_S_ITEMS, 1);
    CHECK(fn_key(VK_F9, 0, &one) == 0);
    fn_start_hot(0);
    fn_start_hot(1);
    CHECK(t_started[0] == 0 && t_started[1] == 0);
    CHECK(!strcmp(fn_option(fn_find("t.two"), "program"), "auto"));

    /* problems are reported, the rest still applies */
    registry_fresh("[functions]\nnope = off\nt.dual = maybe\nt.two = off\n[keys]\nt.any = Ctrl+Nope\n"
                   "[t.two]\ncolour = red\n[weird]\nx = 1\n");
    const char *e = fn_settings_errors();
    CHECK(strstr(e, "unknown function nope") != NULL);
    CHECK(strstr(e, "use on or off") != NULL);
    CHECK(strstr(e, "Ctrl+Nope") != NULL);
    CHECK(strstr(e, "no option colour") != NULL);
    CHECK(strstr(e, "unknown section [weird]") != NULL);
    CHECK(!fn_active(fn_find("t.two")));
    fn_shutdown();
}

static int async_starts,async_stops,async_parent_stops,async_child_starts,async_child_stops;
static int async_start(void){async_starts++;return FN_PENDING;}
static int async_stop(void){async_stops++;return FN_PENDING;}
static void async_parent_stop(void){async_parent_stops++;}
static int async_child_start(void){async_child_starts++;return 0;}
static void async_child_stop(void){async_child_stops++;}
static void test_registry_async_lifetime(void)
{
    puts("registry: asynchronous dependency startup, cancellation and teardown");
    fn_reset();fn_init(NULL);async_starts=async_stops=async_parent_stops=async_child_starts=async_child_stops=0;
    static const fn_def defs[]={
        {.id="async.parent",.life=FN_HOT,.flags=FN_DEFERRED,.stop=async_parent_stop},
        {.id="async.worker",.life=FN_HOT,.flags=FN_DEFERRED,.needs="async.parent",.start=async_start,.stop_async=async_stop},
        {.id="async.child",.life=FN_HOT,.flags=FN_DEFERRED,.needs="async.worker",.start=async_child_start,.stop=async_child_stop}
    };
    CHECK(fn_register(defs,3)==0);fn_settings(NULL,0);
    CHECK(fn_family(&defs[1])==-1); /* fixed definition must survive completions */
    CHECK(fn_start(&defs[2])==FN_PENDING);
    CHECK(async_starts==1&&async_child_starts==0&&fn_running(&defs[0])&&!fn_running(&defs[1]));
    CHECK(fn_start(&defs[2])==FN_PENDING&&async_starts==1);
    char report[8192];fn_report_csv(report,sizeof report);CHECK(strstr(report,",starting,")!=NULL);
    CHECK(fn_start_complete(&defs[1],0));CHECK(!fn_start_complete(&defs[1],0));
    CHECK(fn_start(&defs[2])==0&&async_child_starts==1);
    fn_stop(&defs[0]);
    CHECK(async_child_stops==1&&async_stops==1&&async_parent_stops==0);
    CHECK(!fn_running(&defs[0])&&!fn_running(&defs[1]));
    CHECK(!fn_shutdown()&&!fn_shutdown()&&async_stops==1&&async_parent_stops==0);
    CHECK(fn_stop_complete(&defs[1])&&async_parent_stops==1);
    CHECK(fn_shutdown()&&!fn_stop_complete(&defs[1]));
    CHECK(fn_start(&defs[2])==FN_PENDING&&async_starts==2);
    fn_stop(&defs[0]);CHECK(async_stops==2&&!fn_start_complete(&defs[1],0));
    CHECK(fn_start(&defs[2])==FN_PENDING&&async_starts==2); /* no replacement during cancellation */
    CHECK(fn_stop_complete(&defs[1])&&fn_shutdown());
    CHECK(fn_start(&defs[2])==FN_PENDING&&async_starts==3);
    CHECK(fn_start_complete(&defs[1],-1));CHECK(fn_start(&defs[2])<0&&async_child_starts==1);
    CHECK(fn_shutdown());
    /* Settings disable/reenable while the previous generation is stopping. */
    fn_reset();fn_init(NULL);fn_register(defs,3);fn_settings(NULL,0);fn_start_hot(1);
    CHECK(async_starts==4);
    fn_reload("[functions]\nasync.worker = off\n",strlen("[functions]\nasync.worker = off\n"));
    CHECK(async_stops==3);fn_reload(NULL,0);CHECK(async_starts==4);
    CHECK(fn_stop_complete(&defs[1]));fn_start_hot(1);CHECK(async_starts==5);
    CHECK(fn_start_complete(&defs[1],0)&&fn_running(&defs[1]));
    CHECK(!fn_shutdown());CHECK(fn_stop_complete(&defs[1])&&fn_shutdown());
}

static void test_registry_lifetime(void)
{
    printf("registry: lifetimes\n");
    registry_fresh(NULL);
    uint32_t cursor=0;
    CHECK(fn_start_hot_step(1,NULL)==0);
    CHECK(fn_start_hot_step(1,&cursor)==1);
    CHECK(t_started[0]==1&&t_started[1]==0); /* dependency alone in this step */
    CHECK(fn_start_hot_step(1,&cursor)==1);
    CHECK(t_started[1]==1&&fn_running(fn_find("svc.b")));
    CHECK(fn_start_hot_step(1,&cursor)==0);
    CHECK(fn_start_hot_step(1,&cursor)==0); /* no duplicate starts */
    fn_shutdown();CHECK(t_stopped[0]==1&&t_stopped[1]==1);
    fn_host step_host={host_now,host_arm,NULL};fn_init(&step_host);
    const fn_def failing_steps[]={
        {.id="failed.root",.life=FN_HOT,.flags=FN_DEFERRED,.needs="failed.service",.start=start_svc},
        {.id="failed.service",.life=FN_HOT,.flags=FN_DEFERRED,.start=start_fail}
    };
    fn_register(failing_steps,2);fn_settings(NULL,0);cursor=0;
    CHECK(fn_start_hot_step(1,&cursor)==1);
    int completed=0;for(int attempt=0;attempt<3;attempt++)if(!fn_start_hot_step(1,&cursor)){completed=1;break;}
    CHECK(completed);CHECK(!fn_running(fn_find("failed.root"))&&!fn_running(fn_find("failed.service")));
    fn_shutdown();registry_fresh(NULL);
    fn_ctx one = pane(FN_S_ITEMS, 1);

    /* hot: deferred ones start after the first frame, needs first */
    fn_start_hot(0);
    CHECK(t_started[0] == 1 && t_started[1] == 0);
    fn_start_hot(1);
    CHECK(t_started[1] == 1 && fn_running(fn_find("svc.b")));

    /* cold: started for the call, stopped right after */
    CHECK(fn_key(VK_F5, 0, &one) == 1);
    CHECK(t_started[2] == 1 && t_stopped[2] == 1 && !fn_running(fn_find("t.any")));
    /* ...unless async work holds it */
    const fn_def *any = fn_find("t.any");
    CHECK(fn_hold(any) == 0 && fn_running(any) && t_started[2] == 2);
    CHECK(fn_key(VK_F5, 0, &one) == 1 && fn_running(any) && t_stopped[2] == 1);
    fn_release(any);
    CHECK(!fn_running(any) && t_stopped[2] == 2);
    fn_release(any); /* one too many */
    CHECK(fn_hold(fn_find("t.off")) == -1); /* inactive functions can't be held */

    /* cold functions may not own timers */
    CHECK(fn_timer(any, 100) == -1);
    char report[2048];
    fn_report(report, sizeof report);
    CHECK(strstr(report, "VIOLATIONS 2") != NULL);

    /* warm: stays after use, ticks while running, stops after idle_ms */
    const fn_def *warm = fn_find("t.warm");
    CHECK(fn_key(VK_F11, 0, &one) == 1 && fn_running(warm) && t_started[3] == 1);
    CHECK(t_armed == 1000);                      /* the idle deadline */
    CHECK(fn_timer(warm, 300) == 0 && t_armed == 300);
    t_now += 300;
    fn_tick();
    CHECK(t_ticks == 1 && fn_running(warm));
    t_now += 600;
    fn_key(VK_F11, 0, &one);                     /* use resets the idle clock */
    t_now += 600;
    fn_tick();
    CHECK(fn_running(warm) && t_ticks == 2);
    t_now += 1000;
    fn_tick();
    CHECK(!fn_running(warm) && t_stopped[3] == 1);
    CHECK(t_armed == 0);                         /* nothing due: no timer at all */
    CHECK(fn_hold(warm) == 0);                   /* holds keep it past idle_ms */
    t_now += 5000;
    fn_tick();
    CHECK(fn_running(warm));
    fn_release(warm);                            /* in use until released: idle starts now */
    t_now += 999;
    fn_tick();
    CHECK(fn_running(warm));
    t_now += 1;
    fn_tick();
    CHECK(!fn_running(warm));

    /* a failed start is reported and the function stays off */
    CHECK(fn_key(VK_F3, 0, &one) == -1);
    CHECK(fn_key(VK_F3, 0, &one) == 0);

    /* a cold function's needs are started on use and outlive it */
    CHECK(fn_key(VK_F9, 0, &one) == 1 && fn_running(fn_find("svc.b")));

    /* shutdown: reverse start order, dependents first */
    t_norder = 0;
    fn_shutdown();
    CHECK(t_norder == 2 && t_order[0] == -2 && t_order[1] == -1);
    CHECK(!fn_running(fn_find("svc.a")));

    /* stopping a service stops what relies on it */
    fn_start_hot(0);
    fn_start_hot(1);
    t_norder = 0;
    fn_stop(fn_find("svc.a"));
    CHECK(t_norder == 2 && t_order[0] == -2 && !fn_running(fn_find("svc.b")));
    fn_shutdown();
}

static const fn_def cycle[] = {
    { .id = "c.a", .life = FN_COLD, .where = FN_PANE, .needs = "c.b", .keys = "F1", .run = run0 },
    { .id = "c.b", .life = FN_COLD, .where = FN_PANE, .needs = "c.a", .run = run0 },
    { .id = "c.c", .life = FN_COLD, .where = FN_PANE, .needs = "c.missing", .keys = "F2", .run = run0 },
};

static void test_registry_bad_tables(void)
{
    printf("registry: bad tables\n");
    fn_reset();
    fn_register(cycle, 3);
    fn_register(cycle, 1); /* twice */
    fn_settings(NULL, 0);
    fn_ctx one = pane(FN_S_ITEMS, 1);
    CHECK(fn_key(VK_F1, 0, &one) <= 0);  /* a cycle never starts, and never hangs */
    CHECK(fn_key(VK_F2, 0, &one) == 0);  /* a missing need keeps it off */
    char list[4096];
    fn_list(list, sizeof list);
    CHECK(strstr(list, "c.missing") != NULL);
    fn_reset();
}

/* ---- session file ------------------------------------------------------------ */

/* ---- type icons ---------------------------------------------------------------- */

/* As the engine does it: the extension starts after the last '.' (none for folders). */
static ti_kind kind_of(const wchar_t *name)
{
    uint32_t len = (uint32_t)wcslen(name);
    const wchar_t *dot = wcsrchr(name, L'.');
    return ti_of_name(name, len, dot ? (uint32_t)(dot - name) + 1 : len, 0);
}

static void test_typeicons_kinds(void)
{
    printf("type icons: categories\n");
    ti_reset();
    const char *dup = NULL;
    CHECK(ti_duplicates(&dup) == 0);
    if (dup) printf("  listed twice: %s\n", dup);

    CHECK(kind_of(L"report.pdf") == TI_PDF);
    CHECK(kind_of(L"REPORT.PDF") == TI_PDF); /* case does not matter */
    CHECK(kind_of(L"photo.jpeg") == TI_IMAGE && kind_of(L"raw.CR3") == TI_IMAGE);
    CHECK(kind_of(L"song.flac") == TI_AUDIO && kind_of(L"clip.mkv") == TI_VIDEO);
    CHECK(kind_of(L"budget.xlsx") == TI_SHEET && kind_of(L"data.csv") == TI_SHEET);
    CHECK(kind_of(L"talk.pptx") == TI_SLIDES && kind_of(L"letter.docx") == TI_DOC);
    CHECK(kind_of(L"backup.tar.gz") == TI_ARCHIVE && kind_of(L"x.7z") == TI_ARCHIVE);
    CHECK(kind_of(L"main.rs") == TI_CODE && kind_of(L"app.ts") == TI_CODE && kind_of(L"x.c++") == TI_CODE);
    CHECK(kind_of(L"build.ps1") == TI_SCRIPT && kind_of(L"run.bat") == TI_SCRIPT);
    CHECK(kind_of(L"setup.exe") == TI_PROGRAM && kind_of(L"setup.msi") == TI_PROGRAM);
    CHECK(kind_of(L"Word.lnk") == TI_SHORTCUT && kind_of(L"x.appref-ms") == TI_SHORTCUT);
    CHECK(kind_of(L"kernel32.dll") == TI_SYSTEM && kind_of(L"cfg.json") == TI_DATA && kind_of(L"id.key") == TI_DATA);
    CHECK(kind_of(L"app.db") == TI_DATABASE && kind_of(L"x.ttf") == TI_FONT && kind_of(L"win.iso") == TI_DISC);
    CHECK(kind_of(L"disk.vhd") == TI_DISC && kind_of(L"chip.vhdl") == TI_CODE);
    CHECK(kind_of(L"art.psd") == TI_DESIGN && kind_of(L"notes.md") == TI_TEXT);

    CHECK(kind_of(L".gitignore") == TI_DATA && kind_of(L".env") == TI_DATA); /* dotfiles */
    CHECK(kind_of(L"Makefile") == TI_CODE && kind_of(L"Dockerfile") == TI_CODE);
    CHECK(kind_of(L"LICENSE") == TI_TEXT && kind_of(L"README") == TI_TEXT);
    CHECK(kind_of(L"notes") == TI_UNKNOWN && kind_of(L"trailing.") == TI_UNKNOWN);
    CHECK(kind_of(L"x.thisextensionistoolong") == TI_UNKNOWN);
    CHECK(kind_of(L"\x540D\x524D.txt") == TI_TEXT);  /* the name may be anything */
    CHECK(kind_of(L"file.\x65E5\x672C") == TI_UNKNOWN);
    CHECK(kind_of(L"") == TI_UNKNOWN);
    CHECK(ti_of_name(L"pics.jpg", 8, 5, FILE_ATTRIBUTE_DIRECTORY) == TI_FOLDER);
    CHECK(ti_of_path(L"C:\\x\\movie.mp4", 0) == TI_VIDEO && ti_of_path(L"C:\\x\\movie.mp4", 1) == TI_FOLDER);
    CHECK(ti_of_path(L"Makefile", 0) == TI_CODE);

    /* settings move extensions; the last one wins; ti_reset forgets them */
    CHECK(ti_add(TI_CODE, ".tsx, vue  Svelte") == 0);
    CHECK(kind_of(L"a.tsx") == TI_CODE && kind_of(L"a.VUE") == TI_CODE && kind_of(L"a.svelte") == TI_CODE);
    CHECK(ti_add(TI_CODE, "txt") == 0 && kind_of(L"a.txt") == TI_CODE);
    CHECK(ti_add(TI_DATA, "txt") == 0 && kind_of(L"a.txt") == TI_DATA);
    CHECK(ti_add(TI_CODE, "way-too-long-extension a/b x.y ok") == 3 && kind_of(L"a.ok") == TI_CODE);
    CHECK(ti_add(TI_DRIVE, "x") == 1); /* places are not file categories */
    CHECK(ti_add(TI_CODE, "") == 0);
    ti_reset();
    CHECK(kind_of(L"a.txt") == TI_TEXT && kind_of(L"a.ok") == TI_UNKNOWN);

    for (int k = 0; k < TI_FILE_KINDS; k++) CHECK(ti_find(ti_name((ti_kind)k)) == k);
    CHECK(ti_find("Code") == TI_CODE && ti_find("nope") == -1 && ti_name(TI_DRIVE)[0] == 0);
}

/* A 32-bit DIB to draw into; pixels read back directly. */
typedef struct canvas {
    HDC       dc;
    HBITMAP   bmp, old;
    uint32_t *px;
    int       w, h;
} canvas;

static int canvas_open(canvas *c, int w, int h)
{
    BITMAPINFO bi = { { sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB } };
    c->dc = CreateCompatibleDC(NULL);
    c->bmp = CreateDIBSection(c->dc, &bi, DIB_RGB_COLORS, (void **)&c->px, NULL, 0);
    if (!c->dc || !c->bmp) return -1;
    c->old = SelectObject(c->dc, c->bmp);
    c->w = w;
    c->h = h;
    return 0;
}

static void canvas_close(canvas *c)
{
    SelectObject(c->dc, c->old);
    DeleteObject(c->bmp);
    DeleteDC(c->dc);
}

static void test_typeicons_draw(void)
{
    printf("type icons: drawing\n");
    canvas cv;
    if (canvas_open(&cv, 96, 96)) {
        CHECK(!"no canvas");
        return;
    }
    const uint32_t blank = 0x00010203;
    static const int sizes[] = { 16, 20, 24, 32, 48 };
    int ok_inside = 1, ok_outside = 1, ok_color = 1;
    for (int k = 0; k < TI_COUNT; k++)
        for (int s = 0; s < 5; s++)
            for (uint32_t flags = 0; flags < 4; flags++) {
                int size = sizes[s], x0 = 24, y0 = 24, inside = 0, outside = 0, own = 0;
                for (int i = 0; i < cv.w * cv.h; i++) cv.px[i] = blank;
                ti_draw(cv.dc, (ti_kind)k, x0, y0, size, flags, 0);
                GdiFlush();
                COLORREF want = ti_color((ti_kind)k);
                uint32_t want_px = (uint32_t)(GetRValue(want) << 16 | GetGValue(want) << 8 | GetBValue(want));
                for (int y = 0; y < cv.h; y++)
                    for (int x = 0; x < cv.w; x++) {
                        uint32_t p = cv.px[y * cv.w + x] & 0xFFFFFF;
                        if (p == blank) continue;
                        if (x >= x0 && x < x0 + size && y >= y0 && y < y0 + size) inside++;
                        else outside++;
                        own += p == want_px;
                    }
                if (inside < size * size / 12) {
                    ok_inside = 0;
                    printf("  %s at %d (flags %u): only %d pixels\n", k < TI_FILE_KINDS ? ti_name((ti_kind)k) : "place", size, flags, inside);
                }
                if (outside) {
                    ok_outside = 0;
                    printf("  %s at %d (flags %u): %d pixels outside its box\n", k < TI_FILE_KINDS ? ti_name((ti_kind)k) : "place", size, flags, outside);
                }
                if (!(flags & TI_FADED) && own < size / 2) ok_color = 0;
            }
    CHECK(ok_inside);  /* every icon is visible at every size */
    CHECK(ok_outside); /* and stays in its box */
    CHECK(ok_color);   /* in its category's color */
    canvas_close(&cv);
}

/* app_tests --gallery FILE.bmp: every icon at several sizes, for review. */
static int write_gallery(const wchar_t *file)
{
    static const int sizes[] = { 16, 20, 24, 32, 48 };
    const int cell = 60, label_w = 110, w = label_w + 7 * cell, h = TI_COUNT * cell + 30;
    canvas cv;
    if (canvas_open(&cv, w, h)) return 1;
    RECT all = { 0, 0, w, h };
    SetDCBrushColor(cv.dc, RGB(30, 30, 30));
    FillRect(cv.dc, &all, GetStockObject(DC_BRUSH));
    SetBkMode(cv.dc, TRANSPARENT);
    SetTextColor(cv.dc, RGB(200, 200, 205));
    HFONT font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SelectObject(cv.dc, font);
    static const wchar_t *const heads[] = { L"16", L"20", L"24", L"32", L"48", L"hidden", L"link" };
    for (int i = 0; i < 7; i++) TextOutW(cv.dc, label_w + i * cell + 8, 6, heads[i], (int)wcslen(heads[i]));
    static const wchar_t *const places[] = { L"drive", L"home", L"desktop", L"documents", L"downloads" };
    for (int k = 0; k < TI_COUNT; k++) {
        int y = 30 + k * cell;
        wchar_t name[32];
        if (k < TI_FILE_KINDS) MultiByteToWideChar(CP_UTF8, 0, ti_name((ti_kind)k), -1, name, 32);
        else wcscpy(name, places[k - TI_FILE_KINDS]);
        TextOutW(cv.dc, 10, y + 20, name, (int)wcslen(name));
        for (int s = 0; s < 5; s++) ti_draw(cv.dc, (ti_kind)k, label_w + s * cell + 6, y + (48 - sizes[s]) / 2 + 4, sizes[s], 0, 0);
        ti_draw(cv.dc, (ti_kind)k, label_w + 5 * cell + 6, y + 20, 16, TI_FADED, 0);
        ti_draw(cv.dc, (ti_kind)k, label_w + 6 * cell + 6, y + 20, 16, TI_LINK, 0);
    }
    GdiFlush();
    BITMAPFILEHEADER fh = { 0x4D42 };
    BITMAPINFOHEADER ih = { sizeof ih, w, -h, 1, 32, BI_RGB };
    DWORD bytes = (DWORD)(w * h * 4), wr;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + bytes;
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    int rc = f == INVALID_HANDLE_VALUE;
    if (!rc) {
        WriteFile(f, &fh, sizeof fh, &wr, NULL);
        WriteFile(f, &ih, sizeof ih, &wr, NULL);
        WriteFile(f, cv.px, bytes, &wr, NULL);
        CloseHandle(f);
    }
    DeleteObject(font);
    canvas_close(&cv);
    return rc;
}

static int t_want_icons;
static int wanted_icons(void) { return t_want_icons; }
static const fn_def wanted_defs[] = {
    { .id = "t.wanted", .title = L"Wanted", .life = FN_HOT, .start = start_cold, .stop = stop_cold, .wanted = wanted_icons },
};

static void test_registry_wanted(void)
{
    printf("registry: services the settings don't need\n");
    registry_fresh(NULL);
    fn_register(wanted_defs, 1);
    t_want_icons = 0;
    fn_start_hot(0);
    CHECK(!fn_running(fn_find("t.wanted")) && t_started[2] == 0);
    static char list[16384];
    fn_list(list, sizeof list);
    CHECK(strstr(list, "not started: the current settings don't need it") != NULL);
    CHECK(fn_hold(fn_find("t.wanted")) == 0 && fn_running(fn_find("t.wanted"))); /* still startable on demand */
    fn_release(fn_find("t.wanted"));
    fn_shutdown();

    registry_fresh(NULL);
    fn_register(wanted_defs, 1);
    t_want_icons = 1;
    fn_start_hot(0);
    CHECK(fn_running(fn_find("t.wanted")) && t_started[2] == 1);
    fn_shutdown();
}

/* ---- settings while running ------------------------------------------------------------ */

static int l_conf[3], l_svc_started, l_svc_stopped;
static void l_conf_a(void)
{
    l_conf[0]++;
    if (atoi(fn_option(fn_find("l.a"), "n")) == 7) fn_config_error(fn_find("l.a"), "[l.a] n = 7: seven is not liked");
}
static void l_conf_svc(void) { l_conf[1]++; }
static void l_conf_restart(void) { l_conf[2]++; }
static int l_svc_start(void) { l_svc_started++; return 0; }
static void l_svc_stop(void) { l_svc_stopped++; }

static const fn_opt l_opts[] = {
    { "mode", "one", "which one", "one two" },
    { "n", "5", "how many", "#0..10" },
    { "text", "", "anything" },
    { NULL },
};
static const fn_opt l_ropts[] = { { "x", "a", "read at startup", "a b" }, { NULL } };

static const fn_def live_defs[] = {
    { .id = "l.a", .title = L"Live A", .life = FN_COLD, .where = FN_PANE, .keys = "F2", .opts = l_opts,
      .configure = l_conf_a, .run = run0, .about = "a\tfunction with\noptions" },
    { .id = "l.svc", .title = L"Live service", .life = FN_HOT, .start = l_svc_start, .stop = l_svc_stop,
      .configure = l_conf_svc },
    { .id = "l.dep", .title = L"Needs the service", .life = FN_COLD, .where = FN_PANE, .needs = "l.svc", .run = run0 },
    { .id = "l.restart", .title = L"Restart only", .life = FN_COLD, .flags = FN_RESTART, .opts = l_ropts,
      .configure = l_conf_restart },
    { .id = "l.menu", .title = L"In menus", .life = FN_COLD, .where = FN_PANE, .menus = FN_MENU_ITEMS,
      .keys = "Ctrl+Shift+M, f9", .run = run0 },
};

static void live_fresh(const char *settings)
{
    fn_reset();
    memset(l_conf, 0, sizeof l_conf);
    l_svc_started = l_svc_stopped = 0;
    fn_host h = { host_now, host_arm, NULL };
    fn_init(&h);
    fn_register(live_defs, sizeof live_defs / sizeof live_defs[0]);
    fn_settings(settings, settings ? strlen(settings) : 0);
    fn_configure_all();
}

static int reload(const char *text) { return fn_reload(text, text ? strlen(text) : 0); }

static void test_registry_reload(void)
{
    printf("registry: settings while running\n");
    const fn_def *a = fn_find("l.a");

    /* values that don't fit are reported and the default holds */
    live_fresh("[l.a]\nmode = three\nn = 11\ntext = free\n");
    a = fn_find("l.a");
    const char *e = fn_settings_errors();
    CHECK(strstr(e, "[l.a] mode = three: use one or two") != NULL);
    CHECK(strstr(e, "[l.a] n = 11: use a whole number from 0 to 10") != NULL);
    CHECK(!strcmp(fn_option(a, "mode"), "one") && !strcmp(fn_option(a, "n"), "5"));
    CHECK(!strcmp(fn_option(a, "text"), "free"));
    live_fresh("[l.a]\nmode = TWO\nn = 3\nn = x\n"); /* case doesn't matter; the last one wins, even when it doesn't fit */
    a = fn_find("l.a");
    CHECK(!strcmp(fn_option(a, "mode"), "TWO") && !strcmp(fn_option(a, "n"), "5"));

    /* startup configures everything once */
    live_fresh(NULL);
    a = fn_find("l.a");
    CHECK(l_conf[0] == 1 && l_conf[1] == 1 && l_conf[2] == 1);
    fn_start_hot(0);
    fn_start_hot(1);
    CHECK(l_svc_started == 1);
    uint32_t rev = fn_revision();

    /* the same settings again: nothing changes, nothing is configured */
    CHECK(reload(NULL) == 0 && l_conf[0] == 1 && fn_revision() == rev + 1);
    /* an option: that function is configured again, and sees the new value */
    CHECK(reload("[l.a]\nmode = two\n") == 1 && l_conf[0] == 2 && l_conf[1] == 1);
    CHECK(!strcmp(fn_option(a, "mode"), "two"));
    /* keys only: counted as a change, nothing to configure */
    CHECK(reload("[l.a]\nmode = two\n[keys]\nl.a = F3\n") == 1 && l_conf[0] == 2);
    fn_ctx c = pane(0, 0);
    CHECK(fn_key(VK_F3, 0, &c) == 1 && fn_key(VK_F2, 0, &c) == 0);

    /* switching a service off stops it, and switching it on again starts it */
    CHECK(reload("[functions]\nl.svc = off\n") >= 2 && l_svc_stopped == 1 && !fn_running(fn_find("l.svc")));
    CHECK(!fn_active(fn_find("l.dep")) && fn_key(VK_F2, 0, &c) == 1); /* l.a's key is back to F2 */
    CHECK(l_conf[1] == 2);                                             /* told it is off */
    CHECK(reload(NULL) >= 1 && l_svc_started == 2 && fn_running(fn_find("l.svc")) && fn_active(fn_find("l.dep")));

    /* restart-only: a change is noted but waits for the next start */
    CHECK(reload("[l.restart]\nx = b\n") == 1 && l_conf[2] == 1);

    /* configure's own problems: reported until the settings change again */
    CHECK(reload("[l.a]\nn = 7\n") == 2 && strstr(fn_settings_errors(), "seven is not liked") != NULL);
    CHECK(strstr(fn_settings_errors(), "seven is not liked") != NULL); /* asked again: still there */
    CHECK(reload("[l.a]\nn = 8\n") == 1 && fn_settings_errors()[0] == 0);

    /* what the settings panel gets */
    reload("[l.a]\nmode = two\n[menu]\nl.a = off\n[functions]\nnope = on\n");
    static char d[8192];
    size_t need = fn_describe(NULL, 0);
    CHECK(need > 100 && fn_describe(d, sizeof d) == need && strlen(d) == need);
    CHECK(strstr(d, "fn\tl.a\tLive A\tcold\ton\tF2\tF2\tmkP\t\ta function with options\n") == NULL);
    CHECK(strstr(d, "fn\tl.a\tLive A\tcold\ton\tF2\tF2\tkP\t\ta function with options\n") != NULL); /* no tabs or breaks inside */
    CHECK(strstr(d, "fn\tl.menu\tIn menus\tcold\ton\tCtrl+Shift+M, F9\tCtrl+Shift+M, F9\tkMP\t") != NULL); /* names as written back */
    CHECK(strstr(d, "opt\tmode\ttwo\tone\t1\tone two\twhich one\n") != NULL);
    CHECK(strstr(d, "opt\tn\t5\t5\t0\t#0..10\thow many\n") != NULL);
    CHECK(strstr(d, "fn\tl.restart\tRestart only\tcold\ton\t\t\tr\t") != NULL);
    CHECK(strstr(d, "error\t[functions] unknown function nope\n") != NULL);
    char small[64];
    CHECK(fn_describe(small, sizeof small) == need && strlen(small) == sizeof small - 1); /* cut, still ended */
    reload("[functions]\nl.svc = off\n");
    fn_describe(d, sizeof d);
    CHECK(strstr(d, "fn\tl.dep\tNeeds the service\tcold\tneeds\t") != NULL);

    /* a new settings file: everything commented out, so it changes nothing */
    live_fresh(NULL);
    static char t[8192];
    size_t tn = fn_template(t, sizeof t);
    CHECK(tn > 0 && tn < sizeof t && strstr(t, ";l.a = F2") && strstr(t, "[l.a]") && strstr(t, ";mode = one") &&
          strstr(t, "(one two)") && strstr(t, "(0 to 10)"));
    CHECK(reload(t) == 0 && fn_settings_errors()[0] == 0);
    fn_shutdown();
}

/* fn_settings_edit on `in`; the result, or "!why" when refused (static buffer) */
static const char *edit(const char *in, const char *sec, const char *key, const char *val)
{
    static char res[2048];
    char *o = NULL, why[200];
    size_t n = 0;
    int rc = fn_settings_edit(in, in ? strlen(in) : 0, sec, key, val, &o, &n, why, sizeof why);
    if (rc) snprintf(res, sizeof res, "!%s", why);
    else snprintf(res, sizeof res, "%.*s", (int)n, o);
    free(o);
    return res;
}

static void test_settings_edit(void)
{
    printf("registry: editing the settings text\n");
    live_fresh(NULL);
    /* a new file, then a section added at its end */
    CHECK(!strcmp(edit(NULL, "functions", "l.svc", "off"), "[functions]\r\nl.svc = off\r\n"));
    CHECK(!strcmp(edit("[functions]\r\nl.svc = off\r\n", "l.a", "mode", "two"),
                  "[functions]\r\nl.svc = off\r\n\r\n[l.a]\r\nmode = two\r\n"));
    CHECK(!strcmp(edit("[keys]\nl.a = F3", "l.a", "n", "7"), "[keys]\nl.a = F3\n\n[l.a]\nn = 7\n")); /* \n files stay \n */
    /* the value changes in place: spacing, comments and other lines stay */
    const char *f = "; mine\r\n[l.a]\r\n  mode=one\r\n; note\r\nn = 3\r\n\r\n[functions]\r\nl.dep = off\r\n";
    CHECK(!strcmp(edit(f, "l.a", "MODE", "two"), "; mine\r\n[l.a]\r\n  mode= two\r\n; note\r\nn = 3\r\n\r\n[functions]\r\nl.dep = off\r\n"));
    CHECK(!strcmp(edit(f, "l.a", "text", "x y"), "; mine\r\n[l.a]\r\n  mode=one\r\n; note\r\nn = 3\r\ntext = x y\r\n\r\n[functions]\r\nl.dep = off\r\n"));
    CHECK(!strcmp(edit(f, "functions", "l.dep", "on"), "; mine\r\n[l.a]\r\n  mode=one\r\n; note\r\nn = 3\r\n\r\n[functions]\r\n"));
    /* the last line counts, so that's the one changed; a reset removes them all */
    CHECK(!strcmp(edit("[l.a]\nn = 1\nn = 2\n", "l.a", "n", "4"), "[l.a]\nn = 1\nn = 4\n"));
    CHECK(!strcmp(edit("[l.a]\nn = 1\n[l.svc]\n[l.a]\nn = 2\n", "l.a", "n", NULL), "[l.a]\n[l.svc]\n[l.a]\n"));
    /* the default removes the line: numbers by value, words in any case, text exactly */
    CHECK(!strcmp(edit("[l.a]\nn = 1\nmode = two\n", "l.a", "n", "05"), "[l.a]\nmode = two\n"));
    CHECK(!strcmp(edit("[l.a]\nn = 1\nmode = two\n", "l.a", "mode", "ONE"), "[l.a]\nn = 1\n"));
    CHECK(!strcmp(edit("[functions]\nl.a = off\n", "functions", "l.a", "yes"), "[functions]\n"));
    /* after the commented-out line of a template */
    live_fresh(NULL);
    static char t[8192];
    fn_template(t, sizeof t);
    const char *r = edit(t, "functions", "l.svc", "off");
    CHECK(strstr(r, ";l.svc = on\r\nl.svc = off\r\n;l.dep = on\r\n") != NULL && strlen(r) == strlen(t) + 13);
    r = edit(t, "l.a", "mode", "two");
    CHECK(strstr(r, ";mode = one\r\nmode = two\r\n") != NULL);
    CHECK(reload(r) == 1 && !strcmp(fn_option(fn_find("l.a"), "mode"), "two") && fn_settings_errors()[0] == 0);
    /* a file without a last line end, and a BOM */
    CHECK(!strcmp(edit("\xEF\xBB\xBF[l.a]\r\nn = 1", "l.a", "mode", "two"), "\xEF\xBB\xBF[l.a]\r\nn = 1\r\nmode = two\r\n"));
    CHECK(!strcmp(edit("\xEF\xBB\xBF[l.a]\r\nn = 1", "l.a", "n", NULL), "\xEF\xBB\xBF[l.a]\r\n"));
    /* refused: what doesn't fit, what doesn't exist, more than one line */
    CHECK(!strcmp(edit("", "l.a", "n", "11"), "!n = 11: use a whole number from 0 to 10"));
    CHECK(!strcmp(edit("", "l.a", "mode", "three"), "!mode = three: use one or two"));
    CHECK(!strcmp(edit("", "functions", "l.a", "maybe"), "![functions] l.a = maybe: use on or off"));
    CHECK(!strcmp(edit("", "functions", "nope", "on"), "!no function nope"));
    CHECK(!strcmp(edit("", "l.a", "nope", "1"), "![l.a] has no option nope"));
    CHECK(!strcmp(edit("", "l.a", "text", "a\nb"), "!the value is on more than one line"));
    CHECK(!strcmp(edit("", "l.a", "text", "  spaced  "), "[l.a]\r\ntext = spaced\r\n"));

    /* keys: written as the registry names them, repeats dropped; the default (in any spelling) or none */
    CHECK(!strcmp(edit("", "keys", "l.a", "f3 ,ctrl+shift+x, F3"), "[keys]\r\nl.a = F3, Ctrl+Shift+X\r\n"));
    CHECK(!strcmp(edit("[keys]\nl.a = F3\n", "keys", "l.a", "f2"), "[keys]\n"));
    CHECK(!strcmp(edit("[keys]\nl.a = F3\n", "keys", "l.a", ""), "[keys]\nl.a = none\n"));
    CHECK(!strcmp(edit("", "keys", "l.menu", "ctrl+shift+m,F9"), ""));
    CHECK(!strcmp(edit("", "keys", "l.a", "Ctrl+Hyper"), "!\"Ctrl+Hyper\" is not a key name"));
    CHECK(!strcmp(edit("", "keys", "l.a", "F1, F2, F3, F4, F5"), "!at most 4 keys"));
    CHECK(!strcmp(edit("", "keys", "l.svc", "F1"), "!l.svc has nothing a key could run"));
    /* menus and the palette: only where a function can be; on is the default */
    CHECK(!strcmp(edit("", "menu", "l.menu", "off"), "[menu]\r\nl.menu = off\r\n"));
    CHECK(!strcmp(edit("[menu]\r\nl.menu = off\r\n", "menu", "l.menu", "on"), "[menu]\r\n"));
    CHECK(!strcmp(edit("", "menu", "l.a", "off"), "!l.a is in no context menu"));
    CHECK(!strcmp(edit("", "palette", "l.a", "no"), "[palette]\r\nl.a = off\r\n"));
    CHECK(!strcmp(edit("", "palette", "l.svc", "off"), "!l.svc is never in the palette"));
    CHECK(reload("[keys]\nl.a = F3, Ctrl+Shift+X\n[palette]\nl.a = off\n[menu]\nl.menu = off\n") >= 2 && fn_settings_errors()[0] == 0);
    static char d[8192];
    fn_describe(d, sizeof d);
    CHECK(strstr(d, "fn\tl.a\tLive A\tcold\ton\tF3, Ctrl+Shift+X\tF2\tpkP\t") != NULL);
    CHECK(strstr(d, "fn\tl.menu\tIn menus\tcold\ton\tCtrl+Shift+M, F9\tCtrl+Shift+M, F9\tmkMP\t") != NULL);

    /* key names: what the panel records is what the registry reads back */
    CHECK(fn_key_nameable((fn_binding){ VK_F5, FN_CTRL }) && fn_key_nameable((fn_binding){ VK_OEM_COMMA, FN_CTRL }));
    CHECK(fn_key_nameable((fn_binding){ 'N', FN_CTRL | FN_SHIFT }) && fn_key_nameable((fn_binding){ VK_TAB, FN_ALT }));
    CHECK(!fn_key_nameable((fn_binding){ VK_SHIFT, FN_SHIFT }) && !fn_key_nameable((fn_binding){ VK_LWIN, 0 }));
    fn_shutdown();
}

static const fn_def hidden_defs[] = {
    { .id = "t.hidden", .title = L"Hidden", .life = FN_COLD, .flags = FN_HIDDEN, .where = FN_PANE, .run = run0 },
};

static void test_registry_palette(void)
{
    printf("registry: palette exposure\n");
    registry_fresh(NULL);
    fn_register(hidden_defs, 1);
    CHECK(fn_in_palette(fn_find("t.any")));
    CHECK(fn_in_palette(fn_find("t.needs")));
    CHECK(!fn_in_palette(fn_find("svc.a")));    /* nothing to run */
    CHECK(!fn_in_palette(fn_find("t.off")));    /* optional, not switched on */
    CHECK(!fn_in_palette(fn_find("t.hidden"))); /* FN_HIDDEN */
    CHECK(!fn_in_palette(NULL));

    registry_fresh("[palette]\nt.any = off\n[functions]\nsvc.b = off\n");
    CHECK(fn_settings_errors()[0] == 0);
    CHECK(!fn_in_palette(fn_find("t.any")));
    CHECK(!fn_in_palette(fn_find("t.needs"))); /* what it needs is off */
    fn_ctx c = pane(0, 0);
    CHECK(fn_key(VK_F5, 0, &c) == 1 && t_ran[1] == 1); /* hidden from the palette, still on its key */

    registry_fresh("[palette]\nnope = off\nt.any = maybe\n");
    CHECK(strstr(fn_settings_errors(), "[palette] unknown function nope") != NULL);
    CHECK(strstr(fn_settings_errors(), "t.any = maybe") != NULL);
}

static void test_folder_views(void)
{
    printf("folder views: identities, partial choices and bounded parsing\n");
    folderviews s = {0}, r = {0};
    fv_value v = {FV_ALL, 1, 256, FPE_SORT_MTIME, 1}, got;
    CHECK(fv_set(&s, L"C:\\Photos\\", v) == 1);
    CHECK(fv_get(&s, L"c:/PHOTOS", &got) && got.mask == FV_ALL && got.size == 256);
    CHECK(fv_set(&s, L"c:\\Photos", v) == 0 && s.count == 1);
    CHECK(fv_set(&s, L"C:\\Other", (fv_value){FV_SORT, 0, 160, FPE_SORT_EXT, 0}) == 1);
    CHECK(fv_get(&s, L"C:\\Other", &got) && got.mask == FV_SORT && !got.mode);
    CHECK(fv_set(&s, L"C:\\Other", (fv_value){FV_MODE, 1, 160, 0, 0}) == 1);
    CHECK(fv_get(&s, L"C:\\Other", &got) && got.mask == (FV_SORT|FV_MODE) && got.sort == FPE_SORT_EXT);
    CHECK(fv_set(&s, L"\\\\wsl.localhost\\Ubuntu\\home\\A ", v) == 1);
    CHECK(fv_set(&s, L"\\\\wsl$\\Ubuntu\\home\\a ", (fv_value){FV_SORT,0,160,FPE_SORT_SIZE,1}) == 1);
    CHECK(fv_get(&s, L"\\\\WSL$\\UBUNTU\\home\\A ", &got) && got.mode == 1);
    CHECK(fv_get(&s, L"\\\\wsl.localhost\\Ubuntu\\home\\a ", &got) && got.mask == FV_SORT);
    CHECK(!fv_get(&s, L"\\\\wsl.localhost\\Other\\home\\A ", &got));
    CHECK(!fv_get(&s, L"\\\\wsl.localhost\\Ubuntu\\home\\A", &got));
    CHECK(fv_same_path(L"\\\\wsl$\\Ubuntu\\home\\A", L"\\\\wsl.localhost\\ubuntu\\home\\A"));
    CHECK(!fv_same_path(L"\\\\wsl$\\Ubuntu\\home\\A", L"\\\\wsl.localhost\\Ubuntu\\home\\a"));
    CHECK(fv_set(&s, L"C:\\invalid\xd800", v) == -1);
    CHECK(fv_set(&s, L"C:\\Unicode \x03b2", v) == 1);
    CHECK(fv_set(&s, L"relative", v) == -1);
    CHECK(fv_set(&s, L"C:\\bad\nline", v) == -1);
    CHECK(fv_set(&s, L"C:\\bad", (fv_value){8,0,160,0,0}) == -1);
    CHECK(fv_set(&s, L"C:\\bad", (fv_value){FV_SIZE,0,512,0,0}) == -1);
    char *buf = malloc(65536);
    size_t n = fv_format(&s, buf, 65536);
    CHECK(n && fv_parse(&r, buf, n) == 0 && r.count == s.count);
    CHECK(fv_get(&r, L"C:\\Unicode \x03b2", &got) && got.descending == 1);
    CHECK(fv_format(&s, buf, 10) == 0 && !buf[0]);
    const char bad[] = "# OpenPilot folder views v1\n"
        "7\t1\t256\t3\t1\tC:\\keep\n"
        "7\t1\t256\t3\t1\tC:\\KEEP\n" /* duplicate */
        "7\t1\t256\t3\t1\tC:\\invalid\xff\n"
        "7\t1\t257\t3\t1\tC:\\bad_size\n"
        "7\t1\t256\t9\t1\tC:\\bad_sort\n"
        "7\t1\t256\t3\t-1\tC:\\bad_desc\n"
        "7\t1\t256\t3\t1\trelative\n";
    CHECK(fv_parse(&r, bad, sizeof bad - 1) == 0 && r.count == 1);
    CHECK(fv_parse(&r, "# OpenPilot folder views v2\n", 27) == -1 && r.count == 1);
    CHECK(fv_parse(&r, "x\0y", 3) == -1 && r.count == 1);
    CHECK(fv_parse(&r, NULL, FV_FILE_MAX + 1) == -1);
    CHECK(fv_reset(&r, L"c:\\keep") == 1 && r.count == 0 && r.nremoved == 1);
    CHECK(fv_reset(&r, L"C:\\KEEP") == 1 && r.nremoved == 1);
    CHECK(fv_set(&r, L"C:\\keep", (fv_value){FV_SORT,0,160,0,0}) == 1);
    CHECK(fv_get(&r, L"C:\\keep", &got) && got.mask == FV_SORT && r.nremoved == 1);
    fv_free(&r);
    for (int i = 0; i <= FV_MAX; ++i) {
        wchar_t path[80]; _snwprintf(path,80,L"C:\\bounded\\%d",i);
        CHECK(fv_set(&r,path,v) == 1);
    }
    CHECK(r.count == FV_MAX && !fv_get(&r,L"C:\\bounded\\0",NULL));
    n = fv_format(&r,buf,65536);
    CHECK(n && fv_parse(&s,buf,n) == 0 && s.count == FV_MAX);
    fv_free(&s); fv_free(&r); free(buf);
}

static void test_folder_views_persistence(void)
{
    printf("folder views: atomic save, multi-window merge, resets and failed writes\n");
    wchar_t temp[MAX_PATH], dir[MAX_PATH+80], file[MAX_PATH+100];
    GetTempPathW(MAX_PATH,temp);
    _snwprintf(dir,MAX_PATH+80,L"%lsopenpilot-folderviews-%lu",temp,GetCurrentProcessId());
    CreateDirectoryW(dir,NULL);
    _snwprintf(file,MAX_PATH+100,L"%ls\\folder-views.txt",dir);
    DeleteFileW(file);
    folderviews a={0},b={0},disk={0}; fv_value got;
    CHECK(fv_load(&a,file) == 0 && !a.count);
    CHECK(fv_set(&a,L"C:\\Photos",(fv_value){FV_MODE|FV_SIZE,1,256,0,0}) == 1);
    CHECK(fv_save(&a,file) == 0 && !a.entries[0].changed);
    CHECK(fv_load(&b,file) == 0 && b.count == 1);
    CHECK(fv_set(&a,L"C:\\Photos",(fv_value){FV_SORT,0,160,FPE_SORT_SIZE,1}) == 1);
    CHECK(fv_set(&b,L"C:\\Photos",(fv_value){FV_SIZE,0,96,0,0}) == 1);
    CHECK(fv_save(&a,file) == 0 && fv_save(&b,file) == 0);
    CHECK(fv_load(&disk,file) == 0 && fv_get(&disk,L"C:\\Photos",&got));
    CHECK(got.mask == FV_ALL && got.size == 96 && got.sort == FPE_SORT_SIZE && got.mode == 1);
    CHECK(fv_set(&a,L"C:\\Code",(fv_value){FV_SORT,0,160,FPE_SORT_EXT,0}) == 1);
    CHECK(fv_reset(&b,L"C:\\Photos") == 1 && fv_save(&b,file) == 0);
    CHECK(fv_save(&a,file) == 0 && !fv_get(&a,L"C:\\Photos",NULL)); /* clean stale entry never resurrects */
    CHECK(fv_get(&a,L"C:\\Code",&got) && got.mask == FV_SORT);
    CHECK(fv_set(&a,L"C:\\Photos",(fv_value){FV_ALL,1,256,3,1}) == 1 && fv_save(&a,file) == 0);
    CHECK(fv_load(&b,file) == 0 && fv_reset(&b,L"C:\\Photos") == 1);
    CHECK(fv_set(&b,L"C:\\Photos",(fv_value){FV_SORT,0,160,0,0}) == 1 && fv_save(&b,file) == 0);
    CHECK(fv_load(&disk,file) == 0 && fv_get(&disk,L"C:\\Photos",&got) && got.mask == FV_SORT);
    HANDLE lock=CreateFileW(file,GENERIC_READ,0,NULL,OPEN_EXISTING,0,NULL);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CHECK(fv_set(&a,L"C:\\Pending",(fv_value){FV_SORT,0,160,4,1}) == 1);
    CHECK(fv_save(&a,file) != 0 && a.entries[0].changed == FV_SORT);
    if(lock != INVALID_HANDLE_VALUE)CloseHandle(lock);
    CHECK(fv_save(&a,file) == 0 && fv_load(&disk,file) == 0 && fv_get(&disk,L"C:\\Pending",NULL));
    HANDLE h=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);DWORD written;
    const char unknown[]="# OpenPilot folder views v9\r\n";
    CHECK(h != INVALID_HANDLE_VALUE && WriteFile(h,unknown,sizeof unknown-1,&written,NULL));
    if(h != INVALID_HANDLE_VALUE)CloseHandle(h);
    CHECK(fv_set(&a,L"C:\\New",(fv_value){FV_MODE,1,160,0,0}) == 1);
    CHECK(fv_save(&a,file) == ERROR_INVALID_DATA && a.entries[0].changed == FV_MODE);
    CHECK(fv_load(&disk,file) == ERROR_INVALID_DATA); /* future versions are not overwritten */
    fv_free(&a);fv_free(&b);fv_free(&disk);DeleteFileW(file);RemoveDirectoryW(dir);
}

static void test_session(void)
{
    printf("session file\n");
    session s, r;
    session_defaults(&s);
    s.has_rect = 1;
    SetRect(&s.rect, 10, 20, 1290, 780);
    s.maximized = 1;
    s.tree = 0;
    s.tree_w = 300;
    s.ratio = 0.62;
    s.hidden = 1;
    s.active = 1;
    wchar_t p1[] = L"C:\\Users\\TestUser", p2[] = L"D:\\Fotos 2026\\\x00C9t\x00E9 \x65E5\x672C", p3[] = L"\\\\srv\\share\\x";
    wchar_t spaced[] = L"\\\\wsl.localhost\\Ubuntu\\tmp\\A ";
    wchar_t plain[] = L"\\\\wsl.localhost\\Ubuntu\\tmp\\A";
    wchar_t raw[] = L"\\\\wsl.localhost\\Ubuntu\\tmp\\raw\xDCFF";
    wchar_t replacement[] = L"\\\\wsl.localhost\\Ubuntu\\tmp\\raw\xFFFD";
    s.tabs[0][0] = (ses_tab){ p1, 3, 1 };
    s.tabs[0][1] = (ses_tab){ p2, 0, 0 };
    s.tabs[0][2] = (ses_tab){ spaced, 0, 0 };
    s.tabs[0][3] = (ses_tab){ plain, 0, 0 };
    s.tabs[0][4] = (ses_tab){ raw, 0, 0 };
    s.tabs[0][5] = (ses_tab){ replacement, 0, 0 };
    s.ntabs[0] = 6;
    s.cur[0] = 1;
    s.tabs[1][0] = (ses_tab){ p3, 4, 0 };
    s.ntabs[1] = 1;
    s.pins[0] = p2;
    s.pins[1] = spaced; s.pins[2] = plain; s.pins[3] = raw; s.pins[4] = replacement;
    s.npins = 5;
    s.recent[0] = p3;
    s.recent[1] = p1;
    s.recent[2] = spaced; s.recent[3] = plain; s.recent[4] = raw; s.recent[5] = replacement;
    s.nrecent = 6;

    static char buf[8192];
    size_t n = session_format(&s, buf, sizeof buf);
    CHECK(n > 0 && n == strlen(buf));
    session_parse(buf, n, &r);
    CHECK(r.has_rect && r.rect.left == 10 && r.rect.bottom == 780 && r.maximized == 1);
    CHECK(r.tree == 0 && r.dual == 1 && r.hidden == 1 && r.active == 1 && r.tree_w == 300);
    CHECK(r.ratio > 0.619 && r.ratio < 0.621);
    CHECK(r.ntabs[0] == 6 && r.cur[0] == 1 && r.ntabs[1] == 1 && r.cur[1] == 0);
    CHECK(r.tabs[0][0].path && !wcscmp(r.tabs[0][0].path, p1) && r.tabs[0][0].sort == 3 && r.tabs[0][0].desc == 1);
    CHECK(r.tabs[0][1].path && !wcscmp(r.tabs[0][1].path, p2)); /* spaces and non-ASCII survive */
    CHECK(r.tabs[1][0].path && !wcscmp(r.tabs[1][0].path, p3) && r.tabs[1][0].sort == 4);
    CHECK(r.npins == 5 && !wcscmp(r.pins[0], p2));
    CHECK(r.nrecent == 6 && !wcscmp(r.recent[0], p3) && !wcscmp(r.recent[1], p1)); /* newest first */
    const wchar_t *exact[] = { spaced, plain, raw, replacement };
    for (int i = 0; i < 4; i++) {
        CHECK(!wcscmp(r.tabs[0][i + 2].path, exact[i]));
        CHECK(!wcscmp(r.pins[i + 1], exact[i]));
        CHECK(!wcscmp(r.recent[i + 2], exact[i]));
    }
    CHECK(wcscmp(r.tabs[0][2].path, r.tabs[0][3].path));
    CHECK(wcscmp(r.tabs[0][4].path, r.tabs[0][5].path));
    session_free(&r);
    const char *legacy = "[pane1]\ntab = name 0 C:\\trailing \n[pins]\npin = C:\\trailing \n"
                         "[recent]\nfolder = C:\\trailing \n";
    session_parse(legacy, strlen(legacy), &r);
    CHECK(r.ntabs[0] == 1 && !wcscmp(r.tabs[0][0].path, L"C:\\trailing "));
    CHECK(r.npins == 1 && !wcscmp(r.pins[0], L"C:\\trailing "));
    CHECK(r.nrecent == 1 && !wcscmp(r.recent[0], L"C:\\trailing "));
    session_free(&r);
    const char *invalid = "[pane1]\ntab16 = name 0 u16v1:0043zzzz\n"
                          "[pins]\npin16 = u16v1:0000\n[recent]\nfolder = bad\xff\n";
    session_parse(invalid, strlen(invalid), &r);
    CHECK(!r.ntabs[0] && !r.npins && !r.nrecent);
    session_free(&r);
    CHECK(r.ntabs[0] == 0 && r.npins == 0 && r.nrecent == 0);
    CHECK(session_format(&s, buf, 40) == 0 && buf[0] == 0); /* too small: nothing, not half */

    /* damaged or hostile input keeps defaults and stays in range */
    const char *bad = "garbage\n[window]\nrect = 1 2 3\ntree_width = 99999\nratio = 7\nactive = 5\n"
                      "[pane1]\ncurrent = 9\ntab = name\ntab = bogus 1 C:\\x\n[pane3]\ntab = name 0 C:\\y\n"
                      "[pins]\npin =\n";
    session_parse(bad, strlen(bad), &r);
    CHECK(!r.has_rect && r.tree_w == 1200 && r.ratio == 0.85 && r.active == 0);
    CHECK(r.ntabs[0] == 1 && r.cur[0] == 0 && r.tabs[0][0].sort == 0 && r.tabs[0][0].desc == 1);
    CHECK(r.ntabs[1] == 0 && r.npins == 0);
    session_free(&r);
    session_parse(NULL, 0, &r);
    CHECK(r.tree == 1 && r.dual == 1 && r.tree_w == 230 && r.ratio == 0.5);

    /* more tabs than fit are dropped, not overflowed */
    static char many[64 * 1024];
    size_t len = (size_t)sprintf(many, "[pane2]\n");
    for (int i = 0; i < SES_TABS_MAX + 10; i++) len += (size_t)sprintf(many + len, "tab = name 0 C:\\t%d\n", i);
    session_parse(many, len, &r);
    CHECK(r.ntabs[1] == SES_TABS_MAX);
    session_free(&r);
    len = (size_t)sprintf(many, "[recent]\nfolder =\n");
    for (int i = 0; i < SES_RECENT_MAX + 10; i++) len += (size_t)sprintf(many + len, "folder = C:\\r%d\n", i);
    session_parse(many, len, &r);
    CHECK(r.nrecent == SES_RECENT_MAX && !wcscmp(r.recent[0], L"C:\\r0"));
    session_free(&r);
}

static int fz(const wchar_t *pattern, const wchar_t *text) { return fuzzy_score(pattern, text, NULL); }

static void test_fuzzy(void)
{
    printf("fuzzy matching\n");
    CHECK(fz(L"", L"anything") == 0);
    CHECK(fz(L"   ", L"anything") == 0);
    CHECK(fz(L"xyz", L"abc") < 0);
    CHECK(fz(L"ba", L"ab") < 0);                 /* order matters */
    CHECK(fz(L"abcd", L"abc") < 0);
    CHECK(fz(L"DOC", L"Documents") > 0);         /* case does not */
    CHECK(fz(L"sort size", L"Sort by size") > 0); /* every term must match */
    CHECK(fz(L"sort zzz", L"Sort by size") < 0);
    CHECK(fz(L"size sort", L"Sort by size") > 0); /* in any order */

    /* what should rank first */
    CHECK(fz(L"np", L"New Project") > fz(L"np", L"snap pea"));           /* word starts */
    CHECK(fz(L"abc", L"abcxyz") > fz(L"abc", L"axbxcx"));                 /* runs */
    CHECK(fz(L"work", L"C:\\Work") > fz(L"work", L"C:\\Work\\openpilot")); /* in the name */
    CHECK(fz(L"notepad", L"notepad.exe") > fz(L"notepad", L"amd64_microsoft-windows-notepad.resources_31bf3856ad364e35"));
    CHECK(fz(L"dl", L"Downloads") > fz(L"dl", L"Modules"));
    CHECK(fz(L"tr", L"Reopen closed tab") < fz(L"tr", L"Folder tree")); /* 't' then 'r' */

    /* the best placement, not the first one: S of System, not of Windows */
    uint32_t hits[FUZZY_HITS_WORDS];
    CHECK(fuzzy_score(L"ws", L"Windows System", hits) > 0);
    CHECK(hits[0] == ((1u << 0) | (1u << 8)));
    CHECK(fuzzy_score(L"sys", L"C:\\Windows\\System32", hits) > 0);
    CHECK(hits[0] == ((1u << 11) | (1u << 12) | (1u << 13)));
    CHECK(fuzzy_score(L"zz", L"abc", hits) < 0 && hits[0] == 0);

    /* long texts are matched on their start and never overflow */
    static wchar_t longer[2000];
    for (int i = 0; i < 1999; i++) longer[i] = L'a' + (wchar_t)(i % 26);
    longer[1500] = L'#';
    CHECK(fz(L"abc", longer) > 0);
    CHECK(fz(L"#", longer) < 0); /* past FUZZY_TEXT_MAX */
    wchar_t pat[100];
    for (int i = 0; i < 99; i++) pat[i] = L'a';
    pat[99] = 0;
    CHECK(fz(pat, L"aaaa") < 0); /* a term longer than the text */
}

/* An Everything IPC v1 reply, built the way Everything lays it out. */
static size_t ev_reply(uint8_t *buf, size_t cap, const wchar_t *const *names, const wchar_t *const *paths,
                       const DWORD *flags, DWORD n, DWORD total)
{
    DWORD head[7] = { 0, 0, total, 0, 0, n, 0 };
    size_t off = sizeof head + n * 3 * sizeof(DWORD);
    memcpy(buf, head, sizeof head);
    for (DWORD i = 0; i < n; i++) {
        DWORD item[3] = { flags[i], 0, 0 };
        for (int s = 0; s < 2; s++) {
            const wchar_t *str = s ? paths[i] : names[i];
            size_t bytes = (wcslen(str) + 1) * sizeof(wchar_t);
            if (off + bytes > cap) return 0;
            item[1 + s] = (DWORD)off;
            memcpy(buf + off, str, bytes);
            off += bytes;
        }
        memcpy(buf + sizeof head + i * sizeof item, item, sizeof item);
    }
    return off;
}

static LRESULT CALLBACK ev_test_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_COPYDATA) {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)l;
        ev_list **out = (ev_list **)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (cds->dwData == 0x4F50AA01u && out && !*out) *out = ev_parse(cds->lpData, cds->cbData);
        return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

static void test_search_queries(void)
{
    puts("search query contracts and safe Everything scope");wchar_t out[4096];
    CHECK(!fpe_search_validate(L"report *.txt ext:txt;md file: size:>=10mb dm:2024-02-29"));
    CHECK(!fpe_search_validate(L"\"a:b\""));
    CHECK(fpe_search_validate(L"dm:2023-02-29")!=0);
    CHECK(fpe_search_validate(L"size:18446744073709551616")!=0);
    CHECK(fpe_search_validate(L"size:1xb")!=0);
    CHECK(fpe_search_validate(L"regex:.*")!=0);
    CHECK(fpe_search_validate(L"\"unterminated")!=0);
    CHECK(fpe_search_validate(L" ")!=0);
    CHECK(!fpe_search_index_query(L"OR *.pdf ext:pdf;txt file:",out,4096));
    CHECK(wcsstr(out,L"nowildcards:\"OR\"")&&wcsstr(out,L"wfn:wildcards:\"*.pdf\"")&&wcsstr(out,L"ext:\"pdf;txt\""));
    CHECK(fpe_search_index_query(L"size:>1mb",out,4096)==ERROR_NOT_SUPPORTED);
    CHECK(fpe_search_index_query(L"dm:2024-01-01",out,4096)==ERROR_NOT_SUPPORTED);
    CHECK(fpe_search_index_query(L"abc",out,2)==ERROR_BUFFER_OVERFLOW);
    CHECK(ev_folder_query(L"C:\\folder name\\",out,4096)&&!wcscmp(out,L"folder:path:wfn:\"C:\\folder name\""));
    CHECK(ev_folder_query(L"C:\\",out,4096)&&wcsstr(out,L"\"C:\\\""));
    CHECK(!ev_folder_query(L"C:\\wild*",out,4096));
    CHECK(!ev_folder_query(L"C:\\foo",out,3));
}

static void test_everything(void)
{
    printf("Everything replies\n");
    static uint8_t buf[4096];
    const wchar_t *names[] = { L"notes.txt", L"Fotos", L"C:" }, *paths[] = { L"D:\\docs", L"C:\\Users\\TestUser\\", L"" };
    const DWORD flags[] = { 0, 1, 2 };
    size_t len = ev_reply(buf, sizeof buf, names, paths, flags, 3, 1234);
    ev_list *l = ev_parse(buf, len);
    CHECK(l && l->n == 3 && l->total == 1234);
    if (l) {
        CHECK(!wcscmp(l->items[0].path, L"D:\\docs\\notes.txt") && !wcscmp(l->items[0].path + l->items[0].name, L"notes.txt"));
        CHECK(!l->items[0].folder && l->items[1].folder && l->items[2].folder);
        CHECK(!wcscmp(l->items[1].path, L"C:\\Users\\TestUser\\Fotos")); /* no doubled backslash */
        CHECK(!wcscmp(l->items[2].path, L"C:\\") && l->items[2].name == 0); /* a drive */
        free(l);
    }
    CHECK(ev_parse(buf, 0) == NULL);
    CHECK(ev_parse(NULL, 100) == NULL);
    CHECK(ev_parse(buf, 20) == NULL);       /* shorter than the header */
    CHECK(ev_parse(buf, len - 2) == NULL);  /* the last string loses its terminator */
    DWORD lie = 1000000;                    /* more items than bytes */
    memcpy(buf + 5 * sizeof(DWORD), &lie, sizeof lie);
    CHECK(ev_parse(buf, len) == NULL);
    len = ev_reply(buf, sizeof buf, names, paths, flags, 3, 3);
    DWORD wild = 0x7FFFFFF0;                /* an offset outside the reply */
    memcpy(buf + 7 * sizeof(DWORD) + sizeof(DWORD), &wild, sizeof wild);
    CHECK(ev_parse(buf, len) == NULL);
    len = ev_reply(buf, sizeof buf, names, paths, flags, 0, 0);
    l = ev_parse(buf, len);
    CHECK(l && l->n == 0);
    free(l);

    /* a real query, when Everything runs here */
    if (!ev_window()) {
        printf("  (Everything is not running: live query skipped)\n");
        CHECK(ev_query(NULL, 1, L"x", 1) == -1);
        return;
    }
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = ev_test_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"EvTest";
    RegisterClassW(&wc);
    HWND h = CreateWindowExW(0, L"EvTest", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    ev_list *got = NULL;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)&got);
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    wchar_t q[MAX_PATH + 32];
    swprintf(q, MAX_PATH + 32, L"\"%ls\\\" notepad.exe", sysdir);
    CHECK(ev_query(h, 0x4F50AA01u, q, 20) == 0);
    for (DWORD t0 = GetTickCount(); !got && GetTickCount() - t0 < 3000;) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageW(&m);
        if (!got) MsgWaitForMultipleObjects(0, NULL, FALSE, 20, QS_ALLINPUT);
    }
    CHECK(got != NULL);
    int found = 0;
    for (uint32_t i = 0; got && i < got->n; i++) {
        wchar_t want[MAX_PATH + 16];
        swprintf(want, MAX_PATH + 16, L"%ls\\notepad.exe", sysdir);
        found |= !_wcsicmp(got->items[i].path, want) && !got->items[i].folder;
    }
    CHECK(found);
    free(got);
    /* Exercise generated coverage syntax against real Everything, independently
     * of the owned routing fixture: spaced modifiers can silently return zero. */
    got=NULL;CHECK(ev_folder_query(sysdir,q,MAX_PATH+32));
    CHECK(ev_query(h,0x4F50AA01u,q,1)==0);
    for(DWORD t0=GetTickCount();!got&&GetTickCount()-t0<3000;){MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))DispatchMessageW(&m);if(!got)MsgWaitForMultipleObjects(0,NULL,FALSE,20,QS_ALLINPUT);}
    CHECK(got&&got->n==1&&got->items[0].folder&&fpe_path_equal(got->items[0].path,sysdir));free(got);got=NULL;
    wchar_t normalized[2304],scoped[MAX_PATH+2400];
    CHECK(!fpe_search_index_query(L"notepad.exe",normalized,2304));
    swprintf(scoped,MAX_PATH+2400,L"path:nowildcards:\"%ls\\\" %ls",sysdir,normalized);
    CHECK(ev_query(h,0x4F50AA01u,scoped,20)==0);
    for(DWORD t0=GetTickCount();!got&&GetTickCount()-t0<3000;){MSG m;while(PeekMessageW(&m,NULL,0,0,PM_REMOVE))DispatchMessageW(&m);if(!got)MsgWaitForMultipleObjects(0,NULL,FALSE,20,QS_ALLINPUT);}
    found=0;for(uint32_t i=0;got&&i<got->n;i++){wchar_t want[MAX_PATH+16];swprintf(want,MAX_PATH+16,L"%ls\\notepad.exe",sysdir);found|=!_wcsicmp(got->items[i].path,want)&&!got->items[i].folder;}
    CHECK(found);free(got);
    DestroyWindow(h);
}

/* ---- PDF text ------------------------------------------------------------------ */

/* Appends a line as PDFium reports it: characters cw points wide from x,
 * with a generated space between words and a generated "\r\n" after it. */
static uint32_t tx_line(pdfp_char *c, uint32_t n, const wchar_t *s, float x, float y, int last)
{
    const float cw = 6, h = 12;
    for (const wchar_t *p = s; *p; p++) {
        pdfp_char *k = &c[n++];
        memset(k, 0, sizeof *k);
        if (*p == L' ') {
            k->cp = ' ' | PDFP_CH_GEN;
            continue;
        }
        k->cp = *p;
        k->l = x + (float)(p - s) * cw;
        k->r = k->l + cw;
        k->t = y;
        k->b = y + h;
    }
    if (!last) {
        memset(&c[n], 0, 2 * sizeof *c);
        c[n++].cp = '\r' | PDFP_CH_GEN;
        c[n++].cp = '\n' | PDFP_CH_GEN;
    }
    return n;
}

/* head, then filler words, then tail: exactly len characters */
static const wchar_t *tx_fill(wchar_t *out, int len, const wchar_t *head, const wchar_t *tail)
{
    int n = swprintf(out, 128, L"%ls ", head), tl = (int)wcslen(tail);
    for (; n < len - tl; n++) out[n] = (n % 6 == 5 && n < len - tl - 1) ? L' ' : L'o';
    if (out[n - 1] == L' ') out[n - 1] = L'o';
    wcscpy(out + n, tail);
    return out;
}

static wchar_t *tx_build(const pdfp_char *c, uint32_t n, int layout)
{
    pdftext t;
    pdftext_init(&t, layout);
    pdftext_add(&t, 0, 792, c, 0, n);
    return pdftext_finish(&t, NULL);
}

static int count_breaks(const wchar_t *s)
{
    int k = 0;
    for (const wchar_t *p = s; p && (p = wcsstr(p, L"\r\n")); p += 2) k++;
    return k;
}

static void test_pdftext(void)
{
    printf("PDF text: copy shapes\n");
    static pdfp_char c[4096];
    wchar_t a[128], b[128], d[128];
    uint32_t n = 0;
    float y = 100, lh = 14; /* 12-pt cells, 2 points apart */

    /* a paragraph whose first line ends in a split word, its short last line, then the next paragraph */
    n = tx_line(c, n, tx_fill(a, 70, L"Alpha", L"exam-"), 72, y, 0);
    n = tx_line(c, n, tx_fill(b, 70, L"ple", L"end"), 72, y += lh, 0);
    n = tx_line(c, n, L"short last line.", 72, y += lh, 0);
    n = tx_line(c, n, tx_fill(d, 70, L"Next", L"x"), 72, y += lh, 0);
    n = tx_line(c, n, L"fin.", 72, y += lh, 1);
    wchar_t *s = tx_build(c, n, 0);
    CHECK(s && wcsstr(s, L"example") && !wcsstr(s, L"exam-"));
    CHECK(s && wcsstr(s, L"end short last line.\r\nNext") && count_breaks(s) == 1);
    CHECK(s && !wcsncmp(s, L"Alpha ", 6) && wcslen(s) > 4 && !wcscmp(s + wcslen(s) - 4, L"fin."));
    free(s);
    s = tx_build(c, n, 1);
    CHECK(s && wcsstr(s, L"exam-\r\nple") && count_breaks(s) == 4);
    free(s);

    /* extra space between lines starts a paragraph; a bullet does too */
    n = 0, y = 100;
    n = tx_line(c, n, tx_fill(a, 70, L"One", L"x"), 72, y, 0);
    n = tx_line(c, n, tx_fill(b, 70, L"Two", L"y"), 72, y += lh + 12, 0);
    n = tx_line(c, n, L"\x2022 item", 72, y += lh, 1);
    s = tx_build(c, n, 0);
    CHECK(s && count_breaks(s) == 2 && wcsstr(s, L"x\r\nTwo") && wcsstr(s, L"y\r\n\x2022 item"));
    free(s);

    /* a hyphen before a capital or a digit is part of the word; ligatures are spelled out */
    n = 0, y = 100;
    n = tx_line(c, n, tx_fill(a, 70, L"Cases", L"COVID-"), 72, y, 0);
    n = tx_line(c, n, L"19 \xFB01le", 72, y += lh, 1);
    s = tx_build(c, n, 0);
    CHECK(s && wcsstr(s, L"COVID-19 file"));
    free(s);

    /* columns: the left column's last full line continues at the top of the right one */
    n = 0;
    for (int i = 0; i < 3; i++) n = tx_line(c, n, tx_fill(a, 38, i ? L"left" : L"Top", L"l"), 72, 100 + lh * i, 0);
    for (int i = 0; i < 3; i++) n = tx_line(c, n, tx_fill(a, 38, L"right", L"r"), 312, 100 + lh * i, i == 2);
    s = tx_build(c, n, 0);
    CHECK(s && count_breaks(s) == 0 && wcsstr(s, L"l right"));
    free(s);

    /* pages: a full last line runs on into the next page; a short one ends its paragraph */
    pdfp_char c2[512];
    uint32_t n2 = 0;
    n = 0;
    n = tx_line(c, n, tx_fill(a, 70, L"Before", L"x"), 72, 700, 0);
    n = tx_line(c, n, tx_fill(b, 70, L"page", L"end"), 72, 714, 1);
    n2 = tx_line(c2, n2, tx_fill(d, 70, L"after", L"y"), 72, 72, 0);
    n2 = tx_line(c2, n2, L"done.", 72, 86, 1);
    pdftext t;
    pdftext_init(&t, 0);
    pdftext_add(&t, 0, 792, c, 0, n);
    pdftext_add(&t, 1, 792, c2, 0, n2);
    s = pdftext_finish(&t, NULL);
    CHECK(s && count_breaks(s) == 0 && wcsstr(s, L"end after"));
    free(s);
    pdftext_init(&t, 0);
    pdftext_add(&t, 0, 792, c2, 0, n2);
    pdftext_add(&t, 1, 792, c, 0, n);
    s = pdftext_finish(&t, NULL);
    CHECK(s && wcsstr(s, L"done.\r\nBefore"));
    free(s);

    /* a range from the middle of a line; a soft hyphen always goes */
    n = tx_line(c, 0, L"select from here", 72, 100, 1);
    pdftext_init(&t, 0);
    pdftext_add(&t, 0, 792, c, 7, 11);
    s = pdftext_finish(&t, NULL);
    CHECK(s && !wcscmp(s, L"from"));
    free(s);
    n = 0;
    n = tx_line(c, n, tx_fill(a, 70, L"Soft", L"Hyph\xAD"), 72, 100, 0);
    n = tx_line(c, n, L"Enation", 72, 114, 1);
    s = tx_build(c, n, 0);
    CHECK(s && wcsstr(s, L"HyphEnation"));
    free(s);

    /* PDFium joins a split word's lines itself and marks the hyphen: still one word */
    n = tx_line(c, 0, tx_fill(a, 70, L"Joined", L"rela-"), 72, 100, 1);
    c[n - 1].cp |= PDFP_CH_HYPHEN;
    n = tx_line(c, n, L"tively short.", 72, 114, 1);
    s = tx_build(c, n, 0);
    CHECK(s && wcsstr(s, L"relatively short."));
    free(s);
    s = tx_build(c, n, 1);
    CHECK(s && wcsstr(s, L"rela-\r\ntively"));
    free(s);

    /* page furniture between pages: a page number at the bottom, a rotated stamp in the margin */
    n = 0;
    n = tx_line(c, n, tx_fill(a, 70, L"Body", L"runs"), 72, 690, 0);
    n = tx_line(c, n, L"12", 300, 750, 0);
    uint32_t stamp = n;
    n = tx_line(c, n, L"arXiv:1234", 20, 200, 1);
    for (uint32_t i = stamp; i < n; i++) { /* stacked upwards, 6 points each */
        c[i].l = 20, c[i].r = 32;
        c[i].t = 400 - (float)(i - stamp) * 6, c[i].b = c[i].t + 6;
    }
    n2 = tx_line(c2, 0, L"on here.", 72, 72, 1);
    pdftext_init(&t, 0);
    pdftext_add(&t, 0, 792, c, 0, n);
    pdftext_add(&t, 1, 792, c2, 0, n2);
    s = pdftext_finish(&t, NULL);
    CHECK(s && wcsstr(s, L"runs on here.") && !wcsstr(s, L"12") && !wcsstr(s, L"arXiv"));
    free(s);
    pdftext_init(&t, 1);
    pdftext_add(&t, 0, 792, c, 0, n);
    s = pdftext_finish(&t, NULL);
    CHECK(s && wcsstr(s, L"\r\n12\r\n") && wcsstr(s, L"arXiv")); /* layout keeps everything */
    free(s);
    /* a page number alone is what was asked for; a short word at the bottom is text */
    n = tx_line(c, 0, L"12", 300, 750, 1);
    s = tx_build(c, n, 0);
    CHECK(s && !wcscmp(s, L"12"));
    free(s);
    n = 0;
    n = tx_line(c, n, tx_fill(a, 70, L"Words", L"x"), 72, 690, 0);
    n = tx_line(c, n, L"mid", 72, 750, 1);
    s = tx_build(c, n, 0);
    CHECK(s && wcsstr(s, L"mid"));
    free(s);

    /* nothing selected: an empty string, not NULL */
    pdftext_init(&t, 0);
    size_t len = 7;
    s = pdftext_finish(&t, &len);
    CHECK(s && len == 0 && !s[0]);
    free(s);
}

/* ---- PDF positions ------------------------------------------------------------- */

static int file_has(const wchar_t *file, const char *needle)
{
    static char buf[8192];
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD got = 0;
    ReadFile(f, buf, sizeof buf - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    return strstr(buf, needle) != NULL;
}

static void file_put(const wchar_t *file, const char *text)
{
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD w;
    WriteFile(f, text, (DWORD)strlen(text), &w, NULL);
    CloseHandle(f);
}

static void test_pdfpos(void)
{
    printf("PDF positions\n");
    wchar_t file[MAX_PATH], dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    swprintf(file, MAX_PATH, L"%lsfp_app_tests_pos_%lu.txt", dir, GetCurrentProcessId());
    DeleteFileW(file);

    /* nothing yet; set two, save, read back in a new run */
    pdfpos_open(file, 3);
    CHECK(pdfpos_get(L"C:\\a.pdf", 10, 42) == NULL && pdfpos_count() == 0);
    pdfpos_set(L"C:\\a.pdf", 10, 42, 4, 120.5f, 0, 0);
    pdfpos_set(L"C:\\B\xFC" L"cher\\\x65E5\x672C.pdf", 20, 43, 7, 33.0f, 1.5f, 40.0f);
    pdfpos_close();
    CHECK(GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES);
    pdfpos_open(file, 3);
    const pdfpos *e = pdfpos_get(L"c:\\A.PDF", 10, 42); /* Windows paths compare without case */
    CHECK(e && e->page == 4 && e->y > 120.4f && e->y < 120.6f && e->zoom == 0);
    e = pdfpos_get(L"C:\\B\xFC" L"cher\\\x65E5\x672C.pdf", 20, 43);
    CHECK(e && e->page == 7 && e->zoom > 1.49f && e->zoom < 1.51f && e->x > 39.9f);
    /* a file that changed size starts over, but is still known */
    CHECK(pdfpos_get(L"C:\\a.pdf", 11, 42) == NULL && pdfpos_has(L"C:\\a.pdf"));
    CHECK(pdfpos_get(L"C:\\a.pdf", 10, 44) == NULL); /* equal-size replacement */

    /* newest first, at most max: the oldest goes */
    pdfpos_set(L"C:\\c.pdf", 1, 0, 1, 0, 0, 0);
    pdfpos_set(L"C:\\d.pdf", 1, 0, 2, 0, 0, 0);
    CHECK(pdfpos_count() == 3 && !pdfpos_has(L"C:\\a.pdf") && pdfpos_has(L"C:\\d.pdf"));
    pdfpos_close();

    pdfpos_open(file, 10);
    pdfpos_set(L"\\\\wsl.localhost\\Ubuntu\\tmp\\A.pdf", 10, 55, 1, 10, 0, 0);
    pdfpos_set(L"\\\\wsl.localhost\\Ubuntu\\tmp\\a.pdf", 10, 55, 2, 20, 0, 0);
    pdfpos_close();
    pdfpos_open(file, 10);
    const pdfpos *upper = pdfpos_get(L"\\\\wsl.localhost\\Ubuntu\\tmp\\A.pdf", 10, 55);
    const pdfpos *lower = pdfpos_get(L"\\\\wsl.localhost\\ubuntu\\tmp\\a.pdf", 10, 55);
    CHECK(upper && lower && upper->page == 1 && lower->page == 2); /* also survives a restart */
    pdfpos_close();

    /* another window wrote meanwhile: its documents stay, ours go on top */
    pdfpos_open(file, 10);
    pdfpos_set(L"C:\\mine.pdf", 5, 0, 9, 1, 0, 0);
    file_put(file, "# x\r\n5\t1\t2.0\t0.000\t0.0\tC:\\other.pdf\r\n7\t3\t0.0\t0.000\t0.0\tC:\\mine.pdf\r\n");
    CHECK(pdfpos_save() == 0);
    CHECK(file_has(file, "\tC:\\mine.pdf\r\n5\t0\t1\t2.0") && file_has(file, "C:\\other.pdf"));
    CHECK(!file_has(file, "7\t0\t3\t")); /* our newer position replaced the other window's */
    pdfpos_close();

    /* damaged lines are dropped, the rest is kept */
    file_put(file, "garbage\r\n1\t2\r\n3\t4\t5\t6\t7\t\r\n9\t1\t-4\t0\t0\tC:\\neg.pdf\r\n"
                   "8\t2\t10.0\t0.000\t0.0\tC:\\ok.pdf\r\n\xFF\xFE\r\n");
    pdfpos_open(file, 10);
    CHECK(pdfpos_count() == 1 && pdfpos_get(L"C:\\ok.pdf", 8, 0) != NULL);
    pdfpos_close();

    /* remembering nothing writes nothing; nothing changed writes nothing */
    DeleteFileW(file);
    pdfpos_open(file, 0);
    pdfpos_set(L"C:\\a.pdf", 1, 0, 1, 1, 0, 0);
    pdfpos_close();
    CHECK(GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES);
    pdfpos_open(file, 10);
    CHECK(pdfpos_get(L"C:\\a.pdf", 1, 0) == NULL);
    pdfpos_close();
    CHECK(GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES);
}

/* ---- PDF helper ------------------------------------------------------------------ */

/* Three Letter pages of 10-pt Courier (6 pt a character): 40 lines from
 * x = 72, two of them 78 characters long, so the body text spans exactly
 * 72..540 pt. Margin notes and a page number are outliers the fit must
 * ignore. */
static int write_test_pdf(const wchar_t *path)
{
    enum { PAGES = 3 };
    static char buf[200000], content[8000];
    size_t off[3 + 2 * PAGES], n = 0;
    int nobj = 0;
#define PUT(...) (n += (size_t)snprintf(buf + n, sizeof buf - n, __VA_ARGS__))
    PUT("%%PDF-1.4\n");
    off[nobj++] = n;
    PUT("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[nobj++] = n;
    PUT("2 0 obj\n<< /Type /Pages /Kids [4 0 R 6 0 R 8 0 R] /Count %d >>\nendobj\n", PAGES);
    off[nobj++] = n;
    PUT("3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>\nendobj\n");
    for (int p = 0; p < PAGES; p++) {
        size_t c = 0;
        for (int line = 0; line < 40; line++) {
            char text[80];
            int len = line % 20 == 7 ? 78 : 50; /* ragged, like code: few lines are full */
            for (int i = 0; i < len; i++) text[i] = (char)('a' + (i * 7 + line + p) % 26);
            text[len] = 0;
            /* page 3: a word split across lines 0 and 1, for find */
            if (p == 2 && line == 0) memcpy(text + len - 6, " rela-", 6);
            if (p == 2 && line == 1) memcpy(text, "tively ", 7);
            c += (size_t)snprintf(content + c, sizeof content - c, "BT /F1 10 Tf 72 %d Td (%s) Tj ET\n", 720 - line * 15, text);
        }
        /* notes in both margins and a page number: not body text */
        c += (size_t)snprintf(content + c, sizeof content - c,
                              "BT /F1 8 Tf 556 600 Td (note%d) Tj ET\nBT /F1 8 Tf 12 500 Td (side) Tj ET\n"
                              "BT /F1 9 Tf 580 40 Td (%d) Tj ET\n", p, p + 1);
        /* page 3: a 2x2 picture (red, blue / green, white) at x 400..500, 392..492 from the top */
        if (p == 2)
            c += (size_t)snprintf(content + c, sizeof content - c,
                                  "q 100 0 0 100 400 300 cm BI /W 2 /H 2 /CS /RGB /BPC 8 /F /AHx ID "
                                  "FF00000000FF00FF00FFFFFF> EI Q\n");
        off[nobj++] = n;
        PUT("%d 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> "
            "/Contents %d 0 R >>\nendobj\n", 4 + 2 * p, 5 + 2 * p);
        off[nobj++] = n;
        PUT("%d 0 obj\n<< /Length %lu >>\nstream\n%s\nendstream\nendobj\n", 5 + 2 * p, (unsigned long)c, content);
    }
    size_t xref = n;
    PUT("xref\n0 %d\n0000000000 65535 f \n", nobj + 1);
    for (int i = 0; i < nobj; i++) PUT("%010lu 00000 n \n", (unsigned long)off[i]);
    PUT("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%lu\n%%%%EOF\n", nobj + 1, (unsigned long)xref);
#undef PUT
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    DWORD w;
    BOOL ok = WriteFile(f, buf, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(f);
    return ok ? 0 : -1;
}

static int pipe_read(HANDLE h, void *buf, DWORD n)
{
    for (uint8_t *p = buf; n;) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, NULL) || !got) return -1;
        p += got;
        n -= got;
    }
    return 0;
}

/* Reads one reply into buf (up to cap payload bytes); -1 on a broken pipe or a bigger reply. */
static int helper_reply(HANDLE out, pdfp_head *h, void *buf, uint32_t cap)
{
    if (pipe_read(out, h, sizeof *h) || h->size > cap) return -1;
    return h->size ? pipe_read(out, buf, h->size) : 0;
}

static void helper_send(HANDLE in, uint32_t type, uint32_t doc, uint32_t ticket, const void *p, uint32_t n)
{
    pdfp_head h = { type, n, doc, ticket };
    DWORD w;
    WriteFile(in, &h, sizeof h, &w, NULL);
    if (n) WriteFile(in, p, n, &w, NULL);
}

/* One Letter page that is a white picture with a little text on it, as a scanner makes. */
static int write_scan_pdf(const wchar_t *path)
{
    static const char content[] = "q 612 0 0 792 0 0 cm BI /W 1 /H 1 /CS /RGB /BPC 8 /F /AHx ID FFFFFF> EI Q\n"
                                  "BT /F1 10 Tf 72 700 Td (scanned words) Tj ET\n";
    char buf[2048];
    size_t off[5], n = 0;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "%%PDF-1.4\n");
    off[0] = n;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj\n");
    off[1] = n;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "2 0 obj\n<< /Type /Pages /Kids [4 0 R] /Count 1 >>\nendobj\n");
    off[2] = n;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "3 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Courier >>\nendobj\n");
    off[3] = n;
    n += (size_t)snprintf(buf + n, sizeof buf - n,
                          "4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> "
                          "/Contents 5 0 R >>\nendobj\n");
    off[4] = n;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "5 0 obj\n<< /Length %lu >>\nstream\n%s\nendstream\nendobj\n",
                          (unsigned long)(sizeof content - 1), content);
    size_t xref = n;
    n += (size_t)snprintf(buf + n, sizeof buf - n, "xref\n0 6\n0000000000 65535 f \n");
    for (int i = 0; i < 5; i++) n += (size_t)snprintf(buf + n, sizeof buf - n, "%010lu 00000 n \n", (unsigned long)off[i]);
    n += (size_t)snprintf(buf + n, sizeof buf - n, "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%lu\n%%%%EOF\n",
                          (unsigned long)xref);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    DWORD w;
    BOOL ok = WriteFile(f, buf, (DWORD)n, &w, NULL) && w == n;
    CloseHandle(f);
    return ok ? 0 : -1;
}

/* Renders a w x h tile of page `page` at 1 px/pt from (x0, y0) into slot 6; 1 = drawn. */
static int tile(HANDLE in, HANDLE out, uint32_t doc, uint32_t page, uint32_t colors, int x0, int y0, uint32_t w,
                uint32_t h, uint32_t *dark)
{
    static uint32_t ticket = 1000;
    pdfp_render r = { page, 6, 1, 1.0f, x0, y0, w, h, colors };
    helper_send(in, PDFP_RENDER, doc, ++ticket, &r, sizeof r);
    pdfp_head hd;
    pdfp_rendered done = { 0, -1, 0 };
    if (helper_reply(out, &hd, &done, sizeof done) || hd.type != PDFP_RENDERED || hd.ticket != ticket) return 0;
    *dark = done.dark;
    return done.err == 0 && done.slot == 6;
}

static void test_pdf_helper(void)
{
    wchar_t exe[MAX_PATH], helper[MAX_PATH], dir[MAX_PATH], pdf[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy(helper, exe);
    wcscpy(wcsrchr(helper, L'\\') + 1, L"folderwing-pdf.exe");
    if (GetFileAttributesW(helper) == INVALID_FILE_ATTRIBUTES) {
        printf("PDF helper: skipped (no folderwing-pdf.exe; run tools/get-pdfium.ps1)\n");
        return;
    }
    printf("PDF helper: protocol and fit\n");
    GetTempPathW(MAX_PATH, dir);
    swprintf(pdf, MAX_PATH, L"%lsfp_app_tests_%lu.pdf", dir, GetCurrentProcessId());
    CHECK(write_test_pdf(pdf) == 0);

    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE | SEC_COMMIT, 0,
                                        PDFP_SLOTS * PDFP_SLOT_BYTES, NULL);
    uint8_t *slots = section ? MapViewOfFile(section, FILE_MAP_READ, 0, 0, 0) : NULL;
    HANDLE in_r, in_w, out_r, out_w;
    CreatePipe(&in_r, &in_w, &sa, 0);
    CreatePipe(&out_r, &out_w, &sa, 0);
    SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    wchar_t cmd[MAX_PATH + 40];
    swprintf(cmd, MAX_PATH + 40, L"\"%ls\" %llu", helper, (unsigned long long)(uintptr_t)section);
    PROCESS_INFORMATION pi = { 0 };
    BOOL started = slots && CreateProcessW(helper, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CHECK(started);
    CloseHandle(in_r);
    CloseHandle(out_w);
    if (started) {
        static uint8_t reply[sizeof(pdfp_opened) + 16 * sizeof(pdfp_page)];
        pdfp_head h;
        helper_send(in_w, PDFP_OPEN, 7, 0, pdf, (uint32_t)(wcslen(pdf) + 1) * 2);
        CHECK(helper_reply(out_r, &h, reply, sizeof reply) == 0);
        pdfp_opened o;
        memcpy(&o, reply, sizeof o);
        const pdfp_page *pages = (const pdfp_page *)(reply + sizeof o);
        CHECK(h.type == PDFP_OPENED && h.doc == 7 && o.err == 0 && o.npages == 3);
        CHECK(h.size == sizeof o + 3 * sizeof(pdfp_page) && pages[1].w == 612 && pages[1].h == 792);
        CHECK(o.fit_kind == PDFP_FIT_TEXT && o.fit_page == 1);
        /* the longest lines, not the notes at x = 12 and 556 or the page number */
        CHECK(o.fit_x0 > 70 && o.fit_x0 < 73 && o.fit_x1 > 538 && o.fit_x1 < 544);
        CHECK(o.fit_y0 > 60 && o.fit_y0 < 80 && o.fit_y1 > 650 && o.fit_y1 < 700);

        /* a tile at 1 px/pt holding the top-left of the text: ink in slot 5 */
        pdfp_render r = { 0, 5, 1, 1.0f, 64, 60, 128, 64 };
        helper_send(in_w, PDFP_RENDER, 7, 42, &r, sizeof r);
        pdfp_rendered done = { 0, -1 };
        CHECK(helper_reply(out_r, &h, &done, sizeof done) == 0);
        CHECK(h.type == PDFP_RENDERED && h.ticket == 42 && done.slot == 5 && done.err == 0);
        uint32_t dark = 0;
        const uint32_t *px = (const uint32_t *)(slots + 5 * (size_t)PDFP_SLOT_BYTES);
        for (uint32_t i = 0; i < 128 * 64; i++) dark += (px[i] & 0xFF) < 128;
        CHECK(dark > 100 && dark < 128 * 64 / 2);

        /* page text: in order, with cells where the page draws them and PDFium's line breaks */
        static uint8_t tbuf[sizeof(pdfp_text) + 8000 * sizeof(pdfp_char)];
        uint32_t page = 1;
        helper_send(in_w, PDFP_TEXT, 7, 0, &page, sizeof page);
        CHECK(helper_reply(out_r, &h, tbuf, sizeof tbuf) == 0 && h.type == PDFP_TEXTED && h.doc == 7);
        pdfp_text th;
        memcpy(&th, tbuf, sizeof th);
        const pdfp_char *pc = (const pdfp_char *)(tbuf + sizeof th);
        CHECK(th.page == 1 && th.n > 40 * 50 && h.size == sizeof th + th.n * sizeof(pdfp_char));
        /* line 0 of page 1 starts with 'b' at x = 72, on the baseline 72 pt from the top */
        CHECK((pc[0].cp & PDFP_CH_CODE) == 'b' && !(pc[0].cp & PDFP_CH_GEN));
        CHECK(pc[0].l > 71.5f && pc[0].l < 72.5f && pc[0].r - pc[0].l > 5 && pc[0].r - pc[0].l < 7.5f);
        CHECK(pc[0].t < 72 && pc[0].t > 60 && pc[0].b > 72 && pc[0].b < 78);
        uint32_t gen_breaks = 0;
        for (uint32_t i = 0; i < th.n; i++) gen_breaks += pc[i].cp == ('\n' | PDFP_CH_GEN);
        CHECK(gen_breaks >= 39);
        pdftext tt;
        pdftext_init(&tt, 1);
        pdftext_add(&tt, 1, 792, pc, 0, th.n);
        wchar_t *lay = pdftext_finish(&tt, NULL);
        CHECK(lay && !wcsncmp(lay, L"bipwdkry", 8) && wcsstr(lay, L"\r\n"));
        free(lay);
        page = 9; /* no such page: no characters */
        helper_send(in_w, PDFP_TEXT, 7, 0, &page, sizeof page);
        CHECK(helper_reply(out_r, &h, tbuf, sizeof tbuf) == 0 && h.size == sizeof th);

        /* find: one reply per page from `from` around, then the end with the totals */
        {
            static uint8_t fbuf[sizeof(pdfp_found) + PDFP_FIND_PAGE_MAX * sizeof(pdfp_hit)];
            uint8_t req[sizeof(pdfp_find) + 64];
            const wchar_t *queries[] = { L"BIPWDKRY", L"BIPWDKRY", L"relatively", L"zzzzqqq" };
            const uint32_t flags[] = { 0, PDFP_FIND_CASE, 0, 0 };
            for (int qi = 0; qi < 4; qi++) {
                pdfp_find f = { 10 + (uint32_t)qi, flags[qi], 1 };
                size_t ql = (wcslen(queries[qi]) + 1) * sizeof(wchar_t);
                memcpy(req, &f, sizeof f);
                memcpy(req + sizeof f, queries[qi], ql);
                helper_send(in_w, PDFP_FIND, 7, 0, req, (uint32_t)(sizeof f + ql));
                uint32_t pages_seen = 0, hits = 0, first_page = 99, done = 0, ok = 1;
                pdfp_hit first = { 0 };
                while (!done && helper_reply(out_r, &h, fbuf, sizeof fbuf) == 0) {
                    pdfp_found fd;
                    memcpy(&fd, fbuf, sizeof fd);
                    ok &= fd.gen == f.gen && h.doc == 7;
                    if (h.type == PDFP_FIND_DONE) {
                        done = 1;
                        ok &= fd.page == 3 && fd.n == hits;
                    } else if (h.type == PDFP_FOUND) {
                        if (!pages_seen) first_page = fd.page;
                        pages_seen++;
                        ok &= h.size == sizeof fd + fd.n * sizeof(pdfp_hit);
                        if (fd.n && !hits) memcpy(&first, fbuf + sizeof fd, sizeof first);
                        hits += fd.n;
                    }
                }
                CHECK(done && ok && pages_seen == 3 && first_page == 1);
                if (qi == 0) { /* case-insensitive: page 1, line 0, from character 0 */
                    CHECK(hits >= 1 && first.index == 0 && first.count == 8);
                    CHECK(first.l > 71.5f && first.l < 72.5f && first.r > 119 && first.r < 121);
                }
                if (qi == 1) CHECK(hits == 0);
                if (qi == 2) { /* across the line break: one hit, its box spanning both lines */
                    CHECK(hits == 1 && first.count >= 11 && first.b - first.t > 20);
                }
                if (qi == 3) CHECK(hits == 0);
            }
            /* a new search replaces the running one: the old one never reports its end */
            pdfp_find a = { 20, 0, 0 }, b = { 21, 0, 0 };
            memcpy(req, &a, sizeof a);
            memcpy(req + sizeof a, L"e", 4);
            helper_send(in_w, PDFP_FIND, 7, 0, req, sizeof a + 4);
            memcpy(req, &b, sizeof b);
            memcpy(req + sizeof b, L"q", 4);
            helper_send(in_w, PDFP_FIND, 7, 0, req, sizeof b + 4);
            int old_done = 0, new_done = 0;
            while (!new_done && helper_reply(out_r, &h, fbuf, sizeof fbuf) == 0) {
                pdfp_found fd;
                memcpy(&fd, fbuf, sizeof fd);
                if (h.type == PDFP_FIND_DONE) {
                    if (fd.gen == 20) old_done = 1;
                    else new_done = fd.gen == 21;
                }
            }
            CHECK(new_done && !old_done);
        }

        /* colors: tile around page 3's picture at 1 px/pt, from (380, 372) */
        {
            const uint32_t *t = (const uint32_t *)(slots + 6 * (size_t)PDFP_SLOT_BYTES);
            uint32_t dark = 9;
#define TILE(colors) tile(in_w, out_r, 7, 2, colors, 380, 372, 140, 140, &dark)
#define PX(x, y) (t[(y - 372) * 140 + (x - 380)] & 0xFFFFFF)
            CHECK(TILE(PDFP_COLORS_ORIGINAL) && dark == 0);
            CHECK(PX(385, 378) == 0xFFFFFF && PX(470, 470) == 0xFFFFFF && PX(420, 410) == 0xFF0000);
            /* mixed: the paper goes dark, the picture stays as it is */
            CHECK(TILE(PDFP_COLORS_MIXED) && dark == 1);
            CHECK(PX(385, 378) == (PDFP_DARK_LO * 0x010101u) && PX(470, 470) == 0xFFFFFF && PX(420, 410) == 0xFF0000);
            /* dark: the picture too, hues kept: white to the dark paper, blue stays blue */
            CHECK(TILE(PDFP_COLORS_DARK) && dark == 1);
            uint32_t blue = PX(470, 410);
            CHECK(PX(470, 470) == (PDFP_DARK_LO * 0x010101u) && (blue & 0xFF) > 200 && (blue >> 16 & 0xFF) < 60);
            /* pure red is already at middle lightness: still red, in the narrowed range */
            CHECK(PX(420, 410) == ((uint32_t)PDFP_DARK_HI << 16 | PDFP_DARK_LO << 8 | PDFP_DARK_LO));
#undef TILE
#undef PX
        }

        /* stale documents get an error, not a render */
        helper_send(in_w, PDFP_RENDER, 6, 43, &r, sizeof r);
        CHECK(helper_reply(out_r, &h, &done, sizeof done) == 0 && h.ticket == 43 && done.err != 0);

        /* a missing file is an error reply, and the helper carries on */
        wchar_t missing[MAX_PATH];
        swprintf(missing, MAX_PATH, L"%lsfp_app_tests_missing.pdf", dir);
        helper_send(in_w, PDFP_OPEN, 8, 0, missing, (uint32_t)(wcslen(missing) + 1) * 2);
        CHECK(helper_reply(out_r, &h, reply, sizeof reply) == 0 && h.doc == 8);
        memcpy(&o, reply, sizeof o);
        CHECK(o.err != 0 && o.npages == 0);

        /* a scanned page (one picture over all of it, text on top): mixed keeps it as it is */
        wchar_t scan[MAX_PATH];
        swprintf(scan, MAX_PATH, L"%lsfp_app_tests_scan_%lu.pdf", dir, GetCurrentProcessId());
        CHECK(write_scan_pdf(scan) == 0);
        helper_send(in_w, PDFP_OPEN, 9, 0, scan, (uint32_t)(wcslen(scan) + 1) * 2);
        CHECK(helper_reply(out_r, &h, reply, sizeof reply) == 0 && h.doc == 9);
        memcpy(&o, reply, sizeof o);
        CHECK(o.err == 0 && o.npages == 1);
        {
            const uint32_t *t = (const uint32_t *)(slots + 6 * (size_t)PDFP_SLOT_BYTES);
            uint32_t dark = 9;
            CHECK(tile(in_w, out_r, 9, 0, PDFP_COLORS_MIXED, 400, 500, 50, 50, &dark) && dark == 0 &&
                  (t[0] & 0xFFFFFF) == 0xFFFFFF);
            CHECK(tile(in_w, out_r, 9, 0, PDFP_COLORS_DARK, 400, 500, 50, 50, &dark) && dark == 1 &&
                  (t[0] & 0xFFFFFF) == PDFP_DARK_LO * 0x010101u);
        }
        DeleteFileW(scan);

        /* Thumbnail opens never enumerate remaining pages or run text/ink fit. */
        helper_send(in_w,PDFP_THUMB_OPEN,10,0,pdf,(uint32_t)(wcslen(pdf)+1)*2);
        CHECK(helper_reply(out_r,&h,reply,sizeof reply)==0);memcpy(&o,reply,sizeof o);
        CHECK(h.type==PDFP_OPENED&&h.doc==10&&o.err==0&&o.npages==1&&o.fit_kind==PDFP_FIT_PAGE);
        CHECK(h.size==sizeof o+sizeof(pdfp_page));
        pdfp_render thumbnail={.page=0,.slot=0,.epoch=2,.scale=0.2f,.w=123,.h=159,.colors=PDFP_COLORS_ORIGINAL};
        helper_send(in_w,PDFP_RENDER,10,99,&thumbnail,sizeof thumbnail);pdfp_rendered rendered;
        CHECK(helper_reply(out_r,&h,&rendered,sizeof rendered)==0);CHECK(rendered.err==0&&rendered.slot==0&&h.ticket==99);
        CHECK(slots[0]>240&&slots[1]>240&&slots[2]>240);
        helper_send(in_w,PDFP_CLOSE,10,0,NULL,0);
        helper_send(in_w, PDFP_QUIT, 0, 0, NULL, 0);
        CHECK(WaitForSingleObject(pi.hProcess, 5000) == WAIT_OBJECT_0);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CHECK(code == 0);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(in_w);
    CloseHandle(out_r);
    if (slots) UnmapViewOfFile(slots);
    if (section) CloseHandle(section);
    DeleteFileW(pdf);
}

/* ---- families: functions the settings make ---------------------------------- */

static char t_self[64];
static char t_cmd[64];
static int t_member_runs;
static int t_configured;
static void run_member(fn_ctx *c)
{
    snprintf(t_self, sizeof t_self, "%s", fn_self() ? fn_self()->id : "?");
    snprintf(t_cmd, sizeof t_cmd, "%s", fn_self() ? fn_option(fn_self(), "cmd") : "?");
    t_member_runs++;
}
static void configure_member(void)
{
    t_configured++;
    if (!fn_option(fn_self(), "cmd")[0]) fn_config_error(fn_self(), "[%s] needs cmd", fn_self()->id);
}
static int member_applies(const fn_ctx *c) { return strstr(fn_option(fn_self(), "on"), "files") != NULL; }

static const fn_opt member_opts[] = {
    { "title", "", "menu text" },
    { "cmd", "", "what to run" },
    { "on", "files", "where", "+files folders background" },
    { NULL },
};
static const fn_def member = { .id = "r.", .life = FN_COLD, .where = FN_ANYWHERE, .menus = FN_MENU_ITEMS, .group = 1,
                               .opts = member_opts, .about = "a test family", .applies = member_applies,
                               .run = run_member, .configure = configure_member };

static void test_registry_family(void)
{
    printf("registry: families made by settings sections\n");
    const char *ini = "[keys]\nr.one = F8\n[r.one]\ntitle = First one\ncmd = a\n[r.two]\ncmd = b\non = folders files\n"
                      "[r.bad name]\ncmd = c\n[r.three]\non = files cars\n";
    fn_reset();
    t_now = 1000;
    fn_host h = { host_now, host_arm, NULL };
    fn_init(&h);
    fn_register(defs, sizeof defs / sizeof defs[0]);
    CHECK(fn_family(&member) == 0);
    fn_settings(ini, strlen(ini));
    t_configured = 0;
    fn_configure_all();
    const fn_def *one = fn_find("r.one"), *two = fn_find("r.two"), *three = fn_find("r.three");
    CHECK(one && two && three && !fn_find("r.bad name"));
    CHECK(t_configured == 3);
    CHECK(one && !wcscmp(one->title, L"First one") && two && !wcscmp(two->title, L"two"));
    const char *errs = fn_settings_errors();
    CHECK(strstr(errs, "[r.bad name]") != NULL);                   /* a name with a space */
    CHECK(strstr(errs, "\"cars\" is not one of") != NULL);         /* "+" lists: every word must fit */
    CHECK(strstr(errs, "[r.three] needs cmd") != NULL);            /* configure knew which one it served */
    CHECK(three && !strcmp(fn_option(three, "on"), "files"));      /* the default holds */

    fn_ctx c = pane(FN_S_ITEMS | FN_S_ONE, 1);
    CHECK(fn_key(VK_F8, 0, &c) == 1 && !strcmp(t_self, "r.one")); /* keys bind to members */
    CHECK(fn_self() == NULL);
    HMENU m = CreatePopupMenu();
    CHECK(fn_menu(m, FN_MENU_ITEMS, &c) == 5);                    /* any, needs; one, two, three */
    DestroyMenu(m);

    char d[8192];
    fn_describe(d, sizeof d);
    CHECK(strstr(d, "fn\tr.one\tFirst one\t") != NULL && strstr(d, "\tkMPf\t") != NULL);

    /* a reload: one is gone, two stays where it was, four takes one's slot */
    const char *ini2 = "[r.two]\ncmd = b2\n[r.four]\ncmd = d\n[r.three]\ncmd = c\n";
    uint32_t count = fn_count();
    CHECK(fn_reload(ini2, strlen(ini2)) >= 3);
    CHECK(!fn_find("r.one") && fn_find("r.two") == two && fn_find("r.four") == one);
    CHECK(fn_count() == count);                                     /* the gone slot was reused */
    CHECK(fn_key(VK_F8, 0, &c) == 0);                               /* its key went with it */
    CHECK(!wcscmp(fn_find("r.four")->title, L"four"));
    CHECK(!strstr(fn_settings_errors(), "needs cmd"));
    CHECK(fn_run(one, &c) == 1 && !strcmp(t_self, "r.four"));
    fn_list(d, sizeof d);
    CHECK(!strstr(d, "r.one"));
    char tpl[16384];
    fn_template(tpl, sizeof tpl);
    CHECK(strstr(tpl, ";[r.example]") != NULL && !strstr(tpl, ";r.two = on"));

    /* nothing in the file: every member goes */
    CHECK(fn_reload("", 0) >= 3 && !fn_find("r.two") && !fn_find("r.four"));
    fn_reset();
}

static UINT member_menu_id(HMENU menu, const wchar_t *title)
{
    wchar_t shown[160];
    for (int i = 0; i < GetMenuItemCount(menu); i++) {
        UINT id = GetMenuItemID(menu, i);
        if (id != (UINT)-1 && GetMenuStringW(menu, id, shown, 160, MF_BYCOMMAND) &&
            !wcscmp(shown, title)) return id;
    }
    return 0;
}

static void test_visible_action_revision(void)
{
    printf("registry: visible menu and palette actions retain their offered definition\n");
    fn_reset(); fn_host host = { host_now, host_arm, NULL }; fn_init(&host);
    fn_register(defs, sizeof defs / sizeof defs[0]);
    CHECK(fn_family(&member) == 0);
    const char *before = "[r.one]\ntitle = Before\ncmd = before\n";
    CHECK(fn_settings(before, strlen(before)) == 0);
    fn_configure_all(); t_member_runs = 0; t_cmd[0] = 0;
    fn_ctx c = pane(FN_S_ITEMS | FN_S_ONE, 1);
    c.from = FN_FROM_MENU; c.menu_revision = fn_revision();
    uint32_t offered = c.menu_revision;
    HMENU menu = CreatePopupMenu(); fn_menu(menu, FN_MENU_ITEMS, &c);
    UINT old_id = member_menu_id(menu, L"Before");
    CHECK(old_id != 0);
    const char *edited = "[r.one]\ntitle = After\ncmd = after\n";
    CHECK(fn_reload(edited, strlen(edited)) == 1);
    CHECK(fn_menu_run(old_id, &c) == 0 && t_member_runs == 0);
    CHECK(fn_run_id_at_revision("r.one", offered, &c) == 0 && t_member_runs == 0);
    DestroyMenu(menu);

    c.menu_revision = fn_revision(); menu = CreatePopupMenu(); fn_menu(menu, FN_MENU_ITEMS, &c);
    UINT fresh_id = member_menu_id(menu, L"After");
    CHECK(fresh_id && fn_menu_run(fresh_id, &c) == 1 && t_member_runs == 1 && !strcmp(t_cmd, "after"));
    DestroyMenu(menu);

    offered = c.menu_revision; menu = CreatePopupMenu(); fn_menu(menu, FN_MENU_ITEMS, &c);
    old_id = member_menu_id(menu, L"After");
    const fn_def *reused_slot = fn_find("r.one");
    const char *replacement = "[r.two]\ntitle = Replacement\ncmd = replacement\n";
    CHECK(fn_reload(replacement, strlen(replacement)) >= 1 && fn_find("r.two") == reused_slot);
    CHECK(fn_menu_run(old_id, &c) == 0 && fn_run_id_at_revision("r.one", offered, &c) == 0 && t_member_runs == 1);
    DestroyMenu(menu);
    c.menu_revision = fn_revision(); menu = CreatePopupMenu(); fn_menu(menu, FN_MENU_ITEMS, &c);
    fresh_id = member_menu_id(menu, L"Replacement");
    CHECK(fresh_id && fn_menu_run(fresh_id, &c) == 1 && t_member_runs == 2 && !strcmp(t_self, "r.two") &&
          !strcmp(t_cmd, "replacement"));
    DestroyMenu(menu); fn_reset();
}

static int expands_to(const wchar_t *cmd, const wchar_t *const *paths, uint32_t n, int item, const wchar_t *want)
{
    wchar_t out[512];
    int len = relay_expand(cmd, paths, n, item, L"C:\\f d", out, 512);
    if (len < 0 || wcscmp(out, want)) {
        printf("    got \"%ls\"\n", len < 0 ? L"(too long)" : out);
        return 0;
    }
    return 1;
}

static void test_relaycmd(void)
{
    printf("relays: command lines, types, menu texts\n");
    const wchar_t *two[2] = { L"C:\\f d\\a b.txt", L"C:\\f d\\c.md" };
    CHECK(expands_to(L"code %1", two, 2, -1, L"code \"C:\\f d\\a b.txt\""));
    CHECK(expands_to(L"code \"%1\"", two, 2, -1, L"code \"C:\\f d\\a b.txt\""));    /* already quoted */
    CHECK(expands_to(L"x %1", two, 2, 1, L"x \"C:\\f d\\c.md\""));                /* one run per item */
    CHECK(expands_to(L"x %*", two, 2, -1, L"x \"C:\\f d\\a b.txt\" \"C:\\f d\\c.md\""));
    CHECK(expands_to(L"x %V %L", two, 1, -1, L"x \"C:\\f d\\a b.txt\" \"C:\\f d\\a b.txt\""));
    CHECK(expands_to(L"x %D 100%%", two, 2, -1, L"x \"C:\\f d\" 100%"));
    CHECK(expands_to(L"x %1", two, 0, -1, L"x \"C:\\f d\""));                   /* no items: the folder */
    CHECK(expands_to(L"x %Q", two, 2, -1, L"x %Q"));                           /* unknown: left alone */
    SetEnvironmentVariableW(L"FP_TEST_DIR", L"C:\\tools");
    CHECK(expands_to(L"%FP_TEST_DIR%\\t.exe %1", two, 1, -1, L"C:\\tools\\t.exe \"C:\\f d\\a b.txt\""));
    CHECK(expands_to(L"%FP_NOT_SET% x", two, 1, -1, L"%FP_NOT_SET% x"));
    wchar_t tiny[8];
    CHECK(relay_expand(L"x %*", two, 2, -1, NULL, tiny, 8) == -1);
    CHECK(relay_takes_all(L"a %* b") && !relay_takes_all(L"a %%* %1"));

    wchar_t prog[MAX_PATH];
    const wchar_t *rest;
    relay_split(L"\"C:\\Program Files\\x.exe\" -a %1", prog, MAX_PATH, &rest);
    CHECK(!wcscmp(prog, L"C:\\Program Files\\x.exe") && !wcscmp(rest, L"-a %1"));
    relay_split(L"  code.cmd", prog, MAX_PATH, &rest);
    CHECK(!wcscmp(prog, L"code.cmd") && !rest[0]);

    CHECK(relay_type_ok(L"C:\\a\\b.TXT", "txt md") && relay_type_ok(L"C:\\a\\b.md", ".txt, .md"));
    CHECK(!relay_type_ok(L"C:\\a.b\\noext", "txt") && !relay_type_ok(L"C:\\a\\b.txt2", "txt"));
    CHECK(relay_type_ok(L"C:\\a\\noext", ""));

    wchar_t n[64];
    relay_norm(L"  &Add to archive...\tCtrl+A", n, 64);
    CHECK(!wcscmp(n, L"add to archive"));
    relay_norm(L"Scan with Microsoft Defender\u2026", n, 64);
    CHECK(!wcscmp(n, L"scan with microsoft defender"));
    relay_norm(L"Save && Exit", n, 64);
    CHECK(!wcscmp(n, L"save & exit"));
    const wchar_t *next = relay_segment(L"7-&Zip > Add to archive...", n, 64);
    CHECK(!wcscmp(n, L"7-zip") && next && !wcscmp(next, L"Add to archive..."));
    CHECK(relay_segment(next, n, 64) == NULL && !wcscmp(n, L"add to archive"));
}

static void test_termutil(void)
{
    printf("panel terminal: shells, folders reported, cd\n");
    CHECK(term_shell_kind(L"C:\\Windows\\System32\\cmd.exe") == TERM_SH_CMD);
    CHECK(term_shell_kind(L"C:\\Tools\\pwsh.exe.bak") == TERM_SH_OTHER);
    CHECK(term_shell_kind(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe") == TERM_SH_PWSH);
    CHECK(term_shell_kind(L"powershell") == TERM_SH_PWSH);
    CHECK(term_shell_kind(L"git status") == TERM_SH_OTHER);

    CHECK(effect1(L"\\\\wsl.localhost\\Ubuntu\\tmp\\Foo",L"\\\\wsl.localhost\\Ubuntu\\tmp\\foo",0,ALL)==DROPEFFECT_MOVE);
    CHECK(term_shell_kind(L"wsl.exe") == TERM_SH_WSL);
    wchar_t wd[128],wl[260],wa[8192];
    CHECK(term_wsl_split(L"\\\\wsl.localhost\\Ubuntu\\home\\it's",wd,128,wl,260)&&!wcscmp(wl,L"/home/it's"));
    CHECK(term_wsl_split(L"\\\\wsl$\\Ubuntu\\tmp\\a\xF03A\xDCFF",wd,128,wl,260)&&wl[6]==L':'&&wl[7]==0xDCFF);
    CHECK(!term_wsl_split(L"\\\\wsl$\\bad name\\tmp",wd,128,wl,260));
    CHECK(term_wsl_arg(L"\\\\wsl$\\Ubuntu\\tmp\\a\xF03A\xDCFF",wa,8192)&&!wcscmp(wa,L"x2f746d702f613aff"));
    {
        const wchar_t *paths[] = { L"\\\\wsl$\\Ubuntu\\tmp\\A ", L"\\\\wsl$\\Ubuntu\\tmp\\A",
                                   L"\\\\wsl$\\Ubuntu\\tmp\\raw\xDCFF", L"\\\\wsl$\\Ubuntu\\tmp\\raw\xFFFD" };
        char wire[2048]; wchar_t round[MAX_PATH];
        for (int i = 0; i < 4; i++) {
            size_t n = pathwire_encode(paths[i], wire, sizeof wire);
            CHECK(n && pathwire_decode(wire, n, round, MAX_PATH) && !wcscmp(round, paths[i]));
        }
        CHECK(!pathwire_decode("u16v1:0000", 10, round, MAX_PATH));
        CHECK(!pathwire_decode("u16v1:0041z", 11, round, MAX_PATH));
        CHECK(!pathwire_decode("u16v2:0041", 10, round, MAX_PATH));
        CHECK(!pathwire_decode("u16v1:0041", 10, round, 1));
    }
    CHECK(term_cd_command(TERM_SH_WSL,L"\\\\wsl$\\Ubuntu\\tmp",L"\\\\wsl$\\Ubuntu\\it's",wa,8192)&&!wcscmp(wa,L"builtin cd -- $'\\x2f\\x69\\x74\\x27\\x73'"));
    CHECK(!term_cd_command(TERM_SH_WSL,L"\\\\wsl$\\Ubuntu\\tmp",L"\\\\wsl$\\Debian\\tmp",wa,8192));
    CHECK(term_cd_command(TERM_SH_WSL,NULL,L"C:\\x",wa,8192)&&wcsstr(wa,L"wslpath -u"));
    CHECK(term_osc_cwd(9,"9;wsl:Ubuntu;2f746d702f613aff",strlen("9;wsl:Ubuntu;2f746d702f613aff"),wa,8192)&&!wcscmp(wa,L"\\\\wsl.localhost\\Ubuntu\\tmp\\a\xF03A\xDCFF"));
    CHECK(!term_osc_cwd(9,"9;wsl:Ubuntu;00",14,wa,8192));
    CHECK(!term_osc_cwd(9,"9;wsl:Ubuntu;2fzz",16,wa,8192));

    wchar_t d[MAX_PATH];
    const char *o7 = "file://box/C:/Users/me/My%20Stuff";
    CHECK(term_osc_cwd(7, o7, strlen(o7), d, MAX_PATH) && !wcscmp(d, L"C:\\Users\\me\\My Stuff"));
    const char *o7u = "file://box/C:/caf%C3%A9";
    CHECK(term_osc_cwd(7, o7u, strlen(o7u), d, MAX_PATH) && !wcscmp(d, L"C:\\caf\x00E9"));
    const char *o9 = "9;C:\\Work\\x";
    CHECK(term_osc_cwd(9, o9, strlen(o9), d, MAX_PATH) && !wcscmp(d, L"C:\\Work\\x"));
    const char *o9q = "9;\"D:\\\"";
    CHECK(term_osc_cwd(9, o9q, strlen(o9q), d, MAX_PATH) && !wcscmp(d, L"D:\\"));
    const char *o9d = "9;C:";
    CHECK(term_osc_cwd(9, o9d, strlen(o9d), d, MAX_PATH) && !wcscmp(d, L"C:\\"));
    const char *unc = "9;\\\\srv\\share\\x";
    CHECK(term_osc_cwd(9, unc, strlen(unc), d, MAX_PATH) && !wcscmp(d, L"\\\\srv\\share\\x"));
    const char *o1337 = "CurrentDir=E:\\a";
    CHECK(term_osc_cwd(1337, o1337, strlen(o1337), d, MAX_PATH) && !wcscmp(d, L"E:\\a"));
    const char *linux = "file://box/home/me";
    CHECK(!term_osc_cwd(7, linux, strlen(linux), d, MAX_PATH)); /* not a Windows folder */
    CHECK(!term_osc_cwd(9, "4;1;50", 6, d, MAX_PATH));           /* a progress report, not 9;9 */
    CHECK(!term_osc_cwd(133, "A", 1, d, MAX_PATH));

    wchar_t c[600];
    CHECK(term_cd_command(TERM_SH_PWSH, L"C:\\a\\b", L"C:\\a", c, 600) && !wcscmp(c, L"cd .."));
    CHECK(term_cd_command(TERM_SH_PWSH, L"C:\\a", L"C:\\", c, 600) && !wcscmp(c, L"cd .."));
    CHECK(term_cd_command(TERM_SH_CMD, L"C:\\a\\", L"C:\\a\\b c", c, 600) && !wcscmp(c, L"cd \"b c\""));
    CHECK(term_cd_command(TERM_SH_PWSH, L"C:\\", L"C:\\x\\y", c, 600) && !wcscmp(c, L"cd 'x\\y'"));
    CHECK(term_cd_command(TERM_SH_PWSH, L"C:\\a", L"C:\\a\\it's", c, 600) && !wcscmp(c, L"cd 'it''s'"));
    CHECK(term_cd_command(TERM_SH_PWSH, L"C:\\a", L"C:\\a\\[x]", c, 600) && !wcscmp(c, L"cd -LiteralPath 'C:\\a\\[x]'"));
    CHECK(term_cd_command(TERM_SH_PWSH, L"C:\\ab", L"C:\\a", c, 600) && !wcscmp(c, L"cd -LiteralPath 'C:\\a'")); /* not its parent */
    CHECK(term_cd_command(TERM_SH_PWSH, L"C:\\a\\b\\c", L"C:\\a", c, 600) && !wcscmp(c, L"cd -LiteralPath 'C:\\a'"));
    CHECK(term_cd_command(TERM_SH_CMD, L"C:\\a", L"D:\\x", c, 600) && !wcscmp(c, L"cd /d \"D:\\x\""));
    CHECK(term_cd_command(TERM_SH_CMD, NULL, L"D:\\x", c, 600) && !wcscmp(c, L"cd /d \"D:\\x\""));
    CHECK(!term_cd_command(TERM_SH_CMD, NULL, L"C:\\100%done", c, 600)); /* cmd would expand it */
    CHECK(!term_cd_command(TERM_SH_OTHER, NULL, L"C:\\x", c, 600));
    CHECK(!term_cd_command(TERM_SH_PWSH, NULL, L"C:\\x\ny", c, 600));
    CHECK(!term_cd_command(TERM_SH_PWSH, NULL, L"C:\\x", c, 10)); /* too long for the buffer */

    CHECK(term_cmd_prompt(NULL, c, 600) && wcsstr(c, L"$E]9;9;$P$E\\$P$G") && wcsstr(c, L"133;B"));
    CHECK(term_cmd_prompt(L"$T $P$G", c, 600) && wcsstr(c, L"$T $P$G"));
    CHECK(term_base64_utf16(L"ab", c, 600) == 8 && !wcscmp(c, L"YQBiAA=="));
    CHECK(term_base64_utf16(L"abc", c, 600) == 8 && !wcscmp(c, L"YQBiAGMA"));
    CHECK(!term_base64_utf16(L"abc", c, 8));
    CHECK(wcsstr(term_pwsh_hook(), L"133;B") && wcsstr(term_pwsh_hook(), L"9;9;"));
}

/* Operations owns retained state separately from its disposable UI process.
 * Verify exact diagnostic paths, safe clearing/eviction and malformed wire data. */
static void test_operations(void)
{
    printf("operations: retained results and protocol\n");
    ops_model m = { 0 };
    ops_entry *e = ops_add(&m, FPE_OP_COPY, L"C:\\source\\a.txt", 1, L"C:\\target");
    CHECK(e && e->h.state == OPS_PENDING);
    if (!e) return;
    uint64_t token = e->h.token;
    wchar_t path[] = L"C:\\source\\a.txt";
    fpe_job_progress p = { .job = 42, .op = FPE_OP_COPY, .state = FPE_JOB_RUNNING, .items_total = 3,
                           .items_done = 1, .current = path };
    ops_update(e, &p); path[3] = L'X';
    CHECK(!wcscmp(e->current, L"C:\\source\\a.txt"));
    fpe_job_event v = { 42, FPE_JOB_ERROR, ERROR_ACCESS_DENIED, L"C:\\target\\blocked.txt" };
    ops_diagnostic(e, &v);
    CHECK(wcsstr(e->details, v.path) && wcsstr(e->details, L"code 5"));
    v.kind = FPE_JOB_SKIPPED; v.error = ERROR_NOT_SUPPORTED; ops_diagnostic(e, &v);
    CHECK(wcsstr(e->details, L"Directory link was left untouched"));
    uint32_t size; void *wire = ops_encode(e, &size); ops_entry decoded;
    CHECK(wire && ops_decode(wire, size, &decoded));
    if (wire) {
        CHECK(decoded.h.token == token && decoded.h.job == 42);
        CHECK(!wcscmp(decoded.details, e->details)); ops_free_entry(&decoded);
        CHECK(!ops_decode(wire, size - 1, &decoded));
        ops_head *h = wire; uint32_t keep = h->chars[0]; h->chars[0] = 0xFFFFFFFF;
        CHECK(!ops_decode(wire, size, &decoded)); h->chars[0] = keep;
        h->state = 99; CHECK(!ops_decode(wire, size, &decoded)); free(wire);
    }
    ops_clear(&m); CHECK(m.n == 1); /* active work cannot be cleared */
    p.state = FPE_JOB_DONE; p.errors = 1; p.skipped = 1; ops_update(e, &p);
    CHECK(!wcscmp(ops_state(e), L"Finished with errors") && e->h.ended >= e->h.started);
    p.state = FPE_JOB_RUNNING; ops_update(e, &p); CHECK(e->h.state == FPE_JOB_DONE);
    ops_clear(&m); CHECK(m.n == 0);
    for (int i = 0; i < OPS_MAX; i++) CHECK(ops_add(&m, FPE_OP_COPY, L"a", 1, L"b") != NULL);
    CHECK(!ops_add(&m, FPE_OP_COPY, L"c", 1, L"d")); /* all records are live */
    uint64_t oldest = m.entries[0].h.token, live = m.entries[1].h.token;
    m.entries[0].h.state = FPE_JOB_DONE;
    e = ops_add(&m, FPE_OP_MOVE, L"c", 1, L"d");
    CHECK(e && m.n == OPS_MAX && !ops_find(&m, oldest) && ops_find(&m, live));
    for (int i = 0; i < 70; i++) ops_diagnostic(e, &v);
    CHECK(e->h.details_count == 64 && e->h.omitted == 6);
    e->h.state = FPE_JOB_RUNNING; e->h.cancelling = 1; e->h.op = FPE_OP_RECYCLE;
    CHECK(!wcscmp(ops_state(e), L"Stopping after current item"));
    ops_free(&m);
}

/* Replay the host snapshot after a choice was made, before Start. A late
 * READY/replay must not silently change Replace back to Keep both. */
static int panel_send(HANDLE in, uint32_t type, const void *data, uint32_t size)
{
    panelp_head h = { type, size }; DWORD written;
    return WriteFile(in, &h, sizeof h, &written, NULL) && written == sizeof h &&
           (!size || (WriteFile(in, data, size, &written, NULL) && written == size));
}
static void test_operations_panel(void)
{
    printf("operations panel: confirmed choice survives snapshot replay\n");
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *slash = wcsrchr(exe, L'\\');
    CHECK(slash != NULL); if (!slash) return;
    wcscpy(slash + 1, L"folderwing-operations.exe");
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE in = NULL, child_in = NULL, out = NULL, child_out = NULL, section = NULL;
    PROCESS_INFORMATION pi = { 0 }; ops_model model = { 0 };
    CHECK(CreatePipe(&child_in, &in, &sa, 65536));
    CHECK(CreatePipe(&out, &child_out, &sa, 65536));
    if (!in || !out) goto cleanup;
    SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0); SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
    section = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, 400 * 600 * 8, NULL);
    CHECK(section != NULL); if (!section) goto cleanup;
    STARTUPINFOW si = { sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = child_in; si.hStdOutput = child_out;
    CHECK(CreateProcessW(exe, NULL, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi));
    if (!pi.hProcess) goto cleanup;
    CloseHandle(child_in); child_in = NULL; CloseHandle(child_out); child_out = NULL;
    panelp_hello hello = { PANELP_MAGIC, PANELP_VERSION, 96, 400, 600, 1, (uint64_t)(uintptr_t)section, GetCurrentProcessId(), 0 };
    CHECK(panel_send(in, PANELP_HELLO, &hello, sizeof hello));
    ops_entry *e = ops_add(&model, FPE_OP_COPY, L"C:\\a.txt", 1, L"C:\\target");
    uint32_t size = 0; void *packet = e ? ops_encode(e, &size) : NULL;
    CHECK(packet != NULL); if (!packet) goto cleanup;
    uint64_t token = e->h.token;
    CHECK(panel_send(in, PANELP_OP_RESET, NULL, 0));
    CHECK(panel_send(in, PANELP_OP_UPDATE, packet, size));
    CHECK(panel_send(in, PANELP_OP_SELECT, &token, sizeof token));
    panelp_input key = { .msg = WM_KEYDOWN, .wparam = VK_RIGHT };
    CHECK(panel_send(in, PANELP_INPUT, &key, sizeof key));
    CHECK(panel_send(in, PANELP_INPUT, &key, sizeof key));
    CHECK(panel_send(in, PANELP_OP_RESET, NULL, 0));
    CHECK(panel_send(in, PANELP_OP_UPDATE, packet, size));
    CHECK(panel_send(in, PANELP_OP_SELECT, &token, sizeof token));
    key.wparam = VK_RETURN; CHECK(panel_send(in, PANELP_INPUT, &key, sizeof key));
    free(packet);
    int got_action = 0;
    uint64_t until = GetTickCount64() + 3000;
    while (GetTickCount64() < until) {
        DWORD available = 0;
        if (!PeekNamedPipe(out, NULL, 0, NULL, &available, NULL)) break;
        if (available < sizeof(panelp_head)) { Sleep(1); continue; }
        panelp_head h; uint8_t data[256];
        if (pipe_read(out, &h, sizeof h) || h.size > sizeof data || pipe_read(out, data, h.size)) break;
        if (h.type == PANELP_FRAME) panel_send(in, PANELP_FRAME_ACK, NULL, 0);
        if (h.type == PANELP_OP_ACTION && h.size == sizeof(ops_action)) {
            ops_action action; memcpy(&action, data, sizeof action);
            CHECK(action.token == token && action.action == OPS_START && action.policy == FPE_CONFLICT_OVERWRITE);
            got_action = 1; break;
        }
    }
    CHECK(got_action);
cleanup:
    if (pi.hProcess) {
        panel_send(in, PANELP_QUIT, NULL, 0);
        if (WaitForSingleObject(pi.hProcess, 3000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    }
    if (in) CloseHandle(in); if (out) CloseHandle(out);
    if (child_in) CloseHandle(child_in); if (child_out) CloseHandle(child_out);
    if (section) CloseHandle(section);
    ops_free(&model);
}

static void test_linux_path_entry(void)
{
    printf("Linux path entry: literal syntax, UTF-8/byte names, mapping and actual distro/home resolution\n");
    CHECK(pathentry_kind(L"/")==1);CHECK(pathentry_kind(L"/tmp/A ")==1);CHECK(pathentry_kind(L"~")==1);CHECK(pathentry_kind(L"~/x")==1);
    CHECK(pathentry_kind(L"~other/x")==-1);CHECK(pathentry_kind(L"~\\x")==-1);CHECK(!pathentry_kind(L"C:/data"));CHECK(!pathentry_kind(L"//server/share"));CHECK(!pathentry_kind(NULL));
    char bytes[256];uint32_t size=0;CHECK(pathentry_bytes(L"/tmp/β 😀",bytes,sizeof bytes,&size));
    CHECK(size==12 && !memcmp(bytes,"/tmp/\xCE\xB2 \xF0\x9F\x98\x80",12));
    const wchar_t invalid[]={L'/',0xd800,0};CHECK(!pathentry_bytes(invalid,bytes,sizeof bytes,&size));
    CHECK(!pathentry_bytes(L"/long",bytes,1,&size));
    const char raw[]="/tmp/a\\b:%VALUE%;$(cmd) \xFF";
    wchar_t *unc=pathentry_unc("Ubuntu",6,raw,sizeof raw-1);CHECK(unc!=NULL);
    wchar_t distro[64],linux[4096];
    if(unc) {CHECK(pathentry_display(unc,distro,64,linux,4096));CHECK(!wcscmp(distro,L"Ubuntu"));
        CHECK(pathentry_bytes(linux,bytes,sizeof bytes,&size));CHECK(size==sizeof raw-1&&!memcmp(bytes,raw,size));free(unc);}
    CHECK(pathentry_display(L"\\\\wsl$\\Debian\\home\\Case ",distro,64,linux,4096));CHECK(!wcscmp(linux,L"/home/Case ")&&!wcscmp(distro,L"Debian"));
    CHECK(pathentry_display(L"\\\\wsl.localhost\\Ubuntu\\\xF041",distro,64,linux,4096));CHECK(linux[1]==0xf041);
    CHECK(!pathentry_display(L"C:\\data",distro,64,linux,4096));CHECK(!pathentry_unc("bad name",8,"/",1));
    wchar_t *path=NULL;DWORD error=pathentry_resolve(L"C:\\",L"/",0,NULL,&path);CHECK(!error&&path);
    if(path){CHECK(pathentry_display(path,distro,64,linux,4096));CHECK(!wcscmp(linux,L"/"));free(path);path=NULL;}
    error=pathentry_resolve(L"C:\\",L"~",0,NULL,&path);CHECK(!error&&path);if(path){CHECK(pathentry_display(path,distro,64,linux,4096));CHECK(linux[0]==L'/');free(path);path=NULL;}
    wchar_t missing[128];_snwprintf(missing,128,L"/openpilot-path-missing-%lu",GetCurrentProcessId());
    CHECK(pathentry_resolve(NULL,missing,0,NULL,&path)==ERROR_PATH_NOT_FOUND&&path==NULL);
    wchar_t absent[128];_snwprintf(absent,128,L"\\\\wsl.localhost\\openpilot-missing-%lu\\",GetCurrentProcessId());
    CHECK(pathentry_resolve(absent,L"/",0,NULL,&path)==ERROR_BAD_ENVIRONMENT&&path==NULL);
    CHECK(pathentry_resolve(L"\\\\wsl.localhost\\invalid name\\",L"/",0,NULL,&path)==ERROR_NOT_SUPPORTED&&path==NULL);
    HANDLE cancel=CreateEventW(NULL,TRUE,TRUE,NULL);CHECK(cancel!=NULL);if(cancel){CHECK(pathentry_resolve(NULL,L"/",0,cancel,&path)==ERROR_CANCELLED&&path==NULL);CloseHandle(cancel);}
}

static void test_image_util(void)
{
    printf("images: format recognition, fit and alpha\n");
    CHECK(image_extension(L"photo.JPEG"));CHECK(image_extension(L".png"));CHECK(image_extension(L"x.webp"));
    CHECK(image_extension(L"phone.heic"));CHECK(image_extension(L"raw.nef"));CHECK(image_extension(L"x.svg"));
    CHECK(!image_extension(L"x.png.exe"));CHECK(!image_extension(NULL));
    CHECK(image_fit(100,50,400,300,0)==1);CHECK(image_fit(100,50,400,300,1)==4);CHECK(image_fit(1000,500,400,300,0)==0.4);
    CHECK(image_fit(1000,2000,400,300,0)==0.15);CHECK(image_fit(1000,2000,400,300,1)==0.4);
    CHECK(image_fit(0,20,300,300,0)==1);
    CHECK(image_orientation(1)==0);CHECK(image_orientation(2)==8);CHECK(image_orientation(6)==1);
    CHECK(image_orientation(8)==3);CHECK(image_orientation(99)==0);
    uint8_t p[]={240,160,20,128, 255,100,10,0, 240,100,10,255};image_premultiply(p,3);
    CHECK(p[0]==120&&p[1]==80&&p[2]==10&&p[3]==128);
    CHECK(p[4]==0&&p[5]==0&&p[6]==0&&p[7]==0);CHECK(p[8]==240&&p[11]==255);
    CHECK(format_is("Ctrl+Minus",L"Ctrl+Minus"));
}
static void test_image_browse_model(void)
{
    printf("image browsing: eligibility, ordered neighbors, edges and failed rows\n");
    fpe_entry e={0};CHECK(imagebrowse_candidate(NULL,&e,L"x.PNG"));CHECK(!imagebrowse_candidate(NULL,&e,L"x.txt"));
    e.attrs=FILE_ATTRIBUTE_DIRECTORY;CHECK(!imagebrowse_candidate(NULL,&e,L"folder.png"));CHECK(!imagebrowse_candidate(NULL,NULL,L"x.png"));
    imagebrowse_list *l=calloc(1,sizeof *l+4*sizeof(uint32_t));CHECK(l!=NULL);if(!l)return;
    l->count=4;l->rows[0]=0;l->rows[1]=3;l->rows[2]=7;l->rows[3]=12;
    CHECK(imagebrowse_index(l,7)==2);CHECK(imagebrowse_index(l,4)==UINT32_MAX);
    CHECK(imagebrowse_neighbor(l,0,-1)==UINT32_MAX);CHECK(imagebrowse_neighbor(l,12,1)==UINT32_MAX);
    CHECK(imagebrowse_neighbor(l,0,1)==3);CHECK(imagebrowse_neighbor(l,7,-1)==3);
    CHECK(imagebrowse_neighbor(l,4,1)==7);CHECK(imagebrowse_neighbor(l,4,-1)==3);
    CHECK(imagebrowse_neighbor(l,UINT32_MAX,1)==UINT32_MAX);CHECK(imagebrowse_neighbor(l,3,0)==UINT32_MAX);
    CHECK(imagebrowse_remove(l,7));CHECK(l->count==3&&l->rows[2]==12);
    CHECK(imagebrowse_neighbor(l,7,1)==12);CHECK(imagebrowse_neighbor(l,7,-1)==3);
    CHECK(!imagebrowse_remove(l,7));CHECK(imagebrowse_index(l,12)==2);
    CHECK(imagebrowse_remove(l,0)&&imagebrowse_remove(l,3)&&imagebrowse_remove(l,12));
    CHECK(l->count==0&&imagebrowse_neighbor(l,3,1)==UINT32_MAX);
    CHECK(imagebrowse_neighbor(NULL,0,1)==UINT32_MAX);CHECK(imagebrowse_index(NULL,0)==UINT32_MAX);free(l);
}

static int image_reply(HANDLE out,imgp_head *head,void *buf,uint32_t cap)
{
    uint64_t end=GetTickCount64()+5000;DWORD bytes=0;
    do {if(!PeekNamedPipe(out,NULL,0,NULL,&bytes,NULL))return -1;if(bytes>=sizeof *head)break;Sleep(1);}while(GetTickCount64()<end);
    if(bytes<sizeof *head||pipe_read(out,head,sizeof *head)||head->size>cap)return -1;
    return pipe_read(out,buf,head->size);
}
static imgp_info image_test_open(HANDLE in,HANDLE out,const wchar_t *path,uint32_t doc)
{
    uint8_t payload[sizeof(imgp_open)+MAX_PATH*2];imgp_open open={64};memcpy(payload,&open,sizeof open);
    uint32_t n=(uint32_t)(wcslen(path)+1)*2;memcpy(payload+sizeof open,path,n);
    helper_send(in,IMGP_OPEN,doc,0,payload,sizeof open+n);imgp_info info={.err=E_FAIL};imgp_head head;
    CHECK(image_reply(out,&head,&info,sizeof info)==0);CHECK(head.type==IMGP_OPENED&&head.doc==doc);return info;
}
static imgp_rendered image_test_render(HANDLE in,HANDLE out,uint32_t doc,imgp_render *req)
{
    helper_send(in,IMGP_RENDER,doc,123,req,sizeof *req);imgp_head head;imgp_rendered reply={.err=E_FAIL};
    CHECK(image_reply(out,&head,&reply,sizeof reply)==0);CHECK(head.type==IMGP_RENDERED&&head.ticket==123);return reply;
}
static imgp_info image_test_page(HANDLE in,HANDLE out,uint32_t doc,uint32_t page)
{
    imgp_page req={page};helper_send(in,IMGP_PAGE,doc,321,&req,sizeof req);
    imgp_info info={.err=E_FAIL};imgp_head head;
    CHECK(image_reply(out,&head,&info,sizeof info)==0);CHECK(head.type==IMGP_PAGED&&head.doc==doc&&head.ticket==321);
    return info;
}
static void test_image_helper(void)
{
    printf("images: WIC/WebP pixels, orientation, ICC, bounds and file sharing\n");
    wchar_t exe[MAX_PATH],fixture[MAX_PATH],file[MAX_PATH];GetModuleFileNameW(NULL,exe,MAX_PATH);
    wchar_t *slash=wcsrchr(exe,L'\\');CHECK(slash!=NULL);if(!slash)return;slash[1]=0;
    _snwprintf(fixture,MAX_PATH,L"%ls..\\..\\tests\\fixtures\\images\\",exe);wcscat(exe,L"folderwing-image.exe");
    CHECK(GetFileAttributesW(exe)!=INVALID_FILE_ATTRIBUTES);if(GetFileAttributesW(exe)==INVALID_FILE_ATTRIBUTES)return;
    SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE section=NULL,in=NULL,out=NULL,ci=NULL,co=NULL;uint8_t *pixels=NULL;
    PROCESS_INFORMATION pi={0};section=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,IMGP_BANKS*IMGP_BANK_BYTES,NULL);
    pixels=section?MapViewOfFile(section,FILE_MAP_READ,0,0,0):NULL;
    CHECK(pixels&&CreatePipe(&ci,&in,&sa,65536)&&CreatePipe(&out,&co,&sa,65536));if(!pixels||!in||!out)goto cleanup;
    SetHandleInformation(in,HANDLE_FLAG_INHERIT,0);SetHandleInformation(out,HANDLE_FLAG_INHERIT,0);
    STARTUPINFOW si={sizeof si};si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=ci;si.hStdOutput=co;
    wchar_t cmd[MAX_PATH+64];_snwprintf(cmd,MAX_PATH+64,L"\"%ls\" %llu",exe,(unsigned long long)(uintptr_t)section);
    CHECK(CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));if(!pi.hProcess)goto cleanup;
    CloseHandle(ci);ci=NULL;CloseHandle(co);co=NULL;
    const wchar_t *names[]={L"alpha.png",L"alpha.webp",L"animated.webp",L"oriented.jpg",L"oriented.tiff",L"profile.png",L"animated.gif",L"shapes.svg",L"viewbox.svg",L"partial.svg",L"disposal.gif"};
    for(unsigned k=0;k<sizeof names/sizeof *names;++k) {
        _snwprintf(file,MAX_PATH,L"%ls%ls",fixture,names[k]);imgp_info info=image_test_open(in,out,file,k+1);
        CHECK(info.err==0);if(info.err)continue;
        CHECK(info.w==(k==3||k==4?8:k==2||k==6?12:k==8?24:k==10?6:16));CHECK(info.h==(k==3||k==4?16:k==2||k==6?6:k==8?12:k==10?4:8));
        if(k==3||k==4)CHECK(info.orientation==6);
        if(k==5)CHECK(info.color==1);
        if(k==2||k==6)CHECK(info.frames==2);
        imgp_render r={.cw=info.w,.ch=info.h,.w=info.w,.h=info.h};imgp_rendered rep=image_test_render(in,out,k+1,&r);
        CHECK(rep.err==0&&rep.w==info.w&&rep.h==info.h);
        if(k<=1) {
            CHECK(pixels[0]==40&&pixels[1]==20&&pixels[2]==240&&pixels[3]==255);
            const uint8_t *p=pixels+8*4;CHECK(p[0]==120&&p[1]==80&&p[2]==10&&p[3]==128);
            p=pixels+(4*16+8)*4;CHECK(p[0]==0&&p[1]==0&&p[2]==0&&p[3]==0);
            r.straight=1;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0);
            p=pixels+8*4;CHECK(p[0]==240&&p[1]==160&&p[2]==20&&p[3]==128);r.straight=0;
            r.rotation=1;r.cw=r.w=8;r.ch=r.h=16;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0);
            CHECK(pixels[0]==40&&pixels[2]==240&&pixels[3]==255);
            r.rotation=0;r.flip=1;r.cw=r.w=16;r.ch=r.h=8;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0);
            CHECK(pixels[0]==120&&pixels[2]==10&&pixels[3]==128);
            r.flip=0;r.x=15;r.y=0;r.cw=2;rep=image_test_render(in,out,k+1,&r);CHECK(FAILED((HRESULT)rep.err));
            r.x=0;r.cw=16;r.w=IMGP_EDGE+1;rep=image_test_render(in,out,k+1,&r);CHECK(FAILED((HRESULT)rep.err));
        }
        if(k==2)CHECK(pixels[0]==0&&pixels[1]==0&&pixels[2]==255&&pixels[3]==255);
        if(k==2||k==6) {
            CHECK(info.animated && info.frames==2);
            r.frame=1;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&rep.delay_ms>=20);
            CHECK(pixels[0]==255&&pixels[1]==0&&pixels[2]==0&&pixels[3]==255);
            r.frame=0;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0);
            CHECK(pixels[0]==0&&pixels[2]==255&&pixels[3]==255);
            r.frame=info.frames;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==E_INVALIDARG);
        }
        if(k==7) {
            CHECK(!info.partial && !info.animated);
            CHECK(pixels[0]==40&&pixels[1]==20&&pixels[2]==240&&pixels[3]==255);
            CHECK(pixels[32]>=119&&pixels[32]<=121&&pixels[35]==128);
            CHECK(pixels[(4*16+8)*4+3]==0);
            r.straight=1;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&pixels[32]>=238&&pixels[32]<=242);
            r.straight=0;r.rotation=1;r.cw=r.w=8;r.ch=r.h=16;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&pixels[2]==240);
            r.rotation=0;r.flip=1;r.cw=r.w=16;r.ch=r.h=8;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&pixels[3]==128);
            r.flip=0;r.x=8;r.y=4;r.cw=r.w=8;r.ch=r.h=4;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&pixels[3]==0);
            r.x=15;r.cw=2;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==E_INVALIDARG);
        }
        if(k==9) CHECK(info.partial && !info.animated);
        if(k==10) {
            CHECK(info.animated&&info.frames==4&&info.loops==2&&rep.delay_ms==120);
            r.frame=1;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&rep.delay_ms==70);
            CHECK(pixels[2]==255&&pixels[(1*6+1)*4+1]==255);
            r.frame=2;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&rep.delay_ms==90);
            CHECK(pixels[(1*6+1)*4+2]==255&&pixels[(1*6+3)*4]==255);
            r.frame=3;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&rep.delay_ms==110);
            CHECK(pixels[1]==255&&pixels[(1*6+3)*4+3]==0&&pixels[(1*6+1)*4+2]==255);
            r.frame=1;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&pixels[(1*6+1)*4+1]==255);
            r.frame=0;rep=image_test_render(in,out,k+1,&r);CHECK(rep.err==0&&pixels[2]==255&&pixels[3]==255);
        }
    }
    _snwprintf(file,MAX_PATH,L"%lspages.tiff",fixture);
    imgp_info pages=image_test_open(in,out,file,30);
    CHECK(pages.err==0&&pages.pages==3&&pages.page==0&&!pages.animated);
    CHECK(pages.w==16&&pages.h==8&&pages.orientation==1&&!pages.color);
    CHECK(!wcscmp(pages.camera,L"Red page"));
    pages=image_test_page(in,out,30,1);
    CHECK(pages.err==0&&pages.pages==3&&pages.page==1);
    CHECK(pages.w==6&&pages.h==10&&pages.orientation==6&&pages.color==1);
    CHECK(!wcscmp(pages.camera,L"Green page")&&!wcscmp(pages.date,L"2026:10:14 12:00:00"));
    imgp_render page_render={.frame=1,.cw=6,.ch=10,.w=6,.h=10};
    imgp_rendered page_reply=image_test_render(in,out,30,&page_render);
    CHECK(page_reply.err==0&&pixels[1]>150&&pixels[2]<80&&pixels[3]==255);
    page_render.frame=0;page_reply=image_test_render(in,out,30,&page_render);CHECK(page_reply.err==E_INVALIDARG);
    pages=image_test_page(in,out,30,2);
    CHECK(pages.err==0&&pages.w==8&&pages.h==12&&pages.orientation==3&&!pages.color);
    CHECK(!wcscmp(pages.camera,L"Blue page"));
    page_render=(imgp_render){.frame=2,.cw=8,.ch=12,.w=8,.h=12};
    page_reply=image_test_render(in,out,30,&page_render);CHECK(page_reply.err==0&&pixels[0]==240&&pixels[2]==20);
    pages=image_test_page(in,out,30,3);CHECK(pages.err==E_INVALIDARG);
    page_reply=image_test_render(in,out,30,&page_render);CHECK(page_reply.err==0); /* refused page retains source */
    pages=image_test_page(in,out,31,0);CHECK(pages.err==E_ABORT);
    pages=image_test_page(in,out,30,0);CHECK(pages.err==0&&pages.w==16&&pages.h==8&&!pages.color);
    for(unsigned kind=0;kind<2;kind++) {
        _snwprintf(file,MAX_PATH,L"%ls%ls",fixture,kind?L"page_corrupt.tiff":L"page_oversize.tiff");
        pages=image_test_open(in,out,file,40+kind);CHECK(pages.err==0&&pages.pages==3);
        pages=image_test_page(in,out,40+kind,1);
        if(!kind)CHECK(pages.err==WINCODEC_ERR_IMAGESIZEOUTOFRANGE);
        else {
            if(SUCCEEDED((HRESULT)pages.err)) {
                page_render=(imgp_render){.frame=1,.cw=10,.ch=6,.w=10,.h=6};
                page_reply=image_test_render(in,out,40+kind,&page_render);CHECK(FAILED((HRESULT)page_reply.err));
            } else CHECK(FAILED((HRESULT)pages.err));
        }
        pages=image_test_page(in,out,40+kind,2);CHECK(pages.err==0&&pages.page==2&&pages.w==8&&pages.h==12);
        page_render=(imgp_render){.frame=2,.cw=8,.ch=12,.w=8,.h=12};
        page_reply=image_test_render(in,out,40+kind,&page_render);CHECK(page_reply.err==0&&pixels[0]==240);
    }
    const wchar_t *badsvg[]={L"doctype.svg",L"oversize.svg",L"broken.svg"};
    for(unsigned k=0;k<3;++k) {_snwprintf(file,MAX_PATH,L"%ls%ls",fixture,badsvg[k]);imgp_info info=image_test_open(in,out,file,15+k);CHECK(FAILED((HRESULT)info.err));}
    wchar_t temp[MAX_PATH],renamed[MAX_PATH];GetTempPathW(MAX_PATH,temp);_snwprintf(file,MAX_PATH,L"%lsopenpilot-image-%lu.png",temp,GetCurrentProcessId());
    _snwprintf(renamed,MAX_PATH,L"%lsopenpilot-image-%lu-renamed.png",temp,GetCurrentProcessId());
    wchar_t source[MAX_PATH];_snwprintf(source,MAX_PATH,L"%lsalpha.png",fixture);CHECK(CopyFileW(source,file,FALSE));
    imgp_info info=image_test_open(in,out,file,20);CHECK(info.err==0);CHECK(MoveFileExW(file,renamed,MOVEFILE_REPLACE_EXISTING));CHECK(DeleteFileW(renamed));
    imgp_render r={.cw=16,.ch=8,.w=16,.h=8};imgp_rendered rep=image_test_render(in,out,20,&r);CHECK(rep.err==0);
    info=image_test_open(in,out,file,21);CHECK(FAILED((HRESULT)info.err));
    HANDLE f=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);DWORD wrote;
    CHECK(f!=INVALID_HANDLE_VALUE);if(f!=INVALID_HANDLE_VALUE){WriteFile(f,"not an image",12,&wrote,NULL);CloseHandle(f);}
    info=image_test_open(in,out,file,22);CHECK(FAILED((HRESULT)info.err));DeleteFileW(file);
    helper_send(in,IMGP_QUIT,0,0,NULL,0);CHECK(WaitForSingleObject(pi.hProcess,3000)==WAIT_OBJECT_0);
cleanup:
    if(pi.hProcess){if(WaitForSingleObject(pi.hProcess,0)!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,1);CloseHandle(pi.hProcess);CloseHandle(pi.hThread);}
    HANDLE handles[]={in,out,ci,co,section};if(pixels)UnmapViewOfFile(pixels);for(unsigned i=0;i<5;++i)if(handles[i])CloseHandle(handles[i]);
}

static void test_text_model(void)
{
    printf("text preview: decoding, syntax, columns, wrapping and bounded search\n");
    CHECK(text_file_language(L"photo.jpg")==-1);CHECK(text_file_language(L"file.PY")==TEXT_PYTHON);
    CHECK(text_file_language(L"README")==TEXT_PLAIN);CHECK(text_file_language(L"config.yaml")==TEXT_CONFIG);
    CHECK(text_file_language(L"x.json")==TEXT_JSON);CHECK(text_file_language(L"index.html")==TEXT_XML);
    const uint8_t utf[]={0xEF,0xBB,0xBF,'A',0xC4,0x83,0xF0,0x9F,0x98,0x80,'\r','\n','B','\r','C'};text_data d;
    CHECK(text_decode(utf,sizeof utf,0,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_UTF8&&d.chars==8);
    CHECK(d.text[1]==0x103&&d.text[2]==0xD83D&&d.text[3]==0xDE00&&d.text[4]=='\n'&&d.text[6]=='\n');text_data_free(&d);
    const uint8_t le[]={255,254,'A',0,'\r',0,'\n',0,'B',0},be[]={254,255,0,'A',0,'\n',0,'B'};
    CHECK(text_decode(le,sizeof le,0,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_UTF16LE&&!wcscmp(d.text,L"A\nB"));text_data_free(&d);
    CHECK(text_decode(be,sizeof be,0,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_UTF16BE&&!wcscmp(d.text,L"A\nB"));text_data_free(&d);
    const uint8_t le32[]={255,254,0,0,'A',0,0,0,0,0xF6,1,0};
    CHECK(text_decode(le32,sizeof le32,0,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_UTF32LE&&d.chars==3&&d.text[1]==0xD83D);text_data_free(&d);
    const uint8_t be32[]={0,0,254,255,0,0,0,'A'};
    CHECK(text_decode(be32,sizeof be32,0,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_UTF32BE&&!wcscmp(d.text,L"A"));text_data_free(&d);
    const uint8_t legacy[]={'c','a','f',0xE9,' ',0x80};
    CHECK(text_decode(legacy,sizeof legacy,0,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_1252&&(d.flags&TEXTP_GUESSED)&&d.text[3]==0xE9&&d.text[5]==0x20AC);text_data_free(&d);
    const uint8_t cut[]={'A',0xF0,0x9F};
    CHECK(text_decode(cut,sizeof cut,1,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_UTF8&&d.chars==1&&(d.flags&TEXTP_PARTIAL)&&!(d.flags&TEXTP_REPLACED));text_data_free(&d);
    CHECK(text_decode(cut,sizeof cut,0,TEXT_UTF8,&d)==0);CHECK(d.flags&TEXTP_REPLACED);text_data_free(&d);
    const uint8_t no_bom[]={'A',0,'B',0,'C',0,'D',0};CHECK(text_decode(no_bom,sizeof no_bom,0,TEXT_AUTO,&d)==0);CHECK(d.encoding==TEXT_UTF16LE&&d.chars==4);text_data_free(&d);
    const uint8_t binary[]={'H','i',0,'x'};CHECK(text_decode(binary,sizeof binary,0,TEXT_AUTO,&d)==-2);
    CHECK(text_decode(NULL,0,0,TEXT_AUTO,&d)==0);CHECK(d.chars==0);text_data_free(&d);
    const char code[]="int answer = 42; /* comment\ncontinues */\nreturn \"yes\";";
    CHECK(text_decode((uint8_t*)code,sizeof code-1,0,TEXT_AUTO,&d)==0);text_highlight(&d,TEXT_C);
    CHECK(d.style[0]==TEXT_KEYWORD&&d.style[13]==TEXT_NUMBER&&d.style[17]==TEXT_COMMENT);
    uint32_t ret=(uint32_t)(wcsstr(d.text,L"return")-d.text);CHECK(d.style[ret]==TEXT_KEYWORD&&d.style[ret+7]==TEXT_STRING);text_data_free(&d);
    const char json[]="{\"name\": \"value\", \"n\": 12, \"ok\": true}";
    CHECK(text_decode((uint8_t*)json,sizeof json-1,0,TEXT_AUTO,&d)==0);text_highlight(&d,TEXT_JSON);CHECK(d.style[1]==TEXT_LABEL&&d.style[9]==TEXT_STRING);text_data_free(&d);
    const char py[]="def f():\n    \"\"\"multi\nline\"\"\"\n    # note\n    return True\n";
    CHECK(text_decode((uint8_t*)py,sizeof py-1,0,TEXT_AUTO,&d)==0);text_highlight(&d,TEXT_PYTHON);CHECK(d.style[0]==TEXT_KEYWORD&&d.style[13]==TEXT_STRING);text_data_free(&d);
    const wchar_t s[]=L"12345\tAB\n\nZ";uint32_t n=(uint32_t)wcslen(s),cols[40],lines,max;textp_row rows[40];text_columns(s,n,cols);
    CHECK(cols[5]==5&&cols[6]==8&&cols[8]==10&&cols[9]==0);
    uint32_t nr=text_layout(s,n,0,rows,40,&lines,&max);CHECK(nr==3&&lines==3&&max==10&&rows[0].end==8&&rows[1].start==rows[1].end);
    nr=text_layout(s,n,4,rows,40,&lines,&max);CHECK(nr==5&&rows[0].end==4&&rows[1].line==1&&rows[1].end==6&&rows[2].line==1);
    nr=text_layout(L"hello world",11,8,rows,40,&lines,&max);CHECK(nr==2&&rows[0].end==6&&rows[1].start==6);
    uint32_t units;CHECK(text_cell(L"\xD83D\xDE00",0,2,0,&units)==2&&units==2);
    uint32_t hits[8],more;CHECK(text_matches(L"aaaaa",5,L"aaa",3,hits,8,&more)==3&&hits[0]==0&&hits[2]==2&&!more);
    CHECK(text_matches(L"aaaaa",5,L"a",1,hits,2,&more)==2&&more);
    CHECK(text_matches(L"hello",5,L"",0,hits,8,&more)==0);
    CHECK(textp_snapshot_bytes(TEXTP_MAX_CHARS,TEXTP_MAX_CHARS+1)<=TEXTP_BANK_BYTES);
    uint8_t *large=malloc(TEXTP_MAX_CHARS+10);CHECK(large!=NULL);if(large){memset(large,'a',TEXTP_MAX_CHARS+10);CHECK(text_decode(large,TEXTP_MAX_CHARS+10,0,TEXT_AUTO,&d)==0);CHECK(d.chars==TEXTP_MAX_CHARS&&(d.flags&TEXTP_PARTIAL));text_data_free(&d);free(large);}
}
static int text_test_reply(HANDLE out,textp_head *head,void *buf,uint32_t cap)
{
    uint64_t end=GetTickCount64()+5000;DWORD bytes=0;
    do{if(!PeekNamedPipe(out,NULL,0,NULL,&bytes,NULL))return -1;if(bytes>=sizeof *head)break;Sleep(1);}while(GetTickCount64()<end);
    if(bytes<sizeof *head||pipe_read(out,head,sizeof *head)||head->size>cap)return -1;return pipe_read(out,buf,head->size);
}
static textp_info text_test_open(HANDLE in,HANDLE out,const wchar_t *file,uint32_t doc,uint32_t columns)
{
    uint8_t payload[sizeof(textp_open)+MAX_PATH*2];textp_open open={0,1,columns,TEXT_AUTO,TEXT_C};memcpy(payload,&open,sizeof open);
    uint32_t n=(uint32_t)(wcslen(file)+1)*2;memcpy(payload+sizeof open,file,n);helper_send(in,TEXTP_OPEN,doc,22,payload,sizeof open+n);
    textp_head h;textp_info info={.err=E_FAIL};CHECK(text_test_reply(out,&h,&info,sizeof info)==0);CHECK(h.type==TEXTP_READY&&h.doc==doc);return info;
}
static void test_text_helper(void)
{
    printf("text helper: immutable snapshots, search, clipboard, bounds and sharing\n");
    wchar_t exe[MAX_PATH],file[MAX_PATH],temp[MAX_PATH];GetModuleFileNameW(NULL,exe,MAX_PATH);wcscpy(wcsrchr(exe,L'\\')+1,L"folderwing-text.exe");
    CHECK(GetFileAttributesW(exe)!=INVALID_FILE_ATTRIBUTES);if(GetFileAttributesW(exe)==INVALID_FILE_ATTRIBUTES)return;
    GetTempPathW(MAX_PATH,temp);_snwprintf(file,MAX_PATH,L"%lsopenpilot-text-%lu.c",temp,GetCurrentProcessId());
    HANDLE f=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);DWORD wrote;const char source[]="int value = 42;\r\n// note\r\nVALUE = value;\n";
    CHECK(f!=INVALID_HANDLE_VALUE);if(f==INVALID_HANDLE_VALUE)return;WriteFile(f,source,sizeof source-1,&wrote,NULL);CloseHandle(f);
    SECURITY_ATTRIBUTES sa={sizeof sa,NULL,TRUE};HANDLE section=NULL,in=NULL,out=NULL,ci=NULL,co=NULL;uint8_t *banks=NULL;PROCESS_INFORMATION pi={0};
    section=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,TEXTP_BANKS*TEXTP_BANK_BYTES,NULL);banks=section?MapViewOfFile(section,FILE_MAP_READ,0,0,0):NULL;
    CHECK(banks&&CreatePipe(&ci,&in,&sa,65536)&&CreatePipe(&out,&co,&sa,65536));if(!banks||!in||!out)goto cleanup;
    SetHandleInformation(in,HANDLE_FLAG_INHERIT,0);SetHandleInformation(out,HANDLE_FLAG_INHERIT,0);STARTUPINFOW si={sizeof si};si.dwFlags=STARTF_USESTDHANDLES;si.hStdInput=ci;si.hStdOutput=co;
    wchar_t cmd[MAX_PATH+64];_snwprintf(cmd,MAX_PATH+64,L"\"%ls\" %llu",exe,(unsigned long long)(uintptr_t)section);
    CHECK(CreateProcessW(exe,cmd,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&si,&pi));if(!pi.hProcess)goto cleanup;
    CloseHandle(ci);ci=NULL;CloseHandle(co);co=NULL;
    textp_info info=text_test_open(in,out,file,1,0);CHECK(info.err==0&&info.lines==4&&info.encoding==TEXT_UTF8);
    CHECK(!wcscmp((wchar_t*)banks,L"int value = 42;\n// note\nVALUE = value;\n"));
    CHECK(banks[textp_style_offset(info.chars)]==TEXT_KEYWORD);
    CHECK(DeleteFileW(file)); /* the helper retains no source handle */
    textp_layout layout={1,8};helper_send(in,TEXTP_LAYOUT,1,23,&layout,sizeof layout);textp_head h;textp_info wrapped;
    CHECK(text_test_reply(out,&h,&wrapped,sizeof wrapped)==0);CHECK(wrapped.err==0&&wrapped.bank==1&&wrapped.rows>info.rows);
    CHECK(!wcscmp((wchar_t*)banks,L"int value = 42;\n// note\nVALUE = value;\n")); /* bank 0 was not overwritten */
    uint8_t query[sizeof(textp_find)+12];textp_find find={7,5};memcpy(query,&find,sizeof find);memcpy(query+sizeof find,L"value",12);helper_send(in,TEXTP_FIND,1,0,query,sizeof query);
    uint8_t result[sizeof(textp_found)+40];CHECK(text_test_reply(out,&h,result,sizeof result)==0);textp_found found;memcpy(&found,result,sizeof found);
    CHECK(h.type==TEXTP_FOUND&&found.err==0&&found.count==3&&found.gen==7);
    textp_copy copy={1,0,info.chars};helper_send(in,TEXTP_COPY,1,24,&copy,sizeof copy);textp_copied copied;
    CHECK(text_test_reply(out,&h,&copied,sizeof copied)==0);CHECK(copied.err==0&&copied.bank==1&&copied.bytes>2);
    CHECK(!wcscmp((wchar_t*)(banks+TEXTP_BANK_BYTES),L"int value = 42;\r\n// note\r\nVALUE = value;\r\n"));
    copy.end=info.chars+1;helper_send(in,TEXTP_COPY,1,25,&copy,sizeof copy);CHECK(text_test_reply(out,&h,&copied,sizeof copied)==0);CHECK(FAILED((HRESULT)copied.err));
    info=text_test_open(in,out,file,2,0);CHECK(FAILED((HRESULT)info.err));
    f=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);CHECK(f!=INVALID_HANDLE_VALUE);if(f!=INVALID_HANDLE_VALUE){WriteFile(f,"hi\0binary",9,&wrote,NULL);CloseHandle(f);}
    info=text_test_open(in,out,file,3,0);CHECK(FAILED((HRESULT)info.err)&&(info.flags&TEXTP_BINARY));DeleteFileW(file);
    f=CreateFileW(file,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);CHECK(f!=INVALID_HANDLE_VALUE);if(f!=INVALID_HANDLE_VALUE){char buf[4096];memset(buf,'x',sizeof buf);for(unsigned i=0;i<270;++i)WriteFile(f,buf,sizeof buf,&wrote,NULL);CloseHandle(f);}
    info=text_test_open(in,out,file,4,80);CHECK(info.err==0&&(info.flags&TEXTP_PARTIAL)&&info.read_bytes==1024*1024&&info.chars==1024*1024);DeleteFileW(file);
    helper_send(in,TEXTP_QUIT,0,0,NULL,0);CHECK(WaitForSingleObject(pi.hProcess,3000)==WAIT_OBJECT_0);
cleanup:
    if(pi.hProcess){if(WaitForSingleObject(pi.hProcess,0)!=WAIT_OBJECT_0)TerminateProcess(pi.hProcess,1);CloseHandle(pi.hProcess);CloseHandle(pi.hThread);}
    if(banks)UnmapViewOfFile(banks);HANDLE handles[]={in,out,ci,co,section};for(unsigned i=0;i<5;++i)if(handles[i])CloseHandle(handles[i]);DeleteFileW(file);
}

static void test_thumbnail_geometry(void)
{
    printf("thumbnail grid: rows, hit testing, last page and bounded aspect ratios\n");
    thumb_grid g=thumb_grid_make(600,500,160,8,40);
    CHECK(g.columns==3&&g.cell_w==200&&g.cell_h==216&&g.rows==2);
    CHECK(thumb_grid_hit(g,17,0,0,0)==0);
    CHECK(thumb_grid_hit(g,17,0,210,10)==1);
    CHECK(thumb_grid_hit(g,17,3,410,220)==8);
    CHECK(thumb_grid_hit(g,17,12,410,220)==-1);
    CHECK(thumb_grid_hit(g,17,0,-1,10)==-1&&thumb_grid_hit(g,17,0,600,10)==-1);
    CHECK(thumb_grid_top(g,17,100)==12&&thumb_grid_top(g,17,4)==3);
    CHECK(thumb_grid_top(g,0,100)==0&&thumb_grid_top(g,2,100)==0);
    g=thumb_grid_make(60,10,256,8,40);CHECK(g.columns==1&&g.rows==1);
    uint32_t w,h;thumb_fit(4000,2000,160,0,&w,&h);CHECK(w==160&&h==80);
    thumb_fit(8,16,160,0,&w,&h);CHECK(w==8&&h==16);
    thumb_fit(612,792,256,1,&w,&h);CHECK(w==198&&h==256);
    thumb_fit(1,100000,512,0,&w,&h);CHECK(w==1&&h==512);
    thumb_fit(0,10,160,0,&w,&h);CHECK(!w&&!h);
    thumb_fit(100,100,513,0,&w,&h);CHECK(!w&&!h);
}
static int thumbnail_disposed;
static void thumbnail_test_dispose(void *p){++thumbnail_disposed;free(p);}
static void test_thumbnail_cache(void)
{
    printf("thumbnail cache: byte budget, LRU, metadata invalidation, pinning and WSL identity\n");
    thumb_cache c;thumb_cache_init(&c,128,thumbnail_test_dispose);thumbnail_disposed=0;
    CHECK(thumb_cache_put(&c,L"C:\\a.png",10,20,160,4,4,0,malloc(64),64,0)!=NULL);
    CHECK(thumb_cache_put(&c,L"C:\\b.png",10,20,160,4,4,0,malloc(64),64,0)!=NULL);
    CHECK(c.bytes==128);CHECK(thumb_cache_find(&c,L"c:\\A.PNG",10,20,160)!=NULL);
    CHECK(thumb_cache_put(&c,L"C:\\c.png",10,20,160,4,4,0,malloc(64),64,0)!=NULL);
    CHECK(!thumb_cache_find(&c,L"C:\\b.png",10,20,160));CHECK(c.bytes==128&&thumbnail_disposed==1);
    CHECK(!thumb_cache_find(&c,L"C:\\a.png",10,21,160));CHECK(c.bytes==64&&thumbnail_disposed==2);
    thumb_entry *entry=thumb_cache_find(&c,L"C:\\c.png",10,20,160);CHECK(entry!=NULL);if(entry)entry->pinned=1;
    void *too=malloc(129);CHECK(!thumb_cache_put(&c,L"C:\\huge.png",10,20,160,1,1,0,too,129,0));free(too);
    CHECK(thumb_cache_put(&c,L"C:\\d.png",10,20,160,4,4,0,malloc(64),64,1)!=NULL);
    void *extra=malloc(64);CHECK(!thumb_cache_put(&c,L"C:\\e.png",10,20,160,4,4,0,extra,64,0));free(extra);
    CHECK(c.bytes==128);thumb_cache_clear(&c);CHECK(c.bytes==0&&thumbnail_disposed==4);
    CHECK(thumb_cache_put(&c,L"\\\\wsl.localhost\\Ubuntu\\tmp\\A.png",10,20,160,4,4,0,malloc(64),64,0)!=NULL);
    CHECK(thumb_cache_put(&c,L"\\\\wsl.localhost\\Ubuntu\\tmp\\a.png",10,20,160,4,4,0,malloc(64),64,0)!=NULL);
    CHECK(c.bytes==128);CHECK(thumb_cache_find(&c,L"\\\\wsl.localhost\\Ubuntu\\tmp\\A.png",10,20,160)!=NULL);
    CHECK(thumb_cache_find(&c,L"\\\\wsl.localhost\\Ubuntu\\tmp\\a.png",10,20,160)!=NULL);
    CHECK(!thumb_cache_find(&c,L"\\\\wsl.localhost\\Ubuntu\\tmp\\A.png",11,20,160));CHECK(c.bytes==64);
    CHECK(thumb_cache_find(&c,L"\\\\wsl.localhost\\Ubuntu\\tmp\\a.png",10,20,160)!=NULL);
    CHECK(thumb_cache_put(&c,L"C:\\broken.png",3,30,160,0,0,-1,NULL,0,0)!=NULL);
    CHECK(thumb_cache_find(&c,L"C:\\broken.png",3,30,160)->error==-1);
    CHECK(!thumb_cache_find(&c,L"C:\\broken.png",4,30,160));
    thumb_cache_clear(&c);CHECK(c.bytes==0&&thumbnail_disposed==6);
}

static void test_relay_draft(void)
{
    printf("relay drafts: shape, UTF-8, placeholders, quoting and import identity\n");
    relay_draft d;
    relay_draft_init(&d);
    char why[200];
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1);
    strcpy(d.name, "sample");
    strcpy(d.title, "Inspect selected files");
    strcpy(d.program, "C:\\Program Files\\tool.exe");
    strcpy(d.arguments, "%* --folder %D");
    CHECK(relay_draft_validate(&d, why, sizeof why) == 0);
    wchar_t line[RELAY_LINE_MAX], expanded[1024];
    CHECK(relay_draft_line(&d, line, RELAY_LINE_MAX) > 0);
    CHECK(!wcscmp(line, L"\"C:\\Program Files\\tool.exe\" %* --folder %D"));
    const wchar_t *paths[] = {L"C:\\a b.txt", L"C:\\c.txt"};
    CHECK(relay_expand(line, paths, 2, -1, L"C:\\", expanded, 1024) > 0);
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(expanded, &argc);
    CHECK(argv && argc == 5);
    if (argv) {
        CHECK(!wcscmp(argv[1], paths[0]) && !wcscmp(argv[2], paths[1]));
        CHECK(!wcscmp(argv[4], L"C:\\"));
        LocalFree(argv);
    }
    CHECK(relay_expand(L"tool \"%D\"", NULL, 0, -1, L"C:\\", expanded, 1024) > 0);
    argv = CommandLineToArgvW(expanded, &argc);
    CHECK(argv && argc == 2);
    if (argv) {
        CHECK(!wcscmp(argv[1], L"C:\\"));
        LocalFree(argv);
    }
    const wchar_t *embedded[] = {L"C:\\folder\\one\\\"two.txt"};
    CHECK(relay_expand(L"tool %1", embedded, 1, -1, L"C:\\", expanded, 1024) > 0);
    argv = CommandLineToArgvW(expanded, &argc);
    CHECK(argv && argc == 2);
    if (argv) {
        CHECK(!wcscmp(argv[1], embedded[0]));
        LocalFree(argv);
    }
    SetEnvironmentVariableW(L"DIR_NOT_SET_OPENPILOT", NULL);
    CHECK(relay_expand(L"tool %DIR_NOT_SET_OPENPILOT%", NULL, 0, -1, L"C:\\", expanded, 1024) > 0);
    CHECK(!wcscmp(expanded, L"tool %DIR_NOT_SET_OPENPILOT%"));
    strcpy(d.arguments, "%Q");
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1 && strstr(why, "Unknown placeholder"));
    strcpy(d.arguments, "\"%*\"");
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1 && strstr(why, "Remove quotes"));
    strcpy(d.arguments, "\"%1");
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1 && strstr(why, "Close"));
    strcpy(d.arguments, "100%% %NAME% %1");
    CHECK(relay_draft_validate(&d, why, sizeof why) == 0);
    d.scopes = 0;
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1);
    d.scopes = 3;
    strcpy(d.name, "bad]\n[functions");
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1);
    strcpy(d.name, "sample");
    d.title[0] = (char)0xFF;
    d.title[1] = 0;
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1);
    strcpy(d.title, "Inspect");
    memset(d.program, 'x', sizeof d.program);
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1);
    strcpy(d.program, "cmd");
    strcpy(d.types, "*.txt");
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1);
    strcpy(d.types, ".txt, md");
    CHECK(relay_draft_validate(&d, why, sizeof why) == 0);
    relay_catalog *catalog = calloc(1, sizeof *catalog);
    CHECK(catalog != NULL);
    if (!catalog)
        return;
    relay_catalog_entry e = {.kind = RELAY_ITEM, .scopes = RELAY_FILES};
    strcpy(e.title, "7-Zip action");
    strcpy(e.handler, "{00000000-0000-0000-0000-000000000000}");
    CHECK(relay_catalog_add(catalog, &e) == 1);
    e.scopes = RELAY_FOLDERS;
    CHECK(relay_catalog_add(catalog, &e) == 0 && catalog->count == 1 && catalog->entries[0].scopes == 3);
    relay_catalog_draft(&e, &d);
    CHECK(!strcmp(d.name, "7_zip_action") && d.kind == RELAY_ITEM && d.fallback == 0);
    CHECK(relay_draft_validate(&d, why, sizeof why) == -1 && strstr(why, "exact Explorer"));
    strcpy(d.action, "7-Zip > Add to archive");
    CHECK(relay_draft_validate(&d, why, sizeof why) == 0);
    e.kind = RELAY_RUN;
    strcpy(e.command, "\"C:\\Program Files\\tool.exe\" %1 --mode inspect");
    e.handler[0] = 0;
    relay_catalog_draft(&e, &d);
    CHECK(!strcmp(d.program, "C:\\Program Files\\tool.exe") && !strcmp(d.arguments, "%1 --mode inspect") &&
          d.window == 2);
    for (unsigned i = 0; i < RELAY_CATALOG_MAX; ++i) {
        snprintf(e.command, sizeof e.command, "tool --id %u", i);
        relay_catalog_add(catalog, &e);
    }
    CHECK(catalog->count == RELAY_CATALOG_MAX && catalog->omitted > 0);
    free(catalog);
}
static const fn_opt draft_options[] = {{"title", ""},
                                       {"run", ""},
                                       {"new", ""},
                                       {"template", ""},
                                       {"item", ""},
                                       {"verb", ""},
                                       {"sendto", ""},
                                       {"handler", ""},
                                       {"fallback", "on", "", "on off"},
                                       {"on", "", "", "+files folders background"},
                                       {"types", ""},
                                       {"count", "any", "", "any one"},
                                       {"window", "auto", "", "auto panel normal minimized hidden"},
                                       {NULL}};
static const fn_def draft_family = {.id = "relay.",
                                    .life = FN_COLD,
                                    .where = FN_PANE,
                                    .menus = FN_MENU_ITEMS | FN_MENU_FOLDER,
                                    .opts = draft_options,
                                    .run = run_member};
static const fn_def draft_conflict = {.id = "existing.action",
                                      .title = L"Existing action",
                                      .life = FN_COLD,
                                      .where = FN_PANE,
                                      .keys = "Ctrl+L",
                                      .run = run_member};
static void test_relay_draft_commit(void)
{
    printf(
        "relay draft transactions: create/edit, no mutation before commit, conflicts and INI preservation\n");
    fn_reset();
    fn_init(NULL);
    CHECK(fn_family(&draft_family) == 0);
    CHECK(fn_register(&draft_conflict, 1) == 0);
    fn_settings("", 0);
    fn_configure_all();
    relay_draft d;
    relay_draft_init(&d);
    strcpy(d.name, "tool");
    strcpy(d.title, "Tool ă");
    strcpy(d.program, "cmd");
    strcpy(d.arguments, "/c echo %1");
    strcpy(d.keys, "Ctrl+Alt+F8");
    d.window = 4;
    d.menu = 0;
    d.scopes = 5;
    const char original[] = "\xEF\xBB\xBF; retain comment\r\n[existing.action]\r\n; retain this too\r\n";
    char *out = NULL;
    size_t n = 0;
    char why[200];
    CHECK(relay_draft_apply(&d, original, sizeof original - 1, &out, &n, why, sizeof why) == 0);
    CHECK(out != NULL && n > sizeof original);
    CHECK(!fn_find("relay.tool"));
    CHECK(strstr(out, "; retain comment\r\n") && strstr(out, "[relay.tool]\r\n") &&
          strstr(out, "relay.tool = Ctrl+Alt+F8") && strstr(out, "relay.tool = off"));
    CHECK(fn_reload(out, n) > 0);
    const fn_def *f = fn_find("relay.tool");
    CHECK(f && fn_active(f) && fn_in_palette(f));
    HMENU menu = CreatePopupMenu();
    fn_ctx target = pane(FN_S_ITEMS | FN_S_ONE, 1);
    CHECK(fn_menu(menu, FN_MENU_ITEMS, &target) == 0);
    DestroyMenu(menu);
    CHECK(!strcmp(fn_option(f, "run"), "\"cmd\" /c echo %1"));
    CHECK(!strcmp(fn_option(f, "on"), "files background"));
    fn_binding bindings[4];
    CHECK(fn_bindings(f, bindings, 4) == 1 && bindings[0].vk == VK_F8 &&
          (bindings[0].mods & (FN_CTRL | FN_ALT)) == (FN_CTRL | FN_ALT));
    char *next = NULL;
    size_t size = 0;
    CHECK(relay_draft_apply(&d, out, n, &next, &size, why, sizeof why) == -1 &&
          strstr(why, "already exists") && !next);
    d.editing = 1;
    strcpy(d.keys, "Ctrl+L");
    CHECK(relay_draft_apply(&d, out, n, &next, &size, why, sizeof why) == -1 &&
          strstr(why, "existing.action") && !next);
    d.share_key = 1;
    CHECK(relay_draft_apply(&d, out, n, &next, &size, why, sizeof why) == 0);
    free(next);
    next = NULL;
    strcpy(d.keys, "Not a real key");
    CHECK(relay_draft_apply(&d, out, n, &next, &size, why, sizeof why) == -1 && !next);
    strcpy(d.keys, "none");
    memset(d.arguments, 'a', 700);
    d.arguments[700] = 0;
    CHECK(relay_draft_apply(&d, out, n, &next, &size, why, sizeof why) == 0);
    CHECK(strlen(fn_option(f, "run")) < 100); /* live registry still old */
    CHECK(fn_reload(next, size) > 0);
    CHECK(strlen(fn_option(f, "run")) > 700);
    free(out);
    out = next;
    n = size;
    next = NULL;
    d.kind = RELAY_ITEM;
    d.program[0] = d.arguments[0] = 0;
    strcpy(d.action, "Inspect item");
    strcpy(d.handler, "{00000000-0000-0000-0000-000000000000}");
    CHECK(relay_draft_apply(&d, out, n, &next, &size, why, sizeof why) == 0);
    CHECK(strstr(next, "fallback = off") && !strstr(next, "run ="));
    CHECK(fn_reload(next, size) > 0);
    CHECK(!strcmp(fn_option(f, "item"), "Inspect item"));
    free(next);
    CHECK(fn_settings_family_edit(out, n, "relay.invalid", "other.section", "title", "oops", &next, &size,
                                  why, sizeof why) == -1 &&
          !next);
    free(out);
    fn_reset();
}
static void test_relay_catalog_registry(void)
{
    printf("relay discovery: private registry metadata and preserved command templates\n");
    wchar_t ext[64], prog[128], key[512];
    _snwprintf(ext, 64, L".op_relay_test_%lu", GetCurrentProcessId());
    _snwprintf(prog, 128, L"OpenPilot.Relay.Test.%lu", GetCurrentProcessId());
    HKEY h;
    _snwprintf(key, 512, L"Software\\Classes\\%ls", ext);
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS);
    RegSetValueExW(h, NULL, 0, REG_SZ, (BYTE *)prog, (DWORD)(wcslen(prog) + 1) * 2);
    RegCloseKey(h);
    _snwprintf(key, 512, L"Software\\Classes\\%ls\\shell\\proof\\command", prog);
    CHECK(RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) == ERROR_SUCCESS);
    const wchar_t command[] = L"\"C:\\Never run this\\tool.exe\" --inspect %1";
    RegSetValueExW(h, NULL, 0, REG_SZ, (BYTE *)command, sizeof command);
    RegCloseKey(h);
    _snwprintf(key, 512, L"Software\\Classes\\%ls\\shell\\proof", prog);
    CHECK(RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_WRITE, &h) == ERROR_SUCCESS);
    const wchar_t title[] = L"OpenPilot proof action";
    RegSetValueExW(h, L"MUIVerb", 0, REG_SZ, (BYTE *)title, sizeof title);
    RegCloseKey(h);
    relay_catalog *catalog = calloc(1, sizeof *catalog);
    CHECK(catalog != NULL);
    if (catalog) {
        wchar_t file[128];
        _snwprintf(file, 128, L"C:\\fixture%ls", ext);
        relay_catalog_scan(catalog, file);
        int found = 0;
        for (unsigned i = 0; i < catalog->count; ++i) {
            relay_catalog_entry *e = &catalog->entries[i];
            if (!strcmp(e->title, "OpenPilot proof action")) {
                relay_draft d;
                relay_catalog_draft(e, &d);
                CHECK(e->kind == RELAY_RUN && e->scopes == RELAY_FILES && e->types[0]);
                CHECK(!strcmp(d.program, "C:\\Never run this\\tool.exe") &&
                      !strcmp(d.arguments, "--inspect %1"));
                found = 1;
            }
        }
        CHECK(found);
        CHECK(catalog->count <= RELAY_CATALOG_MAX);
        free(catalog);
    }
    _snwprintf(key, 512, L"Software\\Classes\\%ls", prog);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
    _snwprintf(key, 512, L"Software\\Classes\\%ls", ext);
    RegDeleteTreeW(HKEY_CURRENT_USER, key);
}
static void test_relay_pinned_helper(void)
{
    printf("relay helper: an unavailable pinned extension never builds the full menu\n");
    wchar_t exe[MAX_PATH], file[MAX_PATH], temp[MAX_PATH];
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy(wcsrchr(exe, L'\\') + 1, L"folderwing-shell.exe");
    GetTempPathW(MAX_PATH, temp);
    _snwprintf(file, MAX_PATH, L"%lsopenpilot-pinned-%lu.dat", temp, GetCurrentProcessId());
    HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    CHECK(f != INVALID_HANDLE_VALUE);
    if (f != INVALID_HANDLE_VALUE)
        CloseHandle(f);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE ci = NULL, co = NULL, in = NULL, out = NULL;
    PROCESS_INFORMATION pi = {0};
    CHECK(CreatePipe(&ci, &in, &sa, 65536) && CreatePipe(&out, &co, &sa, 65536));
    if (!in || !out)
        goto done;
    SetHandleInformation(in, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = ci;
    si.hStdOutput = co;
    wchar_t cmd[MAX_PATH + 4];
    _snwprintf(cmd, MAX_PATH + 4, L"\"%ls\"", exe);
    CHECK(CreateProcessW(exe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi));
    if (!pi.hProcess)
        goto done;
    CloseHandle(ci);
    ci = NULL;
    CloseHandle(co);
    co = NULL;
    char path[MAX_PATH * 4], request[2048];
    WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL);
    int n = snprintf(request, sizeof request,
                     "op item\nname An unavailable pinned action\ncount 2\nhandler "
                     "{00000000-0000-0000-0000-000000000000}\nfallback 0\nitem %s\nend\n",
                     path);
    DWORD wrote;
    CHECK(WriteFile(in, request, n, &wrote, NULL) && wrote == (DWORD)n);
    uint64_t until = GetTickCount64() + 3000;
    DWORD bytes = 0;
    while (GetTickCount64() < until) {
        if (!PeekNamedPipe(out, NULL, 0, NULL, &bytes, NULL) || bytes)
            break;
        Sleep(5);
    }
    CHECK(bytes > 0);
    if (bytes) {
        char reply[2048];
        DWORD got = 0;
        ReadFile(out, reply, sizeof reply - 1, &got, NULL);
        reply[got] = 0;
        CHECK(!strncmp(reply, "err ", 4) && strstr(reply, "malformed shell request"));
    }
    n = snprintf(request, sizeof request,
                 "op item\nname An unavailable pinned action\ncount 1\nhandler "
                 "{00000000-0000-0000-0000-000000000000}\nfallback 0\nitem %s\nend\n",
                 path);
    CHECK(WriteFile(in, request, n, &wrote, NULL) && wrote == (DWORD)n);
    until = GetTickCount64() + 3000;
    bytes = 0;
    while (GetTickCount64() < until) {
        if (!PeekNamedPipe(out, NULL, 0, NULL, &bytes, NULL) || bytes)
            break;
        Sleep(5);
    }
    CHECK(bytes > 0);
    if (bytes) {
        char reply[2048];
        DWORD got = 0;
        ReadFile(out, reply, sizeof reply - 1, &got, NULL);
        reply[got] = 0;
        CHECK(!strncmp(reply, "err ", 4) && strstr(reply, "could not be loaded"));
    }
    CloseHandle(in);
    in = NULL;
    CHECK(WaitForSingleObject(pi.hProcess, 3000) == WAIT_OBJECT_0);
done:
    if (pi.hProcess) {
        if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0)
            TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    HANDLE handles[] = {ci, co, in, out};
    for (unsigned i = 0; i < 4; ++i)
        if (handles[i])
            CloseHandle(handles[i]);
    DeleteFileW(file);
}


static void test_bulk_rename_rules(void)
{
    puts("bulk rename rules: literal replacement, extension policy and bounded names");
    rename_rules r={.keep_extension=1,.start=1,.digits=3};wchar_t out[260],shown[1600];uint32_t n;
    CHECK(rename_transform(L"photo.txt",0,0,0,&r,out)&&!wcscmp(out,L"photo.txt"));
    wcscpy(r.find,L"photo");wcscpy(r.replace,L"trip");wcscpy(r.prefix,L"2026_");wcscpy(r.suffix,L"_edited");r.numbering=1;r.start=7;r.digits=4;
    CHECK(rename_transform(L"photo.JPG",0,0,2,&r,out)&&!wcscmp(out,L"2026_trip_edited_0009.JPG"));
    CHECK(rename_transform(L"PHOTO.JPG",0,0,0,&r,out)&&!wcscmp(out,L"2026_PHOTO_edited_0007.JPG"));
    r=(rename_rules){.keep_extension=1};wcscpy(r.suffix,L"-new");
    CHECK(rename_transform(L"folder.txt",1,0,0,&r,out)&&!wcscmp(out,L"folder.txt-new"));
    CHECK(rename_transform(L".env",0,0,0,&r,out)&&!wcscmp(out,L".env-new"));
    CHECK(rename_transform(L"archive.tar.gz",0,0,0,&r,out)&&!wcscmp(out,L"archive.tar-new.gz"));
    r=(rename_rules){.keep_extension=0};wcscpy(r.find,L".txt");wcscpy(r.replace,L".md");
    CHECK(rename_transform(L"readme.txt",0,0,0,&r,out)&&!wcscmp(out,L"readme.md"));
    r.keep_extension=1;CHECK(rename_transform(L"readme.txt",0,0,0,&r,out)&&!wcscmp(out,L"readme.txt"));
    wcscpy(r.find,L"aa");wcscpy(r.replace,L"b");CHECK(rename_transform(L"aaaa.txt",0,0,0,&r,out)&&!wcscmp(out,L"bb.txt"));
    r=(rename_rules){.keep_extension=1};wcscpy(r.find,L":");wcscpy(r.replace,L"_");
    CHECK(rename_transform(L"a\uf03a" L"b.txt",0,1,0,&r,out)&&!wcscmp(out,L"a_b.txt"));
    r=(rename_rules){.keep_extension=1};wcscpy(r.prefix,L"\\");
    CHECK(rename_transform(L"file",0,1,0,&r,out)&&!wcscmp(out,L"\uf05c" L"file"));
    CHECK(!rename_transform(L"file",0,0,0,&r,out));wcscpy(r.prefix,L"/");CHECK(!rename_transform(L"file",0,1,0,&r,out));
    r=(rename_rules){0};wcscpy(r.find,L"file");CHECK(!rename_transform(L"file",0,0,0,&r,out));
    r=(rename_rules){0};for(int i=0;i<127;i++)r.prefix[i]=r.suffix[i]=L'x';
    CHECK(rename_transform(L"a",0,0,0,&r,out)&&wcslen(out)==255);CHECK(!rename_transform(L"abc",0,0,0,&r,out));
    wchar_t unicode[140];for(int i=0;i<129;i++)unicode[i]=0xe9;unicode[129]=0;r=(rename_rules){0};
    CHECK(!rename_transform(unicode,0,1,0,&r,out));CHECK(rename_transform(unicode,0,0,0,&r,out));
    r=(rename_rules){.numbering=1,.digits=0};CHECK(!rename_transform(L"a",0,0,0,&r,out));r.digits=10;CHECK(!rename_transform(L"a",0,0,0,&r,out));
    r.digits=3;r.start=999999999;CHECK(rename_transform(L"a",0,0,0,&r,out)&&!wcscmp(out,L"a_999999999"));CHECK(!rename_transform(L"a",0,0,1,&r,out));
    CHECK(rename_decimal(L"0007",0,999999999,&n)&&n==7);CHECK(!rename_decimal(L"-1",0,999999999,&n));CHECK(!rename_decimal(L"1x",0,999999999,&n));
    CHECK(!rename_decimal(L"1000000000",0,999999999,&n));CHECK(!rename_decimal(L"",0,9,&n));CHECK(!rename_decimal(L"0",1,9,&n));
    rename_display(L"\xd83d\xdc80.txt",1,shown,1600);CHECK(!wcscmp(shown,L"\xd83d\xdc80.txt"));
    rename_display(L"a\uf00a" L"b\xdcff",1,shown,1600);CHECK(!wcscmp(shown,L"a\\u000ab\\udcff"));
}

static void test_linux_name_codec(void)
{
    puts("Linux names: alias collisions, prefix quoting and every Unicode scalar round-trip");
    unsigned pairs=0;wchar_t mapped[260],literal[260],again[260],pretty[1600];char bytes[1024];uint32_t bn;
    for(uint32_t c=1;c<128;c++)if(fpe_lx_mapped(c)){
        wchar_t ascii[]={L'n',(wchar_t)c,0},pua[]={L'n',(wchar_t)(0xf000+c),0};wchar_t a[260],b[260];
        CHECK(linkdialog_name(ascii,a)&&a[1]==0xf000+c&&a[2]==0);
        CHECK(linkdialog_name(pua,b)&&b[1]==0xf080&&b[2]==0xf000+c&&b[3]==0);
        CHECK(wcscmp(a,b)!=0);CHECK(fpe_lx_literal(b,literal,260,0)&&!wcscmp(literal,pua));
        CHECK(pathentry_bytes(pua,bytes,sizeof bytes,&bn));uint8_t native[1024];size_t length=0;
        CHECK(fpe_lx_to_bytes(b,native,sizeof native,0,&length)&&length==bn&&!memcmp(native,bytes,bn));
        rename_rules rules={0};CHECK(rename_transform(b,0,1,0,&rules,again)&&!wcscmp(again,b));
        rules.find[0]=(wchar_t)(0xf000+c);wcscpy(rules.replace,L"X");CHECK(rename_transform(b,0,1,0,&rules,again)&&!wcscmp(again,L"nX"));
        rename_display(b,1,pretty,1600);CHECK(wcsstr(pretty,L"\\uf0")!=NULL);pairs++;
    }
    CHECK(pairs==39);
    wchar_t shown_a[100],shown_b[100];
    fpe_lx_display(L"literal-\uf080\uf03a",shown_a,100,1);
    CHECK(linkdialog_name(L"literal-\\uf03a",mapped));fpe_lx_display(mapped,shown_b,100,1);CHECK(wcscmp(shown_a,shown_b)!=0);
    fpe_lx_display(L"literal-\uf00a",shown_a,100,1);fpe_lx_display(L"literal-\u240a",shown_b,100,1);CHECK(wcscmp(shown_a,shown_b)!=0);
    fpe_lx_display(L"literal-\xdcff",shown_a,100,0);CHECK(linkdialog_name(L"literal-\\udcff",mapped));fpe_lx_display(mapped,shown_b,100,0);CHECK(wcscmp(shown_a,shown_b)!=0);
    /* An independent Windows UTF-8 converter is the round-trip oracle. */
    unsigned bad=0;
    for(uint32_t u=1;u<=0x10ffff;u++){
        if(u>=0xd800&&u<=0xdfff)continue;wchar_t raw[3]={0};int units=1;
        if(u>=0x10000){uint32_t v=u-0x10000;raw[0]=(wchar_t)(0xd800+(v>>10));raw[1]=(wchar_t)(0xdc00+(v&1023));units=2;}else raw[0]=(wchar_t)u;
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,raw,units,bytes,8,NULL,NULL);size_t wn=0,rn=0;uint8_t roundtrip[8];
        if(!n||!fpe_lx_from_bytes((uint8_t*)bytes,n,mapped,260,1,0,&wn)||!fpe_lx_to_bytes(mapped,roundtrip,8,0,&rn)||rn!=(size_t)n||memcmp(roundtrip,bytes,n)||!fpe_lx_literal(mapped,literal,260,0)||wcscmp(literal,raw))bad++;
    }
    CHECK(!bad);
    uint8_t corpus[255],roundtrip[255];for(unsigned i=0;i<255;i++)corpus[i]=(uint8_t)(i+1);size_t wn=0,rn=0;
    CHECK(fpe_lx_from_bytes(corpus,255,mapped,260,1,0,&wn)&&fpe_lx_to_bytes(mapped,roundtrip,255,0,&rn)&&rn==255&&!memcmp(corpus,roundtrip,255));
    CHECK(!fpe_lx_to_bytes(L"\uf080",roundtrip,255,0,&rn));CHECK(!fpe_lx_to_bytes(L"\uf080x",roundtrip,255,0,&rn));
    CHECK(!fpe_lx_to_bytes(L"\xd800",roundtrip,255,0,&rn));CHECK(!fpe_lx_to_bytes(L"\xdd00",roundtrip,255,0,&rn));
    const uint8_t pua[]={0xef,0x80,0xba};wchar_t guard[]={L'X',L'Y',L'Z',L'!'};
    CHECK(!fpe_lx_from_bytes(pua,3,guard,2,1,0,&wn)&&guard[2]==L'Z'&&guard[3]==L'!');
    CHECK(fpe_lx_from_bytes(pua,3,guard,3,1,0,&wn)&&wn==2&&guard[0]==0xf080&&guard[1]==0xf03a&&guard[2]==0&&guard[3]==L'!');
    CHECK(linkdialog_name(L"\uf080\uf03a",mapped)&&!wcscmp(mapped,L"\uf080\uf080\uf080\uf03a"));
    for(int i=0;i<85;i++)literal[i]=0xf03a;literal[85]=0;CHECK(linkdialog_name(literal,mapped)&&wcslen(mapped)==170);
    literal[85]=0xf03a;literal[86]=0;CHECK(!linkdialog_name(literal,mapped));
    const char raw_path[]="/tmp/a\xef\x80\xba/\xef\x82\x80";wchar_t *path=pathentry_unc("Ubuntu",6,raw_path,sizeof raw_path-1);
    CHECK(path&&!wcscmp(path,L"\\\\wsl.localhost\\Ubuntu\\tmp\\a\uf080\uf03a\\\uf080\uf080"));
    if(path){
        wchar_t distro[128],linux[4096],arg[8192],osc[8192];
        CHECK(pathentry_display(path,distro,128,linux,4096)&&!wcscmp(linux,L"/tmp/a\uf03a/\uf080"));
        CHECK(pathentry_bytes(linux,bytes,sizeof bytes,&bn)&&bn==sizeof raw_path-1&&!memcmp(bytes,raw_path,bn));
        CHECK(term_wsl_split(path,distro,128,linux,4096)&&!wcscmp(linux,L"/tmp/a\uf03a/\uf080"));
        CHECK(term_wsl_arg(path,arg,8192)&&!wcscmp(arg,L"x2f746d702f61ef80ba2fef8280"));
        CHECK(!term_wsl_arg(L"\\\\wsl.localhost\\Ubuntu\\a\uf080",arg,8192));
        CHECK(!term_cd_command(TERM_SH_WSL,NULL,L"\\\\wsl.localhost\\Ubuntu\\a\uf080",arg,8192));
        const char report[]="9;wsl:Ubuntu;2f746d702f61ef80ba2fef8280";
        CHECK(term_osc_cwd(9,report,sizeof report-1,osc,8192)&&!wcscmp(path,osc));
        CHECK(fpe_wsl_requires_native(path));CHECK(!fpe_wsl_requires_native(L"C:\\a\uf080\uf03a"));
        CHECK(fpe_wsl_requires_native(L"\\\\?\\UNC\\wsl.localhost\\Ubuntu\\a\uf080\uf03a"));
        fpe_wsl_config off={0};fpe_wsl_configure(&off);CHECK(fpe_wsl_requires_native(path));fpe_wsl_configure(NULL);
        CHECK(shell_create_shortcut(path,L"C:\\unused-f06.lnk")==ERROR_NOT_SUPPORTED);
        session before,after;session_defaults(&before);before.ntabs[0]=1;before.tabs[0][0].path=_wcsdup(path);before.npins=1;before.pins[0]=_wcsdup(path);before.nrecent=1;before.recent[0]=_wcsdup(path);
        char serialized[8192];size_t n=session_format(&before,serialized,sizeof serialized);CHECK(n>0);session_parse(serialized,n,&after);
        CHECK(after.ntabs[0]==1&&!wcscmp(after.tabs[0][0].path,path)&&after.npins==1&&!wcscmp(after.pins[0],path)&&after.nrecent==1&&!wcscmp(after.recent[0],path));session_free(&before);session_free(&after);
        folderviews views={0},restored={0};fv_value value={.mask=FV_MODE,.mode=1,.size=160};CHECK(fv_set(&views,path,value)==1);value.mode=0;CHECK(fv_set(&views,L"\\\\wsl.localhost\\Ubuntu\\tmp\\a\uf03a",value)==1&&views.count==2);
        n=fv_format(&views,serialized,sizeof serialized);CHECK(n>0&&fv_parse(&restored,serialized,n)==0);CHECK(restored.count==2&&fv_get(&restored,path,&value)&&value.mode==1);fv_free(&views);fv_free(&restored);free(path);
    }
}

/* Actual Settings source in the native test process, private HWND/pipe only. */
static HGLOBAL WINAPI settings_clip_alloc(UINT flags,SIZE_T bytes);
static LPVOID WINAPI settings_clip_lock(HGLOBAL h);
static HGLOBAL WINAPI settings_clip_free(HGLOBAL h);
static BOOL WINAPI settings_clip_open(HWND h);
static BOOL WINAPI settings_clip_empty(void);
static BOOL WINAPI settings_clip_close(void);
static BOOL WINAPI settings_clip_available(UINT format);
static HANDLE WINAPI settings_clip_get(UINT format);
static HANDLE WINAPI settings_clip_set(UINT format,HANDLE h);
#define GlobalAlloc settings_clip_alloc
#define GlobalLock settings_clip_lock
#define GlobalFree settings_clip_free
#define OpenClipboard settings_clip_open
#define EmptyClipboard settings_clip_empty
#define CloseClipboard settings_clip_close
#define IsClipboardFormatAvailable settings_clip_available
#define GetClipboardData settings_clip_get
#define SetClipboardData settings_clip_set
#define wWinMain settings_fixture_entry
#include "../app/fpsettings.c"
#undef wWinMain
#undef GlobalAlloc
#undef GlobalLock
#undef GlobalFree
#undef OpenClipboard
#undef EmptyClipboard
#undef CloseClipboard
#undef IsClipboardFormatAvailable
#undef GetClipboardData
#undef SetClipboardData

/* Bounded WinAPI-boundary capsule uses real HGLOBAL memory/ownership. It does
 * not change the desktop clipboard; the explicit live route uses actual APIs. */
static struct {int enabled,opened,busy;HANDLE unicode;} settings_clip_capsule;
static int settings_clip_live;
/* Faults affect only the included test instance, never shipped Settings. */
static struct {
    unsigned alloc_calls,lock_calls,set_calls,fail_alloc,fail_lock,fail_get,fail_open,fail_empty,fail_sets,nhandles;
    unsigned open_calls,empty_calls;int open_ok,empty_ok,set_ok;DWORD open_error,empty_error,set_error;
    HANDLE handles[8];int owned[8];ebox *observed_editor;ebox before;int publish_observed,editor_unchanged_at_publish;
} settings_clip_fault;
static HGLOBAL WINAPI settings_clip_alloc(UINT flags,SIZE_T bytes)
{
    if(++settings_clip_fault.alloc_calls==settings_clip_fault.fail_alloc)return NULL;
    HGLOBAL h=GlobalAlloc(flags,bytes);if(h&&settings_clip_fault.nhandles<8){unsigned i=settings_clip_fault.nhandles++;settings_clip_fault.handles[i]=h;settings_clip_fault.owned[i]=1;}return h;
}
static LPVOID WINAPI settings_clip_lock(HGLOBAL h){if(++settings_clip_fault.lock_calls==settings_clip_fault.fail_lock)return NULL;return GlobalLock(h);}
static HGLOBAL WINAPI settings_clip_free(HGLOBAL h)
{
    HGLOBAL left=GlobalFree(h);if(!left)for(unsigned i=0;i<settings_clip_fault.nhandles;i++)if(settings_clip_fault.handles[i]==h)settings_clip_fault.owned[i]=0;return left;
}
static BOOL WINAPI settings_clip_open(HWND h){if(settings_clip_fault.fail_open)return FALSE;if(settings_clip_capsule.enabled){if(settings_clip_capsule.busy||settings_clip_capsule.opened)return FALSE;settings_clip_capsule.opened=1;return TRUE;}settings_clip_fault.open_calls++;BOOL ok=OpenClipboard(h);settings_clip_fault.open_ok=ok;settings_clip_fault.open_error=ok?0:GetLastError();return ok;}
static BOOL WINAPI settings_clip_close(void){if(settings_clip_capsule.enabled){int opened=settings_clip_capsule.opened;settings_clip_capsule.opened=0;return opened;}return CloseClipboard();}
static BOOL WINAPI settings_clip_available(UINT format){return settings_clip_capsule.enabled?(format==CF_UNICODETEXT&&settings_clip_capsule.unicode!=NULL):IsClipboardFormatAvailable(format);}
static BOOL WINAPI settings_clip_empty(void){if(settings_clip_fault.fail_empty)return FALSE;if(settings_clip_capsule.enabled){if(!settings_clip_capsule.opened)return FALSE;if(settings_clip_capsule.unicode)GlobalFree(settings_clip_capsule.unicode);settings_clip_capsule.unicode=NULL;return TRUE;}settings_clip_fault.empty_calls++;BOOL ok=EmptyClipboard();settings_clip_fault.empty_ok=ok;settings_clip_fault.empty_error=ok?0:GetLastError();return ok;}
static HANDLE WINAPI settings_clip_get(UINT format){if(settings_clip_fault.fail_get)return NULL;return settings_clip_capsule.enabled?(format==CF_UNICODETEXT?settings_clip_capsule.unicode:NULL):GetClipboardData(format);}
static HANDLE WINAPI settings_clip_set(UINT format,HANDLE h)
{
    unsigned call=++settings_clip_fault.set_calls;
    if(settings_clip_fault.observed_editor){settings_clip_fault.publish_observed=1;settings_clip_fault.editor_unchanged_at_publish=!memcmp(settings_clip_fault.observed_editor,&settings_clip_fault.before,sizeof(ebox));}
    if(settings_clip_fault.fail_sets&(1u<<(call-1)))return NULL;
    HANDLE accepted;if(settings_clip_capsule.enabled){accepted=settings_clip_capsule.opened&&format==CF_UNICODETEXT?h:NULL;if(accepted)settings_clip_capsule.unicode=h;}else {accepted=SetClipboardData(format,h);settings_clip_fault.set_ok=accepted!=NULL;settings_clip_fault.set_error=accepted?0:GetLastError();}if(accepted)for(unsigned i=0;i<settings_clip_fault.nhandles;i++)if(settings_clip_fault.handles[i]==h)settings_clip_fault.owned[i]=0;return accepted;
}

static HANDLE settings_fixture_pipe;
static void settings_fixture_snapshot(unsigned revision,int linger)
{
    char snapshot[1800];int len=snprintf(snapshot,sizeof snapshot,
        "rev\t%u\nfile\tC:\\openpilot-owned-settings-fixture.ini\t1\n"
        "fn\tpreview.pdf\tPDF preview\tcold\ton\tCtrl+P\tCtrl+P\tkmp\t\tPDF fixture\n"
        "opt\tlinger\t%d\t5\t%d\t#0..3600\tSeconds\n"
        "opt\tcolors\tmixed\tmixed\t0\tmixed dark original\tColors\n",revision,linger,linger!=5);
    take_snapshot(snapshot,(uint32_t)len);layout();
}
static void settings_fixture_begin(int linger)
{
    memset(&A,0,sizeof A);A.dpi=96;A.q.max=127;A.fe.max=255;
    A.rec_ri=A.msg_ri=A.conf.ri=-1;A.kf=mk(H_SEARCH,-1,-1,0);A.hover=mk(-1,-1,-1,0);
    CHECK(CreatePipe(&settings_fixture_pipe,&A.out,NULL,65536));
    A.hwnd=CreateWindowExW(0,L"STATIC",L"Settings input fixture",WS_POPUP,0,0,0,0,NULL,NULL,GetModuleHandleW(NULL),NULL);CHECK(A.hwnd!=NULL);
    make_fonts();pixels_map(NULL,1100,900);settings_fixture_snapshot(17,linger);
    CHECK(A.s.n==1&&A.s.recs[0].nopts==2&&A.nhot>4);
}
static void settings_fixture_end(void)
{
    CHECK(DestroyWindow(A.hwnd));snap_free(&A.s);measured_clear();
    if(A.mdc)DeleteDC(A.mdc);if(A.back)DeleteObject(A.back);
    HFONT fonts[]={A.f_text,A.f_bold,A.f_small,A.f_mono,A.f_head};
    for(unsigned i=0;i<sizeof fonts/sizeof *fonts;i++)if(fonts[i])DeleteObject(fonts[i]);
    CloseHandle(A.out);CloseHandle(settings_fixture_pipe);memset(&A,0,sizeof A);
}
static int settings_fixture_message(uint32_t type,char *body,DWORD cap)
{
    DWORD available=0,read=0;panelp_head h;
    CHECK(PeekNamedPipe(settings_fixture_pipe,NULL,0,NULL,&available,NULL)&&available>=sizeof h);
    if(available<sizeof h)return 0;
    CHECK(ReadFile(settings_fixture_pipe,&h,sizeof h,&read,NULL)&&read==sizeof h);
    CHECK(h.type==type&&h.size<cap);if(h.size>=cap)return 0;
    CHECK(ReadFile(settings_fixture_pipe,body,h.size,&read,NULL)&&read==h.size);body[h.size]=0;return (int)h.size;
}
static void settings_fixture_empty(void)
{
    DWORD available=99;CHECK(PeekNamedPipe(settings_fixture_pipe,NULL,0,NULL,&available,NULL)&&available==0);
}
static void settings_fixture_click(hid id)
{
    const hot *h=find_hot(id);CHECK(h!=NULL);if(!h)return;
    panelp_input click={.msg=WM_LBUTTONDOWN,.x=(h->r.left+h->r.right)/2,.y=(h->r.top+h->r.bottom)/2+top_h()-A.scroll};
    const hot *at=hit(click.x,click.y);CHECK(at&&hid_eq(at->id,id));input(&click);
}
static void test_settings_click_unchanged_field_keeps_action(void)
{
    puts("Settings click: unchanged field keeps original logical choice across Reset insertion");
    for(int linger=5;linger<=7;linger+=2){
        settings_fixture_begin(linger);settings_fixture_click(mk(H_FIELD,0,0,0));CHECK(A.f_open&&!A.f_saving);
        CHECK(!find_hot(mk(H_RESET,0,0,0)));settings_fixture_empty();
        settings_fixture_click(mk(H_CHOICE,0,1,2));
        char body[1600];CHECK(settings_fixture_message(PANELP_SET,body,sizeof body)>0);
        CHECK(!strcmp(body,"1\t17\npreview.pdf\tcolors\tset\toriginal"));
        CHECK(!A.f_open&&A.wait_req==1&&hid_eq(A.kf,mk(H_CHOICE,0,1,2)));
        CHECK((find_hot(mk(H_RESET,0,0,0))!=NULL)==(linger!=5));
        CHECK(!wcscmp(A.s.recs[0].opts[0].value,linger==5?L"5":L"7"));settings_fixture_empty();settings_fixture_end();
    }
}
static void test_settings_click_changed_field_queues_original_action(void)
{
    puts("Settings click: changed field queues original action after matching save/snapshot");
    settings_fixture_begin(7);settings_fixture_click(mk(H_FIELD,0,0,0));
    panelp_input typed_eight={.msg=WM_CHAR,.wparam='8'};input(&typed_eight);CHECK(!wcscmp(A.fe.t,L"8"));
    settings_fixture_click(mk(H_CHOICE,0,1,2));char body[1600];
    CHECK(settings_fixture_message(PANELP_SET,body,sizeof body)>0);CHECK(!strcmp(body,"1\t17\npreview.pdf\tlinger\tset\t8"));
    CHECK(A.f_saving&&A.queued.used&&A.queued.n==1&&!A.queued.field);
    CHECK(!wcscmp(A.queued.item[0].sec,L"preview.pdf")&&!wcscmp(A.queued.item[0].key,L"colors")&&!wcscmp(A.queued.item[0].val,L"original"));
    settings_fixture_empty();settings_fixture_snapshot(18,8);saved("1\tok\t",6);
    CHECK(settings_fixture_message(PANELP_SET,body,sizeof body)>0);CHECK(!strcmp(body,"2\t18\npreview.pdf\tcolors\tset\toriginal"));
    CHECK(!A.f_open&&A.completed==1&&A.save_status==1&&A.wait_req==2);settings_fixture_empty();settings_fixture_end();
}
static void test_settings_click_record_cancel_keeps_action(void)
{
    puts("Settings click: recorder cancellation releases capture and retains intended choice");
    settings_fixture_begin(7);settings_fixture_click(mk(H_KEY_ADD,0,-1,0));
    char body[1600];CHECK(settings_fixture_message(PANELP_CAPTURE,body,sizeof body)==4);uint32_t captured=0;memcpy(&captured,body,4);CHECK(captured==1&&A.rec_ri==0);
    settings_fixture_click(mk(H_CHOICE,0,1,2));
    CHECK(settings_fixture_message(PANELP_CAPTURE,body,sizeof body)==4);memcpy(&captured,body,4);CHECK(captured==0&&A.rec_ri==-1);
    CHECK(settings_fixture_message(PANELP_SET,body,sizeof body)>0);CHECK(!strcmp(body,"1\t17\npreview.pdf\tcolors\tset\toriginal"));
    CHECK(hid_eq(A.kf,mk(H_CHOICE,0,1,2)));settings_fixture_empty();settings_fixture_end();
}

/* N10 exercises the shipped parser, recorder, layout and input path through
 * its actual outbound pipe; no mirrored snapshot state machine. */
static void settings_reload_snapshot(unsigned revision,const char *first,const char *second,int swapped_options,int changed_choices,int changed_keys)
{
    char body[4000];int at=snprintf(body,sizeof body,"rev\t%u\nfile\tC:\\openpilot-owned-settings-fixture.ini\t1\n",revision);
    const char *ids[]={first,second};
    for(int i=0;i<2&&ids[i];i++){
        at+=snprintf(body+at,sizeof body-(size_t)at,"fn\t%s\tFixture %d\tcold\ton\t%s\tCtrl+P\tkmp\t\tReload fixture\n",ids[i],i,changed_keys?"Ctrl+J":i?"Ctrl+K":"Ctrl+P");
        const char *field="opt\tlinger\t7\t5\t1\t#0..3600\tSeconds\n";
        const char *choice=changed_choices?"opt\tcolors\tmixed\tmixed\t0\toriginal dark mixed\tColors\n":"opt\tcolors\tmixed\tmixed\t0\tmixed dark original\tColors\n";
        at+=snprintf(body+at,sizeof body-(size_t)at,"%s%s",swapped_options?choice:field,swapped_options?field:choice);
    }
    take_snapshot(body,(uint32_t)at);layout();
}
static void settings_reload_capture(unsigned expected)
{
    char body[80];uint32_t capture=99;int size=settings_fixture_message(PANELP_CAPTURE,body,sizeof body);
    CHECK(size==4);if(size==4)memcpy(&capture,body,4);CHECK(capture==expected);
}
static void settings_reload_key(UINT vk,uint32_t mods)
{
    panelp_input in={.msg=WM_KEYDOWN,.wparam=vk,.mods=mods};input(&in);
}
static void test_settings_reload_replacement_releases_recorder(void)
{
    puts("Settings reload: same-count relay/function replacement cancels capture and cannot acquire old key input");
    const char *old[]={"relay.old","function.old"};const char *replacement[]={"relay.new","function.new"};
    for(int i=0;i<2;i++){
        settings_fixture_begin(7);settings_reload_snapshot(18,old[i],"other.stable",0,0,0);
        settings_fixture_click(mk(H_FIELD,0,0,0));CHECK(A.f_open);
        settings_fixture_click(mk(H_KEY_ADD,0,-1,0));CHECK(A.rec_ri==0&&!A.f_open);settings_reload_capture(1);
        settings_reload_snapshot(19,replacement[i],"other.stable",0,0,0);
        CHECK(A.rec_ri==-1);settings_reload_capture(0);CHECK(hid_eq(A.kf,mk(H_SEARCH,-1,-1,0)));
        settings_reload_key('J',PANELP_M_CTRL);CHECK(!A.wait_req&&!A.pending);settings_fixture_empty();settings_fixture_end();
    }
}
static void test_settings_reload_count_and_unchanged_release_capture(void)
{
    puts("Settings reload: changed-count and unchanged snapshots both release active recorder exactly once");
    for(int unchanged=0;unchanged<2;unchanged++){
        settings_fixture_begin(7);settings_reload_snapshot(18,"function.first","function.second",0,0,0);
        record_start(1);settings_reload_capture(1);
        settings_reload_snapshot(19,"function.first",unchanged?"function.second":NULL,0,0,0);
        CHECK(A.s.n==(unchanged?2:1)&&A.rec_ri==-1);settings_reload_capture(0);settings_fixture_empty();
        settings_reload_snapshot(20,"function.first",unchanged?"function.second":NULL,0,0,0);settings_fixture_empty();settings_fixture_end();
    }
}
static void test_settings_reload_clears_conflict_and_message_targets(void)
{
    puts("Settings reload: conflict, other-record indexes and key messages never survive same-count replacement");
    settings_fixture_begin(7);settings_reload_snapshot(18,"function.first","function.second",0,0,0);
    record_start(0);settings_reload_capture(1);settings_reload_key('K',PANELP_M_CTRL);settings_reload_capture(0);
    CHECK(A.conf.ri==0&&A.conf.n==1&&A.conf.others[0]==1&&A.conf.name[0]);CHECK(A.kf.kind==H_CONF_MOVE);
    key_message(1,L"Old record message");A.hover=mk(H_CONF_ADD,0,-1,0);settings_fixture_empty();
    settings_reload_snapshot(19,"replacement.first","replacement.second",0,0,0);
    CHECK(A.conf.ri==-1&&A.conf.n==0&&!A.conf.name[0]);CHECK(A.msg_ri==-1&&!A.key_msg[0]);
    for(int i=0;i<16;i++)CHECK(A.conf.others[i]==0);
    CHECK(A.kf.kind==H_SEARCH&&A.hover.kind==-1);CHECK(!find_hot(mk(H_CONF_ADD,0,-1,0))&&!find_hot(mk(H_CONF_MOVE,0,-1,0)));
    settings_reload_key(VK_RETURN,0);settings_fixture_empty();settings_fixture_end();
}
static void test_settings_reload_remaps_control_identity(void)
{
    puts("Settings reload: focus/hover follow record and option IDs through reorder, invalidate changed sublists/removal");
    settings_fixture_begin(7);settings_reload_snapshot(18,"function.first","function.second",0,0,0);
    A.kf=mk(H_CHOICE,0,1,2);A.hover=mk(H_FIELD,1,0,0);A.hover_edit=1;
    settings_reload_snapshot(19,"function.second","function.first",1,0,0);
    CHECK(hid_eq(A.kf,mk(H_CHOICE,1,0,2)));CHECK(hid_eq(A.hover,mk(H_FIELD,0,1,0))&&!A.hover_edit);
    settings_reload_key(VK_RETURN,0);char body[1600];CHECK(settings_fixture_message(PANELP_SET,body,sizeof body)>0);
    CHECK(!strcmp(body,"1\t19\nfunction.first\tcolors\tset\toriginal"));settings_fixture_empty();
    /* Resolve the normal save before testing independent identity invalidation. */
    saved("1\tok\t",6);
    A.kf=mk(H_CHOICE,1,0,2);A.hover=mk(H_KEY_DEL,0,-1,0);
    settings_reload_snapshot(20,"function.second","function.first",1,1,1);
    CHECK(A.kf.kind==H_SEARCH&&A.hover.kind==-1);settings_fixture_empty();
    A.kf=mk(H_KEY_ADD,1,-1,0);A.hover=mk(H_PILL,1,-1,0);
    settings_reload_snapshot(21,"function.second",NULL,1,1,1);
    CHECK(A.kf.kind==H_SEARCH&&A.hover.kind==-1);settings_fixture_empty();settings_fixture_end();
}
static void test_settings_recording_without_reload_still_saves(void)
{
    puts("Settings recorder control: ordinary recording still publishes exact current record/revision");
    settings_fixture_begin(7);record_start(0);settings_reload_capture(1);
    settings_reload_key('J',PANELP_M_CTRL);settings_reload_capture(0);char body[1600];
    CHECK(settings_fixture_message(PANELP_SET,body,sizeof body)>0);CHECK(!strcmp(body,"1\t17\nkeys\tpreview.pdf\tset\tCtrl+P, Ctrl+J"));
    CHECK(A.rec_ri==-1&&A.wait_req==1);settings_fixture_empty();settings_fixture_end();
}

/* Only fixture acquisition may wait for a busy system clipboard. A completed
 * mismatch is final; product Cut is never retried by this harness. */
static int settings_clipboard_open_fixture(HWND owner)
{
    uint64_t deadline=GetTickCount64()+1500;
    do{if(OpenClipboard(owner))return 1;Sleep(2);}while(GetTickCount64()<deadline);
    printf("CLIPBOARD_FIXTURE unavailable error=%lu\n",GetLastError());CHECK(0);return 0;
}
/* Copy the caller's clipboard formats in memory only; never log their data.
 * Unsupported private/GDI ownership formats fail the fixture before mutation.
 * Restoration preserves duplicable data, not clipboard owner or sequence. */
typedef struct settings_clipboard_backup { UINT format[256];HANDLE data[256];unsigned count; } settings_clipboard_backup;
static void settings_clipboard_free(UINT format,HANDLE h)
{
    if(!h)return;
    if(format==CF_BITMAP||format==CF_PALETTE)DeleteObject(h);
    else if(format==CF_ENHMETAFILE)DeleteEnhMetaFile(h);
    else if(format==CF_METAFILEPICT){METAFILEPICT *p=GlobalLock(h);if(p){DeleteMetaFile(p->hMF);GlobalUnlock(h);}GlobalFree(h);}
    else GlobalFree(h);
}
static int settings_clipboard_backup_begin(settings_clipboard_backup *b)
{
    memset(b,0,sizeof *b);if(!settings_clipboard_open_fixture(A.hwnd))return 0;
    UINT format=0;int ok=1;SetLastError(0);
    while((format=EnumClipboardFormats(format))!=0){
        if(b->count==256||format==CF_OWNERDISPLAY||(format>=CF_PRIVATEFIRST&&format<=CF_PRIVATELAST)||(format>=CF_GDIOBJFIRST&&format<=CF_GDIOBJLAST)){ok=0;break;}
        HANDLE original=GetClipboardData(format),copy=original?OleDuplicateData(original,(CLIPFORMAT)format,GMEM_MOVEABLE):NULL;
        if(!copy){ok=0;break;}b->format[b->count]=format;b->data[b->count++]=copy;SetLastError(0);
    }
    if(!format&&GetLastError())ok=0;CloseClipboard();CHECK(ok);
    if(!ok){for(unsigned i=0;i<b->count;i++)settings_clipboard_free(b->format[i],b->data[i]);b->count=0;}
    return ok;
}
static void settings_clipboard_backup_end(settings_clipboard_backup *b)
{
    int opened=settings_clipboard_open_fixture(A.hwnd);CHECK(opened);
    if(opened){CHECK(EmptyClipboard());for(unsigned i=0;i<b->count;i++){HANDLE h=SetClipboardData(b->format[i],b->data[i]);CHECK(h!=NULL);if(h)b->data[i]=NULL;}CHECK(CloseClipboard());}
    for(unsigned i=0;i<b->count;i++)settings_clipboard_free(b->format[i],b->data[i]);
}
static const wchar_t settings_clip_prior[]=L"previous clipboard \x03b2";
static void settings_clipboard_seed(void)
{
    HGLOBAL g=GlobalAlloc(GMEM_MOVEABLE,sizeof settings_clip_prior);void *p=g?GlobalLock(g):NULL;CHECK(p!=NULL);
    if(!p){if(g)GlobalFree(g);return;}memcpy(p,settings_clip_prior,sizeof settings_clip_prior);GlobalUnlock(g);
    if(settings_clip_capsule.enabled){if(settings_clip_capsule.unicode)GlobalFree(settings_clip_capsule.unicode);settings_clip_capsule.unicode=g;return;}
    if(!settings_clipboard_open_fixture(A.hwnd)){GlobalFree(g);return;}CHECK(EmptyClipboard());CHECK(SetClipboardData(CF_UNICODETEXT,g)!=NULL);CHECK(CloseClipboard());
}
static void settings_clipboard_prior(void)
{
    if(!settings_clip_capsule.enabled&&!settings_clipboard_open_fixture(A.hwnd))return;
    HANDLE h=settings_clip_capsule.enabled?settings_clip_capsule.unicode:GetClipboardData(CF_UNICODETEXT);const void *p=h?GlobalLock(h):NULL;
    CHECK(h&&GlobalSize(h)>=sizeof settings_clip_prior&&p&&!memcmp(p,settings_clip_prior,sizeof settings_clip_prior));if(p)GlobalUnlock(h);if(!settings_clip_capsule.enabled)CHECK(CloseClipboard());
}
static int settings_clipboard_selected(void)
{
    if(!settings_clip_capsule.enabled&&!settings_clipboard_open_fixture(A.hwnd))return 0;
    HANDLE h=settings_clip_capsule.enabled?settings_clip_capsule.unicode:GetClipboardData(CF_UNICODETEXT);const wchar_t *p=h?GlobalLock(h):NULL;
    int exact=p&&GlobalSize(h)>=(wcslen(L"\x03b2" L"eta")+1)*sizeof(wchar_t)&&!wcscmp(p,L"\x03b2" L"eta");
    CHECK(exact);if(p)GlobalUnlock(h);if(!settings_clip_capsule.enabled)CHECK(CloseClipboard());return exact;
}
static ebox *settings_cut_editor(int context,int reverse)
{
    ebox *e;
    if(context==0){settings_fixture_click(mk(H_FIELD,0,0,0));e=&A.fe;}
    else if(context==1){A.kf=mk(H_SEARCH,-1,-1,0);e=&A.q;}
    else{builder_open();char body[16];CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);e=&A.br.fields[BF_TITLE];}
    eb_set(e,L"left \x03b2" L"eta right");e->sel=reverse?9:5;e->caret=reverse?5:9;layout();return e;
}
static void settings_cut_key(void){panelp_input cut={.msg=WM_KEYDOWN,.wparam='X',.mods=PANELP_M_CTRL};input(&cut);}
static void settings_cut_no_leak(void){for(unsigned i=0;i<settings_clip_fault.nhandles;i++)CHECK(!settings_clip_fault.owned[i]);}
static HANDLE settings_cut_held,settings_cut_release;
static HWND settings_cut_holder_window;
static DWORD WINAPI settings_cut_holder(void *unused)
{
    (void)unused;HWND hwnd=CreateWindowExW(0,L"STATIC",L"Owned clipboard holder",WS_POPUP,0,0,0,0,NULL,NULL,GetModuleHandleW(NULL),NULL);
    uint64_t deadline=GetTickCount64()+1500;int opened=0;
    if(hwnd)do{if((opened=OpenClipboard(hwnd)))break;Sleep(2);}while(GetTickCount64()<deadline);
    if(!opened){if(hwnd)DestroyWindow(hwnd);SetEvent(settings_cut_held);return 1;}
    settings_cut_holder_window=hwnd;SetEvent(settings_cut_held);DWORD waited=WaitForSingleObject(settings_cut_release,5000);CloseClipboard();DestroyWindow(hwnd);return waited==WAIT_OBJECT_0?0:2;
}
static void settings_cut_busy_case(void)
{
    puts("Settings Cut: busy boundary preserves every editor's draft/selection and prior Unicode");
    settings_fixture_begin(7);settings_clipboard_seed();memset(&settings_clip_fault,0,sizeof settings_clip_fault);
    HANDLE thread=NULL;
    if(settings_clip_live){
        settings_cut_held=CreateEventW(NULL,TRUE,FALSE,NULL);settings_cut_release=CreateEventW(NULL,TRUE,FALSE,NULL);
        thread=CreateThread(NULL,0,settings_cut_holder,NULL,0,NULL);CHECK(thread&&WaitForSingleObject(settings_cut_held,3000)==WAIT_OBJECT_0);
        DWORD held=99;if(thread)GetExitCodeThread(thread,&held);CHECK(held==STILL_ACTIVE);CHECK(GetOpenClipboardWindow()==settings_cut_holder_window);
    }else settings_clip_capsule.busy=1;
    int unexpected=settings_clip_open(A.hwnd);CHECK(!unexpected);if(unexpected)settings_clip_close();
    for(int context=0;context<3;context++){
        A.f_open=A.f_saving=A.br.open=0;A.kf=mk(H_SEARCH,-1,-1,0);eb_set(&A.q,L"");layout();
        ebox *e=settings_cut_editor(context,context&1),before=*e;memset(&settings_clip_fault,0,sizeof settings_clip_fault);
        settings_cut_key();CHECK(!memcmp(e,&before,sizeof before));CHECK(A.notice_err&&A.notice[0]);settings_cut_no_leak();
    }
    if(settings_clip_live){SetEvent(settings_cut_release);CHECK(thread&&WaitForSingleObject(thread,3000)==WAIT_OBJECT_0);DWORD code=99;if(thread)GetExitCodeThread(thread,&code);CHECK(code==0);if(thread)CloseHandle(thread);CloseHandle(settings_cut_held);CloseHandle(settings_cut_release);}
    else settings_clip_capsule.busy=0;
    settings_clipboard_prior();if(settings_clip_live)puts("LIVE_CASE busy editors=3 holder=owned source=draft-retained");settings_fixture_end();
}
static void settings_cut_failures_case(void)
{
    puts("Settings Cut: allocation/lock/read/empty/publication failures retain drafts and previous Unicode bytes");
    settings_fixture_begin(7);
    for(int context=0;context<3;context++)for(int fault=1;fault<=9;fault++){
        A.f_open=A.f_saving=A.br.open=0;A.kf=mk(H_SEARCH,-1,-1,0);eb_set(&A.q,L"");A.notice[0]=0;layout();
        ebox *e=settings_cut_editor(context,fault&1),before=*e;settings_clipboard_seed();memset(&settings_clip_fault,0,sizeof settings_clip_fault);
        if(fault==1)settings_clip_fault.fail_alloc=1;else if(fault==2)settings_clip_fault.fail_lock=1;
        else if(fault==3)settings_clip_fault.fail_alloc=2;else if(fault==4)settings_clip_fault.fail_lock=2;
        else if(fault==5)settings_clip_fault.fail_lock=3;else if(fault==6)settings_clip_fault.fail_get=1;
        else if(fault==7)settings_clip_fault.fail_open=1;else if(fault==8)settings_clip_fault.fail_empty=1;else settings_clip_fault.fail_sets=1;
        settings_cut_key();CHECK(!memcmp(e,&before,sizeof before));CHECK(A.notice_err&&A.notice[0]);settings_cut_no_leak();settings_clipboard_prior();
    }
    memset(&settings_clip_fault,0,sizeof settings_clip_fault);settings_fixture_end();
}
static void settings_cut_success_case(void)
{
    puts("Settings Cut: exact Unicode selection accepted before text removal, normal and reverse selections");
    int failed_before=g_failed;settings_fixture_begin(7);
    for(int context=0;context<3;context++)for(int reverse=0;reverse<2;reverse++){
        A.f_open=A.f_saving=A.br.open=0;A.kf=mk(H_SEARCH,-1,-1,0);eb_set(&A.q,L"");A.notice[0]=0;layout();
        ebox *e=settings_cut_editor(context,reverse);settings_clipboard_seed();memset(&settings_clip_fault,0,sizeof settings_clip_fault);settings_clip_fault.observed_editor=e;settings_clip_fault.before=*e;
        DWORD seq_before=GetClipboardSequenceNumber();settings_cut_key();
        int text_ok=!wcscmp(e->t,L"left  right");CHECK(text_ok&&e->len==11&&e->caret==5&&e->sel==5);
        CHECK(settings_clip_fault.publish_observed&&settings_clip_fault.editor_unchanged_at_publish);settings_cut_no_leak();int exact=settings_clipboard_selected();
        if(settings_clip_live)printf("CUT_TRACE context=%d reverse=%d text_exact=%d len=%d caret=%d sel=%d publish_seen=%d unchanged_at_publish=%d open_calls=%u open_ok=%d open_error=%lu empty_calls=%u empty_ok=%d empty_error=%lu set_calls=%u set_ok=%d set_error=%lu clipboard_exact=%d sequence_before=%lu sequence_after=%lu owner=%p holder=%p\n",context,reverse,text_ok,e->len,e->caret,e->sel,settings_clip_fault.publish_observed,settings_clip_fault.editor_unchanged_at_publish,settings_clip_fault.open_calls,settings_clip_fault.open_ok,settings_clip_fault.open_error,settings_clip_fault.empty_calls,settings_clip_fault.empty_ok,settings_clip_fault.empty_error,settings_clip_fault.set_calls,settings_clip_fault.set_ok,settings_clip_fault.set_error,exact,seq_before,GetClipboardSequenceNumber(),(void*)GetClipboardOwner(),(void*)GetOpenClipboardWindow());
    }
    memset(&settings_clip_fault,0,sizeof settings_clip_fault);if(settings_clip_live)printf("LIVE_CASE %s editors=3 selections=6 publication=before-removal failures=%d\n",g_failed==failed_before?"passed":"failed",g_failed-failed_before);settings_fixture_end();
}
static void settings_cut_restore_case(void)
{
    puts("Settings Cut: failed clipboard restoration is explicit; draft and selection still retained");
    settings_fixture_begin(7);
    ebox *e=settings_cut_editor(0,0),before=*e;settings_clipboard_seed();memset(&settings_clip_fault,0,sizeof settings_clip_fault);settings_clip_fault.fail_sets=3;
    settings_cut_key();CHECK(!memcmp(e,&before,sizeof before));CHECK(settings_clip_fault.set_calls==2);CHECK(A.notice_err&&wcsstr(A.notice,L"could not be restored"));settings_cut_no_leak();
    CHECK(settings_clip_capsule.enabled&&!settings_clip_capsule.unicode);
    memset(&settings_clip_fault,0,sizeof settings_clip_fault);settings_fixture_end();
}

static void settings_cut_component(void (*run)(void))
{
    memset(&settings_clip_capsule,0,sizeof settings_clip_capsule);settings_clip_capsule.enabled=1;run();
    if(settings_clip_capsule.unicode)GlobalFree(settings_clip_capsule.unicode);
    memset(&settings_clip_capsule,0,sizeof settings_clip_capsule);memset(&settings_clip_fault,0,sizeof settings_clip_fault);
}
static void test_settings_cut_busy_boundary_preserves_all_editors(void){settings_cut_component(settings_cut_busy_case);}
static void test_settings_cut_api_failures_preserve_drafts_and_previous_unicode(void){settings_cut_component(settings_cut_failures_case);}
static void test_settings_cut_success_publishes_before_removing_selection(void){settings_cut_component(settings_cut_success_case);}
static void test_settings_cut_failed_restore_keeps_draft_and_reports_failure(void){settings_cut_component(settings_cut_restore_case);}

static void settings_paste_targets(void)
{
    static const char snapshot[]="rev\t18\nfile\tC:\\openpilot-owned-paste-fixture.ini\t1\n"
        "fn\tpreview.pdf\tPDF preview\tcold\ton\tCtrl+P\tCtrl+P\tkmp\t\tPDF fixture\nopt\tlinger\t5\t5\t0\t#0..3600\tSeconds\n"
        "fn\tpreview.image\tImage preview\tcold\ton\tCtrl+I\tCtrl+I\tkmp\t\tImage fixture\nopt\tlinger\t5\t5\t0\t#0..3600\tSeconds\n";
    take_snapshot(snapshot,sizeof snapshot-1);layout();CHECK(A.s.n==2);
}
static void settings_paste_key(void){panelp_input paste={.msg=WM_KEYDOWN,.wparam='V',.mods=PANELP_M_CTRL};input(&paste);}
static void settings_paste_open_field(int index){field_open(&A.s.recs[index],&A.s.recs[index].opts[0]);CHECK(A.f_open&&!wcscmp(A.fe.t,L"5")&&A.fe.caret==1&&A.fe.sel==0);}
static void settings_paste_begin(void){settings_fixture_begin(5);settings_paste_targets();settings_clipboard_seed();settings_paste_open_field(0);}
static void settings_paste_equal_target_case(void)
{
    puts("Settings paste: equal-text PDF/image target switch and close/reopen cancel old request; new request survives");
    for(int reopen=0;reopen<2;reopen++){
        settings_paste_begin();settings_clip_capsule.busy=1;settings_paste_key();CHECK(A.paste.target==&A.fe);
        if(reopen)field_close();settings_paste_open_field(reopen?0:1);ebox before=A.fe;
        settings_clip_capsule.busy=0;paste_tick();CHECK(!memcmp(&A.fe,&before,sizeof before)&&!A.paste.target);
        /* A second request in the new lifetime must not be erased by stale cancellation. */
        settings_clip_capsule.busy=1;settings_paste_key();CHECK(A.paste.target==&A.fe);paste_tick();CHECK(A.paste.target==&A.fe);
        settings_clip_capsule.busy=0;paste_tick();CHECK(!wcscmp(A.fe.t,settings_clip_prior)&&!A.paste.target);ebox once=A.fe;paste_tick();CHECK(!memcmp(&A.fe,&once,sizeof once));
        settings_fixture_empty();settings_fixture_end();
    }
}
static void settings_paste_same_editor_case(void)
{
    puts("Settings paste: unchanged ordinary/search/builder editors retry once; explicit immediate paste supersedes old request");
    for(int context=0;context<3;context++){
        settings_paste_begin();ebox *e=&A.fe;
        if(context==1){field_close();A.kf=mk(H_SEARCH,-1,-1,0);e=&A.q;eb_set(e,L"5");}
        else if(context==2){builder_open();char body[32];CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);e=&A.br.fields[BF_TITLE];eb_set(e,L"5");}
        settings_clip_capsule.busy=1;settings_paste_key();ebox before=*e;CHECK(A.paste.target==e);paste_tick();CHECK(A.paste.target==e&&!memcmp(e,&before,sizeof before));
        settings_clip_capsule.busy=0;paste_tick();CHECK(!A.paste.target&&!wcscmp(e->t,settings_clip_prior));CHECK(e->caret==e->len&&e->sel==e->len);ebox once=*e;paste_tick();CHECK(!memcmp(e,&once,sizeof once));
        eb_set(e,L"5");settings_clip_capsule.busy=1;settings_paste_key();CHECK(A.paste.target==e);settings_clip_capsule.busy=0;settings_paste_key();CHECK(!A.paste.target&&!wcscmp(e->t,settings_clip_prior));once=*e;paste_tick();CHECK(!memcmp(e,&once,sizeof once));
        settings_fixture_empty();settings_fixture_end();
    }
}
static void settings_paste_reload_case(void)
{
    puts("Settings paste: accepted unchanged reload and removed target invalidate reused equal-text field");
    for(int remove=0;remove<2;remove++){
        settings_paste_begin();settings_clip_capsule.busy=1;settings_paste_key();CHECK(A.paste.target==&A.fe);
        if(remove){static const char replacement[]="rev\t19\nfile\tC:\\openpilot-owned-paste-fixture.ini\t1\nfn\tpreview.image\tImage\tcold\ton\tCtrl+I\tCtrl+I\tkmp\t\tImage\nopt\tlinger\t5\t5\t0\t#0..3600\tSeconds\n";take_snapshot(replacement,sizeof replacement-1);layout();CHECK(!A.f_open);settings_paste_open_field(0);}
        else settings_paste_targets();
        ebox before=A.fe;settings_clip_capsule.busy=0;paste_tick();CHECK(!memcmp(&A.fe,&before,sizeof before)&&!A.paste.target);settings_fixture_empty();settings_fixture_end();
    }
}
static void settings_paste_builder_case(void)
{
    puts("Settings paste: builder close/open/rebind/edit and ordinary-field to builder lifetimes cancel reused storage");
    for(int mode=0;mode<4;mode++){
        settings_paste_begin();builder_open();char body[40];CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);
        if(mode==3){static const char relays[]="rev\t19\nfile\tC:\\openpilot-owned-paste-fixture.ini\t1\nfn\trelay.old\t5\tcold\ton\tnone\tnone\tkmp\t\tOld\nopt\trun\tnotepad.exe\t\t0\t\tProgram\nfn\trelay.new\t5\tcold\ton\tnone\tnone\tkmp\t\tNew\nopt\trun\tnotepad.exe\t\t0\t\tProgram\n";take_snapshot(relays,sizeof relays-1);layout();CHECK(!builder_edit(&A.s.recs[0]));CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);}
        else {strcpy(A.br.draft.title,"5");builder_to_fields();}
        A.br.focus=BF_TITLE;A.kf=mk(H_B_FIELD,-1,BF_TITLE,0);CHECK(!wcscmp(A.br.fields[BF_TITLE].t,L"5"));settings_clip_capsule.busy=1;settings_paste_key();CHECK(A.paste.target==&A.br.fields[BF_TITLE]);
        if(mode==0){builder_close();builder_open();CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);eb_set(&A.br.fields[BF_TITLE],L"5");}
        else if(mode==1){builder_open();CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);eb_set(&A.br.fields[BF_TITLE],L"5");}
        else if(mode==2){strcpy(A.br.draft.name,"another-command");builder_to_fields();}
        else {CHECK(!builder_edit(&A.s.recs[1]));CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);}
        ebox before=A.br.fields[BF_TITLE];settings_clip_capsule.busy=0;paste_tick();CHECK(!memcmp(&A.br.fields[BF_TITLE],&before,sizeof before)&&!A.paste.target);settings_fixture_empty();settings_fixture_end();
    }
    settings_paste_begin();settings_clip_capsule.busy=1;settings_paste_key();builder_open();char body[40];CHECK(settings_fixture_message(PANELP_RELAY_GET_CONTEXT,body,sizeof body)==4);CHECK(!A.f_open&&!A.paste.target);settings_clip_capsule.busy=0;paste_tick();settings_fixture_end();
}
static void settings_paste_guard_case(void)
{
    puts("Settings paste: timeout/text/caret/selection guards remain additive");
    for(int guard=0;guard<4;guard++){
        settings_paste_begin();settings_clip_capsule.busy=1;settings_paste_key();CHECK(A.paste.target==&A.fe);
        if(guard==0)A.paste.until=GetTickCount64();else if(guard==1)eb_set(&A.fe,L"6");else if(guard==2)A.fe.caret=0;else A.fe.sel=1;
        ebox before=A.fe;settings_clip_capsule.busy=0;paste_tick();CHECK(!memcmp(&A.fe,&before,sizeof before)&&!A.paste.target);settings_fixture_empty();settings_fixture_end();
    }
}
static void test_settings_paste_equal_text_target_and_reopen_cancel(void){settings_cut_component(settings_paste_equal_target_case);}
static void test_settings_paste_same_editor_retry_and_new_request(void){settings_cut_component(settings_paste_same_editor_case);}
static void test_settings_paste_accepted_reload_and_removal_cancel(void){settings_cut_component(settings_paste_reload_case);}
static void test_settings_paste_builder_lifetimes_cancel(void){settings_cut_component(settings_paste_builder_case);}
static void test_settings_paste_timeout_and_draft_guards(void){settings_cut_component(settings_paste_guard_case);}

static int settings_paste_live_route(void)
{
    HWND owner=CreateWindowExW(0,L"STATIC",L"Owned paste clipboard restoration",WS_POPUP,0,0,0,0,NULL,NULL,GetModuleHandleW(NULL),NULL);CHECK(owner!=NULL);A.hwnd=owner;
    settings_clipboard_backup original;int captured=owner&&settings_clipboard_backup_begin(&original);
    if(captured){
        for(int switched=0;switched<2;switched++){
            settings_paste_begin();settings_cut_held=CreateEventW(NULL,TRUE,FALSE,NULL);settings_cut_release=CreateEventW(NULL,TRUE,FALSE,NULL);HANDLE holder=CreateThread(NULL,0,settings_cut_holder,NULL,0,NULL);
            CHECK(holder&&WaitForSingleObject(settings_cut_held,3000)==WAIT_OBJECT_0);DWORD held=99;if(holder)GetExitCodeThread(holder,&held);CHECK(held==STILL_ACTIVE&&GetOpenClipboardWindow()==settings_cut_holder_window);
            settings_paste_key();CHECK(A.paste.target==&A.fe&&!wcscmp(A.fe.t,L"5"));if(switched)settings_paste_open_field(1);
            SetEvent(settings_cut_release);CHECK(holder&&WaitForSingleObject(holder,3000)==WAIT_OBJECT_0);DWORD code=99;if(holder)GetExitCodeThread(holder,&code);CHECK(code==0);
            if(holder)CloseHandle(holder);CloseHandle(settings_cut_held);CloseHandle(settings_cut_release);
            /* Drive only the existing timer retry while its one original request
             * remains pending. Never resubmit the user's paste action. */
            uint64_t until=GetTickCount64()+1600;do{paste_tick();if(A.paste.target)Sleep(2);}while(A.paste.target&&GetTickCount64()<until);
            CHECK(!A.paste.target&&!wcscmp(A.fe.t,switched?L"5":settings_clip_prior));ebox once=A.fe;paste_tick();CHECK(!memcmp(&once,&A.fe,sizeof once));settings_clipboard_prior();settings_fixture_empty();settings_fixture_end();
            printf("LIVE_CASE paste holder=owned target=%s result=%s\n",switched?"equal-text-other-field":"same-field",switched?"unchanged":"inserted-once");
        }
        A.hwnd=owner;settings_clipboard_backup_end(&original);A.hwnd=NULL;
    }
    if(owner)CHECK(DestroyWindow(owner));printf("\n%d passed, %d failed; 2 live Settings paste cases\n",g_passed,g_failed);return g_failed?1:0;
}

static int settings_cut_live_route(void)
{
    /* Snapshot duplicable external formats in memory, with no content logging.
     * If custody cannot be prepared, fail before changing the clipboard. */
    HWND owner=CreateWindowExW(0,L"STATIC",L"Owned clipboard restoration",WS_POPUP,0,0,0,0,NULL,NULL,GetModuleHandleW(NULL),NULL);CHECK(owner!=NULL);A.hwnd=owner;
    settings_clipboard_backup original;int captured=owner&&settings_clipboard_backup_begin(&original);
    if(captured){settings_clip_live=1;settings_cut_busy_case();settings_cut_success_case();settings_clip_live=0;A.hwnd=owner;settings_clipboard_backup_end(&original);A.hwnd=NULL;}
    if(owner)CHECK(DestroyWindow(owner));
    printf("\n%d passed, %d failed; 2 live Settings Cut cases\n",g_passed,g_failed);return g_failed?1:0;
}

static void settings_fixture_run(const char *name,void (*run)(void))
{
    LARGE_INTEGER frequency,start,end;QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&start);
    int passed=g_passed,failed=g_failed;run();QueryPerformanceCounter(&end);
    printf("CASE %s assertions=%d failures=%d call_ms=%.3f\n",name,g_passed-passed,g_failed-failed,(double)(end.QuadPart-start.QuadPart)*1000/frequency.QuadPart);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc == 2 && !wcscmp(argv[1], L"--relay-contract")) {
        settings_fixture_run("test_relaycmd",test_relaycmd);
        settings_fixture_run("test_relay_draft",test_relay_draft);
        settings_fixture_run("test_relay_draft_commit",test_relay_draft_commit);
        settings_fixture_run("test_relay_catalog_registry",test_relay_catalog_registry);
        settings_fixture_run("test_relay_pinned_helper",test_relay_pinned_helper);
        printf("\n%d passed, %d failed; 5 relay selectors\n",g_passed,g_failed);
        return g_failed ? 1 : 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--pdf-contract")) {
        settings_fixture_run("test_pdftext",test_pdftext);
        settings_fixture_run("test_pdfpos",test_pdfpos);
        settings_fixture_run("test_pdf_helper",test_pdf_helper);
        printf("\n%d passed, %d failed; 3 PDF selectors\n",g_passed,g_failed);
        return g_failed ? 1 : 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--settings-paste-live"))return settings_paste_live_route();
    if (argc == 2 && !wcscmp(argv[1], L"--settings-clipboard-live"))return settings_cut_live_route();
    if (argc == 2 && !wcscmp(argv[1], L"--settings-input")) {
        settings_fixture_run("test_settings_click_unchanged_field_keeps_action",test_settings_click_unchanged_field_keeps_action);
        settings_fixture_run("test_settings_click_changed_field_queues_original_action",test_settings_click_changed_field_queues_original_action);
        settings_fixture_run("test_settings_click_record_cancel_keeps_action",test_settings_click_record_cancel_keeps_action);
        settings_fixture_run("test_settings_edit",test_settings_edit);
        settings_fixture_run("test_keys",test_keys);
        settings_fixture_run("test_registry_settings",test_registry_settings);
        settings_fixture_run("test_registry_reload",test_registry_reload);
        settings_fixture_run("test_settings_cut_busy_boundary_preserves_all_editors",test_settings_cut_busy_boundary_preserves_all_editors);
        settings_fixture_run("test_settings_cut_api_failures_preserve_drafts_and_previous_unicode",test_settings_cut_api_failures_preserve_drafts_and_previous_unicode);
        settings_fixture_run("test_settings_cut_success_publishes_before_removing_selection",test_settings_cut_success_publishes_before_removing_selection);
        settings_fixture_run("test_settings_cut_failed_restore_keeps_draft_and_reports_failure",test_settings_cut_failed_restore_keeps_draft_and_reports_failure);
        settings_fixture_run("test_settings_reload_replacement_releases_recorder",test_settings_reload_replacement_releases_recorder);
        settings_fixture_run("test_settings_reload_count_and_unchanged_release_capture",test_settings_reload_count_and_unchanged_release_capture);
        settings_fixture_run("test_settings_reload_clears_conflict_and_message_targets",test_settings_reload_clears_conflict_and_message_targets);
        settings_fixture_run("test_settings_reload_remaps_control_identity",test_settings_reload_remaps_control_identity);
        settings_fixture_run("test_settings_recording_without_reload_still_saves",test_settings_recording_without_reload_still_saves);
        settings_fixture_run("test_settings_paste_equal_text_target_and_reopen_cancel",test_settings_paste_equal_text_target_and_reopen_cancel);
        settings_fixture_run("test_settings_paste_same_editor_retry_and_new_request",test_settings_paste_same_editor_retry_and_new_request);
        settings_fixture_run("test_settings_paste_accepted_reload_and_removal_cancel",test_settings_paste_accepted_reload_and_removal_cancel);
        settings_fixture_run("test_settings_paste_builder_lifetimes_cancel",test_settings_paste_builder_lifetimes_cancel);
        settings_fixture_run("test_settings_paste_timeout_and_draft_guards",test_settings_paste_timeout_and_draft_guards);
        printf("\n%d passed, %d failed; 21 Settings selectors\n", g_passed, g_failed);
        return g_failed ? 1 : 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--settings-paste-red")) {
        settings_fixture_run("test_settings_paste_equal_text_target_and_reopen_cancel",test_settings_paste_equal_text_target_and_reopen_cancel);
        settings_fixture_run("test_settings_paste_same_editor_retry_and_new_request",test_settings_paste_same_editor_retry_and_new_request);
        settings_fixture_run("test_settings_paste_accepted_reload_and_removal_cancel",test_settings_paste_accepted_reload_and_removal_cancel);
        settings_fixture_run("test_settings_paste_builder_lifetimes_cancel",test_settings_paste_builder_lifetimes_cancel);
        settings_fixture_run("test_settings_paste_timeout_and_draft_guards",test_settings_paste_timeout_and_draft_guards);
        printf("\n%d passed, %d failed; 5 Settings paste selectors\n",g_passed,g_failed);return g_failed?1:0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--settings-reload-red")) {
        settings_fixture_run("test_settings_reload_replacement_releases_recorder",test_settings_reload_replacement_releases_recorder);
        settings_fixture_run("test_settings_reload_count_and_unchanged_release_capture",test_settings_reload_count_and_unchanged_release_capture);
        settings_fixture_run("test_settings_reload_clears_conflict_and_message_targets",test_settings_reload_clears_conflict_and_message_targets);
        printf("\n%d passed, %d failed; 3 Settings reload selectors\n",g_passed,g_failed);
        return g_failed ? 1 : 0;
    }
    if (argc == 2 && !wcscmp(argv[1], L"--settings-cut-red")) {
        settings_fixture_run("test_settings_cut_busy_boundary_preserves_all_editors",test_settings_cut_busy_boundary_preserves_all_editors);
        printf("\n%d passed, %d failed; 1 Settings Cut selector\n",g_passed,g_failed);
        return g_failed ? 1 : 0;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--gallery")) return write_gallery(argv[2]);
    test_settings_paste_equal_text_target_and_reopen_cancel();
    test_settings_paste_same_editor_retry_and_new_request();
    test_settings_paste_accepted_reload_and_removal_cancel();
    test_settings_paste_builder_lifetimes_cancel();
    test_settings_paste_timeout_and_draft_guards();
    test_settings_reload_replacement_releases_recorder();
    test_settings_reload_count_and_unchanged_release_capture();
    test_settings_reload_clears_conflict_and_message_targets();
    test_settings_reload_remaps_control_identity();
    test_settings_recording_without_reload_still_saves();
    test_settings_click_unchanged_field_keeps_action();
    test_settings_click_changed_field_queues_original_action();
    test_settings_click_record_cancel_keeps_action();
    test_settings_cut_busy_boundary_preserves_all_editors();
    test_settings_cut_api_failures_preserve_drafts_and_previous_unicode();
    test_settings_cut_success_publishes_before_removing_selection();
    test_settings_cut_failed_restore_keeps_draft_and_reports_failure();
    test_thumbnail_geometry();
    test_thumbnail_cache();
    test_text_model();
    test_text_helper();
    test_linux_path_entry();
    test_linux_name_codec();
    test_image_util();
    test_image_browse_model();
    test_image_helper();
    test_pdftext();
    test_pdfpos();
    test_pdf_helper();
    test_typeicons_kinds();
    test_typeicons_draw();
    test_registry_wanted();
    test_registry_reload();
    test_settings_edit();
    test_session();
    test_folder_views();
    test_folder_views_persistence();
    test_fuzzy();
    test_everything();
    test_search_queries();
    test_drop_effect();
    test_right_drop_choices();
    test_symlink_names_and_endpoints();
    test_permission_patches();
    test_bulk_rename_rules();
    test_keys();
    test_registry_dispatch();
    test_registry_settings();
    test_registry_async_lifetime();
    test_registry_lifetime();
    test_registry_bad_tables();
    test_registry_palette();
    test_registry_family();
    test_visible_action_revision();
    test_relaycmd();
    test_relay_draft();
    test_relay_draft_commit();
    test_relay_catalog_registry();
    test_relay_pinned_helper();
    test_termutil();
    test_operations();
    test_operations_panel();
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
