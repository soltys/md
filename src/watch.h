#ifndef MD_WATCH_H
#define MD_WATCH_H

#include <SDL3/SDL.h>

/* Watches one file for changes on disk (another editor saving it, git, a
 * tool writing it). When it changes, an event of type `wake_event` with
 * user.code WATCH_EVENT_CODE and user.data1 = the watched path (SDL_free it)
 * is pushed. Watching runs on its own thread; starting a new watch stops the
 * previous one. */
#define WATCH_EVENT_CODE 4

void watch_file(const char *path, Uint32 wake_event);
void watch_stop(void);

#endif
