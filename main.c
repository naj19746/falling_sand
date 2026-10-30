#include <SDL.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "audio.h"
#include "config.h"
#include "keyboard.h"
#include "render.h"
#include "sim.h"
#include "spotify.h"
#include "weather.h"
#include "x11.h"

#define WIN_W           1280    /* --windowed size */
#define WIN_H           720
#define BRUSH_RADIUS    3
#define MIN_GRID        32
#define MOSS_RAIN_PX    100     /* rain this close keeps moss from drying */

/* While the desktop is covered: how often to look again. */
#define COVERED_CHECK_MS 500

/* Lightning: at most this many strikes wait their turn (a long rebase sends
 * one per commit), and they go off at least this far apart. */
#define LIGHTNING_BACKLOG 5
#define LIGHTNING_GAP_MS  400

/* Blaster level smoothing per frame: jump up fast on hits, fall slowly. */
#define LEVEL_ATTACK    0.6f
#define LEVEL_DECAY     0.08f

enum { MODE_DESKTOP, MODE_WINDOWED, MODE_WID };

/* One per SIGUSR1, which the git post-commit hook sends. */
static volatile sig_atomic_t strikes_requested;

static void on_strike(int sig)
{
    (void)sig;
    strikes_requested++;
}

static void listen_for_strikes(int on)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    /* Ignored rather than left at the default, which would kill us. */
    sa.sa_handler = on ? on_strike : SIG_IGN;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
}

/* --strike: signal the running copy, as the git hook does. pkill never
 * matches itself, and exec replaces this sandbg process with it. */
static int send_strike(void)
{
    execlp("pkill", "pkill", "-USR1", "-x", "sandbg", (char *)NULL);
    perror("pkill");
    return 1;
}

static void usage(const char *prog)
{
    printf("usage: %s [--windowed | --wid <id>] [options]\n"
           "\n"
           "  (default)         draw on the desktop, behind all other windows\n"
           "  --windowed        run in a normal window, for testing\n"
           "  --wid <id>        draw into an existing X window (e.g. from xwinwrap)\n"
           "\n"
           "  --config <path>   settings file (default %s)\n"
           "  --set key=value   override one setting from the file\n"
           "  --palette <name>  same as --set palette=<name>\n"
           "  --emitters <n>    same as --set emitters=<n>\n"
           "  --no-audio        same as --set audio=false\n"
           "  --meter           just print the audio level, for tuning\n"
           "  --profile         print per-frame timings every 5 seconds\n"
           "  --weather         print current weather and the rain it gives, then exit\n"
           "  --spotify         show what Spotify is playing and each finished song\n"
           "  --strike          make the running sandbg fire a bolt of lightning\n"
           "  --keyboard        show the Keychron LEDs and whether the keymap is loaded\n"
           "  --keyboard-preview  draw a colored keyboard map in the terminal\n"
           "\n"
           "mouse: left-drag = sand, right-drag = stone, middle-drag = erase\n"
           "keys:  r = reset, d = drain now, s = drop a seed, l = lightning,\n"
           "       space = pause, q/esc = quit\n",
           prog, config_default_path());
}

static void print_weather(const Weather *w)
{
    printf("%s, %.0f%% cloud, %.2f mm/h -> %.1f drops/minute%s\n",
           weather_describe(w->code), w->cloud_cover, w->precip_mmh,
           w->drops_per_min, w->raining ? " (raining: slow evaporation)" : "");
}

static int run_weather(const Config *c)
{
    if (isnan(c->latitude) || isnan(c->longitude)) {
        fprintf(stderr, "set latitude and longitude in the config first\n");
        return 1;
    }
    Weather w;
    if (weather_fetch_now(c->latitude, c->longitude, &w) != 0) {
        fprintf(stderr, "weather: fetch failed\n");
        return 1;
    }
    printf("%.4f, %.4f: ", c->latitude, c->longitude);
    print_weather(&w);
    return 0;
}

/* Print the level the blaster would see, until interrupted. */
static int run_meter(const char *sink)
{
    if (SDL_Init(0) != 0 || audio_start(sink) != 0) {
        fprintf(stderr, "audio: %s\n", SDL_GetError());
        return 1;
    }
    for (;;) {
        float lvl = audio_level();
        int bar = (int)(lvl * 50);
        printf("\rvolume %.2f  level %.2f  [%-50.*s]", audio_volume(), lvl,
               bar, "##################################################");
        fflush(stdout);
        SDL_Delay(50);
    }
}

/* Only one copy on the desktop at a time, so autostart and a manual start
 * don't stack. The lock is held until the process exits. */
static int take_desktop_lock(void)
{
    char path[512];
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (dir && *dir)
        snprintf(path, sizeof path, "%s/sandbg.lock", dir);
    else
        snprintf(path, sizeof path, "/tmp/sandbg-%d.lock", (int)getuid());

    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return 0;   /* can't lock; don't refuse to run over it */
    return flock(fd, LOCK_EX | LOCK_NB) == 0 ? 0 : -1;
}

static SDL_Window *create_window(int mode, unsigned long wid)
{
    SDL_Window *win = NULL;
    int w, h;

    switch (mode) {
    case MODE_WINDOWED:
        win = SDL_CreateWindow("sandbg", SDL_WINDOWPOS_CENTERED,
                               SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H, 0);
        break;

    case MODE_WID:
        win = SDL_CreateWindowFrom((void *)(uintptr_t)wid);
        break;

    case MODE_DESKTOP:
        if (x11_root_size(&w, &h) != 0) {
            fprintf(stderr, "can't open X display (desktop mode needs X11)\n");
            return NULL;
        }
        x11_desktop_hint();
        win = SDL_CreateWindow("sandbg", 0, 0, w, h, SDL_WINDOW_BORDERLESS);
        if (!win)
            break;
        SDL_SetWindowPosition(win, 0, 0);
        if (x11_make_desktop(win) != 0) {
            fprintf(stderr, "couldn't set desktop window properties\n");
            SDL_DestroyWindow(win);
            return NULL;
        }

        int others = x11_count_other_desktops(win);
        if (others > 0)
            fprintf(stderr,
                    "warning: another desktop window is running (probably Caja's\n"
                    "desktop icons) and may cover the sand. To turn it off:\n"
                    "  gsettings set org.mate.background show-desktop-icons false\n");
        break;
    }

    if (!win)
        fprintf(stderr, "creating window: %s\n", SDL_GetError());
    return win;
}

int main(int argc, char **argv)
{
    int mode = MODE_DESKTOP;
    unsigned long wid = 0;
    int profile = 0, show_weather = 0, show_keyboard = 0;
    Config cfg;
    config_defaults(&cfg);

    /* First pass: which config file, so flags can override it after. */
    const char *cfg_path = config_default_path();
    int explicit_cfg = 0;
    for (int i = 1; i + 1 < argc; i++)
        if (strcmp(argv[i], "--config") == 0) {
            cfg_path = argv[i + 1];
            explicit_cfg = 1;
        }
    if (config_load(&cfg, cfg_path) == 1 && explicit_cfg) {
        fprintf(stderr, "can't read %s\n", cfg_path);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *next = i + 1 < argc ? argv[i + 1] : NULL;
        char kv[128];
        int r = 0;

        if (strcmp(arg, "--config") == 0 && next) {
            i++;
        } else if (strcmp(arg, "--set") == 0 && next) {
            snprintf(kv, sizeof kv, "%s", argv[++i]);
            char *eq = strchr(kv, '=');
            if (!eq) {
                fprintf(stderr, "--set expects key=value\n");
                return 1;
            }
            *eq = '\0';
            r = config_set(&cfg, kv, eq + 1);
        } else if (strcmp(arg, "--palette") == 0 && next) {
            r = config_set(&cfg, "palette", argv[++i]);
        } else if (strcmp(arg, "--emitters") == 0 && next) {
            r = config_set(&cfg, "emitters", argv[++i]);
        } else if (strcmp(arg, "--no-audio") == 0) {
            cfg.audio = 0;
        } else if (strcmp(arg, "--weather") == 0) {
            show_weather = 1;
        } else if (strcmp(arg, "--profile") == 0) {
            profile = 1;
        } else if (strcmp(arg, "--meter") == 0) {
            return run_meter(cfg.audio_sink);
        } else if (strcmp(arg, "--strike") == 0) {
            return send_strike();
        } else if (strcmp(arg, "--keyboard") == 0) {
            show_keyboard = 1;
        } else if (strcmp(arg, "--keyboard-preview") == 0) {
            r = config_set(&cfg, "keyboard", "preview");
        } else if (strcmp(arg, "--spotify") == 0) {
            return spotify_watch();
        } else if (strcmp(arg, "--windowed") == 0) {
            mode = MODE_WINDOWED;
        } else if (strcmp(arg, "--wid") == 0 && next) {
            char *end;
            wid = strtoul(argv[++i], &end, 0);   /* accepts 0x... or decimal */
            if (*end || wid == 0) {
                fprintf(stderr, "bad window id: %s\n", argv[i]);
                return 1;
            }
            mode = MODE_WID;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            usage(argv[0]);
            return 1;
        }
        if (r == -2) {
            fprintf(stderr, "unknown setting in: %s %s\n", arg, argv[i]);
            return 1;
        } else if (r != 0) {
            fprintf(stderr, "bad value: %s %s\n", arg, argv[i]);
            return 1;
        }
    }

    const char *err = config_validate(&cfg);
    if (err) {
        fprintf(stderr, "config: %s\n", err);
        return 1;
    }

    if (show_weather)
        return run_weather(&cfg);
    if (show_keyboard)
        return keyboard_probe(cfg.keyboard_device);

    if (mode == MODE_DESKTOP && take_desktop_lock() != 0) {
        fprintf(stderr, "sandbg is already running on the desktop\n");
        return 1;
    }

    listen_for_strikes(cfg.lightning);

    /* SDL blocks the screensaver by default; a wallpaper shouldn't. */
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *win = create_window(mode, wid);
    if (!win) {
        SDL_Quit();
        return 1;
    }

    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    int scale = cfg.scale;
    int gw = (ww + scale - 1) / scale, gh = (wh + scale - 1) / scale;
    if (gw < MIN_GRID || gh < MIN_GRID) {
        fprintf(stderr, "window too small (%dx%d)\n", ww, wh);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }

    Sim sim;
    Render ren = {0};
    if (sim_init(&sim, gw, gh, (uint32_t)SDL_GetPerformanceCounter(), cfg.emitters) != 0 ||
        render_init(&ren, x11_window_id(win), gw, gh, scale, cfg.palette) != 0) {
        fprintf(stderr, "init failed: %s\n", SDL_GetError());
        return 1;
    }

    Keyboard kb;
    if (keyboard_init(&kb, sim.w, sim.h, cfg.keyboard, cfg.keyboard_device,
                      cfg.keyboard_brightness) != 0)
        fprintf(stderr, "keyboard: can't build the LED map (continuing without)\n");

    sim.fill_reset = cfg.fill_reset;
    sim.seed_radius = cfg.seed_radius;
    sim.slip = (uint32_t)(cfg.slipperiness * 1024);
    sim.moss_rain_radius = (MOSS_RAIN_PX + scale / 2) / scale;
    sim.lightning_on = cfg.lightning;
    sim.lightning_reach = cfg.lightning_reach;
    sim.strike_radius = cfg.strike_radius;
    sim.glass_strength = cfg.glass_strength;
    sim.tree_minutes = cfg.tree_minutes;
    sim.tree_thirst_minutes = cfg.tree_thirst_minutes;
    sim.drain_overfill = cfg.drain_overfill;

    if (cfg.audio) {
        if (audio_start(cfg.audio_sink) == 0)
            sim.blaster_on = 1;
        else
            fprintf(stderr, "audio: %s (continuing without)\n", SDL_GetError());
    }

    int use_weather = 0;
    if (cfg.rain_override < 0 && cfg.weather) {
        if (isnan(cfg.latitude) || isnan(cfg.longitude))
            fprintf(stderr, "weather: no latitude/longitude in the config, no rain\n");
        else if (weather_start(cfg.latitude, cfg.longitude) == 0)
            use_weather = 1;
    }
    /* Songs finished that haven't dropped a seed yet are counted as
     * spotify_completed() - seeds_dropped. */
    int use_spotify = 0, seeds_dropped = 0;
    /* Likewise strikes waiting are strikes_requested - strikes_fired. */
    int strikes_fired = 0;
    Uint64 next_strike = 0;
    if (cfg.spotify_seeds) {
        if (spotify_start() == 0)
            use_spotify = 1;
        else
            fprintf(stderr, "spotify: %s (continuing without)\n", SDL_GetError());
    }

    float steps_per_min = 60.0f * cfg.fps * cfg.steps_per_frame;
    if (cfg.rain_override >= 0)
        sim.water_rate = cfg.rain_override / steps_per_min;

    const Uint64 frame_ms = 1000 / cfg.fps;
    int running = 1, paused = 0;
    float level = 0.0f;

    /* Only the desktop can be hidden behind other windows. */
    int covered = 0;
    Uint64 next_cover_check = 0;

    Uint64 prof_start = SDL_GetTicks64(), prof_frames = 0;
    Uint64 prof_sim = 0, prof_draw = 0, prof_covered = 0;
    long prof_awake = 0;

    while (running) {
        Uint64 start = SDL_GetTicks64();
        sim.clock = (uint32_t)(start / 1000);

        if (mode == MODE_DESKTOP && start >= next_cover_check) {
            int was = covered;
            covered = x11_desktop_covered();
            next_cover_check = start + COVERED_CHECK_MS;
            if (was && !covered)
                render_invalidate(&ren);
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            } else if (ev.type == SDL_WINDOWEVENT &&
                       ev.window.event == SDL_WINDOWEVENT_EXPOSED) {
                render_invalidate(&ren);
            } else if (ev.type == SDL_KEYDOWN) {
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE:
                case SDLK_q:     running = 0; break;
                case SDLK_r:     sim_reset(&sim); break;
                case SDLK_d:     sim_start_drain(&sim); break;
                case SDLK_s:     sim_drop_seed(&sim); break;
                case SDLK_l:     sim_strike(&sim); break;
                case SDLK_SPACE: paused = !paused; break;
                }
            }
        }

        /* Only paint while the pointer is over our window. */
        if (cfg.interactive && SDL_GetMouseFocus() == win) {
            int mx, my;
            Uint32 buttons = SDL_GetMouseState(&mx, &my);
            if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))
                sim_paint(&sim, mx / scale, my / scale, BRUSH_RADIUS, SAND);
            if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT))
                sim_paint(&sim, mx / scale, my / scale, BRUSH_RADIUS, STONE);
            if (buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE))
                sim_paint(&sim, mx / scale, my / scale, BRUSH_RADIUS, EMPTY);
        }

        if (use_weather) {
            Weather w;
            weather_get(&w);
            sim.water_rate = w.drops_per_min / steps_per_min;
            sim.raining = w.raining;
        }

        if (sim.blaster_on) {
            float target = audio_level() * cfg.blast_strength;
            if (target > 1.0f)
                target = 1.0f;
            level += (target - level) * (target > level ? LEVEL_ATTACK : LEVEL_DECAY);
            sim.bl.level = level;
        }

        if (covered) {
            /* Nothing visible: skip the work, keep handling events. */
            keyboard_hold(&kb, &sim, (uint32_t)start);
            prof_covered++;
            SDL_Delay(COVERED_CHECK_MS / 5);
            continue;
        }

        /* Held while covered or paused, so no seed falls unseen. */
        if (use_spotify && !paused) {
            int done = spotify_completed();
            for (; seeds_dropped < done; seeds_dropped++)
                sim_drop_seed(&sim);
        }
        if (!paused) {
            int requested = strikes_requested;
            if (requested - strikes_fired > LIGHTNING_BACKLOG)
                strikes_fired = requested - LIGHTNING_BACKLOG;
            if (strikes_fired < requested && start >= next_strike) {
                sim_strike(&sim);
                strikes_fired++;
                next_strike = start + LIGHTNING_GAP_MS;
            }
        }

        Uint64 t0 = SDL_GetPerformanceCounter();
        if (!paused)
            for (int i = 0; i < cfg.steps_per_frame; i++)
                sim_step(&sim);
        Uint64 t1 = SDL_GetPerformanceCounter();
        if (profile)
            prof_awake += sim_awake_chunks(&sim);
        keyboard_update(&kb, &sim, (uint32_t)SDL_GetTicks64());
        render_draw(&ren, &sim);
        Uint64 t2 = SDL_GetPerformanceCounter();

        if (profile) {
            prof_sim += t1 - t0;
            prof_draw += t2 - t1;
            prof_frames++;
            if (SDL_GetTicks64() - prof_start >= 5000) {
                double us = 1e6 / SDL_GetPerformanceFrequency();
                long f = prof_frames ? (long)prof_frames : 1;
                fprintf(stderr,
                        "frames %ld (+%ld covered)  sim %.0f us  draw %.0f us  "
                        "awake chunks %ld/%d  drawn %ld  rects %ld  grains %d  water %d\n",
                        (long)prof_frames, (long)prof_covered, prof_sim * us / f,
                        prof_draw * us / f, prof_awake / f, sim.cw * sim.ch,
                        ren.chunks_drawn / f, ren.rects_sent / f, sim.grains, sim.water);
                prof_start = SDL_GetTicks64();
                prof_frames = prof_covered = prof_sim = prof_draw = 0;
                prof_awake = ren.chunks_drawn = ren.rects_sent = 0;
            }
        }

        Uint64 elapsed = SDL_GetTicks64() - start;
        if (elapsed < frame_ms)
            SDL_Delay((Uint32)(frame_ms - elapsed));
    }

    render_free(&ren);
    keyboard_free(&kb);
    sim_free(&sim);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
