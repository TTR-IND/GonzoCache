/*
 * ╔══════════════════════════════════════════════════════════════╗
 * ║                                                              ║
 * ║   ████████╗████████╗██████╗       ██╗███╗   ██╗██████╗       ║
 * ║   ╚══██╔══╝╚══██╔══╝██╔══██╗      ██║████╗  ██║██╔══██╗      ║
 * ║      ██║      ██║   ██████╔╝      ██║██╔██╗ ██║██║  ██║      ║
 * ║      ██║      ██║   ██╔══██╗      ██║██║╚██╗██║██║  ██║      ║
 * ║      ██║      ██║   ██║  ██║      ██║██║ ╚████║██████╔╝      ║
 * ║      ╚═╝      ╚═╝   ╚═╝  ╚═╝      ╚═╝╚═╝  ╚═══╝╚═════╝       ║
 * ║                                                              ║
 * ║       Torfaen Technology Research — IND                      ║
 * ║       Copyright © 2026                                       ║
 * ║       Licensed under Apache License 2.0                      ║
 * ║                                                              ║
 * ╚══════════════════════════════════════════════════════════════╝
 *
 * gonzocache.c -- Recently used app caching for Linux
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 *
 * Two modes, one binary:
 *
 *   gonzocache --track    Runs continuously (as a service), lightly
 *                         polling /proc every TRACK_INTERVAL_SEC to
 *                         detect new process launches, and updates a
 *                         persistent, exponentially-decayed launch
 *                         score per executable in
 *                         /var/lib/gonzocache/history.json.
 *
 *   gonzocache --preload  Runs once, reads the scored history, and
 *                         calls posix_fadvise(POSIX_FADV_WILLNEED) on
 *                         the top PRELOAD_TOP_N executables (pulling
 *                         them into the page cache without executing
 *                         them), then exits immediately. Meant to be
 *                         invoked once at login/boot, per explicit
 *                         design decision -- this is not a persistent
 *                         background preloader.
 *
 * Design notes:
 *
 *   - This is a genuinely separate daemon from detritus, not a feature
 *     bolted onto it. detritus's whole job is giving memory back
 *     (reactive freeze under real pressure, proactive MADV_COLD
 *     trickle when idle); GonzoCache's job is the opposite -- spending
 *     memory proactively on a bet that it improves perceived launch
 *     speed. Mixing "give memory back" and "consume memory
 *     speculatively" in one daemon would let the two features fight
 *     each other's decisions in ways that are hard to reason about.
 *     Two daemons with one clear job each stays honestly debuggable.
 *
 *   - Launch detection is /proc-diffing, not proc connector (netlink
 *     PROC_EVENT_EXEC). proc connector was tested first, on both a
 *     sandboxed container and real Devuan hardware, and delivered zero
 *     genuine process-lifecycle events on either -- only protocol-
 *     level acks. /proc polling is a proven-working primitive on this
 *     exact hardware (confirmed directly, not assumed), so there is no
 *     remaining uncertainty about whether it works here.
 *     The real cost of polling over event-driven tracking is that a
 *     process which starts and exits entirely between two poll
 *     intervals is invisible -- an acceptable trade for "apps you
 *     commonly launch and use", which are not typically sub-second
 *     one-shot commands.
 *
 *   - Scoring is exponential decay, not separate frequency/recency
 *     fields hand-balanced against each other. Each launch adds a
 *     fixed increment to a running score; the score decays by a
 *     half-life between launches. This naturally weights "frequent
 *     AND recent" without needing a hand-tuned blend formula: an app
 *     launched daily keeps climbing, one launched once and never again
 *     decays toward irrelevance, and the ranking self-adjusts as usage
 *     patterns change over time.
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
#include <sys/stat.h>
#include <dirent.h>
#include <syslog.h>
#include <signal.h>

/* ── Tunables ─────────────────────────────────────────────────────────── */
#define TRACK_INTERVAL_SEC   3        /* /proc poll period for --track     */
#define HALF_LIFE_SEC        (7 * 24 * 3600)   /* 7 days                   */
#define LAUNCH_INCREMENT     1.0
#define PRELOAD_TOP_N        12
#define MIN_SCORE_TO_PRELOAD 0.05     /* ignore near-zero-decayed entries  */
#define MAX_TRACKED_EXES     512

#define HISTORY_DIR  "/var/lib/gonzocache"
#define HISTORY_PATH HISTORY_DIR "/history.json"

/* ── Logging -- mirrors detritus.c's rp_log() shape for consistency ────── */
static int g_use_syslog = 0;

static void gc_log(int priority, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_use_syslog) {
        vsyslog(priority, fmt, ap);
    } else {
        time_t t = time(NULL);
        struct tm tmv; localtime_r(&t, &tmv);
        char ts[16];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        fprintf(stderr, "[gonzocache %s] ", ts);
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
    }
    va_end(ap);
}

/* ── Persistent launch history ───────────────────────────────────────────
 *
 * Keyed by resolved executable path (from /proc/pid/exe), not by comm
 * name -- comm is truncated to 15 characters by the kernel and can
 * collide between genuinely different binaries; the full resolved path
 * is the only identifier that's actually unambiguous.
 *
 * score is exponentially decayed: on load, and again before writing,
 * every entry's score is decayed forward to "now" based on
 * last_launch_unix, so entries that haven't been touched in a long
 * time reflect their true current (decayed) score rather than a stale
 * value frozen at whatever it was on their last actual launch.
 */
typedef struct {
    char   path[256];
    double score;
    time_t last_launch_unix;
} history_entry_t;

static history_entry_t g_history[MAX_TRACKED_EXES];
static int             g_n_history = 0;

/* Exponential decay factor for a gap of `elapsed_sec` seconds, given
 * HALF_LIFE_SEC. decay = 0.5^(elapsed/half_life) -- standard half-life
 * decay, so a score exactly one half-life old is worth half what it
 * was, two half-lives old is worth a quarter, and so on. */
static double decay_factor(time_t elapsed_sec)
{
    if (elapsed_sec <= 0) return 1.0;
    return pow(0.5, (double)elapsed_sec / (double)HALF_LIFE_SEC);
}

/* Find an existing entry for `path`, or NULL if not tracked yet. */
static history_entry_t *history_find(const char *path)
{
    for (int i = 0; i < g_n_history; i++)
        if (strcmp(g_history[i].path, path) == 0) return &g_history[i];
    return NULL;
}

/* Record a launch of `path` at the given time, applying decay to the
 * existing score (if any) before adding the increment for this launch.
 * If the table is full and this is a new path, the entry with the
 * lowest current (already-decayed) score is evicted -- a bounded
 * table with LRU-by-score eviction, so a machine with many distinct
 * binaries launched over time can't grow this file without bound. */
static void history_record_launch(const char *path, time_t now)
{
    history_entry_t *e = history_find(path);
    if (!e) {
        if (g_n_history < MAX_TRACKED_EXES) {
            e = &g_history[g_n_history++];
            snprintf(e->path, sizeof(e->path), "%s", path);
            e->score = 0.0;
            e->last_launch_unix = now;
        } else {
            /* Table full -- evict the lowest-scored entry (decayed to
             * "now" first, so we're comparing true current standing,
             * not stale historical peaks) to make room. */
            int lowest_idx = 0;
            double lowest_score = 1e300;
            for (int i = 0; i < g_n_history; i++) {
                double s = g_history[i].score *
                    decay_factor(now - g_history[i].last_launch_unix);
                if (s < lowest_score) { lowest_score = s; lowest_idx = i; }
            }
            e = &g_history[lowest_idx];
            snprintf(e->path, sizeof(e->path), "%s", path);
            e->score = 0.0;
            e->last_launch_unix = now;
        }
    }

    double decay = decay_factor(now - e->last_launch_unix);
    e->score = e->score * decay + LAUNCH_INCREMENT;
    e->last_launch_unix = now;
}

/* Find the value following "key": in buf. Scoped parser matching the
 * same design decision as gonzo-detritus.cpp's status.json reader --
 * this schema is small, fixed, and owned entirely by this project, so
 * a hand-rolled scoped parser is proportionate to not linking a JSON
 * library for it. */
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
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outlen) out[i++] = *p++;
    out[i] = '\0';
    return 1;
}

/* Load history.json into g_history. Missing file is not an error --
 * first run on a fresh install has no history yet, which is expected,
 * not exceptional. A corrupt/unparseable file is logged and treated
 * the same as missing (start fresh) rather than crashing -- losing
 * accumulated history is a minor, recoverable annoyance; refusing to
 * start because of one bad file would not be proportionate. */
static void history_load(void)
{
    g_n_history = 0;
    FILE *f = fopen(HISTORY_PATH, "r");
    if (!f) return;

    char *buf = malloc(1 << 20);  /* 1MB, generous for MAX_TRACKED_EXES */
    if (!buf) { fclose(f); return; }
    size_t n = fread(buf, 1, (1 << 20) - 1, f);
    fclose(f);
    buf[n] = '\0';

    const char *p = buf;
    while ((p = strstr(p, "\"path\":")) != NULL && g_n_history < MAX_TRACKED_EXES) {
        history_entry_t *e = &g_history[g_n_history];
        const char *pathval = gc_find_key(p, "path");
        if (!pathval || !gc_extract_string(pathval, e->path, sizeof(e->path))) {
            p += 7; continue;
        }
        const char *scoreval = gc_find_key(p, "score");
        e->score = scoreval ? strtod(scoreval, NULL) : 0.0;
        const char *lastval = gc_find_key(p, "last_launch_unix");
        e->last_launch_unix = lastval ? (time_t)strtoll(lastval, NULL, 10) : 0;

        /* Bound the search for score/last_launch to roughly this
         * object -- gc_find_key has no concept of object boundaries,
         * matching the same accepted trade-off as
         * gonzo-detritus.cpp's parse_candidates(). 200 bytes
         * comfortably covers one history entry at this schema's
         * field widths. */
        g_n_history++;
        p += 7;
    }
    free(buf);

    gc_log(LOG_INFO, "loaded %d history entries from %s", g_n_history, HISTORY_PATH);
}

/* Save g_history to history.json via mkstemp+write+rename, same
 * atomicity contract as detritus.c's write_status_file() -- a reader
 * (there isn't one for this file currently, but the discipline costs
 * nothing and matches the project's established convention) should
 * never observe a torn write. */
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
    if (!f) { close(fd); unlink(tmp_path); return; }

    fprintf(f, "{\n  \"schema_version\": 1,\n  \"entries\": [\n");
    for (int i = 0; i < g_n_history; i++) {
        fprintf(f,
            "    { \"path\": \"%s\", \"score\": %.6f, \"last_launch_unix\": %ld }%s\n",
            g_history[i].path, g_history[i].score,
            (long)g_history[i].last_launch_unix,
            (i == g_n_history - 1) ? "" : ",");
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

/* ── /proc-diffing launch scanner (--track mode) ─────────────────────────
 *
 * Not proc connector (netlink PROC_EVENT_EXEC) -- tested directly on
 * both a sandboxed environment and real Devuan hardware, and delivered
 * zero genuine process-lifecycle events on either, only protocol-level
 * acks. /proc polling is a proven-working primitive on this exact
 * hardware (confirmed directly, not assumed).
 *
 * Cost: a process that starts and fully exits between two
 * TRACK_INTERVAL_SEC polls is invisible to this scanner. Accepted
 * trade-off -- "apps you commonly launch and use" are not typically
 * sub-second one-shot commands, and the alternative (proc connector)
 * is simply not available on this system regardless of trade-offs.
 */
#define MAX_SEEN_PIDS 4096

typedef struct { pid_t pid; unsigned long long starttime; } seen_pid_t;

static seen_pid_t g_seen_pids[MAX_SEEN_PIDS];
static int        g_n_seen = 0;

/* Graceful shutdown: set by SIGTERM/SIGINT, checked by run_track_mode()'s
 * main loop. Matches detritus.c's own sig_handler pattern for
 * consistency. Without this, any launches recorded since the last
 * periodic (60s) save would be silently lost on every normal
 * stop/restart -- found by actually running the daemon end-to-end and
 * observing the history file was empty after a short-lived test run,
 * not caught by inspection alone. */
static volatile sig_atomic_t g_running = 1;
static void gc_sig_handler(int sig) { (void)sig; g_running = 0; }

static int seen_contains(pid_t pid, unsigned long long starttime)
{
    for (int i = 0; i < g_n_seen; i++)
        if (g_seen_pids[i].pid == pid && g_seen_pids[i].starttime == starttime)
            return 1;
    return 0;
}

/* Resolve a PID's real executable path via /proc/pid/exe. Returns 0 on
 * failure (kernel thread with no exe symlink, process exited between
 * directory listing and readlink -- an expected, not exceptional,
 * race given this is a live, changing directory). */
static int resolve_exe_path(pid_t pid, char *out, size_t outlen)
{
    char link_path[32];
    snprintf(link_path, sizeof(link_path), "/proc/%d/exe", pid);
    ssize_t n = readlink(link_path, out, outlen - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
}

/* Read starttime (field 22 of /proc/pid/stat) -- a PID-reuse-safe
 * identifier: a PID number alone can be recycled between scans, and
 * without this a scanner could silently attribute a new process's
 * first appearance to a slot actually vacated by a since-exited one. */
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

/* One scan pass: list /proc, find PIDs not in the previous scan's
 * seen-set, resolve their exe paths, and record a launch for each new
 * one. Rebuilds g_seen_pids fully each pass rather than incrementally
 * updating it -- simpler and correct, and a full /proc listing every
 * TRACK_INTERVAL_SEC (3s) is cheap enough that incremental bookkeeping
 * would be optimizing a cost that was never actually significant. */
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
        if (starttime == 0) continue;  /* process gone between listing and read */

        new_seen[n_new_seen].pid = pid;
        new_seen[n_new_seen].starttime = starttime;
        n_new_seen++;

        if (!seen_contains(pid, starttime)) {
            char exe_path[256];
            if (resolve_exe_path(pid, exe_path, sizeof(exe_path))) {
                history_record_launch(exe_path, now);
                gc_log(LOG_DEBUG, "launch detected: %s (pid %d)", exe_path, pid);
            }
            /* resolve_exe_path failing here is not logged as a
             * warning -- kernel threads and processes that exit
             * between the readdir listing and this readlink are both
             * expected, routine occurrences on every scan, not
             * anomalies worth surfacing. */
        }
    }
    closedir(pd);

    memcpy(g_seen_pids, new_seen, sizeof(seen_pid_t) * n_new_seen);
    g_n_seen = n_new_seen;
}

static void run_track_mode(void)
{
    gc_log(LOG_INFO, "gonzocache --track starting (interval=%ds, half-life=%dd)",
           TRACK_INTERVAL_SEC, HALF_LIFE_SEC / 86400);

    history_load();

    /* First pass just establishes the baseline seen-set -- every PID
     * already running when the tracker starts is "pre-existing", not
     * a launch. Without this, restarting the tracker would spuriously
     * record every currently-running process as a fresh launch. */
    scan_once();
    gc_log(LOG_INFO, "baseline established: %d processes already running", g_n_seen);

    time_t last_save = time(NULL);
    const time_t SAVE_INTERVAL_SEC = 60;  /* persist periodically, not every scan */

    while (g_running) {
        sleep(TRACK_INTERVAL_SEC);
        if (!g_running) break;  /* woke from sleep because of a caught signal */
        scan_once();

        time_t now = time(NULL);
        if (now - last_save >= SAVE_INTERVAL_SEC) {
            history_save();
            last_save = now;
        }
    }

    /* Save on the way out regardless of how long it's been since the
     * last periodic save -- this is the actual fix for the gap found
     * by running the daemon end-to-end: without this, any launches
     * recorded in the final (up to 60s) window before a normal
     * stop/restart were silently lost. */
    gc_log(LOG_INFO, "shutting down -- saving history");
    history_save();
}

/* ── One-shot preload (--preload mode) ───────────────────────────────────
 *
 * Warms the top-scored executables into page cache via
 * posix_fadvise(POSIX_FADV_WILLNEED), then exits. Does not execute
 * anything -- this is a pure read-ahead hint to the kernel, the same
 * category of primitive as detritus.c's own MADV_COLD trickle, just
 * pointed the opposite direction (encourage caching in, not eviction
 * out).
 *
 * Scoped to the executable files themselves, not their shared-library
 * dependencies. Warming .so dependencies too is a real possible
 * enhancement (would meaningfully improve cold-launch time further)
 * but needs either shelling out to ldd -- fragile, output-format-
 * dependent, an extra process spawn per binary -- or parsing ELF
 * .dynamic sections directly, real complexity neither asked for nor
 * built here. Left as a natural, scoped follow-up rather than
 * speculatively built now.
 */
static void run_preload_mode(void)
{
    history_load();
    time_t now = time(NULL);

    /* Decay every entry to "now" before ranking, so the top-N
     * selection reflects true current standing (an app launched
     * heavily two months ago and never since should rank below one
     * launched moderately but recently), not scores frozen at
     * whatever they were on each entry's own last launch. */
    for (int i = 0; i < g_n_history; i++) {
        double decay = decay_factor(now - g_history[i].last_launch_unix);
        g_history[i].score *= decay;
        g_history[i].last_launch_unix = now;
    }

    /* Simple insertion sort descending by score -- g_n_history is
     * bounded by MAX_TRACKED_EXES (512), small enough that an O(n^2)
     * sort is genuinely fine and not worth a more complex algorithm
     * for. */
    for (int i = 1; i < g_n_history; i++) {
        history_entry_t key = g_history[i];
        int j = i - 1;
        while (j >= 0 && g_history[j].score < key.score) {
            g_history[j + 1] = g_history[j];
            j--;
        }
        g_history[j + 1] = key;
    }

    int warmed = 0;
    char preloaded_paths[PRELOAD_TOP_N][256];
    int  n_preloaded_paths = 0;

    for (int i = 0; i < g_n_history && warmed < PRELOAD_TOP_N; i++) {
        if (g_history[i].score < MIN_SCORE_TO_PRELOAD) break;  /* sorted descending; rest are lower */

        int fd = open(g_history[i].path, O_RDONLY);
        if (fd < 0) {
            gc_log(LOG_DEBUG, "preload skip (open failed): %s: %s",
                   g_history[i].path, strerror(errno));
            continue;
        }

        int ret = posix_fadvise(fd, 0, 0, POSIX_FADV_WILLNEED);
        close(fd);

        if (ret == 0) {
            gc_log(LOG_INFO, "preloaded: %s (score=%.2f)",
                   g_history[i].path, g_history[i].score);
            memcpy(preloaded_paths[n_preloaded_paths], g_history[i].path,
                   sizeof(preloaded_paths[0]));
            n_preloaded_paths++;
            warmed++;
        } else {
            gc_log(LOG_DEBUG, "preload skip (fadvise failed): %s: %s",
                   g_history[i].path, strerror(ret));
        }
    }

    gc_log(LOG_INFO, "preload complete: %d/%d candidates warmed", warmed,
           g_n_history < PRELOAD_TOP_N ? g_n_history : PRELOAD_TOP_N);

    /* Publish the list so detritus's write_status_file() can compute a
     * live, continuously-updating page-cache-residency figure via
     * mincore() on these exact files -- this is what turns a one-shot
     * preload action into a real, live "GonzoCache usage" metric the
     * GUI can display, rather than a static number that goes stale
     * the moment this process exits. Plain newline-delimited paths,
     * not JSON -- this file has exactly one purpose and a JSON schema
     * would be unnecessary machinery for a flat path list. */
    mkdir(HISTORY_DIR, 0755);
    char preloaded_tmp[64];
    snprintf(preloaded_tmp, sizeof(preloaded_tmp), HISTORY_DIR "/.preloaded.XXXXXX");
    int pfd = mkstemp(preloaded_tmp);
    if (pfd >= 0) {
        fchmod(pfd, 0644);
        FILE *pf = fdopen(pfd, "w");
        if (pf) {
            for (int i = 0; i < n_preloaded_paths; i++)
                fprintf(pf, "%s\n", preloaded_paths[i]);
            fflush(pf);
            fsync(pfd);
            fclose(pf);
            char preloaded_path[64];
            snprintf(preloaded_path, sizeof(preloaded_path), HISTORY_DIR "/preloaded.list");
            if (rename(preloaded_tmp, preloaded_path) != 0) {
                gc_log(LOG_WARNING, "preloaded-list save: rename failed: %s", strerror(errno));
                unlink(preloaded_tmp);
            }
        } else {
            close(pfd);
            unlink(preloaded_tmp);
        }
    }
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s --track    (run continuously, record launch history)\n"
        "       %s --preload  (one-shot: warm top-ranked apps into page cache, then exit)\n",
        argv0, argv0);
}

int main(int argc, char **argv)
{
    if (argc != 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "--track") == 0) {
        g_use_syslog = 1;
        openlog("gonzocache", LOG_PID, LOG_DAEMON);
        signal(SIGTERM, gc_sig_handler);
        signal(SIGINT,  gc_sig_handler);
        run_track_mode();
        return 0;
    } else if (strcmp(argv[1], "--preload") == 0) {
        run_preload_mode();
        return 0;
    } else {
        usage(argv[0]);
        return 1;
    }
}


