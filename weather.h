#ifndef WEATHER_H
#define WEATHER_H

/* Current conditions from Open-Meteo (free, no key), fetched with curl on
 * a background thread every 10 minutes, turned into a rate of water drops. */

typedef struct {
    int   ok;               /* at least one successful fetch */
    int   code;             /* WMO weather code */
    float cloud_cover;      /* % */
    float precip_mmh;       /* mm/hour, from the last 15-minute total */
    float drops_per_min;
    int   raining;          /* slows evaporation */
} Weather;

int  weather_start(double lat, double lon);     /* 0 on success */
void weather_get(Weather *w);

/* Fetch once, synchronously (for --weather). */
int  weather_fetch_now(double lat, double lon, Weather *w);

/* Short description of a WMO weather code. */
const char *weather_describe(int code);

#endif
