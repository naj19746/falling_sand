#include "progress.h"

#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "sim.h"

#define PXO_POLL_MS     5000    /* stat() Pixelorama's config this often */
#define KANA_POLL_MS    30000   /* ask the kana database this often */
#define KANA_MAX_BYTES  (8 << 20)   /* a state document is ~15 KB */
#define MAX_PENDING     100     /* critters waiting; more are dropped */
#define PATH_LEN        1024

static SDL_mutex *lock;
static SDL_cond *wake;

/* Guarded by lock: the queue the main loop takes critters from, and
 * whether it changed since the state file was written. */
static int8_t pending[MAX_PENDING];
static int npending;
static int dirty;

/* The rest belongs to the thread once it's started. */
static char kana_base[256];
static char pxo_path[PATH_LEN];
static char state_path[PATH_LEN];

/* Pixelorama: the config's mtime when last read (pxo_known = 0 until the
 * first look), and every .pxo path seen. */
static int pxo_known;
static struct timespec pxo_mtime;
static char **seen;
static int nseen, cap_seen;

/* Kana: per practice database, the last sessionsCompleted handled and the
 * lastSession.endedAt that went with it ("" if it had none), as the raw
 * JSON token, so a number or a string both work. */
#define ENDED_LEN 64
typedef struct {
    const char *name;
    int known;
    long count;
    char ended[ENDED_LEN];
} KanaDb;
static KanaDb kana[] = { { "hiragana-practice", 0, 0, "" },
                         { "katakana-practice", 0, 0, "" } };
#define NKANA ((int)(sizeof kana / sizeof kana[0]))

static uint32_t rng_state;

static int poll_ms(int normal)
{
    /* For tests: poll everything this often instead. */
    const char *e = getenv("SANDBG_PROGRESS_POLL_MS");
    int ms = e ? atoi(e) : 0;
    return ms > 0 ? ms : normal;
}

/* ---- the queue ---- */

static void earn(int kind)
{
    SDL_LockMutex(lock);
    if (npending < MAX_PENDING)
        pending[npending++] = (int8_t)kind;
    dirty = 1;
    SDL_UnlockMutex(lock);
}

int progress_next(void)
{
    if (!lock)
        return PROGRESS_NONE;
    SDL_LockMutex(lock);
    int kind = npending ? pending[0] : PROGRESS_NONE;
    SDL_UnlockMutex(lock);
    return kind;
}

void progress_dropped(void)
{
    if (!lock)
        return;
    SDL_LockMutex(lock);
    if (npending > 0) {
        memmove(pending, pending + 1, (size_t)--npending);
        dirty = 1;
    }
    SDL_UnlockMutex(lock);
    /* The thread writes the file; no disk work on the main loop. */
    SDL_CondSignal(wake);
}

static const char *kind_name(int kind)
{
    return kind == CR_SMALL ? "small" : kind == CR_LARGE ? "large" : "any";
}

/* ---- seen paths ---- */

static int is_seen(const char *path)
{
    for (int i = 0; i < nseen; i++)
        if (!strcmp(seen[i], path))
            return 1;
    return 0;
}

static void add_seen(const char *path)
{
    if (is_seen(path) || strchr(path, '\n'))
        return;
    if (nseen == cap_seen) {
        int cap = cap_seen ? cap_seen * 2 : 64;
        char **n = realloc(seen, (size_t)cap * sizeof *n);
        if (!n)
            return;
        seen = n;
        cap_seen = cap;
    }
    char *copy = strdup(path);
    if (copy)
        seen[nseen++] = copy;
}

/* ---- the state file ---- */

static void state_file_path(char *out, size_t size)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    if (xdg && *xdg)
        snprintf(out, size, "%s/sandbg/progress", xdg);
    else
        snprintf(out, size, "%s/.local/state/sandbg/progress",
                 getenv("HOME") ? getenv("HOME") : ".");
}

/* mkdir -p for the directory holding path. */
static void make_parent_dirs(const char *path)
{
    char dir[PATH_LEN];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash || slash == dir)
        return;
    *slash = '\0';
    for (char *p = dir + 1; *p; p++)
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0700);
            *p = '/';
        }
    mkdir(dir, 0700);
}

/* Returns 0 if read, 1 if there's no state file yet. */
static int load_state(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 1;
    char line[PATH_LEN + 64];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        long long sec;
        long nsec, count;
        char name[64], ended[ENDED_LEN];
        if (!strncmp(line, "seen ", 5)) {
            add_seen(line + 5);
        } else if (sscanf(line, "pixelorama_mtime %lld.%ld", &sec, &nsec) == 2) {
            pxo_known = 1;
            pxo_mtime.tv_sec = (time_t)sec;
            pxo_mtime.tv_nsec = nsec;
        } else if (sscanf(line, "kana %63s %ld %63s", name, &count, ended) == 3) {
            for (int i = 0; i < NKANA; i++)
                if (!strcmp(kana[i].name, name)) {
                    kana[i].known = 1;
                    kana[i].count = count;
                    /* "-" stands for no lastSession. */
                    snprintf(kana[i].ended, ENDED_LEN, "%s", strcmp(ended, "-") ? ended : "");
                }
        } else if (!strncmp(line, "pending", 7)) {
            /* One letter per critter: a(ny), s(mall), l(arge). */
            for (const char *p = line + 7; *p && npending < MAX_PENDING; p++)
                if (*p == 'a' || *p == 's' || *p == 'l')
                    pending[npending++] = *p == 's' ? CR_SMALL :
                                          *p == 'l' ? CR_LARGE : CR_ANY;
        }
    }
    fclose(f);
    return 0;
}

/* Written to a temp file and renamed over, so a crash never leaves half
 * of one. */
static void save_state(void)
{
    int8_t q[MAX_PENDING];
    SDL_LockMutex(lock);
    int n = npending;
    memcpy(q, pending, (size_t)n);
    dirty = 0;
    SDL_UnlockMutex(lock);

    char tmp[PATH_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", state_path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "progress: can't write %s\n", tmp);
        return;
    }
    fprintf(f, "# sandbg: rewards already counted and critters waiting to drop in\n");
    for (int i = 0; i < NKANA; i++)
        if (kana[i].known)
            fprintf(f, "kana %s %ld %s\n", kana[i].name, kana[i].count,
                    kana[i].ended[0] ? kana[i].ended : "-");
    if (pxo_known)
        fprintf(f, "pixelorama_mtime %lld.%09ld\n",
                (long long)pxo_mtime.tv_sec, (long)pxo_mtime.tv_nsec);
    fprintf(f, "pending ");
    for (int i = 0; i < n; i++)
        fputc(q[i] == CR_SMALL ? 's' : q[i] == CR_LARGE ? 'l' : 'a', f);
    fputc('\n', f);
    for (int i = 0; i < nseen; i++)
        fprintf(f, "seen %s\n", seen[i]);
    int bad = ferror(f);
    bad |= fflush(f) != 0 || fsync(fileno(f)) != 0;
    bad |= fclose(f) != 0;
    if (bad || rename(tmp, state_path) != 0) {
        fprintf(stderr, "progress: can't write %s\n", state_path);
        unlink(tmp);
    }
}

/* ---- Pixelorama ---- */

/* Pixelorama writes config.ini (a Godot ConfigFile) on every save, with
 * last_project_path set to the file just saved. recent_projects only
 * changes when a project is opened, so it's read only to mark old
 * drawings seen. */
typedef struct {
    char last[PATH_LEN];
    char dir[PATH_LEN];
    char recent[16][PATH_LEN];
    int nrecent;
} PxoConfig;

/* Copies a Godot quoted string starting at the quote; returns the
 * character after the closing quote, or NULL. */
static const char *read_quoted(const char *p, char *out, size_t size)
{
    if (*p != '"')
        return NULL;
    size_t n = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1])
            p++;
        if (n + 1 < size)
            out[n++] = *p;
    }
    out[n] = '\0';
    return *p == '"' ? p + 1 : NULL;
}

static int read_pxo_config(const char *path, PxoConfig *c)
{
    memset(c, 0, sizeof *c);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    /* recent_projects can be long: up to 16 paths on one line. */
    static char line[16 * PATH_LEN + 64];
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, "last_project_path=", 18)) {
            read_quoted(line + 18, c->last, sizeof c->last);
        } else if (!strncmp(line, "current_dir=", 12)) {
            read_quoted(line + 12, c->dir, sizeof c->dir);
        } else if (!strncmp(line, "recent_projects=[", 17)) {
            const char *p = line + 17;
            while (c->nrecent < 16 && (p = strchr(p, '"')) != NULL) {
                p = read_quoted(p, c->recent[c->nrecent], PATH_LEN);
                if (!p)
                    break;
                c->nrecent++;
            }
        }
    }
    fclose(f);
    return 0;
}

static int is_pxo(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".pxo");
}

static int ts_before(struct timespec a, struct timespec b)
{
    return a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec);
}

/* A .pxo drawing on disk that hasn't been seen: it's new if it was written
 * since Pixelorama last wrote its config (since = NULL marks it seen
 * without paying). Opening an old drawing puts it in the config too, but
 * the file itself is older. Returns 1 if it pays. */
static int check_pxo(const char *path, const struct timespec *since)
{
    struct stat st;
    if (!is_pxo(path) || is_seen(path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    add_seen(path);
    return since && !ts_before(st.st_mtim, *since);
}

/* Every .pxo in a directory, so a "Save as" missed between two polls
 * still counts, and old drawings beside new ones never do. */
static int scan_dir(const char *dir, const struct timespec *since)
{
    if (!*dir)
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int paid = 0;
    struct dirent *e;
    char path[PATH_LEN];
    while ((e = readdir(d)) != NULL)
        if (is_pxo(e->d_name) &&
            (size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) < sizeof path)
            paid += check_pxo(path, since);
    closedir(d);
    return paid;
}

static void dir_of(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", path);
    char *slash = strrchr(out, '/');
    if (slash)
        *slash = '\0';
    else
        out[0] = '\0';
}

/* Returns 1 if anything changed that should be saved. */
static int poll_pixelorama(void)
{
    struct stat st;
    if (stat(pxo_path, &st) != 0) {
        if (!pxo_known) {
            /* No config yet: drawings in the one that turns up later are
             * new if they were saved after now. */
            pxo_known = 1;
            clock_gettime(CLOCK_REALTIME, &pxo_mtime);
            return 1;
        }
        return 0;
    }
    if (pxo_known && st.st_mtim.tv_sec == pxo_mtime.tv_sec &&
        st.st_mtim.tv_nsec == pxo_mtime.tv_nsec)
        return 0;

    PxoConfig c;
    if (read_pxo_config(pxo_path, &c) != 0)
        return 0;
    /* The first look marks everything already there as seen. */
    const struct timespec *since = pxo_known ? &pxo_mtime : NULL;
    char dir[PATH_LEN];
    int paid = check_pxo(c.last, since);
    dir_of(c.last, dir, sizeof dir);
    paid += scan_dir(dir, since);
    paid += scan_dir(c.dir, since);
    for (int i = 0; i < c.nrecent; i++) {
        paid += check_pxo(c.recent[i], since);
        if (!since) {
            dir_of(c.recent[i], dir, sizeof dir);
            scan_dir(dir, NULL);
        }
    }
    for (int i = 0; i < paid; i++) {
        earn(CR_ANY);
        fprintf(stderr, "progress: new drawing, a critter is on its way\n");
    }
    pxo_known = 1;
    pxo_mtime = st.st_mtim;
    return 1;
}

/* ---- JSON, just enough of it ---- */

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        p++;
    return p;
}

/* Past the JSON value starting at p (strings, objects and arrays nest);
 * NULL if it's cut short. */
static const char *json_skip(const char *p)
{
    p = skip_ws(p);
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++)
            if (*p == '\\' && p[1])
                p++;
        return *p ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') {
                p = json_skip(p);
                if (!p)
                    return NULL;
                p--;
            } else if (*p == '{' || *p == '[') {
                depth++;
            } else if ((*p == '}' || *p == ']') && --depth == 0) {
                return p + 1;
            }
        }
        return NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n')
        p++;
    return p;
}

/* The value of key among obj's own members (not nested ones), or NULL. */
static const char *json_get(const char *obj, const char *key)
{
    if (!obj)
        return NULL;
    const char *p = skip_ws(obj);
    if (*p != '{')
        return NULL;
    size_t klen = strlen(key);
    for (p = skip_ws(p + 1); *p == '"'; ) {
        const char *k = p + 1;
        const char *end = json_skip(p);
        if (!end)
            return NULL;
        p = skip_ws(end);
        if (*p != ':')
            return NULL;
        p = skip_ws(p + 1);
        if ((size_t)(end - 1 - k) == klen && !strncmp(k, key, klen))
            return p;
        p = json_skip(p);
        if (!p)
            return NULL;
        p = skip_ws(p);
        if (*p != ',')
            return NULL;
        p = skip_ws(p + 1);
    }
    return NULL;
}

/* A number member of obj; -1 if it's missing or not a number. */
static int json_num(const char *obj, const char *key, double *out)
{
    const char *v = json_get(obj, key);
    if (!v)
        return -1;
    char *end;
    *out = strtod(v, &end);
    return end == v ? -1 : 0;
}

/* A string member of obj, unescaped only for \" and \\; -1 if missing. */
static int json_str(const char *obj, const char *key, char *out, size_t size)
{
    const char *v = json_get(obj, key);
    return v && read_quoted(v, out, size) ? 0 : -1;
}

/* A member as its raw token: a string's contents or a number's digits.
 * Spaces become '_' so it fits on one state file line. */
static int json_token(const char *obj, const char *key, char *out, size_t size)
{
    const char *v = json_get(obj, key);
    if (!v)
        return -1;
    if (*v == '"') {
        if (!read_quoted(v, out, size))
            return -1;
    } else {
        const char *end = json_skip(v);
        size_t n = end ? (size_t)(end - v) : 0;
        if (n == 0 || n >= size || *v == '{' || *v == '[')
            return -1;
        memcpy(out, v, n);
        out[n] = '\0';
    }
    for (char *c = out; *c; c++)
        if (*c == ' ' || *c == '\t' || *c == '\n')
            *c = '_';
    return out[0] && strcmp(out, "null") ? 0 : -1;
}

/* ---- kana practice ---- */

/* What one GET /api/db/<name> says. The service answers
 * {"state": <AppState or null>, "rev": n}. */
typedef struct {
    long count;             /* state.sessionsCompleted */
    int setting_minutes;    /* state.settings.sessionMinutes, 0 if missing */
    int has_last;           /* state.lastSession is there */
    char ended[ENDED_LEN];  /* lastSession.endedAt */
    int minutes;            /* lastSession.minutes */
    char reason[32];        /* lastSession.endReason */
} KanaSeen;

/* Fetch one database with curl. 0 on success. */
static int kana_fetch(const char *base, const char *name, KanaSeen *k)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd, "curl -s --fail -m 5 '%s/api/db/%s' 2>/dev/null", base, name);
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    size_t cap = 64 << 10, n = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (n + 1 == cap) {
            char *b = cap < KANA_MAX_BYTES ? realloc(buf, cap * 2) : NULL;
            if (!b) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = b;
            cap *= 2;
        }
        size_t got = fread(buf + n, 1, cap - 1 - n, p);
        if (got == 0)
            break;
        n += got;
    }
    int failed = pclose(p) != 0 || !buf;
    if (failed) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    memset(k, 0, sizeof *k);
    const char *state = json_get(buf, "state");
    double v;
    int ok = state != NULL;
    if (ok && !strncmp(state, "null", 4)) {
        /* Never practised: nothing saved yet. */
    } else if (ok && json_num(state, "sessionsCompleted", &v) == 0) {
        k->count = (long)v;
        if (json_num(json_get(state, "settings"), "sessionMinutes", &v) == 0)
            k->setting_minutes = (int)v;
        const char *last = json_get(state, "lastSession");
        if (last && *last == '{' && json_token(last, "endedAt", k->ended, sizeof k->ended) == 0) {
            k->has_last = 1;
            if (json_num(last, "minutes", &v) == 0)
                k->minutes = (int)v;
            json_str(last, "endReason", k->reason, sizeof k->reason);
        }
    } else {
        ok = 0;
    }
    free(buf);
    return ok ? 0 : -1;
}

static uint32_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* The pay for one finished set of this many minutes. */
static void pay_set(const char *name, int minutes)
{
    int n = 0;
    if (minutes >= 15) {
        /* Two critters, one of each kind: a colony needs both. */
        earn(CR_SMALL);
        earn(CR_LARGE);
        n = 2;
    } else if (minutes >= 10 || (minutes >= 5 && (rng() & 1))) {
        earn(CR_ANY);
        n = 1;
    }
    fprintf(stderr, "progress: %s: a %d-minute set, %d critter%s\n",
            name, minutes, n, n == 1 ? "" : "s");
}

/* Returns 1 if anything changed that should be saved. */
static int poll_kana(KanaDb *db)
{
    KanaSeen k;
    /* Down or unreachable: change nothing, and catch up when it's back. */
    if (kana_fetch(kana_base, db->name, &k) != 0)
        return 0;
    const char *ended = k.has_last ? k.ended : "";
    int new_last = k.has_last && strcmp(ended, db->ended) != 0;

    if (db->known && k.count > db->count) {
        long sets = k.count - db->count;
        if (new_last) {
            /* The latest set says how it ended. Sets missed while the
             * service was down count at its length, since theirs isn't
             * known. Quitting early or running out of kana pays nothing. */
            if (!strcmp(k.reason, "time") || !strcmp(k.reason, "cards"))
                pay_set(db->name, k.minutes);
            for (long i = 1; i < sets; i++)
                pay_set(db->name, k.minutes);
        } else {
            /* An app without lastSession: every set counts, at the length
             * chosen in its settings. */
            for (long i = 0; i < sets; i++)
                pay_set(db->name, k.setting_minutes);
        }
    }
    /* The first look, or a reset or import that lowered the count, only
     * sets where counting starts from. */
    if (db->known && db->count == k.count && !new_last)
        return 0;
    db->known = 1;
    db->count = k.count;
    snprintf(db->ended, ENDED_LEN, "%s", ended);
    return 1;
}

/* ---- the thread ---- */

static int progress_thread(void *unused)
{
    (void)unused;
    Uint64 next_pxo = 0, next_kana = 0;
    for (;;) {
        Uint64 now = SDL_GetTicks64();
        int changed = 0;
        if (kana_base[0] && now >= next_kana) {
            for (int i = 0; i < NKANA; i++)
                changed |= poll_kana(&kana[i]);
            next_kana = SDL_GetTicks64() + (Uint64)poll_ms(KANA_POLL_MS);
        }
        if (pxo_path[0] && now >= next_pxo) {
            changed |= poll_pixelorama();
            next_pxo = now + (Uint64)poll_ms(PXO_POLL_MS);
        }

        SDL_LockMutex(lock);
        changed |= dirty;
        SDL_UnlockMutex(lock);
        if (changed)
            save_state();

        /* Sleep until the next poll, or until a critter drops in. */
        Uint64 wake_at = !pxo_path[0] ? next_kana :
                         !kana_base[0] || next_pxo < next_kana ? next_pxo : next_kana;
        now = SDL_GetTicks64();
        SDL_LockMutex(lock);
        if (!dirty && wake_at > now)
            SDL_CondWaitTimeout(wake, lock, (Uint32)(wake_at - now));
        SDL_UnlockMutex(lock);
    }
    return 0;
}

/* Only one sandbg may collect rewards, or a --windowed copy beside the
 * desktop one would pay each of them twice. Held until exit. */
static int take_lock(void)
{
    char path[PATH_LEN + 8];
    snprintf(path, sizeof path, "%s.lock", state_path);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;   /* can't lock; run anyway */
    return flock(fd, LOCK_EX | LOCK_NB) == 0 ? 0 : -1;
}

int progress_start(const char *kana_url, const char *pxo_config)
{
    if (!*kana_url && !*pxo_config)
        return -1;
    snprintf(kana_base, sizeof kana_base, "%s", kana_url);
    /* So "http://host:8080/" works too. */
    size_t n = strlen(kana_base);
    while (n > 0 && kana_base[n - 1] == '/')
        kana_base[--n] = '\0';
    snprintf(pxo_path, sizeof pxo_path, "%s", pxo_config);
    rng_state = (uint32_t)time(NULL) ^ (uint32_t)SDL_GetPerformanceCounter() ^ 0x9e3779b9u;
    if (!rng_state)
        rng_state = 1;
    state_file_path(state_path, sizeof state_path);
    make_parent_dirs(state_path);
    if (take_lock() != 0) {
        fprintf(stderr, "progress: another sandbg is collecting rewards\n");
        return -1;
    }
    load_state(state_path);

    lock = SDL_CreateMutex();
    wake = SDL_CreateCond();
    SDL_Thread *t = lock && wake ? SDL_CreateThread(progress_thread, "progress", NULL) : NULL;
    if (!t)
        return -1;
    SDL_DetachThread(t);
    return 0;
}

/* ---- --progress ---- */

static void print_pixelorama(const char *path)
{
    PxoConfig c;
    struct stat st;
    if (!*path) {
        printf("pixelorama: off\n");
        return;
    }
    if (stat(path, &st) != 0 || read_pxo_config(path, &c) != 0) {
        printf("pixelorama: can't read %s\n", path);
        return;
    }
    char when[64];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&st.st_mtim.tv_sec));
    printf("pixelorama: %s (written %s)\n", path, when);
    if (c.last[0])
        printf("  newest drawing: %s%s\n", c.last,
               is_seen(c.last) ? " (already counted)" : "");
    else
        printf("  no drawing saved yet\n");
}

static void print_kana(const char *base, const KanaDb *db)
{
    KanaSeen k;
    if (kana_fetch(base, db->name, &k) != 0) {
        printf("%s: can't reach %s/api/db/%s\n", db->name, base, db->name);
        return;
    }
    printf("%s: %ld sets done (%d minutes chosen)", db->name, k.count, k.setting_minutes);
    if (db->known)
        printf(", %ld already counted", db->count);
    printf("\n");
    if (k.has_last)
        printf("  last set: %d minutes, ended by %s, at %s%s\n", k.minutes,
               k.reason[0] ? k.reason : "?", k.ended,
               !strcmp(k.ended, db->ended) ? " (already counted)" : "");
    else
        printf("  no lastSession: every set counts at the chosen length\n");
}

int progress_print(const char *kana_url, const char *pxo_config)
{
    state_file_path(state_path, sizeof state_path);
    int none = load_state(state_path);

    char base[256];
    snprintf(base, sizeof base, "%s", kana_url);
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/')
        base[--n] = '\0';
    if (!*base)
        printf("kana: off\n");
    for (int i = 0; *base && i < NKANA; i++)
        print_kana(base, &kana[i]);
    print_pixelorama(pxo_config);

    printf("state: %s%s\n", state_path, none ? " (not written yet)" : "");
    printf("critters waiting: %d", npending);
    for (int i = 0; i < npending; i++)
        printf("%s%s", i ? ", " : " (", kind_name(pending[i]));
    printf("%s\n", npending ? ")" : "");
    return 0;
}
