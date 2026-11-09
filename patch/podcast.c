/* Podcasts and audiobooks (docs/internals.md#podcasts-on-now-playing): on a spoken track the side
 * buttons skip back and forward, and held they jump between its chapters; Now Playing's heart stars
 * a Pocket episode in Pocket Casts (q2-pocket); and Now Playing has Description and Chapters pages
 * after its info page. */
#include "navigation.h"

/* PC_HOLD_MS, SKIP_BACK_S and SKIP_FWD_S are in offsets.inc. A key-down PC_REPEAT_MS after the
 * last is a new press (a lost release). Previous held CHAPTER_BACK_S into a chapter goes to its
 * start, else to the one before. A hold waits CHAPTER_WAIT_MS for the chapters being read; a spoken
 * track's are read once it has played CHAPTER_DELAY_MS. */
#define PC_REPEAT_MS 400
#define CHAPTER_BACK_S 3
#define CHAPTER_WAIT_MS 4000
#define CHAPTER_DELAY_MS 2000
/* stock Now Playing's held side key seek (docs/internals.md#scrub-ipod) */
#define NP_KEY_SEEK 0xa3a6c0
/* "<1|0>\t<path>\n": q2-pocket stars the episode, or not, in Pocket Casts; and q2-pocket's list of
 * the downloaded episodes Pocket Casts has starred. A star asked for shows until that list changes,
 * or STAR_WAIT_MS. */
#define STAR_ASK "/tmp/q2-star"
#define STARRED "/tmp/q2-starred"
#define STAR_WAIT_MS 60000
#define STARRED_MAX (64 * 1024)
#define DESC_MAX (16 * 1024)
#define PAGE_H 186 /* tools/ipod.py NP_SLIDE_H from the lyrics' top, as visualizer.c's VIS_H */
#define PAGE_X 16  /* tools/ipod.py NP_MARGIN */
#define ROW_H 44
#define TIME_W 76
#define ROWS_MAX 200 /* the Chapters page's rows; a hold still reaches every chapter */
#define POLL_MS 500
#define GREY 0xffaaaaaau
enum { CH_NONE, CH_READING, CH_DONE };

static struct {
    /* the side key whose press is ours: its press's time, its last down's, whether its hold ran */
    unsigned key, down_at, seen_at, hold_timer;
    int held;
    /* the playing track (listen_key) and its file's chapters, read once it has played a moment */
    unsigned track, since, polled, read_for, want_at;
    int state, count, want; /* want: a hold's jump, +1 or -1, waiting for the reader */
    int seen;               /* track and since hold a look's (0 before the first) */
    chapters *reading;
    chapter *rows;
    /* q2-pocket's starred list as last read (stat words: mtime, its ns, size), a star asked for */
    char *starred;
    unsigned stamp[3], ask_key, ask_at;
    int ask_on, painted;
    /* Now Playing's pages: the window, its slide_view, the two scroll views and the text */
    void *win, *slide, *desc_view, *desc_text, *ch_view;
    char *desc;
    int desc_at, lit, shown_count;
    unsigned shown_desc, shown_rows; /* the track each page shows, ~0 to build again */
} pc;

/* The local track playing or paused when it is a spoken one (navigation.c spoken), else 0. */
static void *spoken_now(void) {
    unsigned at, n, type = (unsigned)MCL(MCL_TYPE), s = (unsigned)mclGetPlayStatus();
    if (media_owner() != 1 || (s != 2 && s != 3)) return 0;
    if (type != 1 && (type & 0xf000) != 0xf000) return 0; /* a stream (rate_track) */
    void *r = queue_now(&at, &n);
    return r && spoken(P(r, REC_PATH)) ? r : 0;
}

/* Chapters the file itself holds: a spoken file's, not a CUE track's, whose sheet has them, as the
 * hold menu's Chapters (navigation.c chapter_file); an M4B too long to open has none. */
static int readable(void *r) {
    const char *path = P(r, REC_PATH);
    return spoken(path) && !I(r, REC_CUE_START) && !I(r, REC_CUE_END) && mp4_fits(path);
}

static void read_start(void *r) {
    if (pc.reading)
        return;         /* the last track's, still being reaped: podcast_poll starts this one's */
    pc.state = CH_DONE; /* none, unless the reader starts */
    if (!readable(r)) return;
    pc.reading = chapters_start(P(r, REC_PATH), time_now_ms());
    pc.read_for = pc.track;
    if (pc.reading) pc.state = CH_READING;
}

/* The chapter playing at sec: the last starting at or before it. */
static int chapter_at(int sec) {
    int i = 0;
    while (i + 1 < pc.count && pc.rows[i + 1].sec <= sec) ++i;
    return i;
}

/* A hold: the next chapter, or this one's start (the one before within CHAPTER_BACK_S of it); with
 * none, past the first or the last, the next or previous track, as the button's press is elsewhere.
 */
static void chapter_jump(int dir) {
    int sec = np_second();
    if (pc.count && sec >= 0) {
        int i = chapter_at(sec), to = dir > 0                                 ? i + 1
                                      : sec - pc.rows[i].sec > CHAPTER_BACK_S ? i
                                                                              : i - 1;
        if (to >= 0 && to < pc.count) {
            np_seek(pc.rows[to].sec);
            if (g_backlight_status) toast(pc.rows[to].title);
            return;
        }
    }
    dir > 0 ? player_next_music() : player_prev_music();
}

/* Another track than the last look's: its description, and its chapters still to read. */
static void track_check(void *r, unsigned now) {
    unsigned track = r ? listen_key(r) : 0;
    if (track == pc.track && pc.seen) return;
    pc.track = track;
    pc.since = now;
    pc.seen = 1;
    free(pc.rows);
    pc.rows = 0;
    pc.count = pc.want = 0;
    /* a CUE track's sheet has its chapters; whether an M4B fits waits for the read (readable) */
    const char *path = r ? P(r, REC_PATH) : 0;
    pc.state =
        path && spoken(path) && !I(r, REC_CUE_START) && !I(r, REC_CUE_END) ? CH_NONE : CH_DONE;
    if (pc.reading) chapters_cancel(pc.reading); /* reaped by podcast_poll, its rows dropped */
    free(pc.desc);
    pc.desc = 0; /* read when its page shows it (pages_poll) */
}

static void hold(int dir) {
    void *r = spoken_now();
    track_check(r, (unsigned)time_now_ms()); /* before podcast_poll's look */
    if (pc.state == CH_NONE && r) read_start(r);
    if (pc.state != CH_DONE) {
        pc.want = dir; /* podcast_poll jumps once they are read */
        pc.want_at = (unsigned)time_now_ms();
        return;
    }
    chapter_jump(dir);
}

/* A press: back SKIP_BACK_S or on SKIP_FWD_S, within the track. */
static void skip(int dir) {
    int sec = np_second(), total = 0, now = -1;
    if (sec < 0 || player_playtime_and_length(&now, &total) < 0) return;
    int to = sec + (dir > 0 ? SKIP_FWD_S : -SKIP_BACK_S);
    if (to < 0) to = 0;
    if (total > 0 && to >= total) to = total - 1; /* the end: on to the next track */
    np_seek(to);
}

static int hold_fire(const void *unused) {
    pc.hold_timer = 0;
    if (pc.key && !pc.held) {
        pc.held = 1;
        hold(pc.key == KEY_FWD_BTN ? 1 : -1);
    }
    return 0;
}

/* The side buttons on a spoken track, local, with no page of their own in front: phase 1 a
 * key-down (ringnav_keydown, after stock's lock), 2 its release (ringnav, first), 3 stock's long
 * press (ringnav_keylong). A press decided ours at its down stays ours to its release; stock sees
 * none of it, so Now Playing's own held seek never starts. Returns 1 when the key is ours. */
int podcast_key(unsigned key, int phase) {
    if (key != KEY_FWD_BTN && key != KEY_BACK_BTN) return 0;
    unsigned now = (unsigned)time_now_ms();
    if (phase == 1) {
        if (pc.key == key && now - pc.seen_at < PC_REPEAT_MS) {
            pc.seen_at = now; /* the press's repeat */
            return 1;
        }
        void *top = window_manager_get_top_window(window_manager());
        const char *name = top ? widget_get_prop_str(top, "name", "") : "";
        stop_timer(&pc.hold_timer);
        pc.key = 0;
        /* the date and sleep timer fields, a game, a dialog (the restart toast) keep their own */
        if (!spoken_now() || media_focused() || !tk_strcmp(name, "manualtime_page") ||
            !tk_strcmp(name, "sleepshutdown_page") || !tk_strcmp(name, "games_page") ||
            (top && !tk_strcmp(widget_get_type(top), "dialog")))
            return 0;
        pc.key = key;
        pc.down_at = pc.seen_at = now;
        pc.held = 0;
        pc.hold_timer = timer_add(hold_fire, (void *)0, PC_HOLD_MS);
        return 1;
    }
    if (pc.key != key) return 0;
    if (phase == 3) return 1;
    stop_timer(&pc.hold_timer);
    pc.key = 0;
    *(volatile unsigned char *)NP_KEY_SEEK = 0; /* in case stock's page saw the long press */
    if (pc.held) return 1;
    /* a slow loop (the screen off) can bring the release before the hold's timer */
    if (now - pc.down_at >= PC_HOLD_MS)
        hold(key == KEY_FWD_BTN ? 1 : -1);
    else
        skip(key == KEY_FWD_BTN ? 1 : -1);
    return 1;
}

/* A headset's Next and Previous (headset.c media_key) skip on a spoken track, as the buttons do. */
int podcast_media(unsigned key) {
    if ((key != KEY_FWD_BTN && key != KEY_BACK_BTN) || !spoken_now()) return 0;
    skip(key == KEY_FWD_BTN ? 1 : -1);
    return 1;
}

/* q2-pocket's starred list, read again when it changes; 0 (none) while q2-pocket isn't signed in.
 */
static void starred_load(void) {
    unsigned s[36] = { 0 };
    if (__xstat(3, STARRED, s)) {
        free(pc.starred);
        pc.starred = 0;
        return;
    }
    if (pc.starred && s[16] == pc.stamp[0] && s[17] == pc.stamp[1] && s[12] == pc.stamp[2]) return;
    void *f = fopen(STARRED, "rb");
    char *b = f ? calloc(1, STARRED_MAX + 2) : 0;
    int n = b ? (int)fread(b + 1, 1, STARRED_MAX, f) : -1;
    if (f) fclose(f);
    if (n < 0) {
        free(b);
        return; /* the last list stays */
    }
    b[0] = '\n'; /* each path between newlines */
    b[n + 1] = 0;
    free(pc.starred);
    pc.starred = b;
    pc.stamp[0] = s[16], pc.stamp[1] = s[17], pc.stamp[2] = s[12];
    pc.ask_key = 0; /* q2-pocket has answered */
}

/* The playing track's path when q2-pocket can star it: under Podcasts, q2-pocket signed in. */
static const char *starrable(void) {
    unsigned at, n, type = (unsigned)MCL(MCL_TYPE);
    if (type != 1 && (type & 0xf000) != 0xf000) return 0;
    void *r = queue_now(&at, &n);
    const char *path = r ? P(r, REC_PATH) : 0;
    if (!path || !tk_str_start_with(path, "/mnt/mmc/Podcasts/") || strlen(path) > 512) return 0;
    starred_load();
    return pc.starred ? path : 0;
}

static int starred(const char *path) {
    unsigned key = fnv(FNV_SEED, (const unsigned char *)path);
    if (pc.ask_key == key && (unsigned)time_now_ms() - pc.ask_at < STAR_WAIT_MS) return pc.ask_on;
    char line[520];
    tk_snprintf(line, sizeof line, "\n%s\n", path);
    return strstr(pc.starred, line) != 0;
}

/* Now Playing's heart (ratings.c rate_tick and rate_bind): on a starrable track the star, back to
 * My Fav's once it plays another. */
void podcast_heart(void *fav) {
    if (!fav) return;
    const char *path = starrable(), *img;
    if (path) {
        img = starred(path) ? "play_fav" : "play_unfav";
        pc.painted = 1;
    } else if (pc.painted) {
        unsigned at, n;
        void *r = queue_now(&at, &n);
        img = r && checkFavExist(r) ? "play_fav" : "play_unfav";
        pc.painted = 0;
    } else
        return;
    if (tk_strcmp(widget_get_prop_str(fav, "image", ""), img)) image_base_set_image(fav, img);
}

/* The heart's tap (ratings.c rate_fav_click): stars or unstars a starrable track in Pocket Casts,
 * through q2-pocket, instead of My Fav. Returns 0 for any other track. */
int podcast_star(void *fav) {
    const char *path = starrable();
    if (!path) return 0;
    int on = !starred(path);
    char line[540];
    int n = tk_snprintf(line, sizeof line, "%d\t%s\n", on, path);
    if (n <= 0 || n >= (int)sizeof line ||
        !blob_io(STAR_ASK, STAR_ASK ".tmp", line, (unsigned)n, 1)) {
        toast("Could not star");
        return 1;
    }
    pc.ask_key = fnv(FNV_SEED, (const unsigned char *)path);
    pc.ask_on = on;
    pc.ask_at = (unsigned)time_now_ms();
    podcast_heart(fav);
    toast(on ? "Starred in Pocket Casts" : "Star removed");
    return 1;
}

/* A spoken track's description, beside it: <folder>/.description/<name>.txt (q2-pocket writes
 * it), for its page. A song has none, and opens nothing on the card. */
static void desc_load(void *r) {
    free(pc.desc);
    pc.desc = 0;
    const char *path = r ? P(r, REC_PATH) : 0, *slash = path ? strrchr(path, '/') : 0;
    if (!slash || !spoken(path)) return;
    const char *dot = strrchr(slash, '.');
    int dir = (int)(slash - path), base = (int)((dot ? dot : slash + strlen(slash)) - slash - 1);
    char file[600];
    if (dir + base + 20 >= (int)sizeof file) return;
    tk_snprintf(file, sizeof file, "%.*s/.description/%.*s.txt", dir, path, base, slash + 1);
    void *f = fopen(file, "rb");
    char *s = f ? calloc(1, DESC_MAX + 1) : 0;
    int n = s ? (int)fread(s, 1, DESC_MAX, f) : 0;
    if (f) fclose(f);
    if (n == DESC_MAX) { /* cut short: not within a character, its lead byte and what followed */
        while (n && ((unsigned char)s[n - 1] & 0xc0) == 0x80) --n;
        if (n && ((unsigned char)s[n - 1] & 0xc0) == 0xc0) --n;
    }
    if (n <= 0) {
        free(s);
        return;
    }
    s[n] = 0;
    pc.desc = s;
}

static int pages_gone(void *win, void *event) {
    if (win == pc.win) pc.win = pc.slide = pc.desc_view = pc.desc_text = pc.ch_view = 0;
    return 0;
}

/* A page of the slide_view, where the lyrics are, with a scroll view taking up-and-down drags. */
static void *page(void *slide, int top, int w) {
    void *p = widget_factory_create_widget(widget_factory(), "view", slide, 0, top, w, PAGE_H);
    void *view = p ? scroll_view_create(p, 0, 0, w, PAGE_H) : 0;
    if (!view) {
        if (p) widget_destroy(p);
        return 0;
    }
    widget_set_prop_int(view, "yslidable", 1);
    widget_set_prop_int(view, "xslidable", 0);
    return view;
}

/* ringnav_playing, before the visualizer, which stays last: Description and Chapters follow the
 * art, lyrics and info pages, so their places (1 the lyrics, 2 the info) stay. Either page that
 * can't be made leaves both out. */
void podcast_attach(void *win) {
    void *slide = widget_lookup(win, "slide_view", 1),
         *lrc = slide ? widget_lookup(slide, "view_lrc", 1) : 0;
    if (!slide) return;
    int top = lrc ? I(lrc, W_Y) : 0,
        w = I(slide, W_W) > 0 && I(slide, W_W) <= 375 ? I(slide, W_W) : 375;
    int at = (int)widget_count_children(slide);
    void *desc = page(slide, top, w),
         *text = desc ? label_create(desc, PAGE_X, 0, w - 2 * PAGE_X, PAGE_H) : 0,
         *ch = text ? page(slide, top, w) : 0;
    if (!ch) {
        if (desc) widget_destroy(P(desc, W_PARENT));
        return;
    }
    widget_use_style(text, "s_label_white20c");
    widget_set_prop_int(text, "style:normal:font_size", 16);
    widget_set_prop_int(text, "line_wrap", 1);
    widget_set_prop_int(text, "word_wrap", 1);
    pc.win = win;
    pc.slide = slide;
    pc.desc_at = at;
    pc.desc_view = desc;
    pc.desc_text = text;
    pc.ch_view = ch;
    pc.shown_desc = pc.shown_rows = ~0u;
    widget_on(win, EVT_DESTROY, pages_gone, win);
    pc.polled = 0; /* podcast_poll fills them on its next pass */
}

static void desc_show(void) {
    void *t = pc.desc_text;
    int w = I(pc.desc_view, W_W) - 2 * PAGE_X;
    widget_set_text_utf8(t, pc.desc ? pc.desc : "No description");
    widget_set_prop_str(t, "style:normal:text_align_h", pc.desc ? "left" : "center");
    widget_set_prop_str(t, "style:normal:text_align_v", pc.desc ? "top" : "middle");
    widget_set_prop_int(t, "style:normal:text_color", (int)(pc.desc ? 0xffffffffu : GREY));
    widget_move_resize(t, PAGE_X, 0, w, PAGE_H);
    if (pc.desc) label_resize_to_content(t, (unsigned)w, (unsigned)w, PAGE_H, 0x7fff);
    widget_set_prop_int(pc.desc_view, "virtual_h", I(t, W_H) + (pc.desc ? PAGE_X : 0));
    scroll_view_set_offset(pc.desc_view, 0, 0);
}

static int chapter_tap(void *ctx, void *event) {
    int i = (int)(long)ctx;
    void *r = spoken_now();
    if (r && listen_key(r) == pc.track && i < pc.count) np_seek(pc.rows[i].sec);
    return 0; /* a STOP would keep the slide_view's grab (rate_fav_click) */
}

static void rows_show(void) {
    void *view = pc.ch_view;
    int w = I(view, W_W), n = pc.count < ROWS_MAX ? pc.count : ROWS_MAX;
    widget_destroy_children(view);
    pc.lit = -1;
    pc.shown_count = n;
    if (!n) {
        void *none =
            label(view, PAGE_X, 0, w - 2 * PAGE_X, PAGE_H, "s_scrlabel_white20c", 16, GREY);
        widget_set_text_utf8(none, pc.state == CH_DONE ? "No chapters" : "Reading chapters…");
        widget_set_prop_int(view, "virtual_h", PAGE_H);
        return;
    }
    for (int i = 0; i < n; ++i) {
        chapter *c = &pc.rows[i];
        char at[16];
        void *item = list_item_create(view, 0, i * ROW_H, w, ROW_H);
        widget_use_style(item, "s_listitem_black");
        void *title =
            label(item, PAGE_X, 0, w - 2 * PAGE_X - TIME_W, ROW_H, "s_scrlabel_white20l", 16, GREY);
        widget_set_text_utf8(title, c->title);
        if (c->sec >= 3600)
            tk_snprintf(at, sizeof at, "%d:%02d:%02d", c->sec / 3600, c->sec / 60 % 60,
                        c->sec % 60);
        else
            tk_snprintf(at, sizeof at, "%d:%02d", c->sec / 60, c->sec % 60);
        void *t =
            label(item, w - PAGE_X - TIME_W, 0, TIME_W, ROW_H, "s_scrlabel_white20r", 14, GREY);
        widget_set_text_utf8(t, at);
        widget_on(item, EVT_CLICK, chapter_tap, (void *)(long)i);
    }
    widget_set_prop_int(view, "virtual_h", n * ROW_H);
}

/* The chapter playing in white; off the page, the list follows it. */
static void rows_light(void) {
    int sec = np_second(), i = pc.shown_count && sec >= 0 ? chapter_at(sec) : -1;
    if (i >= pc.shown_count) i = -1;
    if (i == pc.lit) return;
    for (int k = 0; k < 2; ++k) {
        int at = k ? i : pc.lit;
        void *item = at >= 0 ? widget_get_child(pc.ch_view, (unsigned)at) : 0;
        if (item) {
            widget_set_prop_int(widget_get_child(item, 0), "style:normal:text_color",
                                (int)(k ? 0xffffffffu : GREY));
            widget_invalidate_force(item, 0);
        }
    }
    pc.lit = i;
    if (i >= 0 && widget_get_prop_int(pc.slide, "value", 0) != pc.desc_at + 1) {
        int max = pc.shown_count * ROW_H - PAGE_H, y = i * ROW_H;
        scroll_view_set_offset(pc.ch_view, 0, y < max ? y : max > 0 ? max : 0);
    }
}

static void pages_poll(void *r) {
    if (!pc.win) return;
    if (pc.shown_desc != pc.track) {
        pc.shown_desc = pc.track;
        desc_load(r);
        desc_show();
    }
    unsigned rows = pc.track ^ (unsigned)pc.state;
    if (pc.shown_rows != rows) {
        pc.shown_rows = rows;
        rows_show();
    }
    if (g_backlight_status) rows_light();
}

/* Every UI pass (ringnav_sleep): the chapters' reader, then every POLL_MS the track: another one's
 * description and, once a spoken one has played CHAPTER_DELAY_MS, its chapters; and the pages. */
void podcast_poll(void) {
    if (pc.reading && chapters_poll(pc.reading, time_now_ms())) {
        chapters *c = pc.reading;
        pc.reading = 0;
        if (pc.read_for == pc.track && pc.state == CH_READING) {
            int n = c->failed ? 0 : c->count;
            pc.rows = n ? calloc((unsigned)n, sizeof *pc.rows) : 0;
            pc.count = pc.rows ? n : 0;
            for (int i = 0; i < pc.count; ++i) {
                pc.rows[i] = c->rows[i];
                if (!*pc.rows[i].title)
                    tk_snprintf(pc.rows[i].title, sizeof pc.rows[i].title, "Chapter %d", i + 1);
            }
            pc.state = CH_DONE;
            int want = pc.want; /* cleared first: a toast's loop runs this poll again */
            pc.want = 0;
            if (want && (unsigned)time_now_ms() - pc.want_at < CHAPTER_WAIT_MS && spoken_now())
                chapter_jump(want);
        }
        free(c);
    }
    unsigned now = (unsigned)time_now_ms(), at, n;
    if (now - pc.polled < POLL_MS && pc.polled) return;
    pc.polled = now;
    void *r = queue_now(&at, &n);
    track_check(r, now);
    /* read for Now Playing's page once the track has played a moment, or at once for a hold */
    if (r && pc.state == CH_NONE && !pc.reading &&
        ((pc.win && now - pc.since >= CHAPTER_DELAY_MS) || pc.want))
        read_start(r);
    pages_poll(r);
}
