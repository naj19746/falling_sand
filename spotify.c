#include "spotify.h"

#include <SDL.h>
#include <dbus/dbus.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define POLL_MS       1000
#define RETRY_MS      5000              /* Spotify not running */
#define NEAR_END_US   (10 * 1000000LL)  /* this close to the end = finished */
#define RESTART_US    (5 * 1000000LL)   /* a jump back this far = started over */

#define BUS_NAME  "org.mpris.MediaPlayer2.spotify"
#define OBJ_PATH  "/org/mpris/MediaPlayer2"
#define PLAYER    "org.mpris.MediaPlayer2.Player"

static SDL_atomic_t completed;

typedef struct {
    char    track[256];
    int64_t len, pos;       /* microseconds */
} Status;

/* What the last polls saw, to tell a finished song from a skipped one. */
typedef struct {
    char    track[256];
    int64_t pos;
    int     near_end;
} Tracker;

/* Returns 1 if the song that was playing has just finished. */
static int tracker_update(Tracker *t, const Status *st)
{
    int changed = strcmp(st->track, t->track) != 0;
    int done = t->near_end && (changed || st->pos + RESTART_US < t->pos);
    if (done || changed)
        t->near_end = 0;
    if (changed)
        snprintf(t->track, sizeof t->track, "%s", st->track);
    t->pos = st->pos;
    if (st->len > 0 && st->pos + NEAR_END_US >= st->len)
        t->near_end = 1;
    return done;
}

/* MPRIS allows any integer type here; Spotify sends lengths as uint64. */
static int64_t read_int(DBusMessageIter *var)
{
    DBusMessageIter v;
    dbus_message_iter_recurse(var, &v);
    switch (dbus_message_iter_get_arg_type(&v)) {
    case DBUS_TYPE_INT64:  { dbus_int64_t  x; dbus_message_iter_get_basic(&v, &x); return x; }
    case DBUS_TYPE_UINT64: { dbus_uint64_t x; dbus_message_iter_get_basic(&v, &x); return (int64_t)x; }
    case DBUS_TYPE_INT32:  { dbus_int32_t  x; dbus_message_iter_get_basic(&v, &x); return x; }
    case DBUS_TYPE_UINT32: { dbus_uint32_t x; dbus_message_iter_get_basic(&v, &x); return x; }
    default: return 0;
    }
}

static void read_metadata(DBusMessageIter *var, Status *st)
{
    DBusMessageIter dict, entry, v;
    dbus_message_iter_recurse(var, &v);
    if (dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_ARRAY)
        return;
    for (dbus_message_iter_recurse(&v, &dict);
         dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY;
         dbus_message_iter_next(&dict)) {
        const char *key;
        dbus_message_iter_recurse(&dict, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        if (!strcmp(key, "mpris:length")) {
            st->len = read_int(&entry);
        } else if (!strcmp(key, "mpris:trackid")) {
            DBusMessageIter id;
            dbus_message_iter_recurse(&entry, &id);
            int t = dbus_message_iter_get_arg_type(&id);
            if (t == DBUS_TYPE_OBJECT_PATH || t == DBUS_TYPE_STRING) {
                const char *s;
                dbus_message_iter_get_basic(&id, &s);
                snprintf(st->track, sizeof st->track, "%s", s);
            }
        }
    }
}

/* One Properties.GetAll call for the track and position. Returns 0 on
 * success, -1 if Spotify isn't running or didn't answer. */
static int poll_status(DBusConnection *conn, Status *st)
{
    memset(st, 0, sizeof *st);
    DBusMessage *msg = dbus_message_new_method_call(BUS_NAME, OBJ_PATH,
            "org.freedesktop.DBus.Properties", "GetAll");
    if (!msg)
        return -1;
    const char *iface = PLAYER;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, NULL);
    dbus_message_unref(msg);
    if (!reply)
        return -1;

    DBusMessageIter it, dict, entry;
    int ok = dbus_message_iter_init(reply, &it) &&
             dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY;
    if (ok) {
        for (dbus_message_iter_recurse(&it, &dict);
             dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY;
             dbus_message_iter_next(&dict)) {
            const char *key;
            dbus_message_iter_recurse(&dict, &entry);
            dbus_message_iter_get_basic(&entry, &key);
            dbus_message_iter_next(&entry);
            if (!strcmp(key, "Metadata"))
                read_metadata(&entry, st);
            else if (!strcmp(key, "Position"))
                st->pos = read_int(&entry);
        }
    }
    dbus_message_unref(reply);
    return ok && st->track[0] ? 0 : -1;
}

static DBusConnection *connect_bus(void)
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        SDL_SetError("session bus: %s", err.message);
        dbus_error_free(&err);
        return NULL;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    return conn;
}

static int spotify_thread(void *data)
{
    DBusConnection *conn = data;
    Tracker t = {0};
    for (;;) {
        Status st;
        if (poll_status(conn, &st) != 0) {
            /* Closed or not started; a song cut off by quitting isn't done. */
            memset(&t, 0, sizeof t);
            SDL_Delay(RETRY_MS);
            continue;
        }
        if (tracker_update(&t, &st))
            SDL_AtomicAdd(&completed, 1);
        SDL_Delay(POLL_MS);
    }
    return 0;
}

int spotify_start(void)
{
    dbus_threads_init_default();
    DBusConnection *conn = connect_bus();
    if (!conn)
        return -1;
    SDL_Thread *th = SDL_CreateThread(spotify_thread, "spotify", conn);
    if (!th) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return -1;
    }
    SDL_DetachThread(th);
    return 0;
}

int spotify_completed(void)
{
    return SDL_AtomicGet(&completed);
}

int spotify_watch(void)
{
    DBusConnection *conn = connect_bus();
    if (!conn) {
        fprintf(stderr, "%s\n", SDL_GetError());
        return 1;
    }
    Tracker t = {0};
    int count = 0, running = -1;
    for (;;) {
        Status st;
        int ok = poll_status(conn, &st) == 0;
        if (ok != running) {
            if (!ok)
                printf("\nspotify: not running\n");
            running = ok;
        }
        if (!ok) {
            memset(&t, 0, sizeof t);
            SDL_Delay(RETRY_MS);
            continue;
        }
        if (tracker_update(&t, &st))
            printf("\ncompleted (%d so far): a seed drops\n", ++count);
        printf("\r%s  %lld/%llds%s   ", st.track, (long long)(st.pos / 1000000),
               (long long)(st.len / 1000000), t.near_end ? "  (near end)" : "");
        fflush(stdout);
        SDL_Delay(POLL_MS);
    }
}
