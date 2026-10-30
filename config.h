#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int   scale;            /* screen pixels per grain */
    int   fps;
    int   steps_per_frame;  /* simulation speed */
    char  palette[32];
    int   emitters;
    float fill_reset;       /* drain when this fraction of the screen is sand */
    int   audio;            /* sound-driven blaster on/off */
    float blast_strength;   /* multiplies the audio level */
    char  audio_sink[256];  /* output the blaster listens to; "" = default */
    int   interactive;      /* mouse can paint sand/stone */
    int   weather;          /* rain from the current weather */
    double latitude, longitude;     /* NAN = not set */
    float rain_override;    /* drops/minute; < 0 = use the weather */
    int   spotify_seeds;    /* a seed drops for each song Spotify finishes */
    int   seed_radius;      /* cells of sand a seed turns to dirt */
    float slipperiness;     /* 0-1: how readily grains slide off slopes */
    int   lightning;        /* strike on each git commit (SIGUSR1) */
    int   lightning_reach;  /* cells from the blaster a bolt can land */
    int   strike_radius;    /* cells around the impact that change */
    int   glass_strength;   /* cells of weight glass holds before breaking */
    int   tree_minutes;     /* time for a tree to grow fully */
    int   tree_thirst_minutes;  /* minutes without water before a tree dies */
    float drain_overfill;   /* drain anyway, even with trees alive, at this fill */
    char  keyboard[16];     /* auto, off or preview */
    char  keyboard_device[64];  /* "" = find the Keychron raw HID interface */
    float keyboard_brightness;  /* 0-1 */
} Config;

void config_defaults(Config *c);

/* ~/.config/sandbg/config.ini (honors $XDG_CONFIG_HOME). */
const char *config_default_path(void);

/* Returns 0 if loaded, 1 if the file doesn't exist. Bad lines are reported
 * and skipped, keeping the previous value. */
int config_load(Config *c, const char *path);

/* Sets a single key from a string; used by the file and by --set. */
int config_set(Config *c, const char *key, const char *value);

/* Returns NULL if valid, otherwise a message about the first bad value. */
const char *config_validate(const Config *c);

#endif
