/*
 * gonzocache.c -- Launch tracker and watermarked file cache for Linux
 *
 * Licensed under the Apache License, Version 2.0.
 *
 * Two modes, one binary:
 *
 *   gonzocache --track    Root service. Records launches with an
 *                         exponentially-decayed score, and holds the
 *                         top executables (plus the files they actually
 *                         mapped at last launch) in the page cache.
 *                         A monitor thread drops that hold when
 *                         MemAvailable crosses the low watermark, and
 *                         warms again when RAM is comfortable.
 *
 *   gonzocache --preload  One-shot login helper. posix_fadvise + a
 *                         sequential touch of the same ranked list,
 *                         then exits. Does not pin. Safe to run as
 *                         the desktop user.
 *
 * The cache is file-backed. Pages live in the kernel page cache, which
 * is what a subsequent exec() actually hits. A private anonymous copy
 * of the file (mmap ANONYMOUS + read + mlock) is a second heap that
 * exec() will not use — that was the old hotcache mistake.
 *
 * Pinning uses mlock on the file map, and only in --track when running
 * as root. mlock is the "stay resident across streaming I/O" part.
 * Without it the kernel can evict the warm pages the moment a video
 * player reads a few hundred megabytes. With it, only this daemon's
 * watermark may drop them.
 *
 * Detritus is a separate process. This daemon spends file-cache on a
 * launch-speed bet. Detritus gives anonymous memory back under
 * pressure. They meet only at /var/lib/gonzocache/preloaded.list.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <syslog.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ── Tunables ─────────────────────────────────────────────────────────── */

#define TRACK_INTERVAL_SEC     3
#define HALF_LIFE_SEC          (7 * 24 * 3600)
#define LAUNCH_INCREMENT       1.0
#define PRELOAD_TOP_N          12
#define MIN_SCORE_TO_PRELOAD   0.05
#define MAX_TRACKED_EXES       512
#define MAX_FILES_PER_EXE      24
#define MAX_HOLD               64

#define SAVE_INTERVAL_SEC      60
#define POLL_INTERVAL_MS       500

#define CACHE_BUDGET_PERCENT   15          /* of MemTotal                     */
#define CACHE_BUDGET_CAP_BYTES (384ul * 1024ul * 1024ul)
#define MAX_FILE_BYTES         (32ul * 1024ul * 1024ul)
#define READ_CHUNK_SIZE        (1 * 1024 * 1024)

#define LOW_WATERMARK_KB       (400 * 1024)
#define RECOVER_WATERMARK_KB   (600 * 1024)
#define WARM_MIN_AVAIL_KB      (800 * 1024)

#define HISTORY_DIR  "/var/lib/gonzocache"
#define HISTORY_PATH HISTORY_DIR "/history.json"
#define PRELOADED_PATH HISTORY_DIR "/preloaded.list"
#define STATS_PATH     HISTORY_DIR "/stats"

/* ── Logging ──────────────────────────────────────────────────────────── */

static int g_use_syslog = 0;

static void gc_log(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_use_syslog) {
        vsyslog(priority, fmt, ap);
    } else {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        fprintf(stderr, "[gonzocache %s] ", ts);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

/* ── Meminfo ──────────────────────────────────────────────────────────── */

static long read_meminfo_kb(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long val = -1;
    size_t klen = strlen(key);
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0) {
            sscanf(line + klen, "%ld", &val);
            break;
        }
    }
    fclose(f);
    return val;
}

/* ── History ──────────────────────────────────────────────────────────── */

typedef struct {
    char   path[256];
    double score;
    time_t last_launch_unix;
    char   files[MAX_FILES_PER_EXE][256];
    int    n_files;
} history_entry_t;

static history_entry_t g_history[MAX_TRACKED_EXES];
static int             g_n_history = 0;

static const char *SKIP_BASENAME[] = {
    "gonzocache", "detritusd", "rookpager",
    "sh", "dash", "bash", "login", "sudo", "su",
    "sleep", "cat", "sed", "awk", "grep",
    NULL
};

static const char *basename_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static int should_skip_path(const char *path)
{
    if (!path || path[0] != '/') return 1;
    if (strncmp(path, "/proc/", 6) == 0) return 1;
    if (strncmp(path, "/dev/", 5) == 0) return 1;
    const char *base = basename_of(path);
    for (int i = 0; SKIP_BASENAME[i]; i++)
        if (strcmp(base, SKIP_BASENAME[i]) == 0) return 1;
    return 0;
}

static double decay_factor(time_t elapsed_sec)
{
    if (elapsed_sec <= 0) return 1.0;
    return pow(0.5, (double)elapsed_sec / (double)HALF_LIFE_SEC);
}

static history_entry_t *history_find(const char *path)
{
    for (int i = 0; i < g_n_history; i++)
        if (strcmp(g_history[i].path, path) == 0) return &g_history[i];
    return NULL;
}

static void files_add(history_entry_t *e, const char *path)
{
    if (!e || should_skip_path(path)) return;
    if (e->n_files >= MAX_FILES_PER_EXE) return;
    for (int i = 0; i < e->n_files; i++)
        if (strcmp(e->files[i], path) == 0) return;
    snprintf(e->files[e->n_files], sizeof(e->files[0]), "%s", path);
    e->n_files++;
}

static void harvest_maps(pid_t pid, history_entry_t *e)
{
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, '/');
        if (!p) continue;
        size_t n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r')) p[--n] = '\0';
        char *cut = strchr(p, ' ');
        if (cut) *cut = '\0';
        files_add(e, p);
    }
    fclose(f);
}

static void history_record_launch(const char *path, pid_t pid, time_t now)
{
    if (should_skip_path(path)) return;

    history_entry_t *e = history_find(path);
    if (!e) {
        if (g_n_history < MAX_TRACKED_EXES) {
            e = &g_history[g_n_history++];
            memset(e, 0, sizeof(*e));
            snprintf(e->path, sizeof(e->path), "%s", path);
            e->last_launch_unix = now;
        } else {
            int lowest_idx = 0;
            double lowest_score = 1e300;
            for (int i = 0; i < g_n_history; i++) {
                double s = g_history[i].score *
                    decay_factor(now - g_history[i].last_launch_unix);
                if (s < lowest_score) {
                    lowest_score = s;
                    lowest_idx = i;
                }
            }
            e = &g_history[lowest_idx];
            memset(e, 0, sizeof(*e));
            snprintf(e->path, sizeof(e->path), "%s", path);
            e->last_launch_unix = now;
        }
    }

    double decay = decay_factor(now - e->last_launch_unix);
    e->score = e->score * decay + LAUNCH_INCREMENT;
    e->last_launch_unix = now;
    files_add(e, path);
    harvest_maps(pid, e);
}

static const char *gc_find_key(const char *buf, const char *key)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char *p = strstr(buf, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

static int gc_extract_string(const char *p, char *out, size_t outlen)
{
    if (!p || *p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outlen) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

static void history_load(void)
{
    g_n_history = 0;
    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) return;

    char *buf = malloc(1 << 20);
    if (!buf) {
        fclose(f);
        return;
    }
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *p = buf;
    while ((p = strstr(p, "\"path\":")) != NULL && g_n_history < MAX_TRACKED_EXES) {
        history_entry_t *e = &g_history[g_n_history];
        memset(e, 0, sizeof(*e));
        const char *pathval = gc_find_key(p, "path");
        if (!pathval || !gc_extract_string(pathval, e->path, sizeof(e->path))) {
            p += 7;
            continue;
        }
        const char *scoreval = gc_find_key(p, "score");
        e->score = scoreval ? strtod(scoreval, NULL) : 0.0;
        const char *lastval = gc_find_key(p, "last_launch_unix");
        e->last_launch_unix = lastval ? (time_t)strtoll(lastval, NULL, 10) : 0;

        const char *files = strstr(p, "\"files\":");
        const char *next = strstr(p + 7, "\"path\":");
        if (files && (!next || files < next)) {
            const char *q = files;
            while ((q = strstr(q, "\"")) != NULL && e->n_files < MAX_FILES_PER_EXE) {
                if (next && q >= next) break;
                char tmp[256];
                if (!gc_extract_string(q, tmp, sizeof(tmp))) break;
                if (strcmp(tmp, "files") != 0 && tmp[0] == '/')
                    files_add(e, tmp);
                q += strlen(tmp) + 2;
                if (*q == ']') break;
            }
        }

        g_n_history++;
        p += 7;
    }
    free(buf);
    gc_log(LOG_INFO, "loaded %d history entries from %s", g_n_history, HISTORY_PATH);
}

static void history_save(void)
{
    mkdir(HISTORY_DIR, 0755);
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path), HISTORY_DIR "/.history.XXXXXX");
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        gc_log(LOG_WARNING, "history save: mkstemp failed: %s", strerror(errno));
        return;
    }
    fchmod(fd, 0644);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return;
    }

    fprintf(f, "{\n  \"schema_version\": 1,\n  \"entries\": [\n");
    for (int i = 0; i < g_n_history; i++) {
        fprintf(f,
            "    { \"path\": \"%s\", \"score\": %.6f, \"last_launch_unix\": %ld, \"files\": [",
            g_history[i].path, g_history[i].score,
            (long)g_history[i].last_launch_unix);
        for (int k = 0; k < g_history[i].n_files; k++)
            fprintf(f, "%s\"%s\"", k ? ", " : "", g_history[i].files[k]);
        fprintf(f, "] }%s\n", (i == g_n_history - 1) ? "" : ",");
    }
    fprintf(f, "  ]\n}\n");
    fflush(f);
    fsync(fd);
    fclose(f);
    if (rename(tmp_path, HISTORY_PATH) != 0) {
        gc_log(LOG_WARNING, "history save: rename failed: %s", strerror(errno));
        unlink(tmp_path);
    }
}

static void history_rank_now(void)
{
    time_t now = time(NULL);
    for (int i = 0; i < g_n_history; i++) {
        g_history[i].score *= decay_factor(now - g_history[i].last_launch_unix);
        g_history[i].last_launch_unix = now;
    }
    for (int i = 1; i < g_n_history; i++) {
        history_entry_t key = g_history[i];
        int j = i - 1;
        while (j >= 0 && g_history[j].score < key.score) {
            g_history[j + 1] = g_history[j];
            j--;
        }
        g_history[j + 1] = key;
    }
}

/* ── Hold table (file-backed maps) ────────────────────────────────────── */

typedef struct {
    void  *map;
    size_t size;
    int    fd;
    int    locked;
    char   path[256];
} hold_slot_t;

static hold_slot_t     g_hold[MAX_HOLD];
static int             g_n_hold = 0;
static size_t          g_hold_bytes = 0;
static size_t          g_budget_bytes = 0;
static pthread_mutex_t g_hold_lock = PTHREAD_MUTEX_INITIALIZER;
static int             g_can_lock = 0;

static void write_preloaded_list(void)
{
    mkdir(HISTORY_DIR, 0755);
    char tmp[64];
    snprintf(tmp, sizeof(tmp), HISTORY_DIR "/.preloaded.XXXXXX");
    int fd = mkstemp(tmp);
    if (fd < 0) return;
    fchmod(fd, 0644);
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp);
        return;
    }
    pthread_mutex_lock(&g_hold_lock);
    for (int i = 0; i < g_n_hold; i++)
        fprintf(f, "%s\n", g_hold[i].path);
    size_t held = g_hold_bytes;
    int n = g_n_hold;
    pthread_mutex_unlock(&g_hold_lock);
    fflush(f);
    fsync(fd);
    fclose(f);
    if (rename(tmp, PRELOADED_PATH) != 0) unlink(tmp);

    FILE *sf = fopen(STATS_PATH, "w");
    if (sf) {
        fprintf(sf, "status=ok\n");
        fprintf(sf, "files_held=%d\n", n);
        fprintf(sf, "held_mb=%zu\n", held / 1024 / 1024);
        fprintf(sf, "budget_mb=%zu\n", g_budget_bytes / 1024 / 1024);
        fclose(sf);
    }
}

static void hold_drop_index_unlocked(int i, int drop_pages)
{
    hold_slot_t *s = &g_hold[i];
    if (s->map && s->size) {
        if (s->locked) munlock(s->map, s->size);
        if (drop_pages) madvise(s->map, s->size, MADV_DONTNEED);
        munmap(s->map, s->size);
    }
    if (s->fd >= 0) close(s->fd);
    if (g_hold_bytes >= s->size) g_hold_bytes -= s->size;
    else g_hold_bytes = 0;
    g_hold[i] = g_hold[g_n_hold - 1];
    memset(&g_hold[g_n_hold - 1], 0, sizeof(g_hold[0]));
    g_n_hold--;
}

static void hold_drop_all(int drop_pages)
{
    pthread_mutex_lock(&g_hold_lock);
    while (g_n_hold > 0)
        hold_drop_index_unlocked(g_n_hold - 1, drop_pages);
    pthread_mutex_unlock(&g_hold_lock);
}

static int hold_contains_unlocked(const char *path)
{
    for (int i = 0; i < g_n_hold; i++)
        if (strcmp(g_hold[i].path, path) == 0) return 1;
    return 0;
}

/*
 * Map the file shared (so the pages ARE the page cache), fault it in
 * with a sequential read-sized touch, optionally mlock.
 * Returns bytes held, or 0 on skip/fail.
 */
static size_t hold_one(const char *path, int pin)
{
    if (!path || path[0] != '/') return 0;

    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
        return 0;
    size_t file_size = (size_t)st.st_size;
    if (file_size > MAX_FILE_BYTES) file_size = MAX_FILE_BYTES;

    pthread_mutex_lock(&g_hold_lock);
    if (hold_contains_unlocked(path) || g_n_hold >= MAX_HOLD ||
        g_hold_bytes + file_size > g_budget_bytes) {
        pthread_mutex_unlock(&g_hold_lock);
        return 0;
    }
    pthread_mutex_unlock(&g_hold_lock);

    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    void *map = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return 0;
    }

    /*
     * Sequential touch. On eMMC a 1MB stride matches the device's
     * happy sequential width better than a page-at-a-time walk, and
     * posix_fadvise alone is only a hint — the kernel may ignore it
     * under load. Touching forces the pages in.
     */
    posix_fadvise(fd, 0, (off_t)file_size, POSIX_FADV_WILLNEED);
    volatile unsigned char acc = 0;
    const unsigned char *p = (const unsigned char *)map;
    for (size_t off = 0; off < file_size; off += READ_CHUNK_SIZE)
        acc ^= p[off];
    acc ^= p[file_size - 1];
    (void)acc;

    int locked = 0;
    if (pin && g_can_lock) {
        if (mlock(map, file_size) == 0) locked = 1;
    }

    pthread_mutex_lock(&g_hold_lock);
    if (g_n_hold >= MAX_HOLD || g_hold_bytes + file_size > g_budget_bytes) {
        pthread_mutex_unlock(&g_hold_lock);
        if (locked) munlock(map, file_size);
        munmap(map, file_size);
        close(fd);
        return 0;
    }
    hold_slot_t *s = &g_hold[g_n_hold++];
    s->map = map;
    s->size = file_size;
    s->fd = fd;
    s->locked = locked;
    snprintf(s->path, sizeof(s->path), "%s", path);
    g_hold_bytes += file_size;
    pthread_mutex_unlock(&g_hold_lock);
    return file_size;
}

static int warm_from_history(int pin)
{
    history_rank_now();

    long avail = read_meminfo_kb("MemAvailable:");
    if (avail >= 0 && avail < WARM_MIN_AVAIL_KB) {
        gc_log(LOG_INFO, "warm skipped — MemAvailable=%ld MiB", avail / 1024);
        return 0;
    }

    int warmed = 0;
    for (int i = 0; i < g_n_history && warmed < PRELOAD_TOP_N; i++) {
        if (g_history[i].score < MIN_SCORE_TO_PRELOAD) break;

        size_t got = hold_one(g_history[i].path, pin);
        if (got) {
            gc_log(LOG_INFO, "held: %s (score=%.2f %zu KiB%s)",
                   g_history[i].path, g_history[i].score, got / 1024,
                   pin ? " pinned" : "");
            warmed++;
        }
        for (int k = 0; k < g_history[i].n_files; k++) {
            if (strcmp(g_history[i].files[k], g_history[i].path) == 0)
                continue;
            hold_one(g_history[i].files[k], pin);
        }
    }
    write_preloaded_list();
    gc_log(LOG_INFO, "warm complete: %d apps, %zu MiB / %zu MiB budget",
           warmed, g_hold_bytes / 1024 / 1024, g_budget_bytes / 1024 / 1024);
    return warmed;
}

static void purge_until(long target_kb)
{
    int dropped = 0;
    pthread_mutex_lock(&g_hold_lock);
    while (g_n_hold > 0) {
        long avail = read_meminfo_kb("MemAvailable:");
        if (avail < 0 || avail >= target_kb) break;
        /* Drop the last slot — newest hold is the speculative tail. */
        gc_log(LOG_INFO, "purge: %s (%zu KiB)",
               g_hold[g_n_hold - 1].path, g_hold[g_n_hold - 1].size / 1024);
        hold_drop_index_unlocked(g_n_hold - 1, 1);
        dropped++;
    }
    pthread_mutex_unlock(&g_hold_lock);
    if (dropped) write_preloaded_list();
}

static volatile sig_atomic_t g_running = 1;
static void gc_sig_handler(int sig) { (void)sig; g_running = 0; }

static void *monitor_thread(void *arg)
{
    (void)arg;
    int purged = 0;
    while (g_running) {
        struct timespec ts = {
            .tv_sec  = POLL_INTERVAL_MS / 1000,
            .tv_nsec = (POLL_INTERVAL_MS % 1000) * 1000000L,
        };
        nanosleep(&ts, NULL);
        if (!g_running) break;

        long avail = read_meminfo_kb("MemAvailable:");
        if (avail < 0) continue;

        if (avail < LOW_WATERMARK_KB && g_n_hold > 0) {
            gc_log(LOG_INFO, "pressure: MemAvailable=%ld MiB — purging hold",
                   avail / 1024);
            purge_until(RECOVER_WATERMARK_KB);
            purged = 1;
        } else if (purged && avail > WARM_MIN_AVAIL_KB && g_n_hold == 0) {
            gc_log(LOG_INFO, "recovered: MemAvailable=%ld MiB — rewarming",
                   avail / 1024);
            warm_from_history(1);
            purged = 0;
        }
    }
    return NULL;
}

/* ── Tracker ──────────────────────────────────────────────────────────── */

#define MAX_SEEN_PIDS 4096

typedef struct { pid_t pid; unsigned long long starttime; } seen_pid_t;

static seen_pid_t g_seen_pids[MAX_SEEN_PIDS];
static int        g_n_seen = 0;

static int seen_contains(pid_t pid, unsigned long long starttime)
{
    for (int i = 0; i < g_n_seen; i++)
        if (g_seen_pids[i].pid == pid && g_seen_pids[i].starttime == starttime)
            return 1;
    return 0;
}

static int resolve_exe_path(pid_t pid, char *out, size_t outlen)
{
    char link_path[32];
    snprintf(link_path, sizeof(link_path), "/proc/%d/exe", pid);
    ssize_t n = readlink(link_path, out, outlen - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
}

static unsigned long long read_starttime(pid_t pid)
{
    char path[32];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    unsigned long long starttime = 0;
    if (fgets(buf, sizeof(buf), f)) {
        char *rp = strrchr(buf, ')');
        if (rp) {
            sscanf(rp + 2,
                "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                "%*u %*u %*d %*d %*d %*d %*d %*d %llu", &starttime);
        }
    }
    fclose(f);
    return starttime;
}

static void scan_once(void)
{
    seen_pid_t new_seen[MAX_SEEN_PIDS];
    int n_new_seen = 0;
    time_t now = time(NULL);

    DIR *pd = opendir("/proc");
    if (!pd) return;

    struct dirent *ent;
    while ((ent = readdir(pd)) != NULL && n_new_seen < MAX_SEEN_PIDS) {
        if (ent->d_name[0] < '1' || ent->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 0) continue;

        unsigned long long starttime = read_starttime(pid);
        if (starttime == 0) continue;

        new_seen[n_new_seen].pid = pid;
        new_seen[n_new_seen].starttime = starttime;
        n_new_seen++;

        if (!seen_contains(pid, starttime)) {
            char exe_path[256];
            if (resolve_exe_path(pid, exe_path, sizeof(exe_path)))
                history_record_launch(exe_path, pid, now);
        }
    }
    closedir(pd);
    memcpy(g_seen_pids, new_seen, sizeof(seen_pid_t) * (size_t)n_new_seen);
    g_n_seen = n_new_seen;
}

static void init_budget(void)
{
    long total_kb = read_meminfo_kb("MemTotal:");
    if (total_kb <= 0) total_kb = 2 * 1024 * 1024;
    size_t by_pct = (size_t)total_kb * 1024ull * CACHE_BUDGET_PERCENT / 100ull;
    g_budget_bytes = by_pct < CACHE_BUDGET_CAP_BYTES ? by_pct : CACHE_BUDGET_CAP_BYTES;
    g_can_lock = (geteuid() == 0);
}

static void run_track_mode(void)
{
    init_budget();
    gc_log(LOG_INFO, "gonzocache --track starting (interval=%ds half-life=%dd budget=%zu MiB pin=%s)",
           TRACK_INTERVAL_SEC, HALF_LIFE_SEC / 86400,
           g_budget_bytes / 1024 / 1024, g_can_lock ? "yes" : "no");

    history_load();
    scan_once();
    gc_log(LOG_INFO, "baseline established: %d processes already running", g_n_seen);

    if (g_n_history > 0)
        warm_from_history(1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, monitor_thread, NULL) == 0)
        pthread_detach(tid);
    else
        gc_log(LOG_WARNING, "monitor thread failed: %s", strerror(errno));

    time_t last_save = time(NULL);
    while (g_running) {
        sleep(TRACK_INTERVAL_SEC);
        if (!g_running) break;
        scan_once();
        time_t now = time(NULL);
        if (now - last_save >= SAVE_INTERVAL_SEC) {
            history_save();
            last_save = now;
        }
    }

    gc_log(LOG_INFO, "shutting down — saving history, dropping hold");
    history_save();
    hold_drop_all(1);
}

/*
 * Login one-shot: fault the ranked files into page cache, do not pin,
 * do not stay resident. The maps are released; the pages remain in
 * cache only as long as kswapd leaves them.
 */
static void run_preload_mode(void)
{
    init_budget();
    g_can_lock = 0;
    history_load();
    if (g_n_history == 0) {
        gc_log(LOG_INFO, "preload: no history yet");
        return;
    }
    warm_from_history(0);
    hold_drop_all(0);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s --track    (service: record launches, hold file cache)\n"
        "       %s --preload  (one-shot: warm top-ranked apps, then exit)\n",
        argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "--track") == 0) {
        g_use_syslog = 1;
        openlog("gonzocache", LOG_PID, LOG_DAEMON);
        signal(SIGTERM, gc_sig_handler);
        signal(SIGINT, gc_sig_handler);
        run_track_mode();
        return 0;
    }
    if (strcmp(argv[1], "--preload") == 0) {
        run_preload_mode();
        return 0;
    }
    usage(argv[0]);
    return 1;
}
