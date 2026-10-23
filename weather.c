#include "weather.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REFRESH_MS  (10 * 60 * 1000)    /* Open-Meteo updates every 15 min */
#define RETRY_MS    (2 * 60 * 1000)
#define MAX_DPM     12000.0f            /* 200 drops/s: a downpour */

static SDL_mutex *lock;
static Weather current;
static double lat_, lon_;

const char *weather_describe(int code)
{
    if (code == 0)                return "clear";
    if (code <= 2)                return "partly cloudy";
    if (code == 3)                return "overcast";
    if (code == 45 || code == 48) return "fog";
    if (code >= 51 && code <= 57) return "drizzle";
    if (code >= 61 && code <= 67) return "rain";
    if (code >= 71 && code <= 77) return "snow";
    if (code >= 80 && code <= 82) return "rain showers";
    if (code >= 85 && code <= 86) return "snow showers";
    if (code >= 95)               return "thunderstorm";
    return "unknown";
}

static int is_rain_code(int code)
{
    return (code >= 61 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
}

/* The drop rate is where the weather becomes water. */
static void derive(Weather *w)
{
    float dpm = 0.0f;

    /* Even clear skies give a few stray drops: 2/min at 0% cover, 5 at 40%,
     * 10 at 100%. */
    if (w->cloud_cover < 40.0f)
        dpm = 2.0f + w->cloud_cover / 40.0f * 3.0f;
    else
        dpm = 5.0f + (w->cloud_cover - 40.0f) / 60.0f * 5.0f;
    if ((w->code == 45 || w->code == 48) && dpm < 4.0f)
        dpm = 4.0f;
    if (w->code >= 51 && w->code <= 57)
        dpm += 120.0f;                      /* drizzle: ~2 drops/s */

    /* Measured precipitation: 1 mm/h = 10 drops/s. The rain codes set a
     * floor in case the last 15-minute total hasn't caught up yet. */
    dpm += w->precip_mmh * 600.0f;
    if (is_rain_code(w->code) && dpm < 300.0f)
        dpm = 300.0f;

    w->drops_per_min = dpm > MAX_DPM ? MAX_DPM : dpm;
    w->raining = w->precip_mmh > 0.0f || is_rain_code(w->code) ||
                 (w->code >= 51 && w->code <= 57);
}

/* Value of "key": inside the "current" object, or -1 if missing. */
static int json_num(const char *obj, const char *key, double *out)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = strstr(obj, pat);
    if (!p)
        return -1;
    char *end;
    *out = strtod(p + strlen(pat), &end);
    return end == p + strlen(pat) ? -1 : 0;
}

int weather_fetch_now(double lat, double lon, Weather *w)
{
    char cmd[512];
    snprintf(cmd, sizeof cmd,
             "curl -s --fail -m 20 'https://api.open-meteo.com/v1/forecast?"
             "latitude=%.4f&longitude=%.4f"
             "&current=precipitation,cloud_cover,weather_code' 2>/dev/null",
             lat, lon);

    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, p);
    buf[n] = '\0';
    if (pclose(p) != 0)
        return -1;

    /* "current_units" comes first and has the same keys, so skip to the
     * values object. */
    const char *obj = strstr(buf, "\"current\":{");
    double precip, cloud, code, interval = 900;
    if (!obj || json_num(obj, "precipitation", &precip) ||
        json_num(obj, "cloud_cover", &cloud) || json_num(obj, "weather_code", &code))
        return -1;
    json_num(obj, "interval", &interval);

    memset(w, 0, sizeof *w);
    w->ok = 1;
    w->code = (int)code;
    w->cloud_cover = (float)cloud;
    /* precipitation is the total over the last interval (15 min). */
    w->precip_mmh = (float)(precip * 3600.0 / (interval > 0 ? interval : 900));
    derive(w);
    return 0;
}

static int weather_thread(void *unused)
{
    (void)unused;
    for (;;) {
        Weather w;
        int ok = weather_fetch_now(lat_, lon_, &w) == 0;
        if (ok) {
            SDL_LockMutex(lock);
            current = w;
            SDL_UnlockMutex(lock);
        } else {
            fprintf(stderr, "weather: fetch failed, keeping last conditions\n");
        }
        SDL_Delay(ok ? REFRESH_MS : RETRY_MS);
    }
    return 0;
}

int weather_start(double lat, double lon)
{
    lat_ = lat;
    lon_ = lon;
    lock = SDL_CreateMutex();
    SDL_Thread *t = lock ? SDL_CreateThread(weather_thread, "weather", NULL) : NULL;
    if (!t)
        return -1;
    SDL_DetachThread(t);
    return 0;
}

void weather_get(Weather *w)
{
    SDL_LockMutex(lock);
    *w = current;
    SDL_UnlockMutex(lock);
}
