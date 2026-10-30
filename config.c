#include "config.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "render.h"
#include "sim.h"

void config_defaults(Config *c)
{
    c->scale = 4;
    c->fps = 20;
    c->steps_per_frame = 3;
    snprintf(c->palette, sizeof c->palette, "desert");
    c->emitters = 2;
    c->fill_reset = 0.6f;
    c->audio = 1;
    c->blast_strength = 1.0f;
    c->audio_sink[0] = '\0';
    c->interactive = 1;
    c->weather = 1;
    c->latitude = c->longitude = NAN;
    c->rain_override = -1.0f;
    c->spotify_seeds = 1;
    c->seed_radius = 15;
    c->slipperiness = 0.3f;
    c->lightning = 1;
    c->lightning_reach = 60;
    c->strike_radius = 7;
    c->glass_strength = 35;
    c->tree_minutes = 30;
    c->tree_thirst_minutes = 22;
    c->drain_overfill = 0.8f;
    snprintf(c->keyboard, sizeof c->keyboard, "auto");
    c->keyboard_device[0] = '\0';
    c->keyboard_brightness = 1.0f;
    c->critters = 1;
    c->critter_max = 24;
    c->critter_starve_minutes = 40;
    c->wheat_per_pour = 3;
    snprintf(c->kana_url, sizeof c->kana_url, "http://127.0.0.1:8080");
    config_set(c, "pixelorama_config", "~/.local/share/pixelorama/config.ini");
}

const char *config_default_path(void)
{
    static char path[4096];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(path, sizeof path, "%s/sandbg/config.ini", xdg);
    else
        snprintf(path, sizeof path, "%s/.config/sandbg/config.ini",
                 getenv("HOME") ? getenv("HOME") : ".");
    return path;
}

static int parse_str(const char *s, char *out, size_t size)
{
    if (strlen(s) >= size)
        return -1;
    strcpy(out, s);
    return 0;
}

static int parse_int(const char *s, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_float(const char *s, float *out)
{
    char *end;
    float v = strtof(s, &end);
    if (end == s || *end)
        return -1;
    *out = v;
    return 0;
}

static int parse_double(const char *s, double *out)
{
    char *end;
    double v = strtod(s, &end);
    if (end == s || *end)
        return -1;
    *out = v;
    return 0;
}

static int parse_bool(const char *s, int *out)
{
    if (!strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
        !strcasecmp(s, "on") || !strcmp(s, "1"))
        *out = 1;
    else if (!strcasecmp(s, "false") || !strcasecmp(s, "no") ||
             !strcasecmp(s, "off") || !strcmp(s, "0"))
        *out = 0;
    else
        return -1;
    return 0;
}

int config_set(Config *c, const char *key, const char *value)
{
    if (!strcmp(key, "scale"))           return parse_int(value, &c->scale);
    if (!strcmp(key, "fps"))             return parse_int(value, &c->fps);
    if (!strcmp(key, "steps_per_frame")) return parse_int(value, &c->steps_per_frame);
    if (!strcmp(key, "emitters"))        return parse_int(value, &c->emitters);
    if (!strcmp(key, "fill_reset"))      return parse_float(value, &c->fill_reset);
    if (!strcmp(key, "blast_strength"))  return parse_float(value, &c->blast_strength);
    if (!strcmp(key, "audio"))           return parse_bool(value, &c->audio);
    if (!strcmp(key, "interactive"))     return parse_bool(value, &c->interactive);
    if (!strcmp(key, "weather"))         return parse_bool(value, &c->weather);
    if (!strcmp(key, "latitude"))        return parse_double(value, &c->latitude);
    if (!strcmp(key, "longitude"))       return parse_double(value, &c->longitude);
    if (!strcmp(key, "rain_override"))   return parse_float(value, &c->rain_override);
    if (!strcmp(key, "spotify_seeds"))   return parse_bool(value, &c->spotify_seeds);
    if (!strcmp(key, "seed_radius"))     return parse_int(value, &c->seed_radius);
    if (!strcmp(key, "slipperiness"))    return parse_float(value, &c->slipperiness);
    if (!strcmp(key, "lightning"))       return parse_bool(value, &c->lightning);
    if (!strcmp(key, "lightning_reach")) return parse_int(value, &c->lightning_reach);
    if (!strcmp(key, "strike_radius"))   return parse_int(value, &c->strike_radius);
    if (!strcmp(key, "glass_strength"))  return parse_int(value, &c->glass_strength);
    if (!strcmp(key, "tree_minutes"))    return parse_int(value, &c->tree_minutes);
    if (!strcmp(key, "tree_thirst_minutes")) return parse_int(value, &c->tree_thirst_minutes);
    if (!strcmp(key, "drain_overfill"))  return parse_float(value, &c->drain_overfill);
    if (!strcmp(key, "keyboard_brightness")) return parse_float(value, &c->keyboard_brightness);
    if (!strcmp(key, "critters"))        return parse_bool(value, &c->critters);
    if (!strcmp(key, "critter_max"))     return parse_int(value, &c->critter_max);
    if (!strcmp(key, "critter_starve_minutes"))
        return parse_int(value, &c->critter_starve_minutes);
    if (!strcmp(key, "wheat_per_pour"))  return parse_int(value, &c->wheat_per_pour);
    if (!strcmp(key, "kana_url"))
        return parse_str(value, c->kana_url, sizeof c->kana_url);
    if (!strcmp(key, "pixelorama_config")) {
        /* A leading ~ is the home directory, as in the default. */
        char path[sizeof c->pixelorama_config];
        const char *home = getenv("HOME");
        if (value[0] == '~' && (value[1] == '/' || !value[1]) && home) {
            if ((size_t)snprintf(path, sizeof path, "%s%s", home, value + 1) >= sizeof path)
                return -1;
            value = path;
        }
        return parse_str(value, c->pixelorama_config, sizeof c->pixelorama_config);
    }
    if (!strcmp(key, "keyboard")) {
        if (strlen(value) >= sizeof c->keyboard)
            return -1;
        strcpy(c->keyboard, value);
        return 0;
    }
    if (!strcmp(key, "keyboard_device")) {
        if (strlen(value) >= sizeof c->keyboard_device)
            return -1;
        strcpy(c->keyboard_device, value);
        return 0;
    }
    if (!strcmp(key, "palette")) {
        if (strlen(value) >= sizeof c->palette)
            return -1;
        strcpy(c->palette, value);
        return 0;
    }
    if (!strcmp(key, "audio_sink")) {
        if (strlen(value) >= sizeof c->audio_sink)
            return -1;
        strcpy(c->audio_sink, value);
        return 0;
    }
    return -2;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

int config_load(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 1;

    char line[1280];
    int lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        line[strcspn(line, ";#\r\n")] = '\0';   /* strip comments */
        char *s = trim(line);
        if (!*s || *s == '[')                   /* blank or [section] */
            continue;

        char *eq = strchr(s, '=');
        if (!eq) {
            fprintf(stderr, "%s:%d: expected key=value\n", path, lineno);
            continue;
        }
        *eq = '\0';
        char *key = trim(s), *val = trim(eq + 1);

        int r = config_set(c, key, val);
        if (r == -2)
            fprintf(stderr, "%s:%d: unknown setting '%s'\n", path, lineno, key);
        else if (r != 0)
            fprintf(stderr, "%s:%d: bad value for %s: '%s'\n", path, lineno, key, val);
    }
    fclose(f);
    return 0;
}

const char *config_validate(const Config *c)
{
    static char msg[128];
    int ok = 0;
    for (int i = 0; render_palettes[i]; i++)
        if (!strcmp(render_palettes[i], c->palette))
            ok = 1;

    if (c->scale < 1 || c->scale > 16)
        return "scale must be 1-16";
    if (c->fps < 1 || c->fps > 120)
        return "fps must be 1-120";
    if (c->steps_per_frame < 1 || c->steps_per_frame > 10)
        return "steps_per_frame must be 1-10";
    if (c->emitters < 1 || c->emitters > MAX_EMITTERS) {
        snprintf(msg, sizeof msg, "emitters must be 1-%d", MAX_EMITTERS);
        return msg;
    }
    if (c->fill_reset < 0.05f || c->fill_reset > 0.95f)
        return "fill_reset must be 0.05-0.95";
    if (c->blast_strength < 0.0f || c->blast_strength > 4.0f)
        return "blast_strength must be 0-4";
    if (!isnan(c->latitude) && (c->latitude < -90 || c->latitude > 90))
        return "latitude must be -90 to 90";
    if (!isnan(c->longitude) && (c->longitude < -180 || c->longitude > 180))
        return "longitude must be -180 to 180";
    if (c->rain_override > 12000)
        return "rain_override must be at most 12000 drops/minute";
    if (c->seed_radius < 1 || c->seed_radius > 100)
        return "seed_radius must be 1-100";
    if (c->slipperiness < 0.0f || c->slipperiness > 1.0f)
        return "slipperiness must be 0-1";
    if (c->lightning_reach < 10 || c->lightning_reach > 400)
        return "lightning_reach must be 10-400";
    if (c->strike_radius < 1 || c->strike_radius > 20)
        return "strike_radius must be 1-20";
    if (c->glass_strength < 2 || c->glass_strength > 100)
        return "glass_strength must be 2-100";
    if (c->tree_minutes < 1 || c->tree_minutes > 240)
        return "tree_minutes must be 1-240";
    if (c->tree_thirst_minutes < 1 || c->tree_thirst_minutes > 240)
        return "tree_thirst_minutes must be 1-240";
    if (c->drain_overfill < c->fill_reset || c->drain_overfill > 0.98f)
        return "drain_overfill must be from fill_reset to 0.98";
    if (strcmp(c->keyboard, "auto") && strcmp(c->keyboard, "off") &&
        strcmp(c->keyboard, "preview"))
        return "keyboard must be auto, off or preview";
    if (!(c->keyboard_brightness >= 0.0f && c->keyboard_brightness <= 1.0f))
        return "keyboard_brightness must be 0-1";
    if (c->critter_max < 1 || c->critter_max > 32)
        return "critter_max must be 1-32";
    if (c->critter_starve_minutes < 5 || c->critter_starve_minutes > 600)
        return "critter_starve_minutes must be 5-600";
    if (c->wheat_per_pour < 1 || c->wheat_per_pour > 7)
        return "wheat_per_pour must be 1-7";
    if (c->kana_url[0] && strncmp(c->kana_url, "http://", 7) &&
        strncmp(c->kana_url, "https://", 8))
        return "kana_url must start with http:// or https:// (or be empty)";
    /* It goes into a shell command for curl, inside single quotes. */
    if (strchr(c->kana_url, '\'') || strchr(c->kana_url, '\\'))
        return "kana_url can't contain quotes or backslashes";
    if (!ok) {
        snprintf(msg, sizeof msg, "unknown palette '%s' (desert, pastel, neon, mono)",
                 c->palette);
        return msg;
    }
    return NULL;
}
