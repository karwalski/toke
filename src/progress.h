#ifndef TKC_PROGRESS_H
#define TKC_PROGRESS_H
/* Initialize progress display (call once at start) */
void progress_init(int quiet);
/* Update progress to pct (0-100), only renders at 0,10,20,...,100 */
void progress_update(int pct);
/* Clear progress display and show final state */
void progress_done(void);
#endif
