#include "audio.h"

#include <SDL.h>
#include <math.h>
#include <pulse/error.h>
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <string.h>

#define RATE        11025               /* plenty for a loudness meter */
#define CHUNK       512                 /* samples per wakeup, ~46 ms */
#define FULL_RMS    0.40f               /* RMS treated as "full loudness" */
#define STALE_MS    250                 /* no data this long = silence */

/* Written by the audio threads, read by the main loop. Stored as
 * thousandths so they fit in an SDL_atomic_t. */
static SDL_atomic_t rms_milli;
static SDL_atomic_t vol_milli;
static SDL_atomic_t last_ms;

/* Everything below runs on PulseAudio's mainloop thread, driven by events:
 * audio arriving from the output's monitor, and changes to that output's
 * volume, mute, or (when following it) which output is the default. */

static pa_threaded_mainloop *ml;
static pa_stream *stream;
static char fixed_sink[256];    /* from the config; "" = follow the default */
static char cur_sink[256];

static void context_state_cb(pa_context *c, void *userdata);

static void read_cb(pa_stream *st, size_t nbytes, void *u)
{
    (void)nbytes;
    (void)u;
    const void *data;
    size_t len;
    while (pa_stream_readable_size(st) > 0) {
        if (pa_stream_peek(st, &data, &len) < 0 || len == 0)
            return;
        if (data) {     /* NULL = a gap in the stream; just skip it */
            const int16_t *smp = data;
            size_t n = len / sizeof *smp;
            double sum = 0;
            for (size_t i = 0; i < n; i++)
                sum += (double)smp[i] * smp[i];
            float rms = n ? (float)sqrt(sum / n) / 32768.0f : 0.0f;
            SDL_AtomicSet(&rms_milli, (int)(rms * 1000));
            SDL_AtomicSet(&last_ms, (int)SDL_GetTicks());
        }
        pa_stream_drop(st);
    }
}

/* (Re)start capture from cur_sink's monitor. */
static void start_stream(pa_context *c)
{
    static const pa_sample_spec ss = {
        .format = PA_SAMPLE_S16LE, .rate = RATE, .channels = 1,
    };
    pa_buffer_attr attr = {
        .maxlength = (uint32_t)-1, .tlength = (uint32_t)-1,
        .prebuf = (uint32_t)-1, .minreq = (uint32_t)-1,
        .fragsize = CHUNK * sizeof(int16_t),
    };

    if (stream) {
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }
    char monitor[300];
    snprintf(monitor, sizeof monitor, "%s.monitor", cur_sink);
    stream = pa_stream_new(c, "level meter", &ss, NULL);
    if (!stream)
        return;
    pa_stream_set_read_callback(stream, read_cb, NULL);
    if (pa_stream_connect_record(stream, monitor, &attr, PA_STREAM_ADJUST_LATENCY) < 0)
        fprintf(stderr, "audio: can't record %s: %s\n", monitor,
                pa_strerror(pa_context_errno(c)));
}

static void sink_info_cb(pa_context *c, const pa_sink_info *i, int eol, void *u)
{
    (void)c;
    (void)u;
    if (eol || !i)
        return;
    float vol = i->mute ? 0.0f : (float)pa_cvolume_avg(&i->volume) / PA_VOLUME_NORM;
    SDL_AtomicSet(&vol_milli, (int)(vol * 1000));
}

static void server_info_cb(pa_context *c, const pa_server_info *i, void *u)
{
    (void)u;
    const char *want = fixed_sink[0] ? fixed_sink : i ? i->default_sink_name : NULL;
    if (!want)
        return;
    if (!stream || strcmp(cur_sink, want) != 0) {
        snprintf(cur_sink, sizeof cur_sink, "%s", want);
        start_stream(c);
    }
    pa_operation *op = pa_context_get_sink_info_by_name(c, cur_sink, sink_info_cb, NULL);
    if (op)
        pa_operation_unref(op);
}

static void refresh(pa_context *c)
{
    pa_operation *op = pa_context_get_server_info(c, server_info_cb, NULL);
    if (op)
        pa_operation_unref(op);
}

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t t,
                         uint32_t idx, void *u)
{
    (void)idx;
    (void)u;
    int facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    if (facility == PA_SUBSCRIPTION_EVENT_SINK ||
        facility == PA_SUBSCRIPTION_EVENT_SERVER)
        refresh(c);
}

static void connect_context(void)
{
    pa_context *c = pa_context_new(pa_threaded_mainloop_get_api(ml), "sandbg");
    pa_context_set_state_callback(c, context_state_cb, NULL);
    pa_context_set_subscribe_callback(c, subscribe_cb, NULL);
    /* NOFAIL: wait for the server if it isn't up yet (e.g. at login). */
    pa_context_connect(c, NULL, PA_CONTEXT_NOFAIL, NULL);
}

static void context_state_cb(pa_context *c, void *u)
{
    (void)u;
    switch (pa_context_get_state(c)) {
    case PA_CONTEXT_READY: {
        pa_operation *op = pa_context_subscribe(
            c, PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER, NULL, NULL);
        if (op)
            pa_operation_unref(op);
        refresh(c);
        break;
    }
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
        /* Server restarted: start over with a fresh connection. */
        if (stream) {
            pa_stream_unref(stream);
            stream = NULL;
        }
        pa_context_unref(c);
        connect_context();
        break;
    default:
        break;
    }
}

int audio_start(const char *sink)
{
    snprintf(fixed_sink, sizeof fixed_sink, "%s", sink ? sink : "");
    SDL_AtomicSet(&vol_milli, 1000);

    ml = pa_threaded_mainloop_new();
    if (!ml || pa_threaded_mainloop_start(ml) < 0) {
        SDL_SetError("can't start PulseAudio mainloop");
        return -1;
    }
    pa_threaded_mainloop_lock(ml);
    connect_context();
    pa_threaded_mainloop_unlock(ml);
    return 0;
}

float audio_volume(void)
{
    return SDL_AtomicGet(&vol_milli) / 1000.0f;
}

float audio_level(void)
{
    /* PipeWire stops delivering data when the output goes idle, which
     * would otherwise freeze the last loud value. */
    if ((int)SDL_GetTicks() - SDL_AtomicGet(&last_ms) > STALE_MS)
        return 0.0f;
    float lvl = SDL_AtomicGet(&rms_milli) / 1000.0f / FULL_RMS;
    if (lvl > 1.0f)
        lvl = 1.0f;
    return lvl * audio_volume();
}
