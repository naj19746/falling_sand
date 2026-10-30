#include "keyboard.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KEYCHRON_VID 0x3434

/* Refresh rate of the scene scan and the reports sent to the keyboard.
 * The sim runs at 20 fps by default, so this scans every frame; faster
 * sims are capped at about 30 Hz. */
#define KB_SCAN_MS        33
#define KB_KEEPALIVE_MS   1000  /* the firmware gives up after 3 s */
#define KB_RETRY_MS       30000
#define KB_REPORT         32
#define KB_CHUNK          8     /* LEDs per report: 6 header + 24 color + crc */
#define KB_PROTO_VERSION  1

/* 32-byte raw-HID messages. The QMK interface has no numbered reports;
 * write_report() supplies the leading zero report number hidraw wants. */
#define SB_HELLO0 'S'
#define SB_HELLO1 'B'
#define SB_CMD_PING    'P'
#define SB_CMD_COLORS  'C'
#define SB_CMD_RELEASE 'R'

/* The Q4 ANSI's rgb_matrix layout, in LED-index order (the order the
 * firmware expects), from keyboards/keychron/q4/ansi/info.json. row is the
 * physical key row, x is its left edge in key widths, w its width. */
typedef struct {
    uint8_t row;
    float x, w;
} KeyLed;

static const KeyLed q4_ansi[] = {
    {0,  0.00f, 1.00f}, {0,  1.00f, 1.00f}, {0,  2.00f, 1.00f},
    {0,  3.00f, 1.00f}, {0,  4.00f, 1.00f}, {0,  5.00f, 1.00f},
    {0,  6.00f, 1.00f}, {0,  7.00f, 1.00f}, {0,  8.00f, 1.00f},
    {0,  9.00f, 1.00f}, {0, 10.00f, 1.00f}, {0, 11.00f, 1.00f},
    {0, 12.00f, 1.00f}, {0, 13.00f, 2.00f},
    {1,  0.00f, 1.50f}, {1,  1.50f, 1.00f}, {1,  2.50f, 1.00f},
    {1,  3.50f, 1.00f}, {1,  4.50f, 1.00f}, {1,  5.50f, 1.00f},
    {1,  6.50f, 1.00f}, {1,  7.50f, 1.00f}, {1,  8.50f, 1.00f},
    {1,  9.50f, 1.00f}, {1, 10.50f, 1.00f}, {1, 11.50f, 1.00f},
    {1, 12.50f, 1.00f}, {1, 13.50f, 1.50f},
    {2,  0.00f, 1.75f}, {2,  1.75f, 1.00f}, {2,  2.75f, 1.00f},
    {2,  3.75f, 1.00f}, {2,  4.75f, 1.00f}, {2,  5.75f, 1.00f},
    {2,  6.75f, 1.00f}, {2,  7.75f, 1.00f}, {2,  8.75f, 1.00f},
    {2,  9.75f, 1.00f}, {2, 10.75f, 1.00f}, {2, 11.75f, 1.00f},
    {2, 12.75f, 2.25f},
    {3,  0.00f, 2.25f}, {3,  2.25f, 1.00f}, {3,  3.25f, 1.00f},
    {3,  4.25f, 1.00f}, {3,  5.25f, 1.00f}, {3,  6.25f, 1.00f},
    {3,  7.25f, 1.00f}, {3,  8.25f, 1.00f}, {3,  9.25f, 1.00f},
    {3, 10.25f, 1.00f}, {3, 11.25f, 1.00f}, {3, 12.25f, 2.75f},
    {4,  0.00f, 1.25f}, {4,  1.25f, 1.25f}, {4,  2.50f, 1.25f},
    {4,  3.75f, 6.25f}, {4, 10.00f, 1.25f}, {4, 11.25f, 1.25f},
    {4, 12.50f, 1.25f}, {4, 13.75f, 1.25f},
};

/* One LED per key width for the preview, in the same order as q4_ansi.
 * Wide keys occupy several columns, as they would on the keyboard. */
static const uint8_t q4_units[5][15] = {
    { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 13},
    {14, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27},
    {28, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 40},
    {41, 41, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 52},
    {53, 53, 54, 55, 56, 56, 56, 56, 56, 56, 57, 57, 58, 59, 60},
};

typedef struct {
    uint32_t pile, fall, rain, pool, moss;
} KeyCounts;

typedef struct {
    char path[64];
    char name[128];
    unsigned vid, pid;
    int raw;                    /* 1 yes, 0 no, -1 unknown */
} Candidate;

static uint8_t checksum(const uint8_t *b, int n)
{
    uint8_t sum = 0;
    for (int i = 0; i < n; i++)
        sum += b[i];
    return sum;
}

/* HID report descriptor items, looking for the raw HID application
 * collection that QMK exposes at usage page 0xFF60, usage 0x61. */
static int has_raw_usage(const uint8_t *d, size_t n)
{
    uint32_t page = 0, usage = 0;
    size_t i = 0;
    while (i < n) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                    /* long item */
            if (i + 1 >= n)
                return 0;
            size_t len = d[i];
            i += 2;
            if (i + len > n)
                return 0;
            i += len;
            continue;
        }
        int size = b & 3;
        if (size == 3)
            size = 4;
        int type = (b >> 2) & 3;
        int tag = b >> 4;
        if (i + (size_t)size > n)
            return 0;
        uint32_t val = 0;
        for (int k = 0; k < size; k++)
            val |= (uint32_t)d[i + k] << (8 * k);
        i += (size_t)size;

        if (type == 1 && tag == 0)              /* Usage Page */
            page = val;
        else if (type == 2 && tag == 0)         /* Usage */
            usage = val;
        else if (type == 0 && tag == 0xA && val == 1) {  /* Collection */
            if (page == 0xFF60 && (usage == 0x61 || usage == 0x62))
                return 1;
        }
    }
    return 0;
}

static int read_file(const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    *len = fread(buf, 1, cap, f);
    int bad = ferror(f);
    fclose(f);
    return bad ? -1 : 0;
}

/* Keychron hidraw interfaces, with the raw HID one marked. only overrides
 * discovery with a specific /dev/hidrawN. */
static int find_candidates(Candidate *out, int max, const char *only)
{
    int n = 0;
    if (only && *only) {
        memset(&out[0], 0, sizeof out[0]);
        snprintf(out[0].path, sizeof out[0].path, "%s", only);
        const char *base = strrchr(only, '/');
        base = base ? base + 1 : only;
        char sys[512];
        snprintf(sys, sizeof sys, "/sys/class/hidraw/%s/device/report_descriptor", base);
        uint8_t desc[4096];
        size_t len = 0;
        out[0].raw = read_file(sys, desc, sizeof desc, &len) == 0
                     ? has_raw_usage(desc, len) : -1;
        return 1;
    }

    glob_t g;
    if (glob("/sys/class/hidraw/hidraw*", 0, NULL, &g) != 0)
        return 0;

    for (size_t i = 0; i < g.gl_pathc && n < max; i++) {
        char path[512], line[256];
        unsigned bus = 0, vid = 0, pid = 0;
        char name[128] = "";
        snprintf(path, sizeof path, "%s/device/uevent", g.gl_pathv[i]);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "HID_ID=", 7))
                sscanf(line + 7, "%x:%x:%x", &bus, &vid, &pid);
            else if (!strncmp(line, "HID_NAME=", 9)) {
                size_t len = strcspn(line + 9, "\r\n");
                if (len >= sizeof name)
                    len = sizeof name - 1;
                memcpy(name, line + 9, len);
                name[len] = '\0';
            }
        }
        fclose(f);
        if (vid != KEYCHRON_VID)
            continue;

        Candidate *c = &out[n++];
        memset(c, 0, sizeof *c);
        snprintf(c->path, sizeof c->path, "/dev/%s",
                 strrchr(g.gl_pathv[i], '/') + 1);
        snprintf(c->name, sizeof c->name, "%s", name);
        c->vid = vid;
        c->pid = pid;

        snprintf(path, sizeof path, "%s/device/report_descriptor", g.gl_pathv[i]);
        uint8_t desc[4096];
        size_t len = 0;
        if (read_file(path, desc, sizeof desc, &len) == 0)
            c->raw = has_raw_usage(desc, len);
        else
            c->raw = -1;
    }
    globfree(&g);
    return n;
}

static int write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;
    int waits = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits++ < 20) {
            struct pollfd p = { .fd = fd, .events = POLLOUT };
            if (poll(&p, 1, 10) < 0 && errno != EINTR)
                return -1;
            continue;
        }
        return -1;
    }
    return 0;
}

/* hidraw write() wants the report number first, even when the device has
 * none; zero is the report number for a single unnumbered report. */
static int write_report(int fd, const uint8_t *msg)
{
    uint8_t out[KB_REPORT + 1];
    out[0] = 0;
    memcpy(out + 1, msg, KB_REPORT);
    return write_all(fd, out, sizeof out);
}

/* Same, but never waits: a full USB queue shouldn't stall the frame. The
 * next scan sends the whole frame again. */
static ssize_t write_report_nowait(int fd, const uint8_t *msg)
{
    uint8_t out[KB_REPORT + 1];
    out[0] = 0;
    memcpy(out + 1, msg, KB_REPORT);
    return write(fd, out, sizeof out);
}

static int parse_report(const uint8_t *b, int n, int cmd, int *leds)
{
    if (n >= 33 && b[0] == 0) {         /* some interfaces keep the report ID */
        b++;
        n--;
    }
    if (n < KB_REPORT)
        return -1;
    if (b[0] != SB_HELLO0 || b[1] != SB_HELLO1 || b[2] != KB_PROTO_VERSION)
        return -1;
    if (b[3] != cmd || checksum(b, 31) != b[31])
        return -1;
    if (leds)
        *leds = b[5];
    return 0;
}

static int ping_fd(int fd, int *leds)
{
    uint8_t msg[KB_REPORT];
    memset(msg, 0, sizeof msg);
    msg[0] = SB_HELLO0;
    msg[1] = SB_HELLO1;
    msg[2] = KB_PROTO_VERSION;
    msg[3] = SB_CMD_PING;
    msg[31] = checksum(msg, 31);
    if (write_report(fd, msg) != 0)
        return -1;

    struct pollfd p = { .fd = fd, .events = POLLIN };
    int pr;
    do {
        pr = poll(&p, 1, 50);
    } while (pr < 0 && errno == EINTR);
    if (pr <= 0)
        return -1;

    uint8_t buf[64];
    ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0)
        return -1;
    return parse_report(buf, (int)n, SB_CMD_PING, leds);
}

/* Open and greet a candidate. Returns the fd, or -1; *perm is set when the
 * open was refused. */
static int open_ping(const char *path, int *leds, int *perm)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        if (perm && (errno == EACCES || errno == EPERM))
            *perm = 1;
        return -1;
    }
    if (ping_fd(fd, leds) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void build_map(Keyboard *k)
{
    k->n_layout = (int)(sizeof q4_ansi / sizeof q4_ansi[0]);
    int row_h[5] = {0};
    for (int y = 0; y < k->h; y++) {
        int r = (int)((long)y * 5 / k->h);
        if (r > 4)
            r = 4;
        k->row_at[y] = (uint8_t)r;
        row_h[r]++;
    }

    memset(k->rowkey, 0xFF, (size_t)5 * k->w * sizeof *k->rowkey);
    memset(k->area, 0, sizeof k->area);

    for (int r = 0; r < 5; r++) {
        int last = -1;
        for (int x = 0; x < k->w; x++) {
            float u = (x + 0.5f) * 15.0f / k->w;
            int idx = -1;
            for (int i = 0; i < k->n_layout; i++) {
                if (q4_ansi[i].row != r)
                    continue;
                if (u >= q4_ansi[i].x && u < q4_ansi[i].x + q4_ansi[i].w) {
                    idx = i;
                    break;
                }
            }
            if (idx < 0)                /* rounding at an edge: use the last */
                idx = last;
            if (idx < 0)
                continue;               /* only possible before the first key */
            k->rowkey[r * k->w + x] = (int16_t)idx;
            k->area[idx] += (uint32_t)row_h[r];
            last = idx;
        }
    }
}

int keyboard_init(Keyboard *k, int w, int h, const char *mode,
                  const char *device, float brightness)
{
    memset(k, 0, sizeof *k);
    k->backend = KB_OFF;
    k->w = w;
    k->h = h;
    k->fd = -1;
    k->brightness = brightness < 0.0f ? 0.0f : brightness > 1.0f ? 1.0f : brightness;
    if (!(k->brightness >= 0.0f))       /* catches NaN */
        k->brightness = 0.0f;
    if (device)
        snprintf(k->device, sizeof k->device, "%s", device);

    if (!mode || !strcmp(mode, "off"))
        return 0;
    if (w < 1 || h < 1)
        return -1;
    k->backend = !strcmp(mode, "preview") ? KB_PREVIEW : KB_HID;

    k->rowkey = malloc((size_t)5 * w * sizeof *k->rowkey);
    k->row_at = malloc((size_t)h);
    k->support = malloc((size_t)w);
    if (!k->rowkey || !k->row_at || !k->support) {
        keyboard_free(k);
        return -1;
    }
    build_map(k);
    k->n_leds = k->n_layout;
    for (int i = 0; i < 256; i++)
        k->gamma[i] = (uint8_t)(powf(i / 255.0f, 2.2f) * 255.0f + 0.5f);
    if (k->backend == KB_PREVIEW)
        fprintf(stderr, "keyboard preview: colors of the Q4 LEDs, 15 key widths wide\n");
    return 0;
}

void keyboard_free(Keyboard *k)
{
    if (k->connected && k->fd >= 0) {
        uint8_t msg[KB_REPORT];
        memset(msg, 0, sizeof msg);
        msg[0] = SB_HELLO0;
        msg[1] = SB_HELLO1;
        msg[2] = KB_PROTO_VERSION;
        msg[3] = SB_CMD_RELEASE;
        msg[31] = checksum(msg, 31);
        write_report_nowait(k->fd, msg);
    }
    if (k->fd >= 0)
        close(k->fd);
    free(k->rowkey);
    free(k->row_at);
    free(k->support);
    k->rowkey = NULL;
    k->row_at = NULL;
    k->support = NULL;
    k->fd = -1;
    k->connected = 0;
    k->backend = KB_OFF;
}

static void try_connect(Keyboard *k, uint32_t now)
{
    if (k->connected || (k->next_try && now < k->next_try))
        return;
    k->next_try = now + KB_RETRY_MS;

    Candidate c[16];
    int n = find_candidates(c, 16, k->device);
    int found = 0, perm = 0, nofw = 0;
    for (int i = 0; i < n; i++) {
        if (c[i].raw == 0)
            continue;                   /* definitely not the raw HID one */
        found = 1;
        int leds = 0, p = 0;
        int fd = open_ping(c[i].path, &leds, &p);
        if (fd >= 0) {
            k->fd = fd;
            k->connected = 1;
            k->n_leds = leds > 0 && leds < k->n_layout ? leds : k->n_layout;
            memset(k->sent, 0, sizeof k->sent);
            k->last_send = 0;
            fprintf(stderr, "keyboard: %s on %s (%d LEDs)\n",
                    c[i].name[0] ? c[i].name : "Keychron", c[i].path, leds);
            return;
        }
        if (p)
            perm = 1;
        else
            nofw = 1;
    }

    if (perm && !k->warned_permission) {
        fprintf(stderr,
                "keyboard: can't open %s (permission denied).\n"
                "  install the udev rule and replug the keyboard:\n"
                "    sudo install -m644 70-sandbg-keyboard.rules /etc/udev/rules.d/\n"
                "    sudo udevadm control --reload-rules && "
                "sudo udevadm trigger --subsystem-match=hidraw\n",
                k->device[0] ? k->device : "the raw HID device");
        k->warned_permission = 1;
    } else if (nofw && !k->warned_no_firmware) {
        fprintf(stderr,
                "keyboard: the Keychron's raw HID interface isn't running the sandbg\n"
                "  keymap. See \"Keyboard LEDs\" in README.md to build and flash it.\n");
        k->warned_no_firmware = 1;
    } else if (!found && k->device[0]) {
        fprintf(stderr, "keyboard: %s: no Keychron raw HID interface\n", k->device);
    }
}

static void scan_colors(Keyboard *k, const Sim *s, uint32_t now, uint32_t prev)
{
    KeyCounts c[KB_MAX_LEDS];
    memset(c, 0, sizeof c);
    memset(k->support, 1, (size_t)k->w);

    /* Bottom-up, one row at a time so the cell reads stay sequential.
     * support[x] remembers the nearest thing under the cell in that column:
     * solid ground, or air. A loose cell with air under it is falling. */
    for (int y = k->h - 1; y >= 0; y--) {
        const uint16_t *row = s->cells + (size_t)y * k->w;
        const int16_t *km = k->rowkey + (size_t)k->row_at[y] * k->w;
        for (int x = 0; x < k->w; x++) {
            int t = CELL_TYPE(row[x]);
            if (t == EMPTY) {
                k->support[x] = 0;
                continue;
            }
            if (!FALLS(t) && t != WATER) {
                k->support[x] = 1;
                continue;
            }
            int idx = km[x];
            if (idx < 0)
                continue;
            int resting = k->support[x];
            switch (t) {
            case SAND: case DEBRIS: case SEED:
                if (resting) c[idx].pile++;
                else         c[idx].fall++;
                break;
            case DIRT: case WET_DIRT:
                /* Earth clings over small gaps, so it holds up what's on
                 * it rather than counting as falling. */
                c[idx].pile++;
                k->support[x] = 1;
                break;
            case WATER:
                /* Only drops that haven't landed count as rain, so
                 * trickling puddle water doesn't light the keys. */
                if (CELL_COLOR(row[x]) & RAIN_FRESH) c[idx].rain++;
                else if (resting)                     c[idx].pool++;
                break;
            case MOSS:
                c[idx].moss++;
                k->support[x] = 1;
                break;
            default:
                break;
            }
        }
    }

    float dt = prev ? (now - prev) / 1000.0f : 1.0f / 30.0f;
    if (dt > 0.5f)
        dt = 0.5f;
    float decay = expf(-dt / 0.22f);
    float drop_decay = expf(-dt / 0.12f);

    if (!k->have_bolt) {
        k->bolt_id = s->bolt.id;
        k->have_bolt = 1;
    } else if (s->bolt.id != k->bolt_id) {
        k->bolt_id = s->bolt.id;
        k->flash = 1.0f;
    } else {
        k->flash *= expf(-dt / 0.10f);
    }

    float pile[KB_MAX_LEDS] = {0}, pool[KB_MAX_LEDS] = {0}, moss[KB_MAX_LEDS] = {0};
    float moss_glow[KB_MAX_LEDS] = {0};
    memset(k->leds, 0, sizeof k->leds);
    int n = k->n_leds;
    if (n > KB_MAX_LEDS)
        n = KB_MAX_LEDS;
    if (n < 0)
        n = 0;

    uint32_t piled = 0, drops = 0;
    for (int i = 0; i < n; i++) {
        piled += c[i].pile;
        drops += c[i].rain;
    }

    /* How hard it's raining, from the drops falling on screen per column
     * (scaled to a 270-cell-tall grid), at the default 60 steps a second:
     * about 0.015 in a drizzle (120 drops/min), 0.07 in light rain (600),
     * 0.34 in a downpour (3000). Drops become a glow from about 170 to
     * 1800 drops/min. Smoothed so gusts don't flicker. */
    float density = (float)drops / (float)k->w * 270.0f / (float)k->h;
    k->rain_level += (density - k->rain_level) * (1.0f - expf(-dt / 1.5f));
    float lvl = k->rain_level > 1e-4f ? k->rain_level : 1e-4f;
    float steady = (log10f(lvl) - log10f(0.02f)) / (log10f(0.2f) - log10f(0.02f));
    steady = steady < 0.0f ? 0.0f : steady > 1.0f ? 1.0f : steady;
    steady = steady * steady * (3.0f - 2.0f * steady);

    /* Piles light up with how full their key is, and all of them brighten
     * as the whole pile grows toward a drain. */
    float total = (float)piled / ((float)k->w * (float)k->h);
    float size = s->fill_reset > 0.0f ? total / s->fill_reset : total;
    size = size > 1.0f ? 1.0f : size;
    float pile_gain = 0.25f + 0.75f * size;

    for (int i = 0; i < n; i++) {
        float area = k->area[i] ? (float)k->area[i] : 1.0f;
        float fill = (float)c[i].pile / area;
        pile[i] = powf(fill > 1.0f ? 1.0f : fill, 1.3f) * pile_gain;
        pool[i] = 1.0f - expf(-(float)c[i].pool / area * 10.0f);
        /* Each drop on wet dirt grows a moss spot of about 13 cells. Any
         * moss shows, and the key fills in green as more spots grow. */
        moss[i] = c[i].moss ? 0.3f + 0.7f * (1.0f - expf(-(float)c[i].moss / 25.0f)) : 0.0f;
        moss_glow[i] = moss[i];
        float fall = 1.0f - expf(-(float)c[i].fall * 0.9f);
        k->sand_trail[i] = fmaxf(fall, k->sand_trail[i] * decay);
        /* In a drizzle each drop is a sharp, short flash on its key. */
        float drop = c[i].rain ? 1.0f : 0.0f;
        k->rain_trail[i] = fmaxf(drop, k->rain_trail[i] * drop_decay);
    }

    /* A moss spot spreads onto its neighbors only once it's well grown,
     * so small patches stay distinct spots. */
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            if (i == j || moss[j] < 0.75f)
                continue;
            int drow = (int)q4_ansi[i].row - (int)q4_ansi[j].row;
            if (drow < -1 || drow > 1)
                continue;
            float cxi = q4_ansi[i].x + q4_ansi[i].w * 0.5f;
            float cxj = q4_ansi[j].x + q4_ansi[j].w * 0.5f;
            float reach = (q4_ansi[i].w + q4_ansi[j].w) * 0.5f + 0.3f;
            if (fabsf(cxi - cxj) > reach)
                continue;
            moss_glow[i] = fmaxf(moss_glow[i], (moss[j] - 0.75f) * 1.2f);
        }
    }

    float mean_drops = n ? (float)drops / (float)n : 0.0f;
    for (int i = 0; i < n; i++) {
        float pulse = 0.9f + 0.1f * sinf((float)now * 0.0011f + (float)i * 0.6f);
        float mpulse = 0.85f + 0.15f * sinf((float)now * 0.0009f + (float)i * 0.9f);
        float shimmer = 0.75f + 0.25f * sinf((float)now * 0.0013f + (float)i * 1.7f);
        float pr = pile[i] * pulse;
        float mr = moss_glow[i] * mpulse;
        float sr = k->sand_trail[i] * 0.8f;

        /* Drizzle shows single drops; steady rain fades them into a soft
         * glow over the board, a little stronger where more is falling. */
        float local = mean_drops > 0.0f ? (float)c[i].rain / mean_drops : 0.0f;
        float glow = steady * 0.45f * shimmer * (0.7f + 0.3f * fminf(local, 1.5f));
        float wr = fmaxf(k->rain_trail[i] * (1.0f - 0.75f * steady), glow);

        float r = pr * 0.85f + sr * 1.00f + wr * 0.55f + pool[i] * 0.02f
                + mr * 0.12f + k->flash * 1.00f;
        float g = pr * 0.45f + sr * 0.70f + wr * 0.80f + pool[i] * 0.10f
                + mr * 1.00f + k->flash * 0.95f;
        float b = pr * 0.08f + sr * 0.30f + wr * 1.00f + pool[i] * 0.25f
                + mr * 0.20f + k->flash * 1.00f;

        r *= k->brightness;
        g *= k->brightness;
        b *= k->brightness;
        k->leds[i][0] = r <= 0.0f ? 0 : r >= 1.0f ? 255 : (uint8_t)(r * 255.0f + 0.5f);
        k->leds[i][1] = g <= 0.0f ? 0 : g >= 1.0f ? 255 : (uint8_t)(g * 255.0f + 0.5f);
        k->leds[i][2] = b <= 0.0f ? 0 : b >= 1.0f ? 255 : (uint8_t)(b * 255.0f + 0.5f);
    }
}

static void preview_print(const Keyboard *k)
{
    static uint32_t last;
    uint32_t now = k->last_scan;
    int tty = isatty(STDERR_FILENO);
    if (last && now - last < (uint32_t)(tty ? 100 : 1000))
        return;
    if (tty && last)
        fprintf(stderr, "\033[5A");
    last = now;

    for (int r = 0; r < 5; r++) {
        for (int u = 0; u < 15; u++) {
            int i = q4_units[r][u];
            int rr = 0, gg = 0, bb = 0;
            if (i < k->n_leds) {
                rr = k->leds[i][0];
                gg = k->leds[i][1];
                bb = k->leds[i][2];
            }
            fprintf(stderr, "\033[48;2;%d;%d;%dm  ", rr, gg, bb);
        }
        fprintf(stderr, "\033[0m\n");
    }
    fflush(stderr);
}

static void disconnect(Keyboard *k, uint32_t now)
{
    if (k->fd >= 0)
        close(k->fd);
    k->fd = -1;
    k->connected = 0;
    k->next_try = now + 5000;
    if (!k->warned_disconnected) {
        fprintf(stderr, "keyboard: lost the keyboard, will keep looking\n");
        k->warned_disconnected = 1;
    }
}

static int send_colors(Keyboard *k, uint32_t now, int force)
{
    if (!k->connected)
        return -1;
    int changed = memcmp(k->leds, k->sent, (size_t)k->n_leds * 3) != 0;
    if (!force && !changed && now - k->last_send < KB_KEEPALIVE_MS)
        return 0;

    /* The colors are worked out as they should look. LED brightness is
     * linear in the PWM value, so dim values look far too bright without
     * a gamma curve; the terminal preview doesn't need one. */
    uint8_t out[KB_MAX_LEDS][3];
    for (int i = 0; i < k->n_leds; i++)
        for (int ch = 0; ch < 3; ch++)
            out[i][ch] = k->gamma[k->leds[i][ch]];

    for (int start = 0; start < k->n_leds; start += KB_CHUNK) {
        int count = k->n_leds - start;
        if (count > KB_CHUNK)
            count = KB_CHUNK;
        uint8_t msg[KB_REPORT];
        memset(msg, 0, sizeof msg);
        msg[0] = SB_HELLO0;
        msg[1] = SB_HELLO1;
        msg[2] = KB_PROTO_VERSION;
        msg[3] = SB_CMD_COLORS;
        msg[4] = (uint8_t)start;
        msg[5] = (uint8_t)count;
        memcpy(msg + 6, out[start], (size_t)count * 3);
        msg[31] = checksum(msg, 31);

        if (write_report_nowait(k->fd, msg) != (ssize_t)(KB_REPORT + 1)) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return 0;               /* resend the whole frame next time */
            disconnect(k, now);
            return -1;
        }
    }
    memcpy(k->sent, k->leds, (size_t)k->n_leds * 3);
    k->last_send = now;
    return 0;
}

void keyboard_update(Keyboard *k, const Sim *s, uint32_t now_ms)
{
    if (k->backend == KB_OFF)
        return;
    if (k->backend == KB_HID && !k->connected) {
        try_connect(k, now_ms);
        if (!k->connected)
            return;
    }
    if (k->last_scan && now_ms - k->last_scan < KB_SCAN_MS)
        return;

    uint32_t prev = k->last_scan;
    k->last_scan = now_ms;
    scan_colors(k, s, now_ms, prev);
    if (k->backend == KB_PREVIEW)
        preview_print(k);
    else
        send_colors(k, now_ms, 0);
}

void keyboard_hold(Keyboard *k, const Sim *s, uint32_t now_ms)
{
    if (k->backend != KB_HID || !k->connected)
        return;
    /* A strike just before the desktop was covered would otherwise leave
     * the board white until it's uncovered, so let the flash fade out. */
    if (k->flash > 0.01f && now_ms - k->last_scan >= KB_SCAN_MS) {
        uint32_t prev = k->last_scan;
        k->last_scan = now_ms;
        scan_colors(k, s, now_ms, prev);
        send_colors(k, now_ms, 0);
        return;
    }
    if (now_ms - k->last_send >= KB_KEEPALIVE_MS)
        send_colors(k, now_ms, 1);
}

int keyboard_probe(const char *device)
{
    Candidate c[16];
    int n = find_candidates(c, 16, device);
    if (n == 0) {
        printf("keyboard: no Keychron hidraw devices found (VID %04x)\n",
               KEYCHRON_VID);
        return 1;
    }

    int raw_any = 0, ok = 0, perm_any = 0;
    for (int i = 0; i < n; i++) {
        printf("%s  %s  %04x:%04x  ", c[i].path, c[i].name, c[i].vid, c[i].pid);
        if (c[i].raw == 0) {
            printf("not the raw HID interface\n");
            continue;
        }
        printf("%sraw HID 0xFF60/0x61  ",
               c[i].raw == 1 ? "" : "possibly ");
        raw_any = 1;

        int leds = 0, perm = 0;
        int fd = open_ping(c[i].path, &leds, &perm);
        if (fd >= 0) {
            printf("sandbg keymap: yes (protocol %d, %d LEDs)\n",
                   KB_PROTO_VERSION, leds);
            close(fd);
            ok = 1;
        } else if (perm) {
            printf("permission denied\n");
            perm_any = 1;
        } else {
            printf("sandbg keymap: no response\n");
        }
    }

    if (!raw_any && !(device && *device)) {
        printf("keyboard: no raw HID interface; this may not be a QMK keyboard\n");
    } else if (!ok) {
        if (perm_any) {
            printf("keyboard: give yourself access with:\n"
                   "  sudo install -m644 70-sandbg-keyboard.rules /etc/udev/rules.d/\n"
                   "  sudo udevadm control --reload-rules && "
                   "sudo udevadm trigger --subsystem-match=hidraw\n"
                   "and replug the keyboard. sandbg-setup does this for you.\n");
        } else {
            printf("keyboard: flash the sandbg keymap first; "
                   "see README.md (Keyboard LEDs)\n");
        }
    }
    return 0;
}
