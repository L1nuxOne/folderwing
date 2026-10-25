/* Type icons: see typeicons.h.
 *
 * The extension table is written per category for readability and sorted
 * once on first use; a lookup is a binary search on at most 15 ASCII
 * characters. Shapes are drawn in a 16-unit grid scaled to the box, from
 * rectangles, polygons and thick lines, so they stay crisp at 100% and
 * scale cleanly to 150% and 200%. */
#include "typeicons.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "theme.h"

#define EXT_MAX   15
#define USER_MAX  256
#define TABLE_MAX 512

static const struct {
    ti_kind     kind;
    const char *exts;
} builtin[] = {
    { TI_TEXT, "txt text log md markdown rst nfo diz srt vtt sub ass ssa me 1st" },
    { TI_DOC, "doc docx docm dot dotx dotm odt ott rtf wpd wps pages epub mobi azw azw3 fb2 djvu xps oxps eml msg" },
    { TI_PDF, "pdf" },
    { TI_SHEET, "xls xlsx xlsm xlsb xlt xltx xltm ods ots csv tsv numbers" },
    { TI_SLIDES, "ppt pptx pptm pps ppsx pot potx odp otp" },
    { TI_IMAGE, "jpg jpeg jpe jfif png apng gif bmp dib tif tiff webp heic heif avif jxl ico cur svg svgz tga exr hdr "
                "pcx ppm pgm pbm qoi raw cr2 cr3 crw nef nrw arw srf sr2 dng orf rw2 raf pef srw x3f" },
    { TI_VIDEO, "mp4 m4v mkv webm avi mov qt wmv flv f4v mpg mpeg mpe m2v m2ts mts vob 3gp 3g2 ogv rm rmvb asf divx" },
    { TI_AUDIO, "mp3 wav flac aac m4a m4b ogg oga opus wma aiff aif aifc alac ape mid midi mka ac3 dts amr wv caf" },
    { TI_ARCHIVE, "zip 7z rar tar gz tgz bz2 tbz tbz2 xz txz zst tzst lz lzma lz4 z cab arj cpio jar war ear whl "
                  "nupkg deb rpm gem crate" },
    { TI_CODE, "c h cc cpp cxx c++ hpp hh hxx inl ipp cs csx fs fsi fsx vb java kt kts scala sc groovy gradle go rs zig "
               "py pyw pyi pyx ipynb rb erb php pl pm lua js mjs cjs jsx ts cts tsx vue svelte astro html htm xhtml "
               "css scss sass less styl swift m mm r jl dart ex exs erl hrl hs lhs ml mli clj cljs cljc edn elm nim v "
               "sv vhdl sql asm s nasm f f90 f95 for pas pp d cmake mk tex sty cls bib sol proto hlsl glsl vert "
               "frag shader wgsl cu cuh tf hcl" },
    { TI_SCRIPT, "bat cmd ps1 psm1 psd1 sh bash zsh fish ksh csh vbs vbe wsf wsh ahk au3 reg" },
    { TI_DATA, "json jsonc json5 jsonl ndjson yaml yml toml ini cfg conf config xml xsd xsl xslt plist properties "
               "env editorconfig gitignore gitattributes gitmodules gitkeep npmrc nvmrc dockerignore lock csproj "
               "vcxproj vbproj fsproj sln props targets filters manifest resx dat torrent vcf ics pem crt cer der "
               "key pub pfx p12 csr asc gpg sig" },
    { TI_DATABASE, "db db3 sqlite sqlite3 sdb mdb accdb sdf mdf ldf ndf frm ibd myd dbf kdbx parquet feather arrow "
                   "avro orc h5 hdf5 hdf nc npy npz pkl pickle rdata rds sav dta" },
    { TI_PROGRAM, "exe com scr msi msix msixbundle appx appxbundle appinstaller application msp msu apk aab ipa xap "
                  "gadget" },
    { TI_SYSTEM, "dll sys drv ocx cpl efi mui inf cat pdb lib a o obj so dylib ko exp ilk idb pch ipch tlb winmd "
                 "nls ax acm tsp etl evtx dmp mdmp hiv" },
    { TI_SHORTCUT, "lnk url appref-ms website pif desktop webloc" },
    { TI_FONT, "ttf otf ttc otc woff woff2 fon fnt eot pfb pfm afm bdf pcf" },
    { TI_DISC, "iso img vhd vhdx vmdk vdi qcow2 dmg wim esd swm cue nrg bin toast udf" },
    { TI_DESIGN, "psd psb ai eps indd idml fig sketch xd afdesign afphoto afpub cdr kra xcf blend fbx stl 3mf gltf glb "
                 "dae 3ds max ma mb c4d skp dwg dxf step stp iges igs usd usda usdc usdz ply" },
};

/* Files without an extension that everyone recognises. */
static const struct {
    ti_kind     kind;
    const char *names;
} named[] = {
    { TI_TEXT, "readme license licence changelog changes authors contributors copying notice todo news history "
               "install" },
    { TI_CODE, "makefile gnumakefile dockerfile containerfile rakefile gemfile procfile jenkinsfile vagrantfile "
               "justfile brewfile podfile" },
    { TI_DATA, "codeowners" },
};

typedef struct entry {
    char    ext[EXT_MAX + 1];
    uint8_t kind;
} entry;

static entry   g_table[TABLE_MAX], g_names[64], g_user[USER_MAX];
static int     g_ntable, g_nnames, g_nuser, g_ready, g_dups;
static const char *g_first_dup;

static const char *const kind_names[TI_COUNT] = {
    "folder", "text", "document", "pdf", "spreadsheet", "slides", "image", "video", "audio", "archive",
    "code", "script", "data", "database", "program", "system", "shortcut", "font", "disc", "design", "unknown",
};

static int by_ext(const void *a, const void *b) { return strcmp(((const entry *)a)->ext, ((const entry *)b)->ext); }

/* Next word of a space/comma list, lowercased, without a leading '.'; 0 at the end, -1 if unusable. */
static int next_word(const char **p, char out[EXT_MAX + 1])
{
    const char *s = *p;
    while (*s == ' ' || *s == ',' || *s == '\t') s++;
    if (!*s) {
        *p = s;
        return 0;
    }
    const char *e = s;
    while (*e && *e != ' ' && *e != ',' && *e != '\t') e++;
    *p = e;
    if (*s == '.') s++;
    size_t n = (size_t)(e - s);
    if (!n || n > EXT_MAX) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < 33 || ch > 126 || ch == '.' || ch == '\\' || ch == '/') return -1;
        out[i] = (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
    }
    out[n] = 0;
    return 1;
}

static void fill_table(entry *t, int *n, int max, ti_kind kind, const char *list)
{
    char w[EXT_MAX + 1];
    int r;
    while ((r = next_word(&list, w)) != 0)
        if (r > 0 && *n < max) {
            memcpy(t[*n].ext, w, sizeof w);
            t[(*n)++].kind = (uint8_t)kind;
        }
}

static void ready(void)
{
    if (g_ready) return;
    g_ready = 1;
    for (size_t i = 0; i < sizeof builtin / sizeof builtin[0]; i++)
        fill_table(g_table, &g_ntable, TABLE_MAX, builtin[i].kind, builtin[i].exts);
    for (size_t i = 0; i < sizeof named / sizeof named[0]; i++)
        fill_table(g_names, &g_nnames, 64, named[i].kind, named[i].names);
    qsort(g_table, (size_t)g_ntable, sizeof *g_table, by_ext);
    qsort(g_names, (size_t)g_nnames, sizeof *g_names, by_ext);
    for (int i = 1; i < g_ntable; i++)
        if (!strcmp(g_table[i - 1].ext, g_table[i].ext) && !g_dups++) g_first_dup = g_table[i].ext;
}

int ti_duplicates(const char **first)
{
    ready();
    if (first) *first = g_first_dup;
    return g_dups;
}

static int lookup(const entry *t, int n, const char *key)
{
    entry k;
    memcpy(k.ext, key, EXT_MAX + 1);
    const entry *e = bsearch(&k, t, (size_t)n, sizeof *t, by_ext);
    return e ? e->kind : -1;
}

/* s[0..n) lowercased into out, if it is short printable ASCII. */
static int ascii_key(const wchar_t *s, uint32_t n, char out[EXT_MAX + 1])
{
    if (!n || n > EXT_MAX) return 0;
    for (uint32_t i = 0; i < n; i++) {
        wchar_t c = s[i];
        if (c < 33 || c > 126) return 0;
        out[i] = (char)(c >= L'A' && c <= L'Z' ? c + 32 : c);
    }
    memset(out + n, 0, EXT_MAX + 1 - n);
    return 1;
}

ti_kind ti_of_name(const wchar_t *name, uint32_t len, uint32_t ext, uint32_t attrs)
{
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return TI_FOLDER;
    ready();
    char key[EXT_MAX + 1];
    if (ext < len) {
        if (!ascii_key(name + ext, len - ext, key)) return TI_UNKNOWN;
        for (int i = g_nuser; i-- > 0;) /* the last setting wins */
            if (!strcmp(g_user[i].ext, key)) return (ti_kind)g_user[i].kind;
        int k = lookup(g_table, g_ntable, key);
        if (k >= 0) return (ti_kind)k;
        /* "CMakeLists.txt" is text by extension; "notes.backup" is unknown */
        return TI_UNKNOWN;
    }
    if (!ascii_key(name, len, key)) return TI_UNKNOWN;
    int k = lookup(g_names, g_nnames, key);
    if (k < 0 && key[0] == '.' && key[1]) { /* dotfiles: .gitignore, .env */
        memmove(key, key + 1, EXT_MAX);
        k = lookup(g_table, g_ntable, key);
    }
    return k >= 0 ? (ti_kind)k : TI_UNKNOWN;
}

ti_kind ti_of_path(const wchar_t *path, int folder)
{
    if (folder) return TI_FOLDER;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    uint32_t len = (uint32_t)wcslen(name);
    const wchar_t *dot = wcsrchr(name, L'.');
    return ti_of_name(name, len, dot ? (uint32_t)(dot - name) + 1 : len, 0);
}

const char *ti_name(ti_kind k) { return k < TI_FILE_KINDS ? kind_names[k] : ""; }

int ti_find(const char *name)
{
    for (int k = 0; k < TI_FILE_KINDS; k++)
        if (!_stricmp(kind_names[k], name)) return k;
    return -1;
}

int ti_add(ti_kind k, const char *exts)
{
    int bad = 0, r;
    char w[EXT_MAX + 1];
    if (k >= TI_FILE_KINDS) return 1;
    while ((r = next_word(&exts, w)) != 0) {
        if (r < 0 || g_nuser == USER_MAX) {
            bad++;
            continue;
        }
        memcpy(g_user[g_nuser].ext, w, sizeof w);
        g_user[g_nuser++].kind = (uint8_t)k;
    }
    return bad;
}

void ti_reset(void) { g_nuser = 0; }

COLORREF ti_color(ti_kind k)
{
    static const COLORREF colors[TI_COUNT] = {
        C_FOLDER, C_TI_TEXT, C_TI_DOC, C_TI_PDF, C_TI_SHEET, C_TI_SLIDES, C_TI_IMAGE, C_TI_VIDEO, C_TI_AUDIO,
        C_TI_ARCHIVE, C_TI_CODE, C_TI_SCRIPT, C_TI_DATA, C_TI_DATABASE, C_TI_PROGRAM, C_TI_SYSTEM, C_LINK,
        C_TI_FONT, C_TI_DISC, C_TI_DESIGN, C_TI_UNKNOWN,
        C_DRIVE, C_ACCENT, C_ACCENT, C_TI_DOC, C_TI_SHEET, C_ACCENT,
    };
    return k < TI_COUNT ? colors[k] : C_TI_UNKNOWN;
}

/* ---- drawing --------------------------------------------------------------------- */

typedef struct ctx {
    HDC dc;
    int x, y, size;
} ctx;

static COLORREF mix(COLORREF a, COLORREF b, int pct)
{
    return RGB((GetRValue(a) * (100 - pct) + GetRValue(b) * pct) / 100,
               (GetGValue(a) * (100 - pct) + GetGValue(b) * pct) / 100,
               (GetBValue(a) * (100 - pct) + GetBValue(b) * pct) / 100);
}

static int U(const ctx *c, int v) { return MulDiv(v, c->size, 16); }

static void box(const ctx *c, int x, int y, int w, int h, COLORREF col)
{
    RECT r = { c->x + U(c, x), c->y + U(c, y), c->x + U(c, x + w), c->y + U(c, y + h) };
    if (r.right <= r.left) r.right = r.left + 1;
    if (r.bottom <= r.top) r.bottom = r.top + 1;
    SetDCBrushColor(c->dc, col);
    FillRect(c->dc, &r, GetStockObject(DC_BRUSH));
}

/* A one-unit frame (at least one pixel). */
static void outline(const ctx *c, int x, int y, int w, int h, COLORREF col)
{
    box(c, x, y, w, 1, col);
    box(c, x, y + h - 1, w, 1, col);
    box(c, x, y, 1, h, col);
    box(c, x + w - 1, y, 1, h, col);
}

static void shape(const ctx *c, const POINT *pts, int n, COLORREF col, int ellipse)
{
    POINT p[8];
    for (int i = 0; i < n && i < 8; i++) {
        p[i].x = c->x + U(c, (int)pts[i].x);
        p[i].y = c->y + U(c, (int)pts[i].y);
    }
    SetDCBrushColor(c->dc, col);
    SetDCPenColor(c->dc, col);
    HGDIOBJ ob = SelectObject(c->dc, GetStockObject(DC_BRUSH)), op = SelectObject(c->dc, GetStockObject(DC_PEN));
    if (ellipse) Ellipse(c->dc, p[0].x, p[0].y, p[1].x, p[1].y);
    else Polygon(c->dc, p, n);
    SelectObject(c->dc, ob);
    SelectObject(c->dc, op);
}

static void poly(const ctx *c, const POINT *pts, int n, COLORREF col) { shape(c, pts, n, col, 0); }

static void oval(const ctx *c, int x, int y, int w, int h, COLORREF col)
{
    POINT p[2] = { { x, y }, { x + w, y + h } };
    shape(c, p, 2, col, 1);
}

/* A two-unit-wide stroke through the points. */
static void stroke(const ctx *c, const POINT *pts, int n, COLORREF col)
{
    POINT p[8];
    for (int i = 0; i < n && i < 8; i++) {
        p[i].x = c->x + U(c, (int)pts[i].x);
        p[i].y = c->y + U(c, (int)pts[i].y);
    }
    LOGBRUSH lb = { BS_SOLID, col, 0 };
    int w = U(c, 2) > 1 ? U(c, 2) : 1;
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_SQUARE | PS_JOIN_MITER, (DWORD)w, &lb, 0, NULL);
    HGDIOBJ op = SelectObject(c->dc, pen);
    Polyline(c->dc, p, n);
    SelectObject(c->dc, op);
    DeleteObject(pen);
}

#define P(...) ((const POINT[]){ __VA_ARGS__ })
#define N(...) (int)(sizeof((POINT[]){ __VA_ARGS__ }) / sizeof(POINT))
#define POLY(col, ...) poly(c, P(__VA_ARGS__), N(__VA_ARGS__), col)
#define STROKE(col, ...) stroke(c, P(__VA_ARGS__), N(__VA_ARGS__), col)

void ti_draw(HDC dc, ti_kind k, int x, int y, int size, uint32_t flags, COLORREF color)
{
    ctx cx = { dc, x, y, size }, *c = &cx;
    COLORREF f = color ? color : ti_color(k);
    if (flags & TI_FADED) f = mix(f, C_BG, 50);
    COLORREF t = mix(f, RGB(255, 255, 255), 55), d = mix(f, RGB(0, 0, 0), 45), hole = mix(f, RGB(0, 0, 0), 72);

    switch (k) {
    case TI_FOLDER:
        box(c, 1, 2, 6, 2, d);
        box(c, 1, 3, 14, 3, d);
        box(c, 1, 5, 14, 9, f);
        break;
    case TI_TEXT:
        outline(c, 3, 1, 10, 14, f);
        box(c, 5, 4, 6, 1, f);
        box(c, 5, 7, 6, 1, f);
        box(c, 5, 10, 4, 1, f);
        break;
    case TI_DOC:
        box(c, 3, 1, 10, 14, f);
        box(c, 5, 4, 6, 1, t);
        box(c, 5, 7, 6, 1, t);
        box(c, 5, 10, 6, 1, t);
        break;
    case TI_PDF:
        POLY(f, { 3, 1 }, { 9, 1 }, { 13, 5 }, { 13, 15 }, { 3, 15 });
        POLY(t, { 9, 1 }, { 9, 5 }, { 13, 5 });
        box(c, 5, 9, 6, 3, t);
        break;
    case TI_SHEET:
        box(c, 2, 2, 12, 12, f);
        box(c, 2, 6, 12, 1, d);
        box(c, 2, 10, 12, 1, d);
        box(c, 6, 2, 1, 12, d);
        box(c, 10, 2, 1, 12, d);
        break;
    case TI_SLIDES:
        box(c, 1, 2, 14, 9, f);
        box(c, 4, 5, 8, 1, t);
        box(c, 4, 7, 5, 1, t);
        box(c, 7, 11, 2, 2, f);
        box(c, 4, 13, 8, 2, f);
        break;
    case TI_IMAGE:
        box(c, 1, 2, 14, 12, f);
        box(c, 2, 3, 12, 10, hole);
        POLY(f, { 2, 13 }, { 6, 7 }, { 9, 11 }, { 11, 9 }, { 14, 13 });
        box(c, 10, 4, 2, 2, t);
        break;
    case TI_VIDEO:
        box(c, 1, 3, 14, 10, f);
        POLY(t, { 6, 5 }, { 6, 11 }, { 11, 8 });
        break;
    case TI_AUDIO:
        box(c, 10, 2, 2, 10, f);
        POLY(f, { 11, 2 }, { 15, 4 }, { 15, 7 }, { 11, 5 });
        oval(c, 4, 9, 8, 6, f);
        break;
    case TI_ARCHIVE:
        box(c, 2, 2, 12, 12, f);
        box(c, 2, 5, 12, 1, d);
        box(c, 7, 5, 2, 8, t);
        box(c, 7, 7, 1, 1, d);
        box(c, 8, 9, 1, 1, d);
        box(c, 7, 11, 1, 1, d);
        break;
    case TI_CODE:
        STROKE(f, { 6, 4 }, { 2, 8 }, { 6, 12 });
        STROKE(f, { 10, 4 }, { 14, 8 }, { 10, 12 });
        break;
    case TI_SCRIPT:
        outline(c, 1, 2, 14, 12, f);
        box(c, 1, 2, 14, 2, f);
        STROKE(f, { 4, 6 }, { 6, 8 }, { 4, 10 });
        box(c, 8, 10, 4, 2, f);
        break;
    case TI_DATA:
        box(c, 2, 2, 3, 3, f);
        box(c, 6, 3, 8, 1, f);
        box(c, 2, 7, 3, 3, f);
        box(c, 6, 8, 6, 1, f);
        box(c, 2, 12, 3, 3, f);
        box(c, 6, 13, 7, 1, f);
        break;
    case TI_DATABASE:
        box(c, 3, 3, 10, 10, f);
        oval(c, 3, 10, 10, 5, f);
        box(c, 3, 8, 10, 1, d);
        oval(c, 3, 1, 10, 5, t);
        break;
    case TI_PROGRAM:
        outline(c, 1, 2, 14, 12, f);
        box(c, 1, 2, 14, 3, f);
        box(c, 3, 7, 4, 4, d);
        box(c, 9, 7, 4, 1, f);
        box(c, 9, 9, 3, 1, f);
        break;
    case TI_SYSTEM:
        box(c, 7, 1, 2, 14, f);
        box(c, 1, 7, 14, 2, f);
        POLY(f, { 3, 2 }, { 14, 13 }, { 13, 14 }, { 2, 3 });
        POLY(f, { 13, 2 }, { 14, 3 }, { 3, 14 }, { 2, 13 });
        oval(c, 3, 3, 10, 10, f);
        oval(c, 6, 6, 4, 4, hole);
        break;
    case TI_SHORTCUT:
        outline(c, 1, 1, 14, 14, f);
        STROKE(f, { 5, 12 }, { 5, 8 }, { 10, 8 });
        POLY(f, { 9, 5 }, { 13, 8 }, { 9, 11 });
        break;
    case TI_FONT:
        /* the apex sits low enough that its mitered tip stays in the box */
        STROKE(f, { 3, 14 }, { 8, 4 }, { 13, 14 });
        box(c, 5, 10, 6, 2, f);
        break;
    case TI_DISC:
        oval(c, 1, 1, 14, 14, f);
        oval(c, 5, 5, 6, 6, t);
        oval(c, 6, 6, 4, 4, hole);
        break;
    case TI_DESIGN:
        POLY(f, { 8, 1 }, { 2, 12 }, { 8, 15 });
        POLY(d, { 8, 1 }, { 14, 12 }, { 8, 15 });
        break;
    case TI_DRIVE:
        box(c, 1, 5, 14, 7, d);
        outline(c, 1, 5, 14, 7, f);
        box(c, 11, 8, 2, 1, C_LINK);
        break;
    case TI_LINUX:
        oval(c, 4, 1, 8, 9, f);oval(c, 2, 5, 12, 10, f);
        oval(c, 5, 6, 6, 8, RGB(235,240,245));
        box(c, 6, 3, 1, 2, RGB(255,255,255));box(c, 9, 3, 1, 2, RGB(255,255,255));
        POLY(RGB(239,178,58), {6,6}, {10,6}, {8,8});
        oval(c, 1, 12, 6, 3, RGB(239,178,58));oval(c, 9, 12, 6, 3, RGB(239,178,58));
        break;
    case TI_HOME:
        POLY(f, { 8, 1 }, { 1, 8 }, { 15, 8 });
        box(c, 3, 8, 10, 7, f);
        box(c, 7, 11, 2, 4, hole);
        break;
    case TI_DESKTOP:
        box(c, 1, 2, 14, 9, f);
        box(c, 2, 3, 12, 7, d);
        box(c, 7, 11, 2, 2, f);
        box(c, 4, 13, 8, 2, f);
        break;
    case TI_DOCUMENTS:
        outline(c, 6, 1, 9, 11, f);
        box(c, 2, 4, 9, 11, f);
        box(c, 4, 7, 5, 1, t);
        box(c, 4, 10, 5, 1, t);
        break;
    case TI_DOWNLOADS:
        box(c, 7, 1, 2, 7, f);
        POLY(f, { 3, 7 }, { 13, 7 }, { 8, 12 });
        box(c, 2, 13, 12, 2, f);
        break;
    default: /* TI_UNKNOWN */
        outline(c, 3, 1, 10, 14, f);
        break;
    }
    if (flags & TI_LINK) {
        box(c, 0, 10, 6, 6, RGB(20, 20, 22));
        box(c, 1, 11, 4, 4, C_LINK);
    }
}
