#ifndef AUDIO_H
#define AUDIO_H

/* Loudness of whatever is playing on an output, captured from its monitor
 * through PulseAudio (or PipeWire's Pulse server) on a background thread,
 * and scaled by that output's volume slider (muted = 0). */

/* sink: the output's name (as in `pactl list short sinks`), or NULL/"" to
 * follow whichever output is the default. 0 on success. */
int   audio_start(const char *sink);
float audio_level(void);    /* 0..1; 0 when nothing is playing */
float audio_volume(void);   /* the slider value last read, 0..1+ */

#endif
