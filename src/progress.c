/*
 * progress.c — ASCII art loading bar for slow CLI operations.
 *
 * =========================================================================
 * Role
 * =========================================================================
 * Provides a three-line animated progress display on stderr (only when
 * stderr is a TTY).  The animation shows a "joint" rolling across a
 * dotted track with smoke wisps, accompanied by rotating joke messages.
 *
 * The public API is intentionally tiny:
 *   progress_init()   — detect TTY, set quiet mode for fast-path flags.
 *   progress_update() — redraw at 10 % increments (0, 10, 20, ... 100).
 *   progress_done()   — erase the progress display via ANSI escape codes.
 *
 * The main.c driver calls progress_update() at key pipeline milestones
 * (after lex, parse, name resolution, type check, codegen).  Fast-path
 * modes (--fmt, --pretty, --expand, --check) pass quiet=1 to suppress
 * the bar entirely.
 *
 * Story: 10.2.11
 */

#include "progress.h"

#include <stdio.h>
#include <unistd.h>

/* ── State ─────────────────────────────────────────────────────────── */

static int g_quiet   = 0;   /* --quiet suppresses all output          */
static int g_active  = 0;   /* 1 once we have printed at least once   */
static int g_is_tty  = 0;   /* stderr is a TTY                        */

/* ── Animation frames (11 frames, 0-100 in steps of 10) ───────────── */

static const char *const FRAMES[] = {
    /* 0   */ "  ~\xe2\x89\x88\n"
              "  \xe2\x88\xbf\xcb\x9c\n"
              "*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 10  */ "     \xcb\x9c\n"
              "    ~\xe2\x89\x88\n"
              "....:*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 20  */ "        \xe2\x88\xbf\n"
              "       ~\xcb\x9c\n"
              "..........:*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 30  */ "            \xe2\x89\x88\n"
              "           \xe2\x88\xbf~\n"
              "...............:*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 40  */ "                 \xcb\x9c\n"
              "                ~\xe2\x89\x88\n"
              "....................:*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 50  */ "                      \xe2\x88\xbf\n"
              "                     \xcb\x9c~\n"
              ".........................:*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 60  */ "                           \xe2\x89\x88\n"
              "                          \xe2\x88\xbf\xcb\x9c\n"
              "..............................:*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 70  */ "                               ~\n"
              "                              \xe2\x89\x88\xe2\x88\xbf\n"
              "...................................:*\xe2\x96\x88\xe2\x96\x88\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 80  */ "                                    \xcb\x9c\n"
              "                                   ~\xe2\x89\x88\n"
              "......................................:*\xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 90  */ "                                      \xe2\x88\xbf\n"
              "                                     \xcb\x9c~\n"
              ".......................................: \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93",
    /* 100 */ " \n"
              " \n"
              ".......................................: \xe2\x96\x93\xe2\x96\x93\xe2\x96\x93"
};

/* ── Loading messages (20 messages) ───────────────────────────────── */

static const char *const MESSAGES[] = {
    "Rolling things up\xe2\x80\xa6",
    "Packing it in\xe2\x80\xa6",
    "Getting a good hit on the server\xe2\x80\xa6",
    "Pulling data slowly\xe2\x80\xa6",
    "Blazing through requests\xe2\x80\xa6",
    "Lighting up the pipeline\xe2\x80\xa6",
    "Taking a long drag from the database\xe2\x80\xa6",
    "Inhaling your preferences\xe2\x80\xa6",
    "Passing it to the next process\xe2\x80\xa6",
    "Hitting the cache real smooth\xe2\x80\xa6",
    "Grinding through the queue\xe2\x80\xa6",
    "Cherrying the connection\xe2\x80\xa6",
    "Toasting the buffers\xe2\x80\xa6",
    "Puff puff processing\xe2\x80\xa6",
    "Almost cashed out\xe2\x80\xa6",
    "Deep in rotation\xe2\x80\xa6",
    "Sparking up the backend\xe2\x80\xa6",
    "Cottonmouth loading\xe2\x80\xa6",
    "Holding it in\xe2\x80\xa6",
    "Exhaling results\xe2\x80\xa6"
};

/* ── Public API ────────────────────────────────────────────────────── */

/*
 * progress_init — Initialise progress state and detect TTY.
 *
 * If quiet is non-zero (fast-path modes) or stderr is not a TTY, all
 * subsequent progress_update() calls become no-ops.
 */
void progress_init(int quiet)
{
    g_quiet  = quiet;
    g_active = 0;
    g_is_tty = isatty(STDERR_FILENO);
}

/*
 * progress_update — Redraw the progress bar at the given percentage.
 *
 * Only renders at multiples of 10 (there are exactly 11 animation
 * frames, indexed 0..10).  On repeated calls the cursor is moved up
 * four lines to overwrite the previous frame in-place.
 */
void progress_update(int pct)
{
    if (g_quiet || !g_is_tty) return;
    if (pct < 0)   pct = 0;
    if (pct > 100) pct = 100;

    /* Only render at multiples of 10 */
    if (pct % 10 != 0) return;

    int frame_idx = pct / 10;          /* 0..10 */
    int msg_idx   = pct / 5 % 20;      /* 0..19 */

    /* If we already printed, move cursor up 4 lines to overwrite */
    if (g_active) {
        fprintf(stderr, "\033[4A");
    }

    fprintf(stderr, "%s\n%s\n", MESSAGES[msg_idx], FRAMES[frame_idx]);
    g_active = 1;
}

/*
 * progress_done — Erase the progress display and reset state.
 *
 * Moves the cursor up four lines and clears each with ANSI \033[K,
 * then repositions so subsequent stderr output starts cleanly.
 */
void progress_done(void)
{
    if (g_quiet || !g_is_tty) return;

    /* Clear the progress display: move up 4 lines, clear each */
    if (g_active) {
        fprintf(stderr, "\033[4A");
        fprintf(stderr, "\033[K\n\033[K\n\033[K\n\033[K");
        fprintf(stderr, "\033[4A");
    }

    g_active = 0;
}
