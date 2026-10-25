/* Private, descriptor-held staging for native copies and transfer imports.
 * Directory writers cannot replace a payload inside our 0700 namespace.
 * A process with our UID (or root) is not a sandboxed adversary: separate
 * validation and rename/unlink calls do not provide conditional syscalls. */
#ifndef FP_STAGE_H
#define FP_STAGE_H
#include <sys/random.h>

typedef struct fp_stage {
    int parent, dir, file, leaf, published;
    char name[64];
    struct stat directory, object;
} fp_stage;

/* Persistence failures are transfer failures, including unsupported directory
 * flushes. Only interruption is retryable; never acknowledge a best effort. */
static int fp_stage_sync(int fd)
{
    int rc;
    do rc = fsync(fd); while (rc < 0 && errno == EINTR);
    return rc < 0 ? errno : 0;
}
static int fp_stage_sync_pair(int first, int second)
{
    int error = fp_stage_sync(first);
    int other = first == second ? 0 : fp_stage_sync(second);
    return error ? error : other;
}
static void fp_stage_init(fp_stage *s)
{
    memset(s, 0, sizeof *s);
    s->parent = s->dir = s->file = -1;
}
static int fp_stage_same(const struct stat *a, const struct stat *b)
{
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}
static int fp_stage_directory(fp_stage *s)
{
    struct stat a, b;
    if (fstat(s->dir, &a) || fstatat(s->parent, s->name, &b, AT_SYMLINK_NOFOLLOW)) return 0;
    if (!fp_stage_same(&s->directory, &a) || !fp_stage_same(&a, &b) ||
        !S_ISDIR(a.st_mode) || a.st_uid != geteuid() || (a.st_mode & 0777) != 0700) {
        errno = ESTALE;
        return 0;
    }
    return 1;
}
/* Cleanup never intentionally unlinks an entry whose identity is foreign.
 * If another same-UID process substitutes it, leave it and report failure. */
/* Finish one payload while retaining the private workspace for the next file.
 * Only an owned payload is removed. Every completed file still closes its FD
 * and validates the namespace before its source can be removed. */
static int fp_stage_finish(fp_stage *s)
{
    int error = 0;
    if (s->leaf && !s->published) {
        struct stat st;
        if (fstatat(s->dir, "payload", &st, AT_SYMLINK_NOFOLLOW)) {
            if (errno != ENOENT) error = errno;
        } else if (!fp_stage_same(&s->object, &st)) error = ESTALE;
        else if (unlinkat(s->dir, "payload", 0)) error = errno;
    }
    if (s->file >= 0 && close(s->file) && !error) error = errno;
    s->file = -1; s->leaf = s->published = 0;
    if (s->dir >= 0 && !fp_stage_directory(s) && !error) error = errno;
    return error;
}
static int fp_stage_dispose(fp_stage *s)
{
    int error = fp_stage_finish(s);
    if (s->dir >= 0) {
        if (!fp_stage_directory(s)) { if (!error) error = errno; }
        else if (unlinkat(s->parent, s->name, AT_REMOVEDIR) && !error) error = errno;
        close(s->dir);
    }
    if (s->parent >= 0) close(s->parent);
    fp_stage_init(s);
    return error;
}
static int fp_stage_payload(fp_stage *s, const char *target)
{
    if (target) {
        if (symlinkat(target, s->dir, "payload")) return errno;
        s->file = openat(s->dir, "payload", O_PATH | O_NOFOLLOW | O_CLOEXEC);
    } else s->file = openat(s->dir, "payload", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (s->file < 0 || fstat(s->file, &s->object)) return errno;
    s->leaf = 1;
    return 0;
}
/* target != NULL creates a literal symlink; otherwise a regular R/W file. */
static int fp_stage_create(fp_stage *s, int parent, const char *target)
{
    fp_stage_init(s);
    s->parent = fcntl(parent, F_DUPFD_CLOEXEC, 0);
    if (s->parent < 0) return errno;
    int error = 0, created = 0;
    for (unsigned attempt = 0; attempt < 16; attempt++) {
        unsigned char random[16];
        size_t have = 0;
        while (have < sizeof random) {
            ssize_t n = getrandom(random + have, sizeof random - have, GRND_NONBLOCK);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { error = n < 0 ? errno : EIO; goto failed; }
            have += (size_t)n;
        }
        memcpy(s->name, ".openpilot-stage-", 17);
        static const char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < sizeof random; i++) {
            s->name[17 + i * 2] = hex[random[i] >> 4];
            s->name[18 + i * 2] = hex[random[i] & 15];
        }
        s->name[49] = 0;
        if (!mkdirat(s->parent, s->name, 0700)) { created = 1; break; }
        if (errno != EEXIST) { error = errno; goto failed; }
    }
    if (!created) { error = EEXIST; goto failed; }
    s->dir = openat(s->parent, s->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (s->dir < 0) { error = errno; goto failed; }
    if (fstat(s->dir, &s->directory) || !fp_stage_directory(s)) { error = errno; goto failed; }
    error = fp_stage_payload(s, target);
    if (!error) return 0;
failed:
    fp_stage_dispose(s);
    return error ? error : EIO;
}
/* At most one workspace is cached by each serial helper. Parent changes
 * retire it; every reuse revalidates its identity and private permissions.
 * An injected payload is rejected by O_EXCL/symlinkat and never removed. */
static int fp_stage_reuse(fp_stage *s, int parent, const char *target)
{
    if (s->file >= 0) return EBUSY;
    if (s->dir >= 0) {
        struct stat a, b;
        if (fstat(parent, &a) || fstat(s->parent, &b)) return errno;
        if (!fp_stage_same(&a, &b)) {
            int error = fp_stage_dispose(s);
            if (error) return error;
        } else {
            if (!fp_stage_directory(s)) return errno;
            return fp_stage_payload(s, target);
        }
    }
    return fp_stage_create(s, parent, target);
}
static int fp_stage_validate(fp_stage *s)
{
    struct stat fd, name;
    if (!fp_stage_directory(s)) return errno;
    if (s->file < 0 || fstat(s->file, &fd) || fstatat(s->dir, "payload", &name, AT_SYMLINK_NOFOLLOW)) return ESTALE;
    if (!fp_stage_same(&s->object, &fd) || !fp_stage_same(&fd, &name) ||
        fd.st_mode != name.st_mode || fd.st_uid != name.st_uid || fd.st_gid != name.st_gid ||
        fd.st_size != name.st_size || fd.st_mtim.tv_sec != name.st_mtim.tv_sec ||
        fd.st_mtim.tv_nsec != name.st_mtim.tv_nsec || fd.st_ctim.tv_sec != name.st_ctim.tv_sec ||
        fd.st_ctim.tv_nsec != name.st_ctim.tv_nsec) return ESTALE;
    return 0;
}
/* Flush after final metadata, before the caller's last source/destination
 * guards. Symlinks have O_PATH descriptors: persist their creating directory.
 * Namespace guards must be repeated after a potentially blocking flush. */
static int fp_stage_sync_payload(fp_stage *s)
{
    int error = fp_stage_validate(s);
    if (!error) error = fp_stage_sync(S_ISLNK(s->object.st_mode) ? s->dir : s->file);
    return error ? error : fp_stage_validate(s);
}
/* Call only after sync_payload and the caller's final guards. A rename can
 * succeed even when a following flush fails: mark it published immediately so
 * cleanup retains the complete destination and callers retain their source. */
static int fp_stage_rename(fp_stage *s, const char *name, int replace)
{
    int error = fp_stage_validate(s);
    if (error) return error;
    int rc = replace ? renameat(s->dir, "payload", s->parent, name) :
        (int)syscall(SYS_renameat2, s->dir, "payload", s->parent, name, RENAME_NOREPLACE);
    if (rc) return errno;
    s->published = 1;
    return 0;
}
/* The serial path keeps its original rename-plus-both-barriers contract.
 * A bounded owner may rename after its late guards, then delegate only the
 * namespace barriers. It must not report completion before those finish. */
static int fp_stage_publish(fp_stage *s, const char *name, int replace)
{
    int error = fp_stage_rename(s, name, replace);
    return error ? error : fp_stage_sync_pair(s->parent, s->dir);
}
#endif
