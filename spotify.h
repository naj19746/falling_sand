#ifndef SPOTIFY_H
#define SPOTIFY_H

/* Counts songs Spotify plays to the end, by polling its MPRIS interface on
 * the session bus once a second from a background thread. A song counts
 * when it was seen within a few seconds of its end and then the track
 * changed or started over; skipping mid-song doesn't count. */

int spotify_start(void);        /* 0 on success */
int spotify_completed(void);    /* songs completed since spotify_start */

/* Print what's playing and each completion, until interrupted. */
int spotify_watch(void);

#endif
