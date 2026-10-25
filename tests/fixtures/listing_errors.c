/* Test-only statx fault/deletion boundary. Never linked into shipped folderwing-wsl. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int __real_statx(int, const char *, int, unsigned, struct statx *);
int __wrap_statx(int fd, const char *name, int flags, unsigned mask, struct statx *out)
{
    const char *action = getenv("FP_LIST_TEST_ACTION");
    if (action && !strcmp(name, "fault")) {
        if (!strcmp(action, "delete")) {
            if (unlinkat(fd, name, 0)) return -1;
        } else {
            errno = atoi(action);
            return -1;
        }
    }
    return __real_statx(fd, name, flags, mask, out);
}

/* Only listing mode is exercised. Keep unrelated helper entrypoints inert. */
int fp_rename_main(void) { return 99; }
int fp_sizes_main(void) { return 99; }
int fp_search_main(void) { return 99; }
int fp_index_main(void) { return 99; }
int fp_hash_main(void) { return 99; }
int fp_guard_main(void) { return 99; }
int fp_permission_main(void) { return 99; }
int fp_path_main(void) { return 99; }
int fp_jobs_main(void) { return 99; }
int fp_transfer_main(int importing) { (void)importing; return 99; }
int fp_shell_main(const char *path) { (void)path; return 99; }
