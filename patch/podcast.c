/* Podcasts and audiobooks (docs/internals.md#podcasts-on-now-playing): on a spoken track the side
 * buttons skip back and forward, and held they jump between its chapters. Now Playing (Q2 Podcast's
 * page, tools/ipod.py playing_stock) has the same as buttons beside the art; its star stars a
 * Pocket episode in Pocket Casts (q2-pocket); its Description page follows the art, then Chapters
 * and the Queue, and Transcript last, Chapters and Transcript only while the episode has them. The
 * wheel scrolls Description and Transcript a pixel at a time and the Queue a row at a time. A track
 * played from Folder takes the title the library or its file has. A finished episode is noted for
 * q2-pocket, and one set to Delete When Finished goes from the card. */
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
#define PAGE_X 16 /* tools/ipod.py NP_MARGIN */
#define ROW_H 44
#define TIME_W 76
#define ROWS_MAX 200  /* the Chapters page's rows; a hold still reaches every chapter */
#define QUEUE_ROWS 60 /* the Queue page's Up Next rows (playback.c's PEND_LIMIT is 64) */
/* A Queue row (the user's, 2026-11-15, as Library's Episodes): the show's art, the episode's title
 * over its show, the arrows at its end */
#define Q_ROW_H 56
#define Q_ART 44
#define Q_TEXT_X (PAGE_X + Q_ART + 10)
#define ARROW_W 40
#define POLL_MS 500
#define DESC_TOP 12       /* tools/ipod.py PC_DESC_TOP: Description's and Transcript's first line */
#define TEXT_ROW_PX 20    /* a wheel tick's worth on Description and Transcript (the lyrics' row) */
#define GLIDE_EASE_MS 50  /* the wheel's glide: navigation.c's LYRIC_EASE_MS */
#define ARRIVE_MS 400     /* Description held at its top this long after it comes in front */
#define HOLD_TOUCH_MS 300 /* a finger on Transcript this long after it came in holds it */
/* Transcript: q2-pocket's timed .lrc beside the episode, read this far, as paragraphs of at least
 * TR_PARA_MIN_S, ending on a sentence's end, or TR_PARA_MAX_S at most; laid out TR_BUILD_PASS a UI
 * pass so a long one never stalls Now Playing; a missing one looked for again every TR_LOOK_MS. */
#define TR_MAX (256 * 1024)
#define TR_PARAS 600
#define TR_PARA_MIN_S 45
#define TR_PARA_MAX_S 90
#define TR_BUILD_PASS 6
#define TR_GAP 10
#define TR_LOOK_MS 5000
/* Played: this near the end when it stops or another track starts (the resume ring's RESUME_EDGE_S,
 * which forgets the place there), a track at least FINISH_MIN_S long, not a CUE one. */
#define FINISH_EDGE_S RESUME_EDGE_S
#define FINISH_MIN_S 60
/* "F\t<path>" a finished episode, "D\t<path>" a file deleted, one a line: q2-pocket takes them
 * (marks it played in Pocket Casts, out of Up Next; never fetches a deleted one by itself) and
 * removes the file. Nothing is added while it is DONE_MAX long (no q2-pocket to take it). */
#define DONE_FILE PEQ_ROOT "/mnt/data/ringnav-done"
#define DONE_MAX (64 * 1024)
/* Delete When Finished (the user's, 2026-11-15): the episodes set so, by path key, newest first */
#define DELFIN_FILE PEQ_ROOT "/mnt/data/ringnav-delfinish"
#define DELFIN_N 64
#define EP_CACHE 64       /* episodes the library was asked about, kept */
#define EP_RETRY_MS 30000 /* one it hasn't got (not scanned yet) asked again after this */
#define PODCASTS "/mnt/mmc/Podcasts/"
#define GREY 0xffaaaaaau
enum { CH_NONE, CH_READING, CH_DONE };
enum { B_BACK, B_FWD, B_CHAPTER_BACK, B_CHAPTER_FWD, B_N };
static const char *const button_names[B_N] = { "img_pc_back", "img_pc_fwd", "img_pc_chapprev",
                                               "img_pc_chapnext" };

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
    /* q2-pocket's starred list as last read (stat words: mtime, its ns, size), a star asked for,
     * and the track and star My Fav was last put in step with */
    char *starred;
    unsigned stamp[3], ask_key, ask_at, stat_at, matched_key;
    int ask_on, matched_on, painted;
    /* Now Playing's pages (tools/ipod.py): the window, its slide_view and dots, the hidden view
     * Chapters and Transcript wait in, and each page with its scroll view: Description's (and its
     * text), Chapters', the Queue's and Transcript's */
    void *win, *slide, *dots, *shelf, *desc_page, *desc_view, *desc_text, *ch_page, *ch_view;
    void *q_page, *q_view, *tr_page, *tr_view;
    void *bar, *line[2]; /* the progress bar, and the title and show under the art, which span it */
    unsigned q_sig, q_n; /* the queue as the page shows it (q_sign), its Up Next rows */
    unsigned q_idx[QUEUE_ROWS]; /* their queue indices, in play order */
    unsigned q_again;           /* when to build it again: a row the library hadn't got (0 none) */
    char *desc;
    int lit, shown_count;
    unsigned shown_desc, shown_rows; /* the track each page shows, ~0 to build again */
    char *named;                     /* the title title_fill made from a file's name */
    int skip[2];                     /* the skip lengths, back and forward, once read (seconds) */
    void *front;                     /* the page in front at the last look, since front_at */
    unsigned front_at;
    int desc_moved; /* Description scrolled since it came in front */
} pc;

/* The local track playing or paused, else 0. */
static void *local_now(void) {
    unsigned at, n, type = (unsigned)MCL(MCL_TYPE), s = (unsigned)mclGetPlayStatus();
    if (media_owner() != 1 || (s != 2 && s != 3)) return 0;
    if (type != 1 && (type & 0xf000) != 0xf000) return 0; /* a stream (rate_track) */
    return queue_now(&at, &n);
}

/* The local track playing or paused when it is a spoken one (navigation.c spoken), else 0. */
static void *spoken_now(void) {
    void *r = local_now();
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

/* The skip lengths (the user's, 2026-11-12): each of the two chosen from SKIPS in the ⋯ menu, the
 * Play/Pause hold menu and Playback settings, saved as Q2POD SKIPBACK and SKIPFWD (seconds);
 * SKIP_BACK_S and SKIP_FWD_S until then. Now Playing's buttons show theirs (play_back<s>,
 * play_fwd<s>: tools/ipod.py PC_ICONS). */
static const int SKIPS[] = { 5, 10, 15, 30, 45, 60 };
#define SKIP_N (int)(sizeof SKIPS / sizeof *SKIPS)
static const char *const SKIP_KEY[2] = { "SKIPBACK", "SKIPFWD" };

int skip_count(void) { return SKIP_N; }
int skip_at(int i) { return SKIPS[i < 0 ? 0 : i >= SKIP_N ? SKIP_N - 1 : i]; }

/* A button's length (fwd 0 back, 1 forward), in seconds. */
int skip_seconds(int fwd) {
    fwd = !!fwd;
    if (!pc.skip[fwd]) {
        int def = fwd ? SKIP_FWD_S : SKIP_BACK_S,
            s = config_value("Q2POD", SKIP_KEY[fwd], 100, def), i = 0;
        while (i < SKIP_N && SKIPS[i] != s) ++i;
        pc.skip[fwd] = i < SKIP_N ? s : def;
    }
    return pc.skip[fwd];
}

/* Its place in SKIPS. */
int skip_index(int fwd) {
    int s = skip_seconds(fwd), i = 0;
    while (i < SKIP_N - 1 && SKIPS[i] != s) ++i;
    return i;
}

/* Now Playing's two buttons, each its length's glyph. */
static void skip_icons(void) {
    char img[24];
    for (int fwd = 0; pc.win && fwd < 2; ++fwd) {
        void *b = widget_lookup(pc.win, fwd ? "img_pc_fwd" : "img_pc_back", 1);
        tk_snprintf(img, sizeof img, fwd ? "play_fwd%d" : "play_back%d", skip_seconds(fwd));
        if (b && tk_strcmp(widget_get_prop_str(b, "image", ""), img)) image_base_set_image(b, img);
    }
}

/* A button's length set to s seconds, kept. */
void skip_set(int fwd, int s) {
    fwd = !!fwd;
    pc.skip[fwd] = s;
    write_int_config(s, "Q2POD", SKIP_KEY[fwd]);
    skip_icons();
}

/* A button's length on to the next in SKIPS (the last back to the first); returns it. */
int skip_cycle(int fwd) {
    skip_set(fwd, SKIPS[(skip_index(fwd) + 1) % SKIP_N]);
    return skip_seconds(fwd);
}

/* A row's text for a button's length: "Skip Back: 5 s". */
void skip_label(int fwd, char *out, unsigned size) {
    tk_snprintf(out, size, "%s: %d s", fwd ? "Skip Forward" : "Skip Back", skip_seconds(fwd));
}

/* A press: back or on the buttons' lengths (skip_seconds), within the track. */
static void skip(int dir) {
    int sec = np_second(), total = 0, now = -1;
    if (sec < 0 || player_playtime_and_length(&now, &total) < 0) return;
    int to = sec + (dir > 0 ? skip_seconds(1) : -skip_seconds(0));
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

/* Now Playing's buttons beside the art: skip_seconds back and on in any local track; the previous
 * or next chapter in a spoken one, as the side buttons' holds, else the previous or next track.
 * Returns 0: a STOP would keep the slide_view's grab (rate_fav_click). */
static int button_tap(void *ctx, void *event) {
    int b = (int)(long)ctx;
    void *r = local_now();
    if (!r) return 0;
    if (b == B_BACK || b == B_FWD)
        skip(b == B_FWD ? 1 : -1);
    else if (spoken(P(r, REC_PATH)))
        hold(b == B_CHAPTER_FWD ? 1 : -1);
    else
        b == B_CHAPTER_FWD ? player_next_music() : player_prev_music();
    return 0;
}

/* q2-pocket's starred list, read again when it changes; 0 (none) while q2-pocket isn't signed in.
 * Its stat is looked at once a POLL_MS at most. */
static void starred_load(void) {
    unsigned s[36] = { 0 }, now = (unsigned)time_now_ms();
    if (pc.stat_at && now - pc.stat_at < POLL_MS) return;
    pc.stat_at = now | 1;
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

/* Now Playing's star (ratings.c rate_tick and rate_bind): the build draws My Fav's heart as a star
 * (tools/build.py), so it is one image for both. On a starrable track it shows Pocket Casts' star,
 * and My Fav is put in step with it, once a change: stock's own repaint of the heart (as Now
 * Playing comes back from Quick Settings) then shows the same star, never My Fav's other state. */
void podcast_heart(void *fav) {
    const char *path = fav ? starrable() : 0, *img;
    if (path) {
        int on = starred(path);
        unsigned key = fnv(FNV_SEED, (const unsigned char *)path);
        if ((key != pc.matched_key || on != pc.matched_on) && rate_fav_match(on))
            pc.matched_key = key, pc.matched_on = on;
        img = on ? "play_fav" : "play_unfav";
        pc.painted = 1;
    } else if (fav && pc.painted) { /* another track after a Pocket episode: My Fav's star again */
        unsigned at, n;
        void *r = queue_now(&at, &n);
        img = r && checkFavExist(r) ? "play_fav" : "play_unfav";
        pc.painted = 0, pc.matched_key = 0;
    } else
        return;
    if (tk_strcmp(widget_get_prop_str(fav, "image", ""), img)) image_base_set_image(fav, img);
}

/* The star's tap (ratings.c rate_fav_click): stars or unstars a starrable track in Pocket Casts,
 * through q2-pocket, instead of My Fav, which follows it (podcast_heart). Returns 0 for any other
 * track. */
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

/* Library → Starred (navigation.c ringnav_localclass, before My Fav loads): the downloaded episodes
 * Pocket Casts has starred join My Fav (batch-select's Add, which skips those in it already), so
 * the list holds every star, Pocket's and the Q2's own. Only added: a star taken away on another
 * device leaves My Fav when its episode next plays (podcast_heart). */
void podcast_starred_sync(void) {
    pc.stat_at = 0; /* the list as it is now */
    starred_load();
    if (!pc.starred || !pc.starred[1]) return;
    int k;
    void *songs = staged(songs_unsorted, 0, &k), *add = songs ? _create_deque("stSongInfo") : 0;
    if (!add) {
        if (songs) deque_destroy(songs);
        return;
    }
    deque_init(add);
    char line[520];
    for (unsigned i = 0, n = deque_size(songs); i < n; ++i) {
        void *r = deque_at(songs, i);
        const char *path = P(r, REC_PATH);
        if (!path || !tk_str_start_with(path, "/mnt/mmc/Podcasts/") || strlen(path) > 512) continue;
        tk_snprintf(line, sizeof line, "\n%s\n", path);
        if (strstr(pc.starred, line)) _deque_push_back(add, r);
    }
    unsigned n = deque_size(add);
    if (n) {
        batch_init_selectrecord((int)n);
        for (unsigned i = 0; i < n; i++) batch_set_selectitem((int)i);
        batch_add_file(0xf001, 0xf00a, add, P(p_vector_select_record, 0), 0);
        ratings_copy_due();
    }
    deque_destroy(add);
    deque_destroy(songs);
}

/* name, a file's name, as a title in out: without its extension, a leading date ("2024-10-07 - ",
 * "_" or " " after it: Pocket's and Podcasts' names) and a trailing " [id]" (Pocket's, for a name
 * two episodes share); underscores as spaces in a name without any. */
static void name_title(const char *name, char *out, unsigned size) {
    const char *dot = strrchr(name, '.');
    int n = dot && dot != name ? (int)(dot - name) : (int)strlen(name), i = 0;
    while (i < 10 && i < n && ((name[i] >= '0' && name[i] <= '9') || name[i] == '-')) ++i;
    if (i == 10 && name[4] == '-' && name[7] == '-' && n > 13) {
        name += 10, n -= 10;
        if (n > 3 && !strncmp(name, " - ", 3))
            name += 3, n -= 3;
        else if (*name == '_' || *name == ' ')
            ++name, --n;
    }
    if (n > 3 && name[n - 1] == ']') {
        int k = n - 2;
        while (k > 0 && name[k] != '[') --k;
        if (k > 1 && name[k - 1] == ' ') n = k - 1;
    }
    if ((unsigned)n >= size) n = (int)size - 1;
    memcpy(out, name, (unsigned)n);
    out[n] = 0;
    int spaced = 0;
    for (char *p = out; *p; ++p) spaced |= *p == ' ';
    for (char *p = out; *p && !spaced; ++p)
        if (*p == '_') *p = ' ';
}

/* A record's text field set to s, a copy. The old one is left, not freed: another thread can be
 * reading the record (a few bytes a track played from Folder). Only on the Q2: test/fw's 64-bit
 * build lays stock's records out in their 4-byte slots, which a pointer there would overrun. */
static char *rec_set(void *r, int field, const char *s) {
#if __SIZEOF_POINTER__ == 4
    char *copy = strdup(s);
    if (copy) *(char **)((char *)r + field) = copy;
    return copy;
#else
    (void)r, (void)field, (void)s;
    return 0;
#endif
}

/* Whether title is only the file's name, with or without its extension: what stock's folder
 * listing gives a track it hasn't read (each episode Pocket plays from its Up Next playlist), not
 * a title of its own. */
static int name_only(const char *title, const char *name) {
    if (!title || !name) return 0;
    const char *dot = strrchr(name, '.');
    unsigned n = dot && dot != name ? (unsigned)(dot - name) : strlen(name);
    return !strcmp(title, name) || (strlen(title) == n && !strncmp(title, name, n));
}

/* Whether label w shows s already (a text too long to compare counts as shown, so a long title
 * isn't set again every pass, which would start its scroll over). */
static int shows(void *w, const char *s) {
    char now[512] = "";
    if (strlen(s) >= sizeof now - 8) return 1;
    widget_get_text_utf8(w, now, sizeof now);
    return !strcmp(now, s);
}

/* The library's episodes (the user's, 2026-11-15: rows and Now Playing name an episode by its
 * title, as Library's Episodes do, never its file): what songtable has for a path, its title, show
 * (album) and artist, asked once and kept (EP_CACHE, calloc'd at the first look). One it hasn't got
 * (a download not scanned yet) is asked again after EP_RETRY_MS. ep_db: the database while a page
 * asks about its rows (ep_batch), else each look opens it. */
typedef struct {
    unsigned key, at;
    char *title, *show, *artist,
        *album_artist; /* album_artist: as Artists and Cover Flow's albums have it */
} ep_t;
static ep_t *eps;
static unsigned ep_next;
static void *ep_db;
#define DATABASE "/mnt/data/database.db"

/* s in out, each ' doubled, for an SQL literal; 0 when it doesn't fit. */
static int sql_quoted(char *out, unsigned size, const char *s) {
    unsigned n = 0;
    for (; *s; ++s) {
        if (n + 3 >= size) return 0;
        if (*s == '\'') out[n++] = '\'';
        out[n++] = *s;
    }
    out[n] = 0;
    return 1;
}

static int ep_row(void *ctx, int n, char **v, char **c) {
    ep_t *e = ctx;
    char **to[4] = { &e->title, &e->show, &e->artist, &e->album_artist };
    for (int i = 0; i < 4 && i < n; ++i) *to[i] = v[i] && *v[i] ? strdup(v[i]) : 0;
    return 1; /* the first row is enough */
}

static void ep_batch(int open) {
    if (open && !ep_db && sqlite3_open(DATABASE, &ep_db)) {
        if (ep_db) sqlite3_close(ep_db);
        ep_db = 0;
    } else if (!open && ep_db) {
        sqlite3_close(ep_db);
        ep_db = 0;
    }
}

/* r's episode as songtable has it, by its name and path (stock's own key for a row, the name
 * indexed), or 0. */
static ep_t *ep_of(void *r) {
    const char *path = r ? P(r, REC_PATH) : 0, *name = r ? P(r, REC_NAME) : 0;
    if (!path || !name || (!eps && !(eps = calloc(EP_CACHE, sizeof *eps)))) return 0;
    unsigned key = fnv(FNV_SEED, (const unsigned char *)path) | 1, now = (unsigned)time_now_ms(),
             i = 0;
    while (i < EP_CACHE && eps[i].key != key) ++i;
    if (i < EP_CACHE && (eps[i].title || eps[i].show || now - eps[i].at < EP_RETRY_MS))
        return &eps[i];
    if (i == EP_CACHE) i = ep_next++ % EP_CACHE;
    ep_t *e = &eps[i];
    free(e->title), free(e->show), free(e->artist), free(e->album_artist);
    memset(e, 0, sizeof *e);
    e->key = key, e->at = now;
    char qn[320], qp[720], sql[1200];
    void *db = ep_db;
    if (!sql_quoted(qn, sizeof qn, name) || !sql_quoted(qp, sizeof qp, path)) return e;
    if (!db && sqlite3_open(DATABASE, &db)) {
        if (db) sqlite3_close(db);
        return e;
    }
    tk_snprintf(sql, sizeof sql,
                "select title,album,songer,ifnull(nullif(albumsonger,''),songer) from songtable"
                " where filename='%s' and fileurl='%s' limit 1",
                qn, qp);
    sqlite3_exec(db, sql, ep_row, e, 0);
    if (db != ep_db) sqlite3_close(db);
    return e;
}

/* r's title as a row or Now Playing names an episode: the library's (its tag as the scan read it),
 * else its own, else its file's name tidied (name_title). */
static const char *ep_title(void *r, char *buf, unsigned size) {
    ep_t *e = ep_of(r);
    const char *own = P(r, REC_TITLE), *name = P(r, REC_NAME);
    if (e && e->title && !name_only(e->title, name)) return e->title;
    if (own && *own && !name_only(own, name)) return own;
    name_title(name ? name : "", buf, size);
    return buf;
}

/* r's show: the library's album, else its own, else its folder's name. */
static const char *ep_show(void *r, char *buf, unsigned size) {
    ep_t *e = ep_of(r);
    const char *own = P(r, REC_ALBUM), *path = P(r, REC_PATH),
               *slash = path ? strrchr(path, '/') : 0;
    if (e && e->show) return e->show;
    if (own && *own) return own;
    buf[0] = 0;
    if (slash && slash > path) {
        const char *d = slash - 1;
        while (d > path && *d != '/') --d;
        if (*d == '/') ++d;
        tk_snprintf(buf, size, "%.*s", (int)(slash - d), d);
    }
    return buf;
}

/* r's art for a row: Cover Flow's cover of its show (coverflow.c keys it by its album row's artist
 * and album: the album artist's or the artist's, as the library has them), else stock's disc. */
extern const char *coverflow_art_url(const char *artist, const char *album, char *out);
static const char *ep_art(void *r, char *out) {
    ep_t *e = ep_of(r);
    const char *url = e && e->show ? coverflow_art_url(e->album_artist, e->show, out) : 0;
    if (!url && e && e->show) url = coverflow_art_url(e->artist, e->show, out);
    if (!url) url = coverflow_art_url(P(r, REC_ARTIST), P(r, REC_ALBUM), out);
    return url ? url : "default_album_small";
}

/* A local track without a title of its own (a song played from Folder, as every Pocket episode is,
 * or an episode or a book whose title is only its file's name): the title the player reads from its
 * file; else, for a spoken one, the library's (ep_of: what Library's Episodes show), else its name
 * as a title (name_title). Without an artist, a spoken one's is its show (the album its file
 * names), another's its file's. Now Playing, the lock screen and View Queue then name the episode,
 * not its file. Every UI pass: it is cheap (the library asked once a track), and the player reads a
 * file's tags just as it starts. */
static void title_fill(void) {
    unsigned at, n;
    void *r = queue_now(&at, &n);
    if (!r || I(r, REC_TYPE) != 8 || !P(r, REC_PATH)) return;
    const char *own = P(r, REC_TITLE), *artist = P(r, REC_ARTIST), *name = P(r, REC_NAME);
    int named = pc.named && own == pc.named, voice = spoken(P(r, REC_PATH)), changed = 0;
    if (!own || !*own || named || (voice && name_only(own, name))) {
        const char *tag = now_tag(r, REC_TITLE);
        ep_t *e = voice ? ep_of(r) : 0;
        char buf[256];
        if (tag != own && tag && *tag && !name_only(tag, name)) { /* the player read it */
            changed = rec_set(r, REC_TITLE, tag) != 0;
            pc.named = 0;
        } else if (e && e->title && !name_only(e->title, name)) { /* the library's */
            changed = rec_set(r, REC_TITLE, e->title) != 0;
            pc.named = 0;
        } else if (!named && voice && name) {
            name_title(name, buf, sizeof buf);
            changed = *buf && (pc.named = rec_set(r, REC_TITLE, buf)) != 0;
        }
    }
    if (!artist || !*artist) {
        const char *album = now_tag(r, REC_ALBUM), *tag = now_tag(r, REC_ARTIST),
                   *show = voice && album != P(r, REC_ALBUM) && album && *album ? album
                           : tag != artist && tag && *tag                       ? tag
                                                                                : 0;
        if (show && rec_set(r, REC_ARTIST, show)) changed = 1;
    }
    /* an episode's labels kept to its record each pass: stock writes them again as it reads the
     * file (its name, for one it read no title from) after they were set here */
    if (!pc.win || (!changed && !voice)) return;
    void *t = widget_lookup(pc.win, "scrlabel_title", 1),
         *a = widget_lookup(pc.win, "scrlabel_artist", 1);
    const char *title = P(r, REC_TITLE);
    artist = P(r, REC_ARTIST);
    if (t && title && *title && (changed || !shows(t, title))) widget_set_text_utf8(t, title);
    if (a && artist && *artist && (changed || !shows(a, artist))) widget_set_text_utf8(a, artist);
}

/* ---- Finished episodes and deletes ---- */

/* A line in DONE_FILE for q2-pocket: kind 'F' (finished) or 'D' (deleted), an episode's path under
 * Podcasts. */
static void done_note(char kind, const char *path) {
    unsigned s[36] = { 0 };
    char line[600];
    if (!tk_str_start_with(path, PODCASTS) || (!__xstat(3, DONE_FILE, s) && s[12] >= DONE_MAX))
        return;
    int n = tk_snprintf(line, sizeof line, "%c\t%s\n", kind, path);
    void *f = n > 0 && n < (int)sizeof line ? fopen(DONE_FILE, "a") : 0;
    if (!f) return;
    fwrite(line, 1, (unsigned)n, f);
    fclose(f);
}

static unsigned delfin[DELFIN_N];
static int delfin_read;
static unsigned path_key(const char *path) {
    return fnv(FNV_SEED, (const unsigned char *)path) | 1;
}
static int delfin_at(unsigned key) {
    if (!delfin_read) BLOB_IO(DELFIN_FILE, delfin, 0), delfin_read = 1;
    int i = 0;
    while (i < DELFIN_N && delfin[i] != key) ++i;
    return i < DELFIN_N ? i : -1;
}

/* Delete When Finished, r's: whether it is set. */
int podcast_delfin(void *r) {
    const char *path = r ? P(r, REC_PATH) : 0;
    return path && delfin_at(path_key(path)) >= 0;
}

/* Delete When Finished set (on) or not on r, kept: the newest first, the oldest dropped if full. */
void podcast_delfin_set(void *r, int on) {
    const char *path = r ? P(r, REC_PATH) : 0;
    if (!path) return;
    unsigned key = path_key(path);
    int i = delfin_at(key);
    if (on == (i >= 0)) return;
    if (on) {
        for (i = DELFIN_N - 1; i > 0; --i) delfin[i] = delfin[i - 1];
        delfin[0] = key;
    } else {
        for (; i < DELFIN_N - 1; ++i) delfin[i] = delfin[i + 1];
        delfin[DELFIN_N - 1] = 0;
    }
    BLOB_IO(DELFIN_FILE, delfin, 1);
}

/* The transcript and description q2-pocket writes beside an episode go with it. */
static void sidecars_gone(const char *path) {
    const char *slash = strrchr(path, '/'), *dot = slash ? strrchr(slash, '.') : 0;
    char f[720];
    if (!slash) return;
    int base = (int)((dot && dot > slash + 1 ? dot : slash + strlen(slash)) - path),
        dir = (int)(slash - path);
    tk_snprintf(f, sizeof f, "%.*s.lrc", base, path);
    unlink(f);
    tk_snprintf(f, sizeof f, "%.*s.LRC", base, path);
    unlink(f);
    tk_snprintf(f, sizeof f, "%.*s/.description/%.*s.txt", dir, path, base - dir - 1, slash + 1);
    unlink(f);
}

/* path's file off the card with its sidecars, out of the library (rec, a row naming it: stock's
 * delete by its name, path and CUE times, Cover Flow's lists then taken again) and out of the
 * queue, every row of it but the playing one; q2-pocket told. Returns 0 when the file stays. */
extern int coverflow_delete_song(void *a, void *b);
static int file_gone(const char *path, void *rec) {
    char own[600];
    if (tk_snprintf(own, sizeof own, "%s", path) >= (int)sizeof own) return 0;
    if (unlink(own) && !access(own, 0)) return 0;
    sidecars_gone(own);
    if (rec) coverflow_delete_song(rec, 0); /* before the queue's copy of it can go */
    void *q = P(mcl_pdeqplaylist, 0);
    for (unsigned i = q ? deque_size(q) : 0; i-- > 0;) {
        const char *p = P(deque_at(q, i), REC_PATH);
        if (p && !strcmp(p, own)) playback_drop(i);
    }
    done_note('D', own);
    return 1;
}

/* The queue's row naming path, the playing one only when no other does, or 0. */
static void *queue_row_of(const char *path) {
    void *q = P(mcl_pdeqplaylist, 0), *found = 0;
    unsigned pos = (unsigned)MCL(MCL_POS);
    for (unsigned i = 0, n = q ? deque_size(q) : 0; i < n; ++i) {
        const char *p = P(deque_at(q, i), REC_PATH);
        if (p && !strcmp(p, path) && (!found || i != pos)) found = deque_at(q, i);
    }
    return found;
}

/* An episode played to its end (finish_poll): noted for q2-pocket, and gone when Delete When
 * Finished was set on it. The one still at the queue's position (the queue ended on it) keeps its
 * row, stopped there; its file and library row go. */
static void finished(const char *path) {
    char own[600];
    if (tk_snprintf(own, sizeof own, "%s", path) >= (int)sizeof own) return;
    done_note('F', own);
    unsigned key = path_key(own);
    int i = delfin_at(key);
    if (i < 0) return;
    for (; i < DELFIN_N - 1; ++i) delfin[i] = delfin[i + 1];
    delfin[DELFIN_N - 1] = 0;
    BLOB_IO(DELFIN_FILE, delfin, 1);
    unsigned at, n;
    void *now = queue_now(&at, &n), *rec = queue_row_of(own);
    int gone;
    if (rec && rec == now) {
        gone = !unlink(own) || access(own, 0);
        if (gone) sidecars_gone(own), coverflow_delete_song(rec, 0), done_note('D', own);
    } else
        gone = file_gone(own, rec);
    if (g_backlight_status)
        toast(gone ? "Finished episode deleted" : "Could not delete the episode");
}

/* The playing track as last seen: its path, key, place and length; done once it counted as
 * finished (or was played again from earlier). Every POLL_MS: the track before, gone near its end
 * (another track started, or the queue stopped there), was finished. A new track's first second
 * may still be the last one's, so its place counts from the next look. */
static struct {
    char *path;
    unsigned key;
    int sec, total, done;
} fin;
static void finish_poll(void) {
    unsigned at, n;
    void *r = queue_now(&at, &n);
    int sec = -1, total = 0;
    int on = r && media_owner() == 1 && I(r, REC_TYPE) == 8 && P(r, REC_PATH) &&
             !I(r, REC_CUE_START) && !I(r, REC_CUE_END) &&
             player_playtime_and_length(&sec, &total) >= 0 && total > 0 && sec >= 0;
    unsigned key = on ? listen_key(r) : 0;
    if (on && key == fin.key) {
        if (sec < total - FINISH_EDGE_S - 5) fin.done = 0; /* played again from further back */
        fin.sec = sec, fin.total = total;
        return;
    }
    if (fin.path && !fin.done && fin.total >= FINISH_MIN_S &&
        fin.sec >= fin.total - FINISH_EDGE_S) {
        fin.done = 1;
        finished(fin.path);
    }
    if (!on) return;
    free(fin.path);
    fin.path = strdup(P(r, REC_PATH));
    fin.key = key, fin.sec = 0, fin.total = total, fin.done = 0;
}

/* The Play/Pause hold menu's Delete File (navigation.c QA_DELETE) on Folder's song r, the user's
 * (2026-11-15): asked first. The song playing goes as stock's own Delete Song takes it
 * (delete_playqueue_file: its library row, the file, then the next track plays); any other as
 * file_gone. The folder is listed again. */
void podcast_delete(void *r) {
    const char *p = r ? P(r, REC_PATH) : 0;
    char path[600], name[300];
    if (!p || tk_snprintf(path, sizeof path, "%s", p) >= (int)sizeof path) return;
    struct {
        int kind, ms;
        char text[0x400];
    } prompt = { 1, 0, "" };
    tk_snprintf(prompt.text, sizeof prompt.text, "Delete \"%s\"?", ep_title(r, name, sizeof name));
    st.confirm_verb = "Delete"; /* the confirm's second cell, as playlists.c's confirm_over */
    int answer = navigator_to_with_context("dialog/confirminfo_dialog", &prompt);
    st.confirm_verb = (const char *)0;
    st.confirm_win = (void *)0;
    if (answer != 10) return;
    unsigned at, n;
    void *now = queue_now(&at, &n);
    int ok;
    if (now && media_owner() == 1 && !strcmp(P(now, REC_PATH), path) && queue_row_of(path) == now) {
        sidecars_gone(path);
        done_note('D', path);
        delete_playqueue_file(1);
        ok = access(path, 0) != 0;
    } else
        ok = file_gone(path, r);
    toast(ok ? "File deleted" : "Could not delete the file");
    void *top = window_manager_get_top_window(window_manager());
    if (top && !tk_strcmp(widget_get_prop_str(top, "name", ""), "folder_page")) {
        folder_reload_data();
        folder_refresh(top);
    }
}
/* ---- Now Playing's pages ---- */

/* The wheel on Description, Transcript and the Queue (the user's, 2026-11-15, as the lyrics page
 * scrolled before the enhanced lyrics): while one of them is in front, lit and taller than its
 * page, the wheel posts no ticks (wheel_raw) and its raw travel glides it, a tick's worth
 * TEXT_ROW_PX on the text pages, a row on the Queue; anywhere else it keeps its ticks (the
 * volume). raw: podcast.c took the wheel. The glide eases to `to` (1/16 px), as the lyrics' did. */
static struct {
    void *view;
    int to, on, raw, travel;
    unsigned at;
} gl;

/* Transcript as read for track: its paragraphs' starts (seconds) and texts (offsets into text,
 * each nul-ended), n of them; built: those laid out so far, y the next one's top; lit: the one in
 * white; held: the wheel or a finger moved it (until it leaves the front); set_y: the offset
 * podcast.c last gave it. */
static struct {
    unsigned track, looked;
    char *text;
    int *sec, *off, n, built, y, lit, held, set_y;
} tr;

static int pages_gone(void *win, void *event) {
    if (win != pc.win) return 0;
    pc.win = pc.slide = pc.dots = pc.shelf = pc.desc_page = pc.desc_view = pc.desc_text = 0;
    pc.ch_page = pc.ch_view = pc.q_page = pc.q_view = pc.tr_page = pc.tr_view = 0;
    pc.bar = pc.line[0] = pc.line[1] = pc.front = 0;
    if (gl.raw && wheel_raw == WHEEL_RAW) wheel_raw = WHEEL_TICKS;
    gl.raw = gl.on = 0, gl.view = 0;
    tr.built = tr.y = 0, tr.lit = -1; /* the next page lays it out again */
    return 0;
}

/* The dots: a page each. */
static void dots_count(void) {
    if (pc.dots && pc.slide)
        widget_set_prop_int(pc.dots, "max", (int)widget_count_children(pc.slide));
}

/* page into the slide_view at `at` (in, past its end: last) or back to the shelf. The page in front
 * stays in front, or, when it is the one going, the one before it. Not while a finger is on the
 * screen, where a swipe under way would jump. */
static void page_put(void *page, int in, unsigned at) {
    if (!page || !pc.shelf || !pc.slide || (P(page, W_PARENT) == pc.slide) == in || pointer_down())
        return;
    int front = widget_get_prop_int(pc.slide, "value", 0);
    if (in) {
        widget_remove_child(pc.shelf, page);
        widget_add_child(pc.slide, page);
        unsigned last = widget_count_children(pc.slide) - 1;
        if (at < last)
            widget_restack(page, at);
        else
            at = last;
        if (front >= (int)at) widget_set_prop_int(pc.slide, "value", front + 1);
    } else {
        int idx = widget_index_of(page);
        if (front >= idx && front > 0) widget_set_prop_int(pc.slide, "value", front - 1);
        widget_remove_child(pc.slide, page);
        widget_add_child(pc.shelf, page);
    }
    dots_count();
    widget_invalidate_force(P(pc.slide, W_PARENT), 0);
}

/* ringnav_playing: the pages tools/ipod.py made (the art, Description, the Queue and the info in
 * the slide_view; Chapters and Transcript in view_pc_shelf, hidden, until the episode has some:
 * pages_poll), and the buttons beside the art. A page missing, or not where the build puts it,
 * leaves its part out. */
void podcast_attach(void *win) {
    pc.slide = widget_lookup(win, "slide_view", 1);
    if (!pc.slide) return;
    pc.win = win;
    pc.dots = widget_lookup(win, "slide_indicator1", 1);
    pc.shelf = widget_lookup(win, "view_pc_shelf", 1);
    /* stock's hit test skips insensitive widgets, not hidden ones: the hidden shelf over the pages
     * would take every tap and swipe on them (tools/ipod.py also builds it so, off the screen) */
    if (pc.shelf) widget_set_sensitive(pc.shelf, 0);
    pc.desc_page = widget_lookup(win, "view_pc_desc", 1);
    pc.desc_view = widget_lookup(win, "view_pc_desc_scroll", 1);
    pc.desc_text = widget_lookup(win, "label_pc_desc", 1);
    pc.ch_page = widget_lookup(win, "view_pc_chapters", 1);
    pc.ch_view = widget_lookup(win, "view_pc_chapters_scroll", 1);
    pc.q_page = widget_lookup(win, "view_pc_queue", 1);
    pc.q_view = widget_lookup(win, "view_pc_queue_scroll", 1);
    pc.tr_page = widget_lookup(win, "view_pc_transcript", 1);
    pc.tr_view = widget_lookup(win, "view_pc_transcript_scroll", 1);
    if (pc.ch_page && P(pc.ch_page, W_PARENT) != pc.shelf) pc.ch_page = 0; /* not this build's */
    if (pc.tr_page && P(pc.tr_page, W_PARENT) != pc.shelf) pc.tr_page = 0;
    pc.bar = widget_lookup(win, "slider_play", 1);
    pc.q_sig = 0;
    pc.line[0] = widget_lookup(win, "scrlabel_title", 1);
    pc.line[1] = widget_lookup(win, "scrlabel_artist", 1);
    pc.shown_desc = pc.shown_rows = ~0u;
    pc.front = 0;
    tr.built = tr.y = 0, tr.lit = -1, tr.held = 0;
    for (int b = 0; b < B_N; ++b) {
        void *w = widget_lookup(win, button_names[b], 1);
        if (w) widget_on(w, EVT_CLICK, button_tap, (void *)(long)b);
    }
    dots_count();
    skip_icons();
    widget_on(win, EVT_DESTROY, pages_gone, win);
    pc.polled = 0; /* podcast_poll fills them on its next pass */
}

/* A scroll view's animator (a drag's fling) stops, as navigation.c's lyric_still. */
static void view_still(void *v) {
    void *a = P(v, VIEW_ANIMATOR);
    if (!a) return;
    widget_animator_pause(a);
    widget_animator_destroy(a);
    P(v, VIEW_ANIMATOR) = (void *)0;
}

static int view_max(void *v) {
    int m = I(v, VIEW_CONTENT_H) - I(v, W_H);
    return m > 0 ? m : 0;
}

/* v jumps to y, nothing gliding or flung. */
static void view_at(void *v, int y) {
    view_still(v);
    if (gl.view == v) gl.on = 0;
    scroll_view_set_offset(v, I(v, SCROLL_X), y);
    if (v == pc.tr_view) tr.set_y = y;
}

/* v glides to `to` (1/16 px) within its content; glide_step moves it, every UI pass. */
static void glide_to(void *v, int to) {
    int max = view_max(v) * 16;
    view_still(v);
    if (!gl.on || gl.view != v) gl.at = (unsigned)time_now_ms();
    gl.view = v;
    gl.to = to < 0 ? 0 : to > max ? max : to;
    gl.on = 1;
}

/* A dt / (dt + GLIDE_EASE_MS) share of the way left, at least a pixel, so it eases in whatever the
 * loop's pace; only while its page is in front. */
static void glide_step(void *front) {
    if (!gl.on || !gl.view || P(gl.view, W_PARENT) != front) {
        gl.on = 0;
        return;
    }
    unsigned now = (unsigned)time_now_ms();
    int dt = (int)(now - gl.at), y = I(gl.view, SCROLL_Y), to = (gl.to + 8) >> 4;
    gl.at = now;
    if (to == y) {
        gl.on = 0;
        return;
    }
    if (dt > GLIDE_EASE_MS) dt = GLIDE_EASE_MS; /* a late pass: half the way, no leap */
    int step = (to - y) * dt / (dt + GLIDE_EASE_MS);
    y += step ? step : to > y ? 1 : -1;
    scroll_view_set_offset(gl.view, I(gl.view, SCROLL_X), y);
    if (gl.view == pc.tr_view) tr.set_y = y;
}

/* The wheel, every UI pass (wheel_raw's contract: playback.h). */
static void wheel_poll(void *front) {
    void *wm = window_manager(), *view = 0;
    if (pc.win && front && g_backlight_status && window_manager_get_top_window(wm) == pc.win &&
        !window_manager_is_animating(wm))
        view = front == pc.desc_page ? pc.desc_view
               : front == pc.tr_page ? pc.tr_view
               : front == pc.q_page  ? pc.q_view
                                     : 0;
    if (view && !view_max(view)) view = 0; /* nothing to scroll: the wheel keeps the volume */
    if (!view != !gl.raw) {
        __sync_lock_test_and_set(&wheel_travel, 0);
        gl.travel = 0;
        if (view && wheel_raw == WHEEL_TICKS)
            wheel_raw = WHEEL_RAW, gl.raw = 1;
        else if (!view) {
            if (gl.raw && wheel_raw == WHEEL_RAW) wheel_raw = WHEEL_TICKS;
            gl.raw = 0;
        }
    }
    int travel = gl.raw ? __sync_lock_test_and_set(&wheel_travel, 0) : 0;
    if (!travel || !view) return;
    unsigned now = (unsigned)time_now_ms();
    st.last_input = now; /* no key event comes: the screen's timeout as for one */
    reset_poweroptions_timer(1, 1, 1);
    int unit = WHEEL_TICK_UNITS * 50 / wheel_pct(); /* a tick's travel at the Wheel Sensitivity */
    if (view == pc.q_view) { /* a row a tick; x rises anticlockwise: back up the list */
        gl.travel += travel;
        int steps = gl.travel / unit;
        gl.travel -= steps * unit;
        if (!steps) return;
        int from = gl.on && gl.view == view ? (gl.to + 8) >> 4 : I(view, SCROLL_Y);
        glide_to(view, ((from + Q_ROW_H / 2) / Q_ROW_H - steps) * Q_ROW_H * 16);
        return;
    }
    int from = gl.on && gl.view == view ? gl.to : I(view, SCROLL_Y) * 16;
    glide_to(view, from - travel * TEXT_ROW_PX * 16 / unit);
    if (view == pc.tr_view) tr.held = 1;
    if (view == pc.desc_view) pc.desc_moved = 1;
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

/* Description: the text from DESC_TOP down (the user's, 2026-11-15: not so high), left-aligned. */
static void desc_show(void) {
    void *t = pc.desc_text, *view = pc.desc_view;
    int w = I(view, W_W) - 2 * PAGE_X, h = I(view, W_H) - DESC_TOP;
    widget_set_text_utf8(t, pc.desc ? pc.desc : "No description");
    widget_set_prop_str(t, "style:normal:text_align_h", pc.desc ? "left" : "center");
    widget_set_prop_str(t, "style:normal:text_align_v", pc.desc ? "top" : "middle");
    widget_set_prop_int(t, "style:normal:text_color", (int)(pc.desc ? 0xffffffffu : GREY));
    widget_move_resize(t, PAGE_X, pc.desc ? DESC_TOP : 0, w, pc.desc ? h : I(view, W_H));
    if (pc.desc) label_resize_to_content(t, (unsigned)w, (unsigned)w, (unsigned)h, 0x7fff);
    widget_set_prop_int(view, "virtual_h", pc.desc ? DESC_TOP + I(t, W_H) + PAGE_X : I(view, W_H));
    view_at(view, 0);
}

/* Description starts at its top whenever it comes in front (the user's, 2026-11-15: a swipe's
 * drag, or the last visit, left it further down): out of the front it goes back there, and for
 * ARRIVE_MS after it comes in, unless the wheel or a finger has moved it since. */
static void desc_top(void *front, unsigned now) {
    void *v = pc.desc_view;
    if (!v) return;
    if (front == pc.desc_page && pointer_down() && now - pc.front_at > 100) pc.desc_moved = 1;
    if (!I(v, SCROLL_Y) ||
        (front == pc.desc_page && (pc.desc_moved || now - pc.front_at > ARRIVE_MS)))
        return;
    view_at(v, 0);
}

/* ---- Transcript ---- */

static void transcript_drop(void) {
    free(tr.text), free(tr.sec), free(tr.off);
    tr.text = 0, tr.sec = tr.off = 0;
    tr.n = tr.built = tr.y = tr.held = 0, tr.lit = -1;
    if (pc.tr_view) widget_destroy_children(pc.tr_view);
}

/* "[mm:ss.xx]" at s: its seconds, s moved past every tag on the line; -1 for a line without one. */
static int lrc_time(const char **s) {
    const char *p = *s;
    int sec = -1;
    while (*p == '[' && p[1] >= '0' && p[1] <= '9') {
        int m = 0, x = 0;
        ++p;
        while (*p >= '0' && *p <= '9') m = m * 10 + (*p++ - '0');
        if (*p != ':') return -1;
        ++p;
        while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
        while (*p && *p != ']' && *p != '\n') ++p;
        if (*p != ']') return -1;
        ++p;
        if (sec < 0) sec = m * 60 + x;
    }
    *s = p;
    return sec;
}

/* r's transcript: q2-pocket's timed .lrc beside it (a line a few words, as the cleaner wrote it),
 * read as paragraphs of TR_PARA_MIN_S to TR_PARA_MAX_S. None: tr.n 0. */
static void transcript_load(void *r) {
    const char *path = P(r, REC_PATH), *slash = path ? strrchr(path, '/') : 0,
               *dot = slash ? strrchr(slash, '.') : 0;
    char file[620];
    void *f = 0;
    if (!slash) return;
    int base = (int)((dot && dot > slash + 1 ? dot : slash + strlen(slash)) - path);
    for (int k = 0; k < 2 && !f; ++k) {
        tk_snprintf(file, sizeof file, k ? "%.*s.LRC" : "%.*s.lrc", base, path);
        f = fopen(file, "rb");
    }
    char *raw = f ? calloc(1, TR_MAX + 1) : 0;
    int n = raw ? (int)fread(raw, 1, TR_MAX, f) : 0;
    if (f) fclose(f);
    tr.text = n > 0 ? calloc(1, (unsigned)n + 2) : 0;
    tr.sec = tr.text ? calloc(TR_PARAS, sizeof *tr.sec) : 0;
    tr.off = tr.sec ? calloc(TR_PARAS, sizeof *tr.off) : 0;
    if (!tr.off) {
        free(raw);
        transcript_drop();
        return;
    }
    raw[n] = 0;
    int o = 0, start = -1, end_sentence = 0;
    for (const char *line = raw; *line;) {
        const char *next = line, *s = line;
        while (*next && *next != '\n') ++next;
        int sec = lrc_time(&s);
        while (s < next && *s == ' ') ++s;
        const char *e = next;
        while (e > s && (e[-1] == ' ' || e[-1] == '\r')) --e;
        if (sec >= 0 && e > s) {
            int fresh = start < 0 || sec - start >= TR_PARA_MAX_S ||
                        (end_sentence && sec - start >= TR_PARA_MIN_S);
            if (fresh && tr.n == TR_PARAS) break;
            if (fresh) {
                if (start >= 0) tr.text[o++] = 0;
                tr.sec[tr.n] = sec, tr.off[tr.n++] = o, start = sec;
            } else
                tr.text[o++] = ' ';
            memcpy(tr.text + o, s, (unsigned)(e - s));
            o += (int)(e - s);
            char last = e[-1];
            if ((last == '"' || last == '\'' || last == ')') && e - 1 > s) last = e[-2];
            end_sentence = last == '.' || last == '?' || last == '!';
        }
        line = *next ? next + 1 : next;
    }
    free(raw);
    if (!tr.n) transcript_drop();
}

/* TR_BUILD_PASS more paragraphs laid out, each a wrapped label in Description's text, grey. */
static void transcript_build(void) {
    int w = I(pc.tr_view, W_W) - 2 * PAGE_X;
    if (!tr.built) tr.y = DESC_TOP;
    for (int k = 0; k < TR_BUILD_PASS && tr.built < tr.n; ++k, ++tr.built) {
        void *t = label_create(pc.tr_view, PAGE_X, tr.y, w, TEXT_ROW_PX);
        widget_use_style(t, "s_label_white20c");
        widget_set_prop_int(t, "line_wrap", 1);
        widget_set_prop_int(t, "word_wrap", 1);
        widget_set_prop_int(t, "style:normal:font_size", 16);
        widget_set_prop_str(t, "style:normal:text_align_h", "left");
        widget_set_prop_str(t, "style:normal:text_align_v", "top");
        widget_set_prop_int(t, "style:normal:text_color", (int)GREY);
        widget_set_text_utf8(t, tr.text + tr.off[tr.built]);
        label_resize_to_content(t, (unsigned)w, (unsigned)w, TEXT_ROW_PX, 0x7fff);
        tr.y += I(t, W_H) + TR_GAP;
    }
    widget_set_prop_int(pc.tr_view, "virtual_h", tr.y - TR_GAP + PAGE_X);
}

/* The paragraph playing in white; the page follows it (its top DESC_TOP under the page's), gliding
 * in front and jumping elsewhere, unless the wheel or a finger holds it there, which lasts until
 * the page leaves the front. */
static void transcript_follow(void *front, unsigned now) {
    void *v = pc.tr_view;
    int here = front == pc.tr_page, sec = np_second(), i = -1;
    if (!here)
        tr.held = 0;
    else if (pointer_down() && now - pc.front_at > HOLD_TOUCH_MS && I(v, SCROLL_Y) != tr.set_y)
        tr.held = 1;
    while (sec >= 0 && i + 1 < tr.built && tr.sec[i + 1] <= sec) ++i;
    if (i != tr.lit && g_backlight_status) {
        for (int k = 0; k < 2; ++k) {
            int at = k ? i : tr.lit;
            void *t = at >= 0 && at < tr.built ? widget_get_child(v, (unsigned)at) : 0;
            if (!t) continue;
            widget_set_prop_int(t, "style:normal:text_color", (int)(k ? 0xffffffffu : GREY));
            widget_invalidate_force(t, 0);
        }
        tr.lit = i;
    }
    if (tr.held || i < 0 || pointer_down()) return;
    int to = I(widget_get_child(v, (unsigned)i), W_Y) - DESC_TOP, max = view_max(v);
    if (to > max) to = max;
    if (to < 0) to = 0;
    if (!here) {
        if (I(v, SCROLL_Y) != to) view_at(v, to);
    } else if (!(gl.on && gl.view == v && gl.to == to * 16) && I(v, SCROLL_Y) != to)
        glide_to(v, to * 16);
}

/* Transcript (the user's, 2026-11-15: read as Description is, not as lyrics): its page in the
 * slide_view, last, while the episode has one, its text read once a track (looked for again while
 * missing: q2-pocket may write it later) and laid out a few paragraphs a pass. */
static void transcript_poll(void *r, void *front, unsigned now) {
    if (!pc.tr_page || !pc.tr_view) return;
    if (pc.track != tr.track) {
        transcript_drop();
        tr.track = pc.track, tr.looked = 0;
    }
    if (!tr.n && r && spoken(P(r, REC_PATH)) && (!tr.looked || now - tr.looked >= TR_LOOK_MS)) {
        tr.looked = now | 1;
        transcript_load(r);
    }
    page_put(pc.tr_page, tr.n > 0, ~0u);
    if (!tr.n || P(pc.tr_page, W_PARENT) != pc.slide) return;
    if (tr.built < tr.n) transcript_build();
    transcript_follow(front, now);
}
/* ---- The Queue ---- */

/* An episode's row (the user's, 2026-11-15, as Library's Episodes): the show's art, the title over
 * the show, h high and w wide at y in view; the playing one (lead 0) says so, and an Up Next row
 * has its arrows at its end, up unless it is first, down unless last. The arrows are drawn in the
 * row, not widgets of their own (a list's wheel would stop on them): a tap on them is told by where
 * it lands (podcast_row_arrow). Shared with upnext.c's View Queue. */
void *podcast_row(void *view, int y, int w, int h, void *r, int lead, int up, int down) {
    char title[256], show[160], line[200], url[600];
    void *item = list_item_create(view, 0, y, w, h);
    widget_use_style(item, "s_listitem_black");
    int art = h - 12 < Q_ART ? h - 12 : Q_ART, text_x = PAGE_X + art + 10,
        text_w = w - text_x - (lead ? 2 * ARROW_W : PAGE_X), top = (h - 44) / 2;
    void *a = image_create(item, PAGE_X, (h - art) / 2, art, art);
    image_set_draw_type(a, 4); /* scale_auto, as the stock cover rows */
    image_base_set_image(a, ep_art(r, url));
    void *t = label(item, text_x, top, text_w, 24, "s_scrlabel_white20l", 18, 0xffffffffu);
    widget_set_text_utf8(t, ep_title(r, title, sizeof title));
    const char *s = ep_show(r, show, sizeof show);
    if (!lead) tk_snprintf(line, sizeof line, *s ? "Now Playing · %s" : "Now Playing", s), s = line;
    widget_set_text_utf8(label(item, text_x, top + 24, text_w, 20, "s_scrlabel_white20l", 15, GREY),
                         s);
    for (int k = 0; lead && k < 2; ++k) {
        if (k ? !down : !up) continue;
        void *arrow = image_create(item, w - (2 - k) * ARROW_W, 0, ARROW_W, h);
        image_base_set_image(arrow, k ? "play_qdown" : "play_qup");
        image_set_draw_type(arrow, IMAGE_DRAW_CENTER);
    }
    return item;
}

/* A tap on row item (EVT_CLICK): -1 on its up arrow, 1 on its down arrow, else 0 (a Centre press's
 * click lands on the row's start). */
int podcast_row_arrow(void *item, void *event) {
    if (!item || !event) return 0;
    int xy[2] = { I(event, EVENT_X), I(event, EVENT_Y) }, w = I(item, W_W);
    widget_to_local(item, xy);
    return xy[0] < w - 2 * ARROW_W || xy[0] >= w ? 0 : xy[0] < w - ARROW_W ? -1 : 1;
}

/* The playing local track's row, or 0: the queue's at its position, whatever the play status
 * (the user's, 2026-11-15: a paused or stopped queue said "Nothing playing"). */
static void *queue_lead(void) {
    unsigned at, n, type = (unsigned)MCL(MCL_TYPE);
    if (media_owner() != 1 || (type != 1 && (type & 0xf000) != 0xf000)) return 0;
    return queue_now(&at, &n);
}

/* The Queue page's tap on a row (ctx: its place, 0 the playing one): an arrow moves it
 * (playback_raise: up swaps it with the row before, down with the row after); elsewhere on an Up
 * Next row that episode plays and the one playing goes first in Up Next (playback_play_swap), as
 * Pocket Casts' Up Next does; on the playing one, back to the art. */
static int queue_tap(void *ctx, void *event) {
    int k = (int)(long)ctx,
        arrow = k && event ? podcast_row_arrow(P(event, EVENT_TARGET), event) : 0;
    void *q = P(mcl_pdeqplaylist, 0);
    if (!k) {
        if (pc.slide) widget_set_prop_int(pc.slide, "value", 0);
        return 0;
    }
    if (k < 0 || (unsigned)k > pc.q_n || !q) return 0;
    if (arrow) {
        if (!playback_raise(arrow < 0 ? (unsigned)k - 1 : (unsigned)k))
            toast("This queue can't be reordered");
    } else {
        int at = playback_play_swap(pc.q_idx[k - 1]);
        if (at < 0 && pc.q_idx[k - 1] != (unsigned)MCL(MCL_POS) && pc.q_idx[k - 1] < deque_size(q))
            at = (int)pc.q_idx[k - 1]; /* a queue not ours to reorder: stock's pick */
        if (at >= 0) press_stamp(), player_start_queue(at);
    }
    pc.q_sig = 0; /* shown again */
    return 0;     /* a STOP would keep the slide_view's grab (rate_fav_click) */
}

/* The queue as the page would show it: the playing track, Up Next's indices, the queue. */
static unsigned queue_sign(unsigned *idx, unsigned *n) {
    void *q = P(mcl_pdeqplaylist, 0);
    *n = q ? playback_queue_rows(idx, QUEUE_ROWS) : 0;
    unsigned h = hash_bytes(FNV_SEED, (const unsigned char *)idx, *n * sizeof *idx);
    unsigned at[3] = { (unsigned)MCL(MCL_POS), q ? deque_size(q) : 0u, queue_lead() != 0 };
    h = hash_bytes(h, (const unsigned char *)at, sizeof at);
    return h | 1;
}

/* The Queue page: the playing episode first, then Up Next in the order it plays, each a
 * podcast_row. Built again when the queue, its order or the playing track changes (its scroll
 * kept), and once more EP_RETRY_MS on while a row lacked the library's title. */
static void queue_show(unsigned now) {
    unsigned idx[QUEUE_ROWS], n, sig = queue_sign(idx, &n);
    if (pc.q_again && now - pc.q_again < 0x80000000u) pc.q_again = 0, pc.q_sig = 0;
    if (sig == pc.q_sig) return;
    pc.q_sig = sig;
    pc.q_n = n;
    memcpy(pc.q_idx, idx, n * sizeof *idx);
    void *view = pc.q_view, *q = P(mcl_pdeqplaylist, 0), *lead = queue_lead();
    int w = I(view, W_W), h = I(view, W_H), y = 0, keep = I(view, SCROLL_Y), missing = 0;
    widget_destroy_children(view);
    if (gl.view == view) gl.on = 0;
    ep_batch(1);
    if (lead) {
        widget_on(podcast_row(view, 0, w, Q_ROW_H, lead, 0, 0, 0), EVT_CLICK, queue_tap, (void *)0);
        y = Q_ROW_H;
    }
    for (unsigned i = 0; i < n; ++i) {
        void *r = idx[i] < deque_size(q) ? deque_at(q, idx[i]) : 0;
        if (!r) continue;
        ep_t *e = ep_of(r);
        missing |= !e || !e->title;
        widget_on(podcast_row(view, y, w, Q_ROW_H, r, (int)i + 1, i > 0, i + 1 < n), EVT_CLICK,
                  queue_tap, (void *)(long)(i + 1));
        y += Q_ROW_H;
    }
    ep_batch(0);
    if (!n) {
        void *none = label(view, PAGE_X, y, w - 2 * PAGE_X, h - y > Q_ROW_H ? Q_ROW_H : h - y,
                           "s_scrlabel_white20c", 16, GREY);
        widget_set_text_utf8(none, lead ? "Nothing after this one" : "Nothing playing");
        y += Q_ROW_H;
    }
    widget_set_prop_int(view, "virtual_h", y > h ? y : h);
    if (keep) scroll_view_set_offset(view, 0, keep < view_max(view) ? keep : view_max(view));
    pc.q_again = missing ? (now + EP_RETRY_MS) | 1 : 0;
}

/* ---- Chapters ---- */

static int chapter_tap(void *ctx, void *event) {
    int i = (int)(long)ctx;
    void *r = spoken_now();
    if (r && listen_key(r) == pc.track && i < pc.count) np_seek(pc.rows[i].sec);
    return 0; /* a STOP would keep the slide_view's grab (rate_fav_click) */
}

static void rows_show(void) {
    void *view = pc.ch_view;
    int w = I(view, W_W), h = I(view, W_H), n = pc.count < ROWS_MAX ? pc.count : ROWS_MAX;
    widget_destroy_children(view);
    pc.lit = -1;
    pc.shown_count = n;
    if (!n) {
        widget_set_prop_int(view, "virtual_h", h);
        return; /* the page is out (pages_poll) */
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
    if (i >= 0 && np_front() != pc.ch_page) {
        int max = pc.shown_count * ROW_H - I(pc.ch_view, W_H), y = i * ROW_H;
        scroll_view_set_offset(pc.ch_view, 0, y < max ? y : max > 0 ? max : 0);
    }
}

/* The title and the show span the progress bar, as wide as it is, whatever stock's page code makes
 * them. */
static void rows_span(void) {
    if (!pc.bar) return;
    int x = I(pc.bar, W_X), w = I(pc.bar, W_W);
    for (int i = 0; i < 2; ++i) {
        void *t = pc.line[i];
        if (t && (I(t, W_X) != x || I(t, W_W) != w))
            widget_move_resize(t, x, I(t, W_Y), w, I(t, W_H));
    }
}

static void pages_poll(void *r, unsigned now) {
    if (!pc.win) return;
    rows_span();
    void *front = np_front();
    if (pc.q_view) queue_show(now);
    if (pc.desc_view && pc.desc_text && pc.shown_desc != pc.track) {
        pc.shown_desc = pc.track;
        desc_load(r);
        desc_show();
    }
    unsigned rows = pc.track ^ (unsigned)pc.state;
    if (pc.ch_view && pc.ch_page && pc.shown_rows != rows) {
        pc.shown_rows = rows;
        rows_show();
    }
    /* Chapters (the user's, 2026-11-15): a page, and a dot, only for an episode with chapters,
     * after Description */
    if (pc.ch_page)
        page_put(pc.ch_page, pc.shown_count > 0,
                 pc.desc_page && P(pc.desc_page, W_PARENT) == pc.slide
                     ? (unsigned)widget_index_of(pc.desc_page) + 1
                     : 1);
    if (pc.ch_view && pc.ch_page && pc.shown_count && g_backlight_status) rows_light();
    transcript_poll(r, front, now);
}

/* Every UI pass (ringnav_sleep): the chapters' reader, the playing track's title and the star, the
 * wheel's glide, then every POLL_MS the track: another one's description and, once a spoken one has
 * played CHAPTER_DELAY_MS, its chapters; whether the last one finished; and the pages. */
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
    title_fill();
    void *front = pc.win ? np_front() : 0;
    unsigned now = (unsigned)time_now_ms(), at, n;
    wheel_poll(front);
    glide_step(front);
    if (pc.win) { /* every pass: a swipe's arrival is caught within ARRIVE_MS, not a POLL_MS later
                   */
        if (front != pc.front) pc.front = front, pc.front_at = now, pc.desc_moved = 0;
        desc_top(front, now);
    }
    if (now - pc.polled < POLL_MS && pc.polled) return;
    pc.polled = now;
    void *r = queue_now(&at, &n);
    track_check(r, now);
    finish_poll();
    /* read for Now Playing's page once the track has played a moment, or at once for a hold */
    if (r && pc.state == CH_NONE && !pc.reading &&
        ((pc.win && now - pc.since >= CHAPTER_DELAY_MS) || pc.want))
        read_start(r);
    pages_poll(r, now);
}

/* A skip length row's tap (ctx: its label, named skip_back or skip_fwd): the next length, shown on
 * the row, its icon too. */
static int skip_row_tap(void *ctx, void *event) {
    void *label = ctx, *button = P(label, W_PARENT);
    int fwd = !tk_strcmp(widget_get_prop_str(label, "name", ""), "skip_fwd");
    char text[32], img[24];
    skip_cycle(fwd);
    tk_snprintf(img, sizeof img, fwd ? "play_fwd%d" : "play_back%d", skip_seconds(fwd));
    for (unsigned j = 0, k = widget_count_children(button); j < k; ++j) {
        void *c = widget_get_child(button, j);
        if (!tk_strcmp(widget_get_type(c), "image")) image_base_set_image(c, img);
    }
    skip_label(fwd, text, sizeof text);
    widget_set_text_utf8(label, text);
    return 0;
}

/* Two rows setting the skip lengths, each a tap on to the next (skip_cycle), at the end of view. */
static void skip_rows(void *view) {
    char text[32], img[24];
    for (int fwd = 0; fwd < 2; ++fwd) {
        tk_snprintf(img, sizeof img, fwd ? "play_fwd%d" : "play_back%d", skip_seconds(fwd));
        void *label = list_row(view, img, 0, 0);
        widget_set_name(label, fwd ? "skip_fwd" : "skip_back");
        widget_use_style(label, "s_scrlabel_white20l");
        widget_resize(label, 253, I(label, W_H));
        skip_label(fwd, text, sizeof text);
        widget_set_text_utf8(label, text);
        widget_on(P(label, W_PARENT), EVT_CLICK, skip_row_tap, label);
    }
}

/* The ⋯ page's Queue row: Q2 Podcast's View Queue (upnext.c), the episodes as Now Playing's Queue
 * page shows them, not stock's list of files. */
extern void up_next(int (*keyup)(void *, void *));
static int more_queue(void *ctx, void *event) {
    up_next(page_keyup);
    return 0;
}

/* The ⋯ page's Delete When Finished row (ctx: its label): the playing episode's, turned over. */
static int more_delfin(void *ctx, void *event) {
    void *r = queue_lead();
    if (!r) return 0;
    podcast_delfin_set(r, !podcast_delfin(r));
    list_toggle(ctx, "Delete When Finished", podcast_delfin(r));
    return 0;
}

/* A stock row's icon (its button's first image), else fallback. */
static const char *row_icon(void *row, const char *fallback) {
    void *button = widget_count_children(row) ? widget_get_child(row, 0) : 0;
    for (unsigned j = 0, k = button ? widget_count_children(button) : 0; j < k; ++j) {
        void *c = widget_get_child(button, j);
        const char *img =
            tk_strcmp(widget_get_type(c), "image") ? 0 : widget_get_prop_str(c, "image", "");
        if (img && *img) return img;
    }
    return fallback;
}

/* Now Playing's ⋯ page (onlinematch.c ringnav_playermore), Q2 Podcast's (the user's, 2026-11-12 and
 * 2026-11-15): stock's Download Cover & Lyrics and Look up Artist go, Look up Album is Look up
 * Podcast (an episode's album is its show), Queue opens View Queue's episodes (more_queue), and
 * the skip lengths and Delete When Finished follow. view holds stock's PLAYMORE_COUNT rows, by
 * position (PLAYMORE_ROWS gives each its kind); stock's click reads a row's kind from its button's
 * name, so the rows left keep theirs, and ours are unnamed. */
void podcast_more(void *view) {
    void *gone[PLAYMORE_COUNT];
    unsigned n = 0;
    char icon[32] = "";
    int queue_at = -1;
    for (unsigned i = 0; i < PLAYMORE_COUNT && i < widget_count_children(view); ++i) {
        int kind = ((const int *)PLAYMORE_ROWS)[i];
        void *row = widget_get_child(view, i);
        if (kind == PLAYMORE_SEARCH || kind == PLAYMORE_ARTIST)
            gone[n++] = row;
        else if (kind == PLAYMORE_ALBUM)
            stock_row_as(row, 0, "Look up Podcast");
        else if (kind == PLAYMORE_QUEUE) {
            gone[n++] = row;
            queue_at = (int)i;
            tk_snprintf(icon, sizeof icon, "%s", row_icon(row, "player_playlist"));
        }
    }
    if (queue_at >= 0) { /* ours in its place, before stock's goes */
        void *label = list_row(view, icon, more_queue, 0);
        widget_use_style(label, "s_scrlabel_white20l");
        widget_resize(label, 253, I(label, W_H));
        widget_set_text_utf8(label, "Queue");
        void *item = P(P(label, W_PARENT), W_PARENT);
        /* it opens a page, as stock's did: stock_more_into's mark, for a selected-only chevron */
        if (theme()->chevron_sel) widget_set_visible(image_create(item, 302, 0, 50, 0), 0, 0);
        widget_restack(item, (unsigned)queue_at);
    }
    for (unsigned i = 0; i < n; ++i) widget_destroy(gone[i]);
    skip_rows(view);
    void *r = queue_lead();
    if (r && spoken(P(r, REC_PATH))) {
        void *label = list_row(view, "player_delete", 0, 0);
        widget_on(P(label, W_PARENT), EVT_CLICK, more_delfin, label);
        widget_use_style(label, "s_scrlabel_white20l");
        list_toggle(label, "Delete When Finished", podcast_delfin(r));
    }
}

/* Playback settings (navigation.c ringnav_audioset): the skip lengths, last. */
void podcast_playback_rows(void *view) { skip_rows(view); }
