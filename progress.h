#ifndef PROGRESS_H
#define PROGRESS_H

/* Critters earned outside the sandbox: finished kana practice sets (from
 * the kana platform's database, over curl) and newly saved Pixelorama
 * drawings (from its config.ini). A background thread polls both and keeps
 * a queue of critters waiting to drop in, saved in
 * $XDG_STATE_HOME/sandbg/progress so none are lost or paid twice across
 * restarts. See CRITTERS.md, section 7. */

#define PROGRESS_NONE (-2)  /* progress_next(): nothing waiting */

/* Start the thread. An empty kana_url or pxo_config turns that source off.
 * Returns 0 on success, -1 if neither source is on or another sandbg
 * already collects rewards (only one copy may, so none pay twice). */
int  progress_start(const char *kana_url, const char *pxo_config);

/* The kind (CR_ANY, CR_SMALL or CR_LARGE) of the next critter waiting,
 * without taking it, or PROGRESS_NONE. */
int  progress_next(void);

/* The critter progress_next() returned has dropped in: forget it. */
void progress_dropped(void);

/* --progress: fetch once and print what the thread would see, without
 * changing the saved state. */
int  progress_print(const char *kana_url, const char *pxo_config);

#endif
