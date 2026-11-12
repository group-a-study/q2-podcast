/* Podcasts and audiobooks (docs/internals.md#podcasts-on-now-playing): on a spoken track the side
 * buttons skip back and forward, and held they jump between its chapters. Now Playing (Q2 Podcast's
 * page, tools/ipod.py playing_stock) has the same as buttons beside the art; its star stars a
 * Pocket episode in Pocket Casts (q2-pocket); its Description and Chapters pages follow the art,
 * and the lyrics page is the episode's transcript, there only while the track has one. A track
 * played from Folder takes the title its file has. */
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
#define ROWS_MAX 200 /* the Chapters page's rows; a hold still reaches every chapter */
#define QUEUE_ROWS 60 /* the Queue page's Up Next rows (playback.c's PEND_LIMIT is 64) */
#define ARROW_W 44    /* each of a Queue row's arrows */
#define POLL_MS 500
/* A transcript page stays this long after the player has no lyrics: reading the next track's
 * empties them a moment. */
#define TRANSCRIPT_GONE_MS 2000
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
    /* Now Playing's pages (tools/ipod.py): the window, its slide_view and dots, Description's
     * scroll view and text, Chapters' page and scroll view, the lyrics page and the hidden view it
     * waits in */
    void *win, *slide, *dots, *desc_view, *desc_text, *ch_page, *ch_view, *lyric, *shelf;
    void *bar, *line[2]; /* the progress bar, and the title and show under the art, which span it */
    void *q_page, *q_view;   /* the Queue page and its scroll view */
    unsigned q_sig, q_n;     /* the queue as the page shows it (q_sign), its Up Next rows */
    unsigned q_idx[QUEUE_ROWS]; /* their queue indices, in play order */
    char *desc;
    int lit, shown_count;
    unsigned shown_desc, shown_rows; /* the track each page shows, ~0 to build again */
    unsigned lyric_at;               /* the transcript last wanted (or out) */
    char *named;                     /* the title title_fill made from a file's name */
    int skip[2];                     /* the skip lengths, back and forward, once read (seconds) */
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
        int def = fwd ? SKIP_FWD_S : SKIP_BACK_S, s = config_value("Q2POD", SKIP_KEY[fwd], 100, def), i = 0;
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

/* Now Playing's buttons beside the art: skip_seconds back and on in any local track; the previous or
 * next chapter in a spoken one, as the side buttons' holds, else the previous or next track.
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

/* Whether label w shows s already (a text too long to compare counts as shown, so a long title isn't
 * set again every pass, which would start its scroll over). */
static int shows(void *w, const char *s) {
    char now[512] = "";
    if (strlen(s) >= sizeof now - 8) return 1;
    widget_get_text_utf8(w, now, sizeof now);
    return !strcmp(now, s);
}

/* A local track without a title of its own (a song played from Folder, as every Pocket episode is,
 * its title only its file's name): the title the player reads from its file; until then, and for a
 * file without one, a spoken track's name as a title (name_title). Without an artist, a spoken
 * one's is its show (the album its file names), another's its file's. Now Playing, the lock screen and View Queue then name the
 * episode, not its file. Every UI pass: it is cheap, and the player reads a file's tags just as it
 * starts. */
static void title_fill(void) {
    unsigned at, n;
    void *r = queue_now(&at, &n);
    if (!r || I(r, REC_TYPE) != 8 || !P(r, REC_PATH)) return;
    const char *own = P(r, REC_TITLE), *artist = P(r, REC_ARTIST);
    int named = pc.named && own == pc.named, voice = spoken(P(r, REC_PATH)), changed = 0;
    if (!own || !*own || named || name_only(own, P(r, REC_NAME))) {
        const char *tag = now_tag(r, REC_TITLE);
        char buf[256];
        if (tag != own && tag && *tag && !name_only(tag, P(r, REC_NAME))) { /* the player read it */
            changed = rec_set(r, REC_TITLE, tag) != 0;
            pc.named = 0;
        } else if (!named && voice && P(r, REC_NAME)) {
            name_title(P(r, REC_NAME), buf, sizeof buf);
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
    if (win == pc.win)
        pc.win = pc.slide = pc.dots = pc.desc_view = pc.desc_text = pc.ch_page = pc.ch_view =
            pc.lyric = pc.shelf = pc.bar = pc.line[0] = pc.line[1] = pc.q_page = pc.q_view = 0;
    return 0;
}

/* The dots: a page each, the transcript's too while it is in. */
static void dots_count(void) {
    if (pc.dots && pc.slide)
        widget_set_prop_int(pc.dots, "max", (int)widget_count_children(pc.slide));
}

/* ringnav_playing: the pages tools/ipod.py made (the art, Description, Chapters and the info; the
 * lyrics in view_pc_shelf, hidden, until a track has some: transcript_poll), and the buttons beside
 * the art. A page missing leaves its part out. */
void podcast_attach(void *win) {
    pc.slide = widget_lookup(win, "slide_view", 1);
    if (!pc.slide) return;
    pc.win = win;
    pc.dots = widget_lookup(win, "slide_indicator1", 1);
    pc.desc_view = widget_lookup(win, "view_pc_desc_scroll", 1);
    pc.desc_text = widget_lookup(win, "label_pc_desc", 1);
    pc.ch_page = widget_lookup(win, "view_pc_chapters", 1);
    pc.ch_view = widget_lookup(win, "view_pc_chapters_scroll", 1);
    pc.lyric = widget_lookup(win, "view_lyric", 1);
    pc.shelf = widget_lookup(win, "view_pc_shelf", 1);
    /* stock's hit test skips insensitive widgets, not hidden ones: the hidden shelf over the pages
     * would take every tap and swipe on them (tools/ipod.py also builds it so, off the screen) */
    if (pc.shelf) widget_set_sensitive(pc.shelf, 0);
    if (pc.lyric && P(pc.lyric, W_PARENT) != pc.shelf) pc.lyric = 0; /* not this build's page */
    pc.bar = widget_lookup(win, "slider_play", 1);
    pc.q_page = widget_lookup(win, "view_pc_queue", 1);
    pc.q_view = widget_lookup(win, "view_pc_queue_scroll", 1);
    pc.q_sig = 0;
    pc.line[0] = widget_lookup(win, "scrlabel_title", 1);
    pc.line[1] = widget_lookup(win, "scrlabel_artist", 1);
    pc.lyric_at = (unsigned)time_now_ms();
    pc.shown_desc = pc.shown_rows = ~0u;
    for (int b = 0; b < B_N; ++b) {
        void *w = widget_lookup(win, button_names[b], 1);
        if (w) widget_on(w, EVT_CLICK, button_tap, (void *)(long)b);
    }
    dots_count();
    skip_icons();
    widget_on(win, EVT_DESTROY, pages_gone, win);
    pc.polled = 0; /* podcast_poll fills them on its next pass */
}

/* The lyrics page, an episode's transcript (q2-pocket's .lrc beside it, which the player reads as
 * lyrics), in the slide_view last while the player has lyrics, else back in its hidden view; the
 * page in front when it goes, the one before it. */
static void transcript_poll(unsigned now) {
    void *page = pc.lyric;
    if (!page || !pc.shelf) return;
    int in = P(page, W_PARENT) == pc.slide, want = mclGetLyricSize() > 0;
    if (want == in) pc.lyric_at = now;
    if (want == in || (in && now - pc.lyric_at < TRANSCRIPT_GONE_MS)) return;
    if (in) {
        int last = (int)widget_count_children(pc.slide) - 1;
        if (widget_get_prop_int(pc.slide, "value", 0) >= last)
            widget_set_prop_int(pc.slide, "value", last - 1);
        widget_remove_child(pc.slide, page);
        widget_add_child(pc.shelf, page);
    } else {
        widget_remove_child(pc.shelf, page);
        widget_add_child(pc.slide, page);
    }
    pc.lyric_at = now;
    dots_count();
    widget_invalidate_force(P(pc.slide, W_PARENT), 0);
}

static void desc_show(void) {
    void *t = pc.desc_text, *view = pc.desc_view;
    int w = I(view, W_W) - 2 * PAGE_X, h = I(view, W_H);
    widget_set_text_utf8(t, pc.desc ? pc.desc : "No description");
    widget_set_prop_str(t, "style:normal:text_align_h", pc.desc ? "left" : "center");
    widget_set_prop_str(t, "style:normal:text_align_v", pc.desc ? "top" : "middle");
    widget_set_prop_int(t, "style:normal:text_color", (int)(pc.desc ? 0xffffffffu : GREY));
    widget_move_resize(t, PAGE_X, 0, w, h);
    if (pc.desc) label_resize_to_content(t, (unsigned)w, (unsigned)w, (unsigned)h, 0x7fff);
    widget_set_prop_int(view, "virtual_h", I(t, W_H) + (pc.desc ? PAGE_X : 0));
    scroll_view_set_offset(view, 0, 0);
}

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
        void *none = label(view, PAGE_X, 0, w - 2 * PAGE_X, h, "s_scrlabel_white20c", 16, GREY);
        widget_set_text_utf8(none, pc.state == CH_DONE ? "No chapters" : "Reading chapters…");
        widget_set_prop_int(view, "virtual_h", h);
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
    if (i >= 0 && np_front() != pc.ch_page) {
        int max = pc.shown_count * ROW_H - I(pc.ch_view, W_H), y = i * ROW_H;
        scroll_view_set_offset(pc.ch_view, 0, y < max ? y : max > 0 ? max : 0);
    }
}

/* A queued track's title for the Queue page: its own, else its file's name as a title. */
static const char *queue_title(void *r, char *buf, unsigned size) {
    const char *own = P(r, REC_TITLE), *name = P(r, REC_NAME);
    if (own && *own && !name_only(own, name)) return own;
    name_title(name ? name : "", buf, size);
    return buf;
}

/* The Queue page's tap on a row (ctx: its place, 0 the playing track): that track plays, and the one
 * playing goes first in Up Next (playback_play_swap), as Pocket Casts' Up Next does. */
static int queue_tap(void *ctx, void *event) {
    int k = (int)(long)ctx;
    void *q = P(mcl_pdeqplaylist, 0);
    if (k <= 0 || (unsigned)k > pc.q_n || !q) return 0;
    unsigned at = pc.q_idx[k - 1];
    if (at >= deque_size(q)) return 0;
    playback_play_swap(at);
    press_stamp();
    player_start_queue((int)at);
    pc.q_sig = 0; /* shown again */
    return 0;
}

/* A Queue row's arrow (ctx: the Up Next row, negative for its down arrow): it swaps with the row
 * above or below (playback_raise). */
static int queue_arrow(void *ctx, void *event) {
    int k = (int)(long)ctx;
    if (!playback_raise(k < 0 ? (unsigned)(-k) : (unsigned)k)) toast("This queue can't be reordered");
    pc.q_sig = 0;
    return 0;
}

/* The queue as the page would show it: the playing track, Up Next's indices, the queue. */
static unsigned queue_sign(unsigned *idx, unsigned *n) {
    void *q = P(mcl_pdeqplaylist, 0);
    *n = q ? playback_queue_rows(idx, QUEUE_ROWS) : 0;
    unsigned h = hash_bytes(FNV_SEED, (const unsigned char *)idx, *n * sizeof *idx);
    unsigned at[2] = { (unsigned)MCL(MCL_POS), q ? deque_size(q) : 0u };
    h = hash_bytes(h, (const unsigned char *)at, sizeof at);
    return h | 1;
}

/* The Queue page: the playing track first, in white, then Up Next in the order it plays, each with
 * an up and a down arrow beside it (none up on the first, none down on the last). A tap plays a row
 * (queue_tap). Built again when the queue, its order or the playing track changes. */
static void queue_show(void) {
    unsigned idx[QUEUE_ROWS], n, sig = queue_sign(idx, &n);
    if (sig == pc.q_sig) return;
    pc.q_sig = sig;
    pc.q_n = n;
    memcpy(pc.q_idx, idx, n * sizeof *idx);
    void *view = pc.q_view, *q = P(mcl_pdeqplaylist, 0), *now = local_now();
    int w = I(view, W_W), h = I(view, W_H), y = 0;
    char buf[256];
    widget_destroy_children(view);
    if (now) {
        void *item = list_item_create(view, 0, 0, w, ROW_H);
        widget_use_style(item, "s_listitem_black");
        void *t = label(item, PAGE_X, 0, w - 2 * PAGE_X, ROW_H, "s_scrlabel_white20l", 16, 0xffffffffu);
        widget_set_text_utf8(t, queue_title(now, buf, sizeof buf));
        y = ROW_H;
    }
    if (!n) {
        void *none = label(view, PAGE_X, y, w - 2 * PAGE_X, h - y, "s_scrlabel_white20c", 16, GREY);
        widget_set_text_utf8(none, now ? "Nothing after this one" : "Nothing playing");
    }
    for (unsigned i = 0; i < n; ++i, y += ROW_H) {
        void *item = list_item_create(view, 0, y, w - 2 * ARROW_W, ROW_H);
        widget_use_style(item, "s_listitem_black");
        void *t = label(item, PAGE_X, 0, w - 2 * ARROW_W - PAGE_X, ROW_H, "s_scrlabel_white20l", 16, GREY);
        widget_set_text_utf8(t, idx[i] < deque_size(q) ? queue_title(deque_at(q, idx[i]), buf, sizeof buf) : "");
        widget_on(item, EVT_CLICK, queue_tap, (void *)(long)(i + 1));
        for (int down = 0; down < 2; ++down) {
            if (down ? i + 1 == n : !i) continue;
            void *a = image_create(view, w - (2 - down) * ARROW_W, y, ARROW_W, ROW_H);
            image_base_set_image(a, down ? "play_qdown" : "play_qup");
            image_set_draw_type(a, IMAGE_DRAW_CENTER);
            widget_set_prop_str(a, "clickable", "true");
            widget_on(a, EVT_CLICK, queue_arrow, (void *)(long)(down ? -(int)(i + 1) : (int)i));
        }
    }
    widget_set_prop_int(view, "virtual_h", y > h ? y : h);
}

/* The title and the show span the progress bar, as wide as it is (navigation.c narrows it for a
 * long track's times), whatever stock's page code makes them. */
static void rows_span(void) {
    if (!pc.bar) return;
    int x = I(pc.bar, W_X), w = I(pc.bar, W_W);
    for (int i = 0; i < 2; ++i) {
        void *t = pc.line[i];
        if (t && (I(t, W_X) != x || I(t, W_W) != w)) widget_move_resize(t, x, I(t, W_Y), w, I(t, W_H));
    }
}

static void pages_poll(void *r, unsigned now) {
    if (!pc.win) return;
    rows_span();
    if (pc.q_view) queue_show();
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
    if (pc.ch_view && pc.ch_page && g_backlight_status) rows_light();
    transcript_poll(now);
}

/* Every UI pass (ringnav_sleep): the chapters' reader, the playing track's title and the star, then
 * every POLL_MS the track: another one's description and, once a spoken one has played
 * CHAPTER_DELAY_MS, its chapters; and the pages. */
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
    unsigned now = (unsigned)time_now_ms(), at, n;
    if (now - pc.polled < POLL_MS && pc.polled) return;
    pc.polled = now;
    void *r = queue_now(&at, &n);
    track_check(r, now);
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

/* Now Playing's ⋯ page (onlinematch.c ringnav_playermore), Q2 Podcast's (the user's, 2026-11-12):
 * stock's Download Cover & Lyrics and Look up Artist go, Look up Album is Look up Podcast (an
 * episode's album is its show), and the skip lengths follow. view holds stock's PLAYMORE_COUNT rows,
 * by position (PLAYMORE_ROWS gives each its kind); stock's click reads a row's kind from its
 * button's name, so the rows left keep theirs. */
void podcast_more(void *view) {
    void *gone[PLAYMORE_COUNT];
    unsigned n = 0;
    for (unsigned i = 0; i < PLAYMORE_COUNT && i < widget_count_children(view); ++i) {
        int kind = ((const int *)PLAYMORE_ROWS)[i];
        void *row = widget_get_child(view, i);
        if (kind == PLAYMORE_SEARCH || kind == PLAYMORE_ARTIST)
            gone[n++] = row;
        else if (kind == PLAYMORE_ALBUM)
            stock_row_as(row, 0, "Look up Podcast");
    }
    for (unsigned i = 0; i < n; ++i) widget_destroy(gone[i]);
    skip_rows(view);
}

/* Playback settings (navigation.c ringnav_audioset): the skip lengths, last. */
void podcast_playback_rows(void *view) { skip_rows(view); }

