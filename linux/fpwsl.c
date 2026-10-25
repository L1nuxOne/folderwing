/*
 * folderwing-wsl - OpenPilot's helper inside a WSL distro.
 *
 * Lists folders with getdents64 + statx and watches them with inotify, so
 * the engine gets Linux's view of the files (types, modes, owners, symlink
 * targets) without a 9P round trip per file. Speaks src/wslproto.h on
 * stdin/stdout and exits when stdin closes. Built as a static binary, so it
 * runs in any distro without depending on its libraries:
 *   zig cc -target x86_64-linux-musl -static -Isrc linux/fpwsl.c -o folderwing-wsl
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "wslproto.h"

/* ---- output ------------------------------------------------------------------ */

static int write_all(const void *p, size_t n)
{
    const char *s = p;
    while (n) {
        ssize_t w = write(1, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return 0;
        s += w;
        n -= (size_t)w;
    }
    return 1;
}

static void send_msg(uint32_t type, uint32_t id, const void *payload, uint32_t size)
{
    wslp_head h = { type, size, id };
    if (!write_all(&h, sizeof h) || (size && !write_all(payload, size))) exit(0); /* the engine is gone */
}

/* ENTRIES and OWNERS payloads are gathered here and sent in chunks */
static char     g_out[64 * 1024];
static uint32_t g_used;

static void flush_out(uint32_t type, uint32_t id)
{
    if (g_used) send_msg(type, id, g_out, g_used);
    g_used = 0;
}

static void put(uint32_t type, uint32_t id, const void *rec, uint32_t rec_len, const char *a, uint32_t al,
                const char *b, uint32_t bl)
{
    uint32_t need = wslp_pad8(rec_len + al + bl);
    if (g_used + need > sizeof g_out) flush_out(type, id);
    char *p = g_out + g_used;
    memcpy(p, rec, rec_len);
    if (al) memcpy(p + rec_len, a, al);
    if (bl) memcpy(p + rec_len + al, b, bl);
    memset(p + rec_len + al + bl, 0, need - (rec_len + al + bl));
    g_used += need;
}

/* ---- users and groups ------------------------------------------------------------- */

typedef struct name_id {
    uint32_t id;
    char     name[33];
} name_id;

typedef struct id_table {
    const char *file;
    name_id    *v;
    uint32_t    n;
    struct timespec mtime;
} id_table;

static id_table g_users = { "/etc/passwd" }, g_groups = { "/etc/group" };

/* name:x:id:... lines; re-read when the file changes */
static void table_load(id_table *t)
{
    struct stat st;
    if (stat(t->file, &st) != 0) return;
    if (t->v && st.st_mtim.tv_sec == t->mtime.tv_sec && st.st_mtim.tv_nsec == t->mtime.tv_nsec) return;
    t->mtime = st.st_mtim;
    free(t->v);
    t->v = NULL;
    t->n = 0;
    FILE *f = fopen(t->file, "re");
    if (!f) return;
    uint32_t cap = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *c1 = strchr(line, ':'), *c2 = c1 ? strchr(c1 + 1, ':') : NULL;
        if (!c1 || !c2 || c1 == line || c1 - line > 32) continue;
        char *end;
        unsigned long id = strtoul(c2 + 1, &end, 10);
        if (end == c2 + 1 || *end != ':') continue;
        if (t->n == cap) {
            cap = cap ? cap * 2 : 64;
            name_id *nv = realloc(t->v, cap * sizeof *nv);
            if (!nv) break;
            t->v = nv;
        }
        name_id *e = &t->v[t->n++];
        e->id = (uint32_t)id;
        memcpy(e->name, line, (size_t)(c1 - line));
        e->name[c1 - line] = 0;
    }
    fclose(f);
}

static const char *table_name(const id_table *t, uint32_t id)
{
    for (uint32_t i = 0; i < t->n; i++)
        if (t->v[i].id == id) return t->v[i].name;
    return NULL;
}

/* ---- listing ------------------------------------------------------------------- */

#define SEEN_MAX 64

typedef struct seen {
    uint32_t users[SEEN_MAX], groups[SEEN_MAX];
    uint32_t nusers, ngroups;
} seen;

static void note(uint32_t *v, uint32_t *n, uint32_t id)
{
    for (uint32_t i = 0; i < *n; i++)
        if (v[i] == id) return;
    if (*n < SEEN_MAX) v[(*n)++] = id;
}

static int64_t ns(const struct statx_timestamp *t) { return (int64_t)t->tv_sec * 1000000000 + t->tv_nsec; }

struct linux_dirent64 {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
};

static void send_owners(uint32_t id, const seen *s)
{
    table_load(&g_users);
    table_load(&g_groups);
    for (int kind = 0; kind < 2; kind++) {
        const uint32_t *v = kind ? s->groups : s->users;
        uint32_t n = kind ? s->ngroups : s->nusers;
        for (uint32_t i = 0; i < n; i++) {
            const char *name = table_name(kind ? &g_groups : &g_users, v[i]);
            if (!name) continue;
            wslp_owner o = { v[i], (uint8_t)kind, (uint8_t)strlen(name), 0 };
            put(WSLP_OWNERS, id, &o, sizeof o, name, o.len, NULL, 0);
        }
    }
    flush_out(WSLP_OWNERS, id);
}

static void do_list(uint32_t id, const char *path)
{
    wslp_end end = { 0, 0 };
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        end.err = errno;
        send_msg(WSLP_LIST_END, id, &end, sizeof end);
        return;
    }
    static char buf[64 * 1024];
    static char link[4096];
    seen s = { { 0 }, { 0 }, 0, 0 };
    for (;;) {
        long n = syscall(SYS_getdents64, fd, buf, sizeof buf);
        if (n < 0) {
            end.err = errno;
            break;
        }
        if (n == 0) break;
        for (long off = 0; off < n;) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            const char *nm = d->d_name;
            if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) continue;
            struct statx sx;
            if (statx(fd, nm, AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                      STATX_TYPE | STATX_MODE | STATX_UID | STATX_GID | STATX_SIZE | STATX_MTIME | STATX_BTIME,
                      &sx) != 0) {
                /* Only disappearance proves this enumerated name is obsolete.
                 * Other failures make the snapshot incomplete: comparisons
                 * must receive the error instead of authorizing a partial tree. */
                if (errno == ENOENT) continue;
                end.err = errno;
                break;
            }
            wslp_entry e = { 0 };
            e.mode = sx.stx_mode;
            e.uid = sx.stx_uid;
            e.gid = sx.stx_gid;
            e.size = S_ISDIR(sx.stx_mode) ? 0 : sx.stx_size;
            e.mtime = ns(&sx.stx_mtime);
            e.btime = (sx.stx_mask & STATX_BTIME) ? ns(&sx.stx_btime) : 0;
            size_t nl = strlen(nm);
            e.name_len = (uint16_t)nl;
            ssize_t ll = 0;
            if (S_ISLNK(sx.stx_mode)) {
                ll = readlinkat(fd, nm, link, sizeof link - 1);
                if (ll < 0) ll = 0;
                struct stat to;
                if (fstatat(fd, nm, &to, 0) != 0) e.flags |= WSLP_E_BROKEN;
                else if (S_ISDIR(to.st_mode)) e.flags |= WSLP_E_DIRLINK;
            }
            e.link_len = (uint16_t)ll;
            note(s.users, &s.nusers, e.uid);
            note(s.groups, &s.ngroups, e.gid);
            put(WSLP_ENTRIES, id, &e, sizeof e, nm, (uint32_t)nl, link, (uint32_t)ll);
            end.count++;
        }
        if (end.err) break;
    }
    close(fd);
    flush_out(WSLP_ENTRIES, id);
    send_owners(id, &s);
    send_msg(WSLP_LIST_END, id, &end, sizeof end);
}

/* ---- watching ------------------------------------------------------------------- */

#define CHANGE_GAP_NS 50000000LL /* at most one CHANGED per watch per 50 ms */

typedef struct watch {
    uint32_t id;
    int      wd;
    int      dirty;
    int64_t  sent; /* when the last CHANGED went out */
} watch;

static int      g_in = -1;
static watch   *g_w;
static uint32_t g_nw, g_capw;

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void watch_remove(uint32_t i, int keep_wd)
{
    int wd = g_w[i].wd;
    g_w[i] = g_w[--g_nw];
    if (keep_wd) return;
    for (uint32_t k = 0; k < g_nw; k++)
        if (g_w[k].wd == wd) return; /* another watch on the same folder */
    inotify_rm_watch(g_in, wd);
}

static void do_watch(uint32_t id, const char *path)
{
    wslp_end r = { 0, 0 };
    if (g_in < 0) g_in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int wd = g_in < 0 ? -1
                      : inotify_add_watch(g_in, path,
                                          IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ATTRIB |
                                              IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR);
    if (wd < 0) {
        r.err = errno;
    } else if (g_nw == g_capw) {
        uint32_t cap = g_capw ? g_capw * 2 : 16;
        watch *nv = realloc(g_w, cap * sizeof *nv);
        if (nv) {
            g_w = nv;
            g_capw = cap;
        } else {
            r.err = ENOMEM;
        }
    }
    if (!r.err) g_w[g_nw++] = (watch){ id, wd, 0, 0 };
    send_msg(WSLP_WATCHED, id, &r, sizeof r);
}

static void do_unwatch(uint32_t id)
{
    for (uint32_t i = 0; i < g_nw; i++)
        if (g_w[i].id == id) {
            watch_remove(i, 0);
            return;
        }
}

static void read_events(void)
{
    static char buf[16 * 1024] __attribute__((aligned(8)));
    for (;;) {
        ssize_t n = read(g_in, buf, sizeof buf);
        if (n <= 0) return;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *ev = (struct inotify_event *)p;
            p += sizeof *ev + ev->len;
            for (uint32_t i = 0; i < g_nw;) {
                if (g_w[i].wd != ev->wd) { i++; continue; }
                if (ev->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                    /* the folder is gone: say so once, then forget the watch */
                    send_msg(WSLP_CHANGED, g_w[i].id, NULL, 0);
                    watch_remove(i, (ev->mask & IN_IGNORED) != 0);
                    continue;
                }
                g_w[i].dirty = 1;
                i++;
            }
            if (ev->mask & IN_Q_OVERFLOW)
                for (uint32_t i = 0; i < g_nw; i++) g_w[i].dirty = 1;
        }
    }
}

/* Sends what is due; returns ms until the next one is, or -1 */
static int send_changes(void)
{
    int64_t now = now_ns(), next = -1;
    for (uint32_t i = 0; i < g_nw; i++) {
        if (!g_w[i].dirty) continue;
        int64_t due = g_w[i].sent + CHANGE_GAP_NS;
        if (now >= due) {
            g_w[i].dirty = 0;
            g_w[i].sent = now;
            send_msg(WSLP_CHANGED, g_w[i].id, NULL, 0);
        } else if (next < 0 || due < next) {
            next = due;
        }
    }
    return next < 0 ? -1 : (int)((next - now) / 1000000 + 1);
}

/* ---- requests ------------------------------------------------------------------- */

static int read_all(void *p, size_t n)
{
    char *s = p;
    while (n) {
        ssize_t r = read(0, s, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        s += r;
        n -= (size_t)r;
    }
    return 1;
}

static int handle_request(void)
{
    static char path[WSLP_MAX + 1];
    wslp_head h;
    if (!read_all(&h, sizeof h) || h.size > WSLP_MAX || !read_all(path, h.size)) return 0;
    path[h.size] = 0;
    int bad = memchr(path, 0, h.size) != NULL || (h.size == 0 && h.type != WSLP_UNWATCH);
    switch (h.type) {
    case WSLP_LIST:
        if (bad) {
            wslp_end e = { EINVAL, 0 };
            send_msg(WSLP_LIST_END, h.id, &e, sizeof e);
        } else {
            do_list(h.id, path);
        }
        break;
    case WSLP_WATCH:
        if (bad) {
            wslp_end e = { EINVAL, 0 };
            send_msg(WSLP_WATCHED, h.id, &e, sizeof e);
        } else {
            do_watch(h.id, path);
        }
        break;
    case WSLP_UNWATCH: do_unwatch(h.id); break;
    default: break; /* newer engine, older helper: ignore what isn't known */
    }
    return 1;
}

int fp_rename_main(void);
int fp_sizes_main(void);
int fp_space_main(void);
int fp_duplicate_main(void);
int fp_git_main(void);
int fp_content_main(void);
int fp_search_main(void);
int fp_index_main(void);
int fp_hash_main(void);
int fp_guard_main(void);
int fp_permission_main(void);
int fp_path_main(void);
int fp_jobs_main(void);
int fp_transfer_main(int importing);
int fp_shell_main(const char *path);
int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--search")) return fp_search_main();
    if (argc == 2 && !strcmp(argv[1], "--index")) return fp_index_main();
    if (argc == 2 && !strcmp(argv[1], "--rename")) return fp_rename_main();
    if (argc == 2 && !strcmp(argv[1], "--guard")) return fp_guard_main();
    if (argc == 2 && !strcmp(argv[1], "--hash")) return fp_hash_main();
    if (argc == 2 && !strcmp(argv[1], "--content-search")) return fp_content_main();
    if (argc == 2 && !strcmp(argv[1], "--git-read")) return fp_git_main();
    if (argc == 2 && !strcmp(argv[1], "--duplicate-read")) return fp_duplicate_main();
    if (argc == 2 && !strcmp(argv[1], "--analyze-space")) return fp_space_main();
    if (argc == 2 && !strcmp(argv[1], "--analyze-space-summary")) {extern int fp_space_mode_main(int);return fp_space_mode_main(1);}
    if (argc == 2 && !strcmp(argv[1], "--analyze-space-list")) {extern int fp_space_mode_main(int);return fp_space_mode_main(2);}
    if (argc == 2 && !strcmp(argv[1], "--analyze-space-full")) {extern int fp_space_mode_main(int);return fp_space_mode_main(3);}
    if (argc == 2 && !strcmp(argv[1], "--analyze-space-page")) {extern int fp_space_mode_main(int);return fp_space_mode_main(4);}
    if (argc == 2 && !strcmp(argv[1], "--sizes")) return fp_sizes_main();
    if (argc == 2 && !strcmp(argv[1], "--permissions")) return fp_permission_main();
    if (argc == 2 && !strcmp(argv[1], "--resolve-path")) return fp_path_main();
    if (argc == 2 && !strcmp(argv[1], "--jobs")) return fp_jobs_main();
    if (argc == 2 && !strcmp(argv[1], "--export")) return fp_transfer_main(0);
    if (argc == 2 && !strcmp(argv[1], "--import")) return fp_transfer_main(1);
    if (argc == 3 && !strcmp(argv[1], "--shell")) return fp_shell_main(argv[2]);
    signal(SIGPIPE, SIG_IGN);
    wslp_hello hi = { WSLP_MAGIC, WSLP_VERSION, (uint32_t)getuid(), (uint32_t)getpid() };
    send_msg(WSLP_HELLO, 0, &hi, sizeof hi);
    for (;;) {
        int wait = send_changes();
        struct pollfd fds[2] = { { 0, POLLIN, 0 }, { g_in, POLLIN, 0 } };
        int n = poll(fds, g_in >= 0 ? 2 : 1, wait);
        if (n < 0 && errno != EINTR) return 1;
        if (n <= 0) continue;
        if (g_in >= 0 && (fds[1].revents & POLLIN)) read_events();
        if (fds[0].revents & (POLLIN | POLLHUP | POLLERR))
            if (!handle_request()) return 0;
    }
}
