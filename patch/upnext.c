/* View Queue (docs/internals.md#queue-menu), Q2 Podcast's (the user's, 2026-11-15): the playing
 * episode, then Up Next in the order it plays, each with its show's art, its title over its show,
 * and a ⋯ for its menu (podcast.c podcast_row, the rows of Now Playing's Queue page); the wheel
 * goes a row at a time, as every list. navigation.c's qm_run (the Play/Pause hold's View Queue) and
 * Now Playing's ⋯ page (podcast.c more_queue) open it (up_next), and ringnav_keylong hands a
 * Play/Pause hold on a row to un_hold. */
#include "offsets.inc"
#include "peq.h"
#include "playback.h"
#include "stock.h"

#define UP_NEXT_ROWS 100                                   /* Up Next's snapshot cap */
#define SEL "_ringnav_index"                               /* navigation.c's */
unsigned queue_hash(void *q);                              /* navigation.c */
void *podcast_row(void *view, int y, int w, int h, void *r, int lead);
int podcast_row_more(void *item, void *event);             /* podcast.c */
void podcast_row_menu(unsigned at);                        /* podcast.c */
int media_owner(void);                                     /* headset.c */
static struct {
    void *page;                 /* the page, one at a time */
    unsigned idx[UP_NEXT_ROWS]; /* its Up Next rows' queue indices, in play order */
    void *view;                 /* its list */
    unsigned n, hash, row;      /* Up Next's rows, the queue_hash they were taken from, a hold's row */
    unsigned lead;              /* 1: the list's first row is the playing episode, Up Next's after */
    int sel;                    /* the row selected once the rows are taken again (un_again) */
} un __attribute__((section(".scratch")));

/* The playing local track's row, whatever the play status, or 0. */
static void *un_playing(void) {
    unsigned at, n, type = (unsigned)I(MCL_TYPE, 0);
    void *q = P(mcl_pdeqplaylist, 0);
    at = (unsigned)I(MCL_POS, 0);
    n = q ? deque_size(q) : 0;
    if (media_owner() != 1 || (type != 1 && (type & 0xf000) != 0xf000) || at >= n) return 0;
    return deque_at(q, at);
}

static int un_play(void *ctx, void *event);

/* The rows, taken again; sel, if not -1, the row selected. */
static void un_fill(int sel) {
    void *q = P(mcl_pdeqplaylist, 0), *lead = un_playing();
    un.n = q ? playback_queue_rows(un.idx, UP_NEXT_ROWS) : 0;
    un.lead = lead != 0;
    un.hash = q ? queue_hash(q) : 0;
    int rows = (int)(un.n + un.lead), h = page_h(64);
    void *view = un.view = page_list_or(un.page, un.page, "Queue", "Nothing playing", rows, 64);
    if (lead) widget_on(podcast_row(view, 0, 375, h, lead, 0), EVT_CLICK, un_play, (void *)0);
    for (unsigned i = 0; i < un.n; i++) {
        unsigned row = i + un.lead;
        widget_on(podcast_row(view, (int)row * h, 375, h, deque_at(q, un.idx[i]), (int)i + 1),
                  EVT_CLICK, un_play, (void *)(long)row);
    }
    if (sel >= 0 && rows) ringnav_select(view, sel < rows ? sel : rows - 1, rows);
}

/* The rows taken again off the click that moved one (its row would go under its own dispatch). */
static int un_again(const void *unused) {
    if (un.page) un_fill(un.sel);
    return 0;
}

/* A row's tap or Centre (ctx: the row). Its ⋯ opens its menu (podcast.c podcast_row_menu: moves,
 * Delete When Finished, Delete Episode, Remove from Queue); the playing row goes back to Now
 * Playing; another row plays its episode and the one playing goes first in Up Next
 * (playback_play_swap), as Now Playing's Queue page does, and Now Playing comes in. A queue changed
 * under the page takes the rows again and says so. */
static int un_play(void *ctx, void *event) {
    unsigned row = (unsigned)(long)ctx, k = row - un.lead;
    void *q = P(mcl_pdeqplaylist, 0);
    if (un.lead && !row) {
        navigator_back();
        playback_np();
        return 0;
    }
    if (!q || k >= un.n || queue_hash(q) != un.hash) {
        un.sel = (int)row;
        if (timer_add(un_again, (void *)0, 0)) toast("Queue changed");
        return 0;
    }
    if (event && podcast_row_more(P(event, EVENT_TARGET), event)) { /* its ⋯: its menu */
        un.sel = (int)row;
        podcast_row_menu(un.idx[k]);
        return 0;
    }
    int at = playback_play_swap(un.idx[k]);
    if (at < 0 && un.idx[k] != (unsigned)I(MCL_POS, 0) && un.idx[k] < deque_size(q))
        at = (int)un.idx[k]; /* a queue not ours to reorder: stock's pick */
    if (at >= 0) press_stamp(), player_start_queue(at);
    navigator_back();
    playback_np();
    return 0;
}

/* The page gone: the next View Queue opens a new one. */
static int un_closed(void *ctx, void *event) {
    un.page = 0;
    return 0;
}
/* Its rows taken again, the selection kept: covers made since they were (podcast.c). */
void up_next_again(void) {
    if (un.page && un.view) un_fill(widget_get_prop_int(un.view, SEL, -1));
}
void up_next(int (*keyup)(void *, void *)) {
    if (un.page || !(un.page = page_open("upnext_page", un_closed, keyup))) return;
    un_fill(-1);
}
/* A Play/Pause hold on a row (un_hold, deferred out of its dispatch): the row's episode plays next
 * (playback_move) and the rows are taken again with it first in Up Next, the selection there. A
 * row already next stays there. A queue changed under the page, or a row no longer up next (played
 * since), takes the rows again and says "Queue changed"; the playing row, a page gone or a row out
 * of range does nothing. */
static int un_move(const void *unused) {
    unsigned row = un.row, up[UP_NEXT_ROWS], n = playback_queue_rows(up, UP_NEXT_ROWS), i = 0;
    void *q = P(mcl_pdeqplaylist, 0);
    if (!un.page || row < un.lead || row - un.lead >= un.n || !q) return 0;
    unsigned at = un.idx[row - un.lead];
    while (i < n && up[i] != at) ++i;
    int stale = queue_hash(q) != un.hash || i == n || (i && playback_move(at) < 0);
    un_fill((int)un.lead);
    if (stale) toast("Queue changed");
    return 0;
}
int un_hold(void *top) {
    int row = top && top == un.page ? widget_get_prop_int(un.view, SEL, -1) : -1;
    if (row < 0 || !timer_add(un_move, (void *)0, 0)) return 0;
    un.row = (unsigned)row;
    return 1;
}
