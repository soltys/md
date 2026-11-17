#ifndef MD_TIMING_H
#define MD_TIMING_H

/*
 * Startup timing log: every step from process creation to the first full
 * render of the page, with the step's duration and the running total.
 *
 * MD_TIMING is set by CMake: 1 in Debug builds, and in other configurations
 * only with -DMD_TIMING_LOG=ON. With MD_TIMING 0 every macro below expands to
 * ((void)0) and drops its arguments unevaluated, so a normal Release build
 * contains no timing code at all. `md --version` says whether it is on.
 *
 * Steps are timed per thread: a mark ends the step that began at the
 * previous mark on the same thread. Marks are kept in memory (no I/O while
 * starting) and printed as one report right after the first full render.
 * After that, TIMING_MARK is ignored and TIMING_EVENT lines (images decoded,
 * background layout done, Neovim's first redraw) are printed as they happen.
 */

#ifndef MD_TIMING
#define MD_TIMING 0
#endif

#if MD_TIMING

void timing_init(void);
void timing_thread(const char *name);
void timing_mark(const char *fmt, ...);
void timing_event(const char *fmt, ...);
/* Prints the report; returns ms from process creation to the first full
 * render, or -1 if the report was printed already. */
double timing_first_full_render(void);
const char *timing_log_path(void); /* valid after the report */

#define TIMING_INIT()               timing_init()
#define TIMING_THREAD(name)         timing_thread(name)
#define TIMING_MARK(...)            timing_mark(__VA_ARGS__)
#define TIMING_EVENT(...)           timing_event(__VA_ARGS__)
#define TIMING_FIRST_FULL_RENDER()  timing_first_full_render()

#else

#define TIMING_INIT()               ((void)0)
#define TIMING_THREAD(name)         ((void)0)
#define TIMING_MARK(...)            ((void)0)
#define TIMING_EVENT(...)           ((void)0)
#define TIMING_FIRST_FULL_RENDER()  ((void)0)

#endif

#endif
