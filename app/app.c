/* SPDX-License-Identifier: MPL-2.0 */
/* TUNNEL FM-1: the app around the engine (tun.c). The FM-1's controls as a techno machine:
 *
 *   PLAY     start / stop                     PRESETS  the preset (on the next bar while playing)
 *   SELECT   the track (SEL: the next one)    ALGO     TUNNEL, the macro: the filter closes, the delay and the
 *                                                      reverb swell
 *   HOME     the loops: every track's steps and where it is; K1..K4 SWEEP, FEEDBACK, BPM, DRIVE
 *            white keys F3..F4: mute the eight tracks; G4..G5: select a track
 *   SEQ      the selected track's steps on the 16 white keys (OCT- / OCT+: steps 1-16 / 17-32); K1..K4 LENGTH, RATE,
 *            ROTATE, ROOT; hold a step and K1..K4 are its NOTE, VELOCITY, RATCHET, CHANCE (SEQ while holding: slide)
 *   EDIT     the selected track's sound, two pages        ENV  its mix: LEVEL, PAN, DELAY, REVERB
 *   FX       DELAY > REVERB > MASTER                       LFO  the motion LFO: RATE, DEPTH, DEST, SHAPE
 *   GLO      TEMPO (BPM, SWING, KEY, CLOCK) > DECKS (A drums, B bass and stab, C bell: each from any preset, on the
 *            next bar) > SETUP
 *   REC      the keys play the selected track (bass, stab, bell: chromatic; drums: hit); ARP: arpeggiate them
 *   SAVE     into a user slot (U01..U32): the one playing, or the first free one
 *
 * Part of the unity build (device/unity.c, host/plat_host.c), after the platform: gfx.c and announce.c are in
 * scope. The engine is its own unit (fm1.toml o2). */
#include "fastmath.h"
#include "tun.h"

/* ---------------------------------------------------------------- presets --- */
static const tun_preset_t FACTORY[] = {
#include "factory.h"
};
#define NFACTORY (int)(sizeof FACTORY / sizeof FACTORY[0])
#define NUSER 32
#define PER_OBJ 4
#define NPRESET (NFACTORY + NUSER)
#define USER_MAGIC 0x54554E31u           /* "TUN1" */
typedef struct {
    uint32_t magic;
    tun_preset_t p[PER_OBJ];
} ublock_t;
static PLAT_POOL ublock_t ub[NUSER / PER_OBJ];
_Static_assert(sizeof(ublock_t) <= PLAT_STORE_MAX, "a block of user presets fits a store object");
_Static_assert(sizeof(tun_preset_t) == 672, "a preset is 672 bytes");

static void users_load(void)
{
    int b;
    for (b = 0; b < NUSER / PER_OBJ; b++)
        if (plat_store_load(OBJ_USER0 + (uint32_t)b, &ub[b], sizeof ub[b]) != (int)sizeof ub[b]
            || ub[b].magic != USER_MAGIC) {
            uint32_t i;
            uint8_t *x = (uint8_t *)&ub[b];
            for (i = 0; i < sizeof ub[b]; i++)
                x[i] = 0;
            ub[b].magic = USER_MAGIC;
        }
}
static tun_preset_t *user_slot(int u) { return &ub[(u / PER_OBJ) % (NUSER / PER_OBJ)].p[u % PER_OBJ]; }
static int user_used(int u) { return user_slot(u)->name[0] != 0; }
static const tun_preset_t *preset_at(int i)
{
    if (i < NFACTORY)
        return &FACTORY[i];
    i -= NFACTORY;
    return user_used(i) ? user_slot(i) : &FACTORY[0];
}

/* -------------------------------------------------------------- settings --- */
#define SET_MAGIC 0x54555331u            /* "TUS1" */
typedef struct {
    uint32_t magic;
    uint8_t preset;
    uint8_t ble_off;
    uint8_t midi_ch;                     /* 0 omni, 1..16 */
    uint8_t drum_ch;                     /* the drums' channel in: 10 */
    tun_perf_t perf;
    uint8_t rsv[6];
} settings_t;
static settings_t set;
static uint8_t set_dirty, edit_dirty;

static void settings_load(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || set.magic != SET_MAGIC
        || set.preset >= NPRESET || set.midi_ch > 16 || set.perf.clock_src > 2) {
        uint32_t i;
        uint8_t *b = (uint8_t *)&set;
        for (i = 0; i < sizeof set; i++)
            b[i] = 0;
        set.magic = SET_MAGIC;
        set.preset = 1;
        set.drum_ch = 10;
        set.perf.clock_src = 2;
        set.perf.clock_out = 1;
    }
}

int tun_ble_off(void) { return set.ble_off; }

static int deck_of[NDECKS];              /* the preset each deck last loaded */

static void preset_select(int i)
{
    int d;
    set.preset = (uint8_t)((i % NPRESET + NPRESET) % NPRESET);
    tun_set_preset(preset_at(set.preset));
    for (d = 0; d < NDECKS; d++)
        deck_of[d] = set.preset;
    edit_dirty = 0;
    set_dirty = 1;
}

void app_init(void)
{
    tun_init();
    settings_load();
    users_load();
    tun_set_perf(&set.perf);
    preset_select(set.preset);
}

/* ----------------------------------------------------------------- audio --- */
static volatile int pc_pending = -1;
static volatile uint8_t ui_poke;
static volatile int morph_in = -1;
static int sel_track = TK_KICK;

static int gm_drum(int note)             /* General MIDI drum notes to the tracks */
{
    switch (note) {
    case 35: case 36: return TK_KICK;
    case 37: case 38: case 39: case 40: return TK_SNARE;
    case 42: case 44: return TK_CH;
    case 46: case 49: case 51: case 53: case 59: return TK_OH;
    default: return note >= 41 && note <= 70 ? TK_PERC : -1;
    }
}

static void midi_in(void)
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = (pkt >> 24) & 0x7Fu, ch = (st & 15u) + 1u;
        uint32_t type = st & 0xF0u;
        if (st == 0xF8u || st == 0xFAu || st == 0xFBu || st == 0xFCu) {
            tun_clock((int)st);
            continue;
        }
        if (st >= 0xF0u)
            continue;
        if (type == 0x90u && d2 && ch == set.drum_ch) {
            int tr = gm_drum((int)d1);
            if (tr >= 0)
                tun_note_on(tr, tr == TK_PERC ? (int)d1 - 45 : 0, (int)d2);
            continue;
        }
        if (set.midi_ch && ch != set.midi_ch)
            continue;
        if (type == 0x90u && d2)
            tun_note_on(sel_track, (int)d1 - 48, (int)d2);
        else if (type == 0x80u || type == 0x90u)
            tun_note_off(sel_track, (int)d1 - 48);
        else if (type == 0xB0u && d1 == 1u) {
            tun_morph((int)d2);
            morph_in = (int)d2;
        } else if (type == 0xC0u)
            pc_pending = (int)d1;
    }
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float buf[2 * APP_BLOCK];
    static float gain_now;
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;
    uint32_t i;
    uint8_t rt;
    midi_in();
    if (n > APP_BLOCK)
        n = APP_BLOCK;
    tun_render(buf, n);
    while (tun_clock_out(&rt))           /* MIDI clock out, as it falls due */
        plat_midi_send(0x0Fu | (uint32_t)rt << 8, TO_USB | TO_BLE);
    for (i = 0; i < n; i++) {
        gain_now += (gain - gain_now) * 0.004f;
        lr[2 * i] = (int32_t)(buf[2 * i] * gain_now);
        lr[2 * i + 1] = (int32_t)(buf[2 * i + 1] * gain_now);
    }
}

void app_panic(void) { tun_panic(); }

/* ---------------------------------------------------------------- the pages --- */
/* a knob: what it sets */
enum {
    KN_NONE = -1,
    KN_SND = 0,                          /* 0..7: the selected track's sound */
    KN_MIX = 10,                         /* 10..13: level, pan, delay, reverb */
    KN_G = 20,                           /* 20..20+G_N: the preset's globals */
    KN_LEN = 40, KN_RATE, KN_ROT, KN_ROOT,
    KN_NOTE, KN_VEL, KN_RAT, KN_PROB,    /* a held step's */
    KN_DECK = 50,                        /* 50..52 */
    KN_CLKSRC = 60, KN_CLKOUT, KN_MIDICH, KN_DRUMCH,
};
enum { PG_HOME, PG_SEQ, PG_SND1, PG_SND2, PG_MIX, PG_DLY, PG_REV, PG_MASTER, PG_LFO, PG_TEMPO, PG_DECKS, PG_SETUP,
       NPAGES };
typedef struct {
    const char *title;
    int16_t k[4];
} page_t;
static const page_t PAGES[NPAGES] = {
    [PG_HOME] = {"LOOPS", {KN_G + G_SWEEP, KN_G + G_DLY_FB, KN_G + G_BPM, KN_G + G_DRIVE}},
    [PG_SEQ] = {"STEPS", {KN_LEN, KN_RATE, KN_ROT, KN_ROOT}},
    [PG_SND1] = {"SOUND", {KN_SND + 0, KN_SND + 1, KN_SND + 2, KN_SND + 3}},
    [PG_SND2] = {"SOUND 2", {KN_SND + 4, KN_SND + 5, KN_SND + 6, KN_SND + 7}},
    [PG_MIX] = {"MIX", {KN_MIX + 0, KN_MIX + 1, KN_MIX + 2, KN_MIX + 3}},
    [PG_DLY] = {"DELAY", {KN_G + G_DLY_TIME, KN_G + G_DLY_FB, KN_G + G_DLY_TONE, KN_NONE}},
    [PG_REV] = {"REVERB", {KN_G + G_REV_SIZE, KN_G + G_REV_DAMP, KN_NONE, KN_NONE}},
    [PG_MASTER] = {"MASTER", {KN_G + G_SWEEP, KN_G + G_DRIVE, KN_G + G_VOLUME, KN_NONE}},
    [PG_LFO] = {"MOTION LFO", {KN_G + G_LFO_RATE, KN_G + G_LFO_DEPTH, KN_G + G_LFO_DEST, KN_G + G_LFO_SHAPE}},
    [PG_TEMPO] = {"TEMPO", {KN_G + G_BPM, KN_G + G_SWING, KN_G + G_KEY, KN_CLKSRC}},
    [PG_DECKS] = {"DECKS", {KN_DECK + 0, KN_DECK + 1, KN_DECK + 2, KN_NONE}},
    [PG_SETUP] = {"SETUP", {KN_CLKOUT, KN_MIDICH, KN_DRUMCH, KN_NONE}},
};

static struct {
    int page;
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint32_t rec_t;
    uint32_t turned_at[4], knob_t[4];
    uint8_t redraw;
    uint8_t steppage;                    /* 0: steps 1-16, 1: 17-32 */
    int held_step;                       /* a step key held on the SEQ page, -1 none */
    uint8_t step_edited;                 /* the held step was turned: no toggle on release */
    uint8_t keys_mode, arp;              /* REC: the keys play; ARP: arpeggiated */
    int8_t oct;
    uint8_t morph;
} ui = {.held_step = -1};

static const char *const DECK_NAME[NDECKS] = {"DECK A", "DECK B", "DECK C"};

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}
static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}
static void signed_text(int32_t v, char *t)
{
    t[0] = 0;
    if (v > 0)
        text_cat(t, "+");
    int_text(v, t + (v > 0));
}
static void preset_label(int i, char *t)  /* "042" or "U07" */
{
    if (i >= NFACTORY) {
        i -= NFACTORY - 1;
        t[0] = 'U';
        t[1] = (char)('0' + i / 10);
        t[2] = (char)('0' + i % 10);
    } else {
        t[0] = (char)('0' + i / 100);
        t[1] = (char)('0' + i / 10 % 10);
        t[2] = (char)('0' + i % 10);
    }
    t[3] = 0;
}

static tun_track_t *trk(void) { return &tun_preset()->t[sel_track]; }

static int step_index(void) { return ui.held_step + 16 * ui.steppage; }

static int knob_lo(int id)
{
    if (id >= KN_G && id < KN_G + G_N)
        return TUN_G_MIN[id - KN_G];
    switch (id) {
    case KN_LEN: case KN_MIDICH: case KN_DRUMCH: return id == KN_LEN ? 1 : id == KN_DRUMCH ? 1 : 0;
    case KN_ROOT: return -24;
    case KN_NOTE: return -24;
    case KN_ROT: return -1;
    default: return 0;
    }
}
static int knob_hi(int id)
{
    if (id >= KN_SND && id < KN_SND + TUN_NSND)
        return TUN_SND_MAX[sel_track][id - KN_SND];
    if (id >= KN_G && id < KN_G + G_N)
        return TUN_G_MAX[id - KN_G];
    switch (id) {
    case KN_LEN: return TUN_STEPS;
    case KN_RATE: return 3;
    case KN_ROT: return 1;
    case KN_ROOT: return 24;
    case KN_NOTE: return 36;
    case KN_VEL: case KN_RAT: case KN_PROB: return 3;
    case KN_DECK: case KN_DECK + 1: case KN_DECK + 2: return NPRESET - 1;
    case KN_CLKSRC: return 2;
    case KN_CLKOUT: return 1;
    case KN_MIDICH: case KN_DRUMCH: return 16;
    default: return 127;
    }
}
static int knob_value(int id)
{
    tun_preset_t *p = tun_preset();
    tun_track_t *t = trk();
    if (id < 0)
        return 0;
    if (id < KN_SND + TUN_NSND)
        return t->snd[id - KN_SND];
    if (id >= KN_MIX && id < KN_MIX + 4)
        return id == KN_MIX ? t->level : id == KN_MIX + 1 ? t->pan : id == KN_MIX + 2 ? t->dly : t->rev;
    if (id >= KN_G && id < KN_G + G_N)
        return p->g[id - KN_G];
    if (id >= KN_DECK && id < KN_DECK + NDECKS)
        return deck_of[id - KN_DECK];
    switch (id) {
    case KN_LEN: return t->len;
    case KN_RATE: return t->div;
    case KN_ROT: return 0;
    case KN_ROOT: return t->root;
    case KN_NOTE: return t->st[step_index()].note;
    case KN_VEL: return (int)ST_VEL(t->st[step_index()].f);
    case KN_RAT: return (int)ST_RAT(t->st[step_index()].f);
    case KN_PROB: return (int)ST_PROB(t->st[step_index()].f);
    case KN_CLKSRC: return set.perf.clock_src;
    case KN_CLKOUT: return set.perf.clock_out;
    case KN_MIDICH: return set.midi_ch;
    case KN_DRUMCH: return set.drum_ch;
    default: return 0;
    }
}

static void rotate(tun_track_t *t, int d)
{
    int len = t->len, i;
    tun_step_t tmp;
    if (len < 2)
        return;
    if (d > 0) {
        tmp = t->st[len - 1];
        for (i = len - 1; i > 0; i--)
            t->st[i] = t->st[i - 1];
        t->st[0] = tmp;
    } else {
        tmp = t->st[0];
        for (i = 0; i < len - 1; i++)
            t->st[i] = t->st[i + 1];
        t->st[len - 1] = tmp;
    }
}

static void knob_set(int id, int v)
{
    tun_preset_t *p = tun_preset();
    tun_track_t *t = trk();
    if (id == KN_ROT) {                  /* not a value: a shift of the loop */
        rotate(t, v);
        edit_dirty = 1;
        return;
    }
    v = clamp(v, knob_lo(id), knob_hi(id));
    if (v == knob_value(id))
        return;
    if (id >= KN_DECK && id < KN_DECK + NDECKS) {
        deck_of[id - KN_DECK] = v;
        tun_deck_load(id - KN_DECK, preset_at(v));
        edit_dirty = 1;
        return;
    }
    switch (id) {
    case KN_CLKSRC: set.perf.clock_src = (uint8_t)v; tun_set_perf(&set.perf); set_dirty = 1; return;
    case KN_CLKOUT: set.perf.clock_out = (uint8_t)v; tun_set_perf(&set.perf); set_dirty = 1; return;
    case KN_MIDICH: set.midi_ch = (uint8_t)v; set_dirty = 1; return;
    case KN_DRUMCH: set.drum_ch = (uint8_t)v; set_dirty = 1; return;
    default: break;
    }
    edit_dirty = 1;
    if (id < KN_SND + TUN_NSND)
        t->snd[id - KN_SND] = (uint8_t)v;
    else if (id >= KN_MIX && id < KN_MIX + 4) {
        uint8_t *m[4] = {&t->level, &t->pan, &t->dly, &t->rev};
        *m[id - KN_MIX] = (uint8_t)v;
    } else if (id >= KN_G && id < KN_G + G_N)
        p->g[id - KN_G] = (uint8_t)v;
    else {
        tun_step_t *s = &t->st[step_index()];
        switch (id) {
        case KN_LEN: t->len = (uint8_t)v; break;
        case KN_RATE: t->div = (uint8_t)v; break;
        case KN_ROOT: t->root = (int8_t)v; break;
        case KN_NOTE: s->note = (int8_t)v; s->f |= ST_ON; break;
        case KN_VEL: s->f = (uint8_t)((s->f & ~(3u << 1)) | (uint32_t)v << 1 | ST_ON); break;
        case KN_RAT: s->f = (uint8_t)((s->f & ~(3u << 3)) | (uint32_t)v << 3 | ST_ON); break;
        case KN_PROB: s->f = (uint8_t)((s->f & ~(3u << 5)) | (uint32_t)v << 5 | ST_ON); break;
        default: break;
        }
    }
}

static const char *knob_name(int id)
{
    static const char *const MIX[4] = {"LEVEL", "PAN", "DELAY", "REVERB"};
    static const char *const LOOP[8] = {"LENGTH", "RATE", "ROTATE", "ROOT", "NOTE", "VELOCITY", "RATCHET", "CHANCE"};
    static const char *const SET[4] = {"CLOCK", "CLK OUT", "MIDI CH", "DRUM CH"};
    if (id < 0)
        return "";
    if (id < KN_SND + TUN_NSND)
        return TUN_SND_NAME[sel_track][id - KN_SND];
    if (id >= KN_MIX && id < KN_MIX + 4)
        return MIX[id - KN_MIX];
    if (id >= KN_G && id < KN_G + G_N)
        return TUN_G_NAME[id - KN_G];
    if (id >= KN_LEN && id <= KN_PROB)
        return LOOP[id - KN_LEN];
    if (id >= KN_DECK && id < KN_DECK + NDECKS)
        return DECK_NAME[id - KN_DECK];
    if (id >= KN_CLKSRC && id <= KN_DRUMCH)
        return SET[id - KN_CLKSRC];
    return "";
}

static int knob_is_list(int id)          /* a few named positions, not a sweep */
{
    if (id == KN_ROT)
        return 0;
    return knob_hi(id) - knob_lo(id) <= 11 && id != KN_LEN;
}
static int knob_bipolar(int id)
{
    return id == KN_MIX + 1 || id == KN_G + G_SWEEP || id == KN_ROOT || id == KN_NOTE;
}

static void value_text(int id, int v, char *t)
{
    static const char *const RATE[4] = {"1/16", "1/8", "1/32", "1/16T"};
    static const char *const VEL[4] = {"SOFT", "NORMAL", "ACCENT", "HARD"};
    static const char *const PROB[4] = {"100%", "75%", "50%", "25%"};
    static const char *const DTIME[8] = {"1/32", "1/16", "1/16T", "1/8", "3/16", "1/4", "3/8", "1/2"};
    static const char *const LRATE[10] = {"2 BAR", "1 BAR", "1/2", "3/8", "1/4", "3/16", "1/8", "3/32", "1/16", "1/32"};
    static const char *const LDEST[7] = {"OFF", "BASS", "STAB", "BELL", "ALL", "SWEEP", "HATS"};
    static const char *const LSHAPE[4] = {"TRI", "SAW", "SQUARE", "S+H"};
    static const char *const KEY[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static const char *const CLK[3] = {"INT", "MIDI", "AUTO"};
    static const char *const SNARE[3] = {"SNARE", "CLAP", "RIM"};
    static const char *const PERC[5] = {"TOM", "CONGA", "RIM", "COWBELL", "BLIP"};
    static const char *const BWAVE[3] = {"SAW", "SQUARE", "BOTH"};
    static const char *const CHORD[8] = {"MINOR", "MIN 7", "MIN 9", "SUS 4", "FIFTHS", "MAJ 7", "ALIEN", "OCTAVES"};
    static const char *const SWAVE[3] = {"SAW", "PULSE", "SOFT"};
    static const char *const BMODE[2] = {"FM", "PLUCK"};
    t[0] = 0;
    if (id < 0)
        return;
    if (id < KN_SND + TUN_NSND) {
        int k = id - KN_SND;
        if (k == 0 && sel_track == TK_SNARE) { text_cat(t, SNARE[v % 3]); return; }
        if (k == 0 && sel_track == TK_OH) { text_cat(t, v ? "RIDE" : "OPEN"); return; }
        if (k == 0 && sel_track == TK_PERC) { text_cat(t, PERC[v % 5]); return; }
        if (k == 0 && sel_track == TK_BASS) { text_cat(t, BWAVE[v % 3]); return; }
        if (k == 0 && sel_track == TK_STAB) { text_cat(t, CHORD[v & 7]); return; }
        if (k == 6 && sel_track == TK_STAB) { text_cat(t, SWAVE[v % 3]); return; }
        if (k == 7 && sel_track == TK_STAB) { signed_text(v - 2, t); return; }
        if (k == 0 && sel_track == TK_BELL) { text_cat(t, BMODE[v & 1]); return; }
        if (k == 6 && sel_track == TK_BELL) { signed_text(v - 1, t); return; }
        int_text(v, t);
        return;
    }
    if (id == KN_MIX + 1) { signed_text(v - 64, t); return; }
    switch (id) {
    case KN_G + G_SWEEP: if (v == 64) text_cat(t, "OPEN"); else { text_cat(t, v < 64 ? "LP " : "HP "); int_text(v < 64 ? 64 - v : v - 64, t + 3); } return;
    case KN_G + G_SWING: int_text(50 + (v - 50) / 2, t); text_cat(t, "%"); return;
    case KN_G + G_KEY: text_cat(t, KEY[v % 12]); return;
    case KN_G + G_DLY_TIME: text_cat(t, DTIME[v & 7]); return;
    case KN_G + G_LFO_RATE: text_cat(t, LRATE[v % 10]); return;
    case KN_G + G_LFO_DEST: text_cat(t, LDEST[v % 7]); return;
    case KN_G + G_LFO_SHAPE: text_cat(t, LSHAPE[v & 3]); return;
    case KN_RATE: text_cat(t, RATE[v & 3]); return;
    case KN_ROT: text_cat(t, "< >"); return;
    case KN_ROOT: case KN_NOTE: signed_text(v, t); return;
    case KN_VEL: text_cat(t, VEL[v & 3]); return;
    case KN_RAT: int_text(v + 1, t); text_cat(t, "x"); return;
    case KN_PROB: text_cat(t, PROB[v & 3]); return;
    case KN_CLKSRC: text_cat(t, CLK[v % 3]); return;
    case KN_CLKOUT: text_cat(t, v ? "ON" : "OFF"); return;
    case KN_MIDICH: if (v) int_text(v, t); else text_cat(t, "OMNI"); return;
    case KN_DRUMCH: int_text(v, t); return;
    default:
        if (id >= KN_DECK && id < KN_DECK + NDECKS) {
            preset_label(v, t);
            return;
        }
        int_text(v, t);
        return;
    }
}

/* ---------------------------------------------------------------------- UI --- */
void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void speak(void)
{
    char t[64], l[8];
    if (ann_on() && ann_where()) {
        preset_label(set.preset, l);
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, l);
        text_cat(t, " ");
        text_cat(t, tun_preset()->name);
        text_cat(t, ". ");
        text_cat(t, PAGES[ui.page].title);
        text_cat(t, ", ");
        text_cat(t, TUN_TRACK_NAME[sel_track]);
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void page_cycle(const uint8_t *pages, int n)
{
    int i, next = pages[0];
    for (i = 0; i < n; i++)
        if (ui.page == pages[i])
            next = pages[(i + 1) % n];
    ui.page = next;
    ui.redraw = 1;
    ui.held_step = -1;
    if (ann_on())
        ann_say(PAGES[ui.page].title, 0, ANN_NOW);
}

static void track_select(int t)
{
    sel_track = (t % TUN_TRACKS + TUN_TRACKS) % TUN_TRACKS;
    ui.redraw = 1;
    app_say("TRACK", TUN_TRACK_NAME[sel_track]);
}

static void save(void)
{
    int u, b, rc;
    tun_preset_t *p = tun_preset();
    if (set.preset >= NFACTORY)
        u = set.preset - NFACTORY;
    else {
        for (u = 0; u < NUSER && user_used(u); u++)
            ;
        if (u == NUSER) {
            app_say("SAVE", "NO FREE SLOT");
            return;
        }
    }
    if (!p->name[0])
        p->name[0] = 'U';
    *user_slot(u) = *p;
    b = u / PER_OBJ;
    rc = plat_store_save(OBJ_USER0 + (uint32_t)b, &ub[b], sizeof ub[b]);
    if (rc == 0) {
        set.preset = (uint8_t)(NFACTORY + u);
        set_dirty = 1;
        rc = plat_store_save(OBJ_SETTINGS, &set, sizeof set);
    }
    if (rc == 0) {
        char l[8];
        preset_label(set.preset, l);
        edit_dirty = 0;
        set_dirty = 0;
        app_say("SAVED", l);
    } else {
        app_say("SAVE", "FAILED");
    }
}

static void morph_set(int v)
{
    char t[8];
    ui.morph = (uint8_t)clamp(v, 0, 127);
    tun_morph(ui.morph);
    int_text(ui.morph, t);
    app_say("TUNNEL", t);
}

static void keys_arp_update(uint32_t keys)
{
    int8_t notes[16];
    int n = 0, k;
    for (k = 0; k < NKEYS && n < 16; k++)
        if (keys >> k & 1u)
            notes[n++] = (int8_t)(k + 12 * ui.oct);
    tun_arp(sel_track, notes, n, ui.arp && ui.keys_mode);
}

static void input(void)
{
    static const uint8_t EDIT_P[] = {PG_SND1, PG_SND2}, FX_P[] = {PG_DLY, PG_REV, PG_MASTER},
                         GLO_P[] = {PG_TEMPO, PG_DECKS, PG_SETUP}, HOME_P[] = {PG_HOME}, SEQ_P[] = {PG_SEQ},
                         MIX_P[] = {PG_MIX}, LFO_P[] = {PG_LFO};
    /* the white keys of the 27: F3 G3 A3 B3 C4 D4 E4 F4 G4 A4 B4 C5 D5 E5 F5 G5 */
    static const uint8_t WHITE[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
    uint32_t keys, btn = plat_buttons(), k, now = plat_ms(), down, up;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    down = keys & ~ui.keys;
    up = ~keys & ui.keys;
    if (ui.keys_mode) {                  /* REC: the keys play the selected track */
        for (k = 0; k < NKEYS; k++) {
            int note = (int)k + 12 * ui.oct;
            if (down >> k & 1u && !ui.arp)
                tun_note_on(sel_track, note, (int)touch_vel(k));
            if (up >> k & 1u && !ui.arp)
                tun_note_off(sel_track, note);
        }
        if (ui.arp && (down || up))
            keys_arp_update(keys);
    } else if (ui.page == PG_SEQ) {      /* the white keys are steps */
        int w;
        for (w = 0; w < 16; w++) {
            uint32_t bit = 1u << WHITE[w];
            if (down & bit) {
                ui.held_step = w;
                ui.step_edited = 0;
            }
            if ((up & bit) && ui.held_step == w) {
                if (!ui.step_edited) {
                    tun_step_t *s = &trk()->st[step_index()];
                    s->f ^= ST_ON;
                    if ((s->f & ST_ON) && !ST_VEL(s->f))
                        s->f |= 1u << 1;
                    edit_dirty = 1;
                }
                ui.held_step = -1;
            }
        }
    } else {                             /* the white keys: mutes, then the tracks */
        int w;
        for (w = 0; w < 16; w++)
            if (down & 1u << WHITE[w]) {
                if (w < 8)
                    tun_mute(w, !tun_muted(w));
                else
                    track_select(w - 8);
            }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_PLAY: tun_play(!tun_playing()); break;
        case B_HOME: page_cycle(HOME_P, 1); break;
        case B_SEQ:
            if (ui.page == PG_SEQ && ui.held_step >= 0) {   /* a held step: slide */
                tun_step_t *s = &trk()->st[step_index()];
                s->f ^= ST_SLIDE;
                ui.step_edited = 1;
                edit_dirty = 1;
                app_say("SLIDE", s->f & ST_SLIDE ? "ON" : "OFF");
            } else {
                page_cycle(SEQ_P, 1);
            }
            break;
        case B_EDIT: page_cycle(EDIT_P, 2); break;
        case B_ENV: page_cycle(MIX_P, 1); break;
        case B_FX: page_cycle(FX_P, 3); break;
        case B_LFO: page_cycle(LFO_P, 1); break;
        case B_GLO: page_cycle(GLO_P, 3); break;
        case B_SEL: track_select(sel_track + 1); break;
        case B_SAVE: save(); break;
        case B_REC:
            ui.keys_mode = (uint8_t)!ui.keys_mode;
            if (!ui.keys_mode)
                tun_arp(sel_track, 0, 0, 0);
            app_say("KEYS", ui.keys_mode ? TUN_TRACK_NAME[sel_track] : "OFF");
            if (FM1B_BLE)
                ui.rec_t = now;
            break;
        case B_ARP:
            ui.arp = (uint8_t)!ui.arp;
            if (ui.arp)
                ui.keys_mode = 1;
            keys_arp_update(keys);
            app_say("ARP", ui.arp ? "ON" : "OFF");
            break;
        case B_OCTDN: case B_OCTUP:
            if (ui.page == PG_SEQ && !ui.keys_mode) {
                ui.steppage = k == B_OCTUP;
                ui.redraw = 1;
            } else {
                char t[4];
                ui.oct = (int8_t)clamp(ui.oct + (k == B_OCTUP ? 1 : -1), -2, 2);
                signed_text(ui.oct, t);
                app_say("OCTAVE", t);
            }
            break;
        default: break;
        }
    }
    if (FM1B_BLE && ui.rec_t) {          /* REC held a second: Bluetooth on / off */
        if (btn >> B_REC & 1u) {
            if (now - ui.rec_t >= 1000u) {
                ui.rec_t = 0;
                ui.keys_mode = 0;
                set.ble_off = (uint8_t)!set.ble_off;
                set_dirty = 1;
                plat_ble_enable(!set.ble_off);
                app_say("BLUETOOTH", set.ble_off ? "OFF" : "ON");
            }
        } else {
            ui.rec_t = 0;
        }
    }
    if ((e = plat_enc(EN_SELECT)) != 0)
        track_select(sel_track + e);
    if ((e = plat_enc(EN_ALGO)) != 0) {
        static uint32_t mt;
        uint32_t dt = now - mt;
        mt = now;
        morph_set(ui.morph + e * (dt < 40u ? 4 : dt < 100u ? 2 : 1));
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        char l[8];
        preset_select(set.preset + e);
        preset_label(set.preset, l);
        app_say(l, preset_at(set.preset)->name);
        plat_midi_send(0x0Cu | (0xC0u | (set.midi_ch ? set.midi_ch - 1u : 0u)) << 8 | (uint32_t)(set.preset & 127) << 16,
                       TO_USB | TO_BLE);
    }
    for (k = 0; k < 4u; k++) {
        int id = PAGES[ui.page].k[k], step;
        uint32_t dt;
        if (ui.page == PG_SEQ && ui.held_step >= 0)
            id = KN_NOTE + (int)k;
        if ((e = plat_enc(EN_K1 + (int)k)) == 0 || id < 0)
            continue;
        dt = now - ui.knob_t[k];
        ui.knob_t[k] = now;
        if (id == KN_ROT) {
            knob_set(id, e > 0 ? 1 : -1);
        } else {
            step = knob_is_list(id) ? 1 : dt < 40u ? 4 : dt < 100u ? 2 : 1;
            knob_set(id, knob_value(id) + e * step);
        }
        if (ui.page == PG_SEQ && ui.held_step >= 0)
            ui.step_edited = 1;
        ui.turned_at[k] = now;
        ui.redraw = 1;
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    {                                    /* the lights */
        static const uint8_t PAGE_BTN[NPAGES] = {
            [PG_HOME] = B_HOME, [PG_SEQ] = B_SEQ, [PG_SND1] = B_EDIT, [PG_SND2] = B_EDIT, [PG_MIX] = B_ENV,
            [PG_DLY] = B_FX, [PG_REV] = B_FX, [PG_MASTER] = B_FX, [PG_LFO] = B_LFO, [PG_TEMPO] = B_GLO,
            [PG_DECKS] = B_GLO, [PG_SETUP] = B_GLO};
        uint32_t lit = btn | 1u << PAGE_BTN[ui.page], klit = 0;
        int beat = tun_beat(), w;
        if (tun_playing() && (beat & 3) == 0)
            lit |= 1u << B_PLAY;
        if (ui.keys_mode)
            lit |= 1u << B_REC;
        if (ui.arp)
            lit |= 1u << B_ARP;
        if (edit_dirty || set_dirty)
            lit |= 1u << B_SAVE;
        if (ui.page == PG_SEQ && !ui.keys_mode) {   /* the steps that are on, and the one playing */
            tun_track_t *t = trk();
            int now_s = tun_step_now(sel_track);
            for (w = 0; w < 16; w++) {
                int i = w + 16 * ui.steppage;
                if (i < t->len && ((t->st[i].f & ST_ON) || i == now_s))
                    klit |= 1u << WHITE[w];
            }
        } else if (!ui.keys_mode) {
            for (w = 0; w < 8; w++)
                if (!tun_muted(w))
                    klit |= 1u << WHITE[w];
            klit |= 1u << WHITE[8 + sel_track];
        } else {
            klit = keys;
        }
        plat_glow(1);
        plat_leds(lit, klit);
    }
}

void app_input_only(void) { input(); }

/* ---------------------------------------------------------------- drawing --- */
/* black glass, cyan for what moves, amber for what is chosen, each track its own colour */
#define T_PANEL RGB(8, 10, 14)
#define T_GRID RGB(30, 36, 46)
#define T_DIM RGB(52, 60, 74)
#define T_CYAN RGB(90, 220, 255)
#define T_AMBER RGB(255, 176, 64)
#define T_RED RGB(240, 70, 60)
#define T_KNOB RGB(34, 40, 52)
static const uint16_t TRACK_RGB[TUN_TRACKS] = {RGB(255, 90, 70), RGB(255, 170, 70), RGB(240, 230, 120),
                                               RGB(170, 230, 120), RGB(90, 220, 170), RGB(90, 200, 255),
                                               RGB(150, 140, 255), RGB(230, 130, 255)};

static void draw_dial(int32_t cx, int32_t cy, int32_t rad, float f, int bip, uint16_t col)
{
    int i;
    float a0 = 0.75f * FM_PI, sweep = 1.5f * FM_PI, at = a0 + sweep * f, from = bip ? 1.5f * FM_PI : a0;
    for (i = 0; i <= 54; i++) {
        float a = a0 + sweep * (float)i / 54.0f, c = fm_cosf(a), s = fm_sinf(a);
        int on = bip ? (a >= fm_minf(from, at) && a <= fm_maxf(from, at)) : a <= at;
        int r;
        for (r = rad - 1; r <= rad + 1; r++)
            cv_pset(cx + (int32_t)(c * (float)r), cy + (int32_t)(s * (float)r), on ? col : T_GRID);
    }
    for (i = 0; i < rad - 4; i++) {
        int32_t h = (int32_t)fm_sqrtf((float)((rad - 4) * (rad - 4) - i * i));
        cv_rect(cx - h, cy - i, 2 * h + 1, 1, T_KNOB);
        cv_rect(cx - h, cy + i, 2 * h + 1, 1, T_KNOB);
    }
    cv_line(cx + (int32_t)(fm_cosf(at) * 4.0f), cy + (int32_t)(fm_sinf(at) * 4.0f),
            cx + (int32_t)(fm_cosf(at) * (float)(rad - 5)), cy + (int32_t)(fm_sinf(at) * (float)(rad - 5)), C_WHITE);
}

static void draw_knob(int32_t x, int32_t y, int32_t w, int id, int lit, uint16_t col)
{
    char t[16];
    int lo = knob_lo(id), hi = knob_hi(id), v = knob_value(id);
    if (id < 0)
        return;
    cv_text(x + (w - text_w(&FONT_XS, knob_name(id))) / 2, y, &FONT_XS, knob_name(id), lit ? C_WHITE : C_GRAY);
    value_text(id, v, t);
    if (knob_is_list(id) || id == KN_ROT || (id >= KN_DECK && id < KN_DECK + NDECKS)) {
        cv_rect(x + 3, y + 30, w - 6, 34, T_KNOB);
        cv_text(x + (w - text_w(&FONT_XS, t)) / 2, y + 40, &FONT_XS, t, col);
        return;
    }
    draw_dial(x + w / 2, y + 46, 22, hi > lo ? (float)(v - lo) / (float)(hi - lo) : 0.0f, knob_bipolar(id), col);
    cv_text(x + (w - text_w(&FONT_XS, t)) / 2, y + 74, &FONT_XS, t, lit ? C_WHITE : C_HI);
}

static void draw_loops(void)             /* HOME: every track's loop, the playheads */
{
    tun_preset_t *p = tun_preset();
    int tr, i;
    for (tr = 0; tr < TUN_TRACKS; tr++) {
        const tun_track_t *t = &p->t[tr];
        int32_t y = 2 + tr * 18, len = t->len, now_s = tun_step_now(tr), cw = len > 16 ? 5 : 10;
        uint16_t col = tun_muted(tr) ? T_DIM : TRACK_RGB[tr];
        char nm[5] = {0};
        for (i = 0; i < 4 && TUN_TRACK_NAME[tr][i]; i++)
            nm[i] = TUN_TRACK_NAME[tr][i];
        if (tr == sel_track)
            cv_rect(0, y - 1, 240, 17, RGB(22, 26, 36));
        cv_text(4, y + 1, &FONT_XS, nm, tr == sel_track ? C_WHITE : col);
        for (i = 0; i < len && i < TUN_STEPS; i++) {
            int32_t x = 40 + i * (cw + 1) * (len > 16 ? 1 : 1);
            const tun_step_t *s = &t->st[i];
            uint16_t c = T_GRID;
            int h = 4;
            if (s->f & ST_ON) {
                c = col;
                h = 6 + 2 * (int)ST_VEL(s->f);
            }
            if (i == now_s)
                c = C_WHITE, h = h < 8 ? 8 : h;
            cv_rect(x, y + 14 - h, cw, h, c);
        }
        {                                /* the level, at the right */
            int32_t lv = (int32_t)(tun_level(tr) * 14.0f);
            cv_rect(232, y + 14 - lv, 4, lv, col);
        }
    }
}

static void draw_steps(void)             /* SEQ: the selected track's 16 steps on show */
{
    tun_track_t *t = trk();
    int w, now_s = tun_step_now(sel_track);
    uint16_t col = TRACK_RGB[sel_track];
    char s[24];
    for (w = 0; w < 16; w++) {
        int i = w + 16 * ui.steppage;
        int32_t x = 4 + w * 14 + (w >= 8 ? 4 : 0), y = 4;
        const tun_step_t *st = &t->st[i];
        uint16_t c = i >= t->len ? RGB(16, 18, 22) : T_GRID;
        if (i < t->len && (st->f & ST_ON))
            c = col;
        cv_rect(x, y, 12, 30, c);
        if (i < t->len && (st->f & ST_ON)) {
            int r, v = (int)ST_VEL(st->f);
            cv_rect(x, y + 30 - 7 * (v + 1), 12, 2, C_WHITE);
            for (r = 0; r < (int)ST_RAT(st->f); r++)
                cv_rect(x + 2 + r * 3, y + 33, 2, 3, T_AMBER);
            if (ST_PROB(st->f))
                cv_rect(x + 4, y + 2, 4, 4, T_PANEL);
            if (st->f & ST_SLIDE)
                cv_rect(x + 9, y + 12, 5, 2, T_AMBER);
        }
        if (i == now_s)
            cv_rect(x, y + 37, 12, 3, C_WHITE);
        if (w == ui.held_step)
            cv_rect(x - 1, y - 2, 14, 2, T_AMBER);
    }
    s[0] = 0;
    text_cat(s, ui.steppage ? "STEPS 17-32" : "STEPS 1-16");
    cv_text(4, 44, &FONT_XS, s, C_GRAY);
}

static void draw(void)
{
    char t[24], l[8];
    uint32_t i, now = plat_ms();
    const page_t *pg = &PAGES[ui.page];
    tun_preset_t *p = tun_preset();
    cv_begin(240, 40, T_PANEL);
    cv_rect(0, 38, 240, 2, T_CYAN);
    preset_label(set.preset, l);
    if (now < ui.msg_until) {
        int32_t x = cv_text(8, 8, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 8, &FONT_B, ui.msg[1], T_AMBER);
    } else {
        int32_t x = cv_text(8, 4, &FONT_B, l, T_AMBER);
        x = cv_text(x + 8, 4, &FONT_B, p->name, C_WHITE);
        if (edit_dirty)
            cv_text(x + 2, 4, &FONT_B, "*", T_AMBER);
        t[0] = 0;
        text_cat(t, pg->title);
        if (ui.page != PG_HOME && ui.page < PG_DLY) {
            text_cat(t, "  ");
            text_cat(t, TUN_TRACK_NAME[sel_track]);
        }
        cv_text(8, 24, &FONT_XS, t, ui.page != PG_HOME && ui.page < PG_DLY ? TRACK_RGB[sel_track] : C_GRAY);
        t[0] = 0;
        int_text(p->g[G_BPM], t);
        text_cat(t, tun_playing() ? " BPM >" : " BPM");
        cv_text(232 - text_w(&FONT_XS, t), 24, &FONT_XS, t, tun_playing() ? T_CYAN : C_GRAY);
    }
    cv_blit(0, 0);
    cv_begin(240, 150, T_PANEL);
    if (ui.page == PG_HOME) {
        draw_loops();
    } else if (ui.page == PG_SEQ) {
        int held = ui.held_step >= 0;
        draw_steps();
        for (i = 0; i < 4u; i++)
            draw_knob(4 + (int32_t)i * 58, 58, 56, held ? KN_NOTE + (int)i : pg->k[i],
                      ui.turned_at[i] && now - ui.turned_at[i] < 600u, held ? T_AMBER : TRACK_RGB[sel_track]);
    } else {
        for (i = 0; i < 4u; i++) {
            int id = pg->k[i];
            uint16_t col = ui.page <= PG_MIX ? TRACK_RGB[sel_track] : T_CYAN;
            if (id >= KN_DECK && id < KN_DECK + NDECKS && tun_deck_pending(id - KN_DECK))
                col = T_AMBER;
            draw_knob(4 + (int32_t)i * 58, 20, 56, id, ui.turned_at[i] && now - ui.turned_at[i] < 600u, col);
        }
        if (ui.page == PG_DECKS) {
            int d;
            for (d = 0; d < NDECKS; d++) {
                const char *n = preset_at(deck_of[d])->name;
                cv_text(4 + d * 58 + (56 - text_w(&FONT_XS, n)) / 2 > 0 ? 4 + d * 58 : 4, 110, &FONT_XS, n,
                        tun_deck_pending(d) ? T_AMBER : C_GRAY);
            }
        }
    }
    cv_blit(0, 44);
    cv_begin(240, 44, T_PANEL);
    cv_rect(0, 0, 240, 1, C_LINE);
    {                                    /* the bar: sixteen lights */
        int beat = tun_beat();
        for (i = 0; i < 16u; i++)
            cv_rect(6 + (int32_t)i * 9, 10, 7, 7, (int)i == beat ? T_CYAN : (i % 4u) ? T_GRID : T_DIM);
    }
    t[0] = 0;
    text_cat(t, "TUNNEL ");
    int_text(ui.morph, t + 7);
    cv_text(158, 6, &FONT_XS, t, ui.morph ? T_CYAN : C_DIM);
    t[0] = 0;
    if (ui.keys_mode) {
        text_cat(t, ui.arp ? "ARP " : "KEYS ");
        text_cat(t, TUN_TRACK_NAME[sel_track]);
    } else if (tun_deck_pending(0) || tun_deck_pending(1) || tun_deck_pending(2)) {
        text_cat(t, "ON THE BAR");
    }
    cv_text(6, 24, &FONT_XS, t, T_AMBER);
    cv_blit(0, 196);
}

/* ----------------------------------------------------------------- splash --- */
/* the boot screen: a tunnel of frames rushing to a vanishing point, scan lines, the name. Two bands of 120 rows */
static void splash_band(int32_t y0, int32_t h)
{
    int32_t y, i;
    cv_begin(240, (uint32_t)h, RGB(4, 6, 10));
    cv_oy = -y0;
    cv_y0 = 0;
    cv_y1 = h;
    for (i = 0; i < 14; i++) {           /* the frames: each nearer one bigger, brighter */
        float f = (float)i / 13.0f, s = 6.0f + 114.0f * f * f;
        int32_t cx = 120, cy = 104, hw = (int32_t)s, hh = (int32_t)(s * 0.78f);
        int c = 40 + (int)(200.0f * f);
        uint16_t col = RGB(c / 3, c * 4 / 5, c);
        cv_rect(cx - hw, cy - hh, 2 * hw, 1, col);
        cv_rect(cx - hw, cy + hh, 2 * hw, 1, col);
        cv_rect(cx - hw, cy - hh, 1, 2 * hh, col);
        cv_rect(cx + hw - 1, cy - hh, 1, 2 * hh, col);
    }
    for (i = 0; i < 4; i++) {            /* the rails into the dark */
        int32_t ex = i < 2 ? 0 : 239, ey = (i & 1) ? 196 : 12;
        cv_line(120, 104, ex, ey, RGB(30, 80, 100));
    }
    for (y = y0; y < y0 + h; y += 3)     /* scan lines */
        cv_rect(0, y, 240, 1, RGB(0, 0, 0));
    {
        const char *n = "TUNNEL";
        int32_t w = text_w(&FONT_L, n), x = 120 - w / 2;
        cv_rect(x - 8, 84, w + 16, 40, RGB(4, 6, 10));
        cv_text(x + 2, 88, &FONT_L, n, RGB(0, 90, 120));
        cv_text(x, 86, &FONT_L, n, RGB(120, 230, 255));
    }
    {
        const char *s = "HYPNOTIC TECHNO  FM-1";
        int32_t w = text_w(&FONT_XS, s);
        cv_text(120 - w / 2, 214, &FONT_XS, s, RGB(255, 176, 64));
    }
    for (i = 0; i < 16; i++)             /* sixteen steps, the first of each beat lit */
        cv_rect(16 + i * 13, 234, 9, 4, (i % 4) ? RGB(30, 40, 52) : RGB(90, 220, 255));
    cv_oy = 0;
    cv_blit(0, (uint32_t)y0);
}

void app_splash(void)
{
    splash_band(0, 120);
    splash_band(120, 120);
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, T_PANEL);
    ui.page = PG_HOME;
    ui.redraw = 1;
}

void app_frame(void)
{
    static uint32_t last_foot, last_msg;
    uint32_t now = plat_ms();
    int pc = pc_pending;
    if (pc >= 0) {
        pc_pending = -1;
        preset_select(pc);
        ui.redraw = 1;
    }
    if (morph_in >= 0) {
        ui.morph = (uint8_t)morph_in;
        morph_in = -1;
    }
    if (ui_poke) {
        ui_poke = 0;
        ui.redraw = 1;
    }
    input();
    speak();
    if (now - last_foot >= 33u) {        /* the loops move: redraw about 30 times a second */
        last_foot = now;
        ui.redraw = 1;
    }
    if (ui.msg_until && now >= ui.msg_until && last_msg != ui.msg_until) {
        last_msg = ui.msg_until;
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "preset"))
        *got = set.preset;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "track"))
        *got = sel_track;
    else if (!strcmp(what, "playing"))
        *got = tun_playing();
    else if (!strcmp(what, "dirty"))
        *got = edit_dirty;
    else if (!strcmp(what, "morph"))
        *got = ui.morph;
    else if (!strcmp(what, "keys"))
        *got = ui.keys_mode;
    else if (!strncmp(what, "deck", 4))
        *got = deck_of[atoi(what + 4) % NDECKS];
    else if (!strncmp(what, "mute", 4))
        *got = tun_muted(atoi(what + 4));
    else if (!strncmp(what, "len", 3))
        *got = tun_preset()->t[atoi(what + 3) & 7].len;
    else if (!strncmp(what, "on", 2) && what[2] >= '0' && what[2] <= '7') {   /* onT.S: track T step S on */
        int tr = what[2] - '0', s = atoi(what + 4);
        *got = tun_preset()->t[tr].st[s & 31].f & ST_ON;
    } else if (!strncmp(what, "step", 4))
        *got = tun_step_now(atoi(what + 4) & 7);
    else if (!strncmp(what, "g", 1) && what[1] >= '0' && what[1] <= '9')
        *got = tun_preset()->g[atoi(what + 1) % G_N];
    else
        return 0;
    return 1;
}
#endif
