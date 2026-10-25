/* Standalone benchmark probe, deliberately outside the product build.
 * Build with the same static-musl target as folderwing-wsl and a pinned SQLite
 * amalgamation. Each invocation measures one fresh connection (warm OS
 * cache), then seven warm queries. RSS excludes the Python benchmark driver. */
#define _POSIX_C_SOURCE 200809L
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/resource.h>

static void check(int rc, sqlite3 *db)
{
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite %d: %s\n", rc, db ? sqlite3_errmsg(db) : "open failed");
        exit(1);
    }
}
static double now(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t)) exit(1);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1000000.0;
}
static unsigned long peak_rss(void)
{
    /* ru_maxrss can include the Python parent's pre-exec high-water mark on
     * Linux. VmHWM belongs to this executable's new address space. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) exit(1);
    char line[256];
    unsigned long peak = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "VmHWM: %lu kB", &peak) == 1) break;
    fclose(f);
    if (!peak) exit(1);
    return peak;
}
int main(int argc, char **argv)
{
    if (argc != 4 || (strcmp(argv[2], "scan") && strcmp(argv[2], "trigram") && strcmp(argv[2], "rebuild"))) {
        fprintf(stderr, "usage: wsl_index_probe DATABASE scan|trigram|rebuild GLOB_PATTERN\n");
        return 2;
    }
    sqlite3 *db = NULL;
    int rebuild = !strcmp(argv[2], "rebuild");
    check(sqlite3_open_v2(argv[1], &db, rebuild ? SQLITE_OPEN_READWRITE : SQLITE_OPEN_READONLY, NULL), db);
    check(sqlite3_exec(db, "PRAGMA cache_size=-16384; PRAGMA mmap_size=0; PRAGMA temp_store=FILE;", NULL, NULL, NULL), db);
    sqlite3_stmt *marker = NULL;
    check(sqlite3_prepare_v2(db, "PRAGMA application_id", -1, &marker, NULL), db);
    if (sqlite3_step(marker) != SQLITE_ROW || sqlite3_column_int(marker, 0) != 0x4f504942) {
        fprintf(stderr, "Only an OpenPilot benchmark database may be opened\n");
        return 1;
    }
    check(sqlite3_finalize(marker), db);
    if (rebuild) {
        check(sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;", NULL, NULL, NULL), db);
        double start = now();
        check(sqlite3_exec(db, "BEGIN IMMEDIATE; INSERT INTO names(names) VALUES('rebuild');"
                          "INSERT INTO names(names) VALUES('optimize'); COMMIT;", NULL, NULL, NULL), db);
        double elapsed = now() - start;
        struct rusage usage;
        if (getrusage(RUSAGE_SELF, &usage)) return 1;
        printf("{\"sqlite_version\":\"%s\",\"sqlite_source_id\":\"%s\",\"rebuild_ms\":%.6f,"
               "\"sqlite_peak_allocated_bytes\":%lld,\"peak_process_rss_kib\":%lu,"
               "\"inherited_ru_maxrss_kib\":%ld}\n",
               sqlite3_libversion(), sqlite3_sourceid(), elapsed,
               (long long)sqlite3_memory_highwater(0), peak_rss(), usage.ru_maxrss);
        check(sqlite3_close(db), db);
        return 0;
    }
    const char *sql = !strcmp(argv[2], "trigram") ?
        "SELECT e.path,e.size,e.mtime,e.kind FROM names n JOIN entries e ON e.id=n.rowid WHERE n.name GLOB ? LIMIT 301" :
        "SELECT path,size,mtime,kind FROM entries WHERE name GLOB ? LIMIT 301";
    double elapsed[8];
    unsigned expected = 0;
    for (int i = 0; i < 8; i++) {
        double start = now();
        sqlite3_stmt *s = NULL;
        check(sqlite3_prepare_v2(db, sql, -1, &s, NULL), db);
        check(sqlite3_bind_text(s, 1, argv[3], -1, SQLITE_STATIC), db);
        unsigned rows = 0;
        int rc;
        while ((rc = sqlite3_step(s)) == SQLITE_ROW) {
            /* Consume the same path and metadata columns as the palette. */
            if (sqlite3_column_type(s, 0) != SQLITE_BLOB || !sqlite3_column_bytes(s, 0)) return 1;
            (void)sqlite3_column_blob(s, 0);
            for (int k = 1; k < 4; k++) (void)sqlite3_column_int64(s, k);
            rows++;
        }
        if (rc != SQLITE_DONE) check(rc, db);
        check(sqlite3_finalize(s), db);
        elapsed[i] = now() - start;
        if (i && rows != expected) return 1;
        expected = rows;
    }
    int cache = 0, unused = 0;
    check(sqlite3_db_status(db, SQLITE_DBSTATUS_CACHE_USED, &cache, &unused, 0), db);
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) return 1;
    printf("{\"sqlite_version\":\"%s\",\"sqlite_source_id\":\"%s\",\"rows\":%u,"
           "\"first_query_ms\":%.6f,\"warm_query_ms\":[", sqlite3_libversion(), sqlite3_sourceid(), expected, elapsed[0]);
    for (int i = 1; i < 8; i++) printf("%s%.6f", i == 1 ? "" : ",", elapsed[i]);
    printf("],\"sqlite_allocated_bytes\":%lld,\"sqlite_peak_allocated_bytes\":%lld,"
           "\"sqlite_cache_bytes\":%d,\"peak_process_rss_kib\":%lu,\"inherited_ru_maxrss_kib\":%ld}\n",
           (long long)sqlite3_memory_used(), (long long)sqlite3_memory_highwater(0), cache, peak_rss(), usage.ru_maxrss);
    check(sqlite3_close(db), db);
    return 0;
}
