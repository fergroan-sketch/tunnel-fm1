/* SPDX-License-Identifier: MPL-2.0 */
/* TUNNEL FM-1's engine (tun.h): the eight voices, the sequencer, the decks and the effects.
 *
 *   KICK    a sine swept down from PUNCH above its pitch, a click, drive
 *   SNARE   snare (two tones and bright noise), clap (three noise bursts and a tail) or rim, by MODE
 *   HATS    six square oscillators at inharmonic ratios through a band-pass and a high-pass; the closed hat
 *           chokes the open one; OPEN / RIDE has a ride mode (higher, longer, a bell)
 *   PERC    tom, conga, rim, cowbell or blip, by MODE
 *   BASS    one voice: saw, square or both and a sub, a resonant four-pole ladder, an accent, slides
 *   STAB    a chord of up to four notes, two detuned saws each, a resonant low-pass with a short envelope
 *   BELL    four voices: FM (a sine on a sine) or a pluck (triangle and a triangle an octave up, a dark low-pass),
 *           with a slow warble
 *   SYNTH   four voices in the manner of a mid-80s one-oscillator polysynth: saw, pulse, a swept pulse, a swept
 *           saw (the "hoover"), with a square sub, or sines; a resonant 24 dB low-pass, ADSR, and a stereo chorus
 * then:     each track's level and pan to the mix, its sends to a ping-pong delay and a reverb; the kick ducks the
 *           melodic tracks and the returns; the mix, the returns, the DJ filter, the compressor, the drive, the
 *           volume.
 *
 * The sequencer counts 96 ticks a beat (internal, or 4 to each MIDI clock). Each track keeps its own step count and
 * plays step (count mod its length) at its own rate, so a 7-step loop rolls against a 16-step one. A deck's load and
 * a new preset wait for the next bar. Control-rate parts (envelopes, the clock, the LFO) run every CR samples. */
#include "tun.h"
#include "fastmath.h"
#include "plat.h"

#define SYN_NWAVE 8                      /* the synth's waves (synth_render) */
#define CR 32
#define INV_SR (1.0f / TUN_SR)
#define CR_DT ((float)CR * INV_SR)
#define PPQN 96
#define BAR (4 * PPQN)

const char *const TUN_TRACK_NAME[TUN_TRACKS] = {"KICK", "SNARE", "CL HAT", "OP HAT", "PERC", "BASS", "STAB", "BELL",
                                                "SYNTH"};
const char *const TUN_SND_NAME[TUN_TRACKS][TUN_NSND] = {
    {"TUNE", "DECAY", "PUNCH", "SWEEP", "CLICK", "DRIVE", "LOW CUT", "TAIL"},
    {"MODE", "TUNE", "DECAY", "NOISE", "SNAP", "TONE", "DRIVE", "SPREAD"},
    {"TUNE", "DECAY", "TONE", "METAL", "NOISE", "-", "-", "-"},
    {"MODE", "TUNE", "DECAY", "TONE", "METAL", "BELL", "-", "-"},
    {"MODE", "TUNE", "DECAY", "BEND", "TONE", "DRIVE", "-", "-"},
    {"WAVE", "CUTOFF", "RES", "ENV", "DECAY", "ACCENT", "DRIVE", "GLIDE"},
    {"CHORD", "CUTOFF", "RES", "ENV", "DECAY", "DETUNE", "WAVE", "OCTAVE"},
    {"MODE", "RATIO", "TONE", "DECAY", "CUTOFF", "WARBLE", "OCTAVE", "SPREAD"},
    {"WAVE", "CUTOFF", "RES", "ENV", "ATTACK", "DECAY", "SUSTAIN", "CHORUS"},
};
const uint8_t TUN_SND_MAX[TUN_TRACKS][TUN_NSND] = {
    {127, 127, 127, 127, 127, 127, 127, 127},
    {2, 127, 127, 127, 127, 127, 127, 127},
    {127, 127, 127, 127, 127, 0, 0, 0},
    {1, 127, 127, 127, 127, 127, 0, 0},
    {5, 127, 127, 127, 127, 127, 0, 0},  /* PERC: tom conga rim cowbell blip sine */
    {4, 127, 127, 127, 127, 127, 127, 127},   /* BASS: saw square both sine sine+saw */
    {7, 127, 127, 127, 127, 127, 3, 4},  /* STAB WAVE: saw pulse soft sine */
    {1, 15, 127, 127, 127, 127, 4, 127},
    {SYN_NWAVE - 1, 127, 127, 127, 127, 127, 127, 127},
};
const char *const TUN_G_NAME[G_N] = {"BPM", "SWING", "KEY", "TIME", "FEEDBACK", "TONE", "SIZE", "DAMP", "SWEEP",
                                     "DRIVE", "VOLUME", "RATE", "DEPTH", "DEST", "SHAPE", "COMP", "DUCK",
                                     "DUCK REL"};
const uint8_t TUN_G_MAX[G_N] = {200, 100, 11, 7, 127, 127, 127, 127, 127, 127, 127, 9, 127, 7, 3, 127, 127, 127};
const uint8_t TUN_G_MIN[G_N] = {60, 50, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

/* ------------------------------------------------------------------ helpers --- */
static inline float rnd(uint32_t *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(int32_t)*s * (1.0f / 2147483648.0f);
}
static inline float sat(float x)
{
    float x2;
    if (x > 3.0f)
        return 1.0f;
    if (x < -3.0f)
        return -1.0f;
    x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
static inline float wrap1(float x) { return x - fm_floorf(x); }
static inline float mtof(float n) { return 440.0f * fm_exp2f((n - 69.0f) * (1.0f / 12.0f)); }
static float coef(float t)               /* a per-sample step that is 99% there in t seconds */
{
    float k = INV_SR * 4.6f / t;
    return k >= 1.0f ? 1.0f : 1.0f - fm_expf(-k);
}
static float decay_mul(float t)          /* per-sample multiplier that falls 60 dB in t seconds */
{
    return fm_expf(-6.9f * INV_SR / t);
}
static float t_of(int v, float lo, float hi)   /* 0..127 as a time, exponential */
{
    return lo * fm_exp2f((float)v * (1.0f / 127.0f) * fm_log2f(hi / lo));
}
static float kn(int v) { return (float)v * (1.0f / 127.0f); }
static inline float blep(float t, float dt)    /* polyBLEP */
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

/* a state-variable filter, trapezoidal (zero-delay feedback: stable at any cutoff). f: svf_f(hz); q: damping, 2 is
 * none, towards 0 rings */
typedef struct {
    float lp, bp;                        /* the two integrators' states */
} svf_t;
static inline float svf_run(svf_t *s, float x, float g, float k, float *bp)
{
    float v1 = (s->bp + g * (x - s->lp)) / (1.0f + g * (g + k)), v2 = s->lp + g * v1;
    s->bp = 2.0f * v1 - s->bp;
    s->lp = 2.0f * v2 - s->lp;
    *bp = v1;
    return v2;
}
static inline float svf_lp(svf_t *s, float x, float g, float k)
{
    float bp;
    return svf_run(s, x, g, k, &bp);
}
static inline float svf_bp(svf_t *s, float x, float g, float k)
{
    float bp;
    svf_run(s, x, g, k, &bp);
    return bp;
}
static inline float svf_f(float hz)
{
    return fm_tanf(FM_PI * fm_clampf(hz, 10.0f, 0.45f * TUN_SR) * INV_SR);
}

/* ------------------------------------------------------------------- state --- */
static tun_preset_t P;
static tun_preset_t deck_next[NDECKS], preset_next;
static volatile uint8_t deck_wait[NDECKS], preset_wait;
static tun_perf_t F = {2, 1};
static float morph;
static uint16_t mutes;

/* drums */
typedef struct {
    float env, envm, penv, penvm, ph, f0, click, vel, tail, tailm;
    float burst_t;                       /* clap: time since the hit */
    int mode;
    svf_t f1, f2;
    float hp;
} drum_t;
static drum_t D[5];
static float hat_ph[6];
static uint32_t nrng = 0x2468ACE1u;

/* bass */
typedef struct {
    float ph, sub, pitch, target, glide, gate_left;
    float aenv, fenv, acc, vel;
    float s[4];
    int on;
} bass_t;
static bass_t B;

/* stab and bell voices */
#define NSTAB 4
#define NBELL 4
typedef struct {
    float ph[2], inc[2], env, envm, fenv, fenvm, vel;
    svf_t f;
    int on;
} stab_t;
static stab_t S[NSTAB];
typedef struct {
    float cph, mph, inc, env, envm, ienv, ienvm, vel, pan;
    svf_t f;
    int note;
    uint32_t age;
} bell_t;
static bell_t L[NBELL];
static uint32_t bell_age;
static float warble_ph;

/* the synth: four voices, one oscillator each (and its sub), a 24 dB low-pass of two state-variable stages */
#define NSYN 4
typedef struct {
    float ph, sub, inc, scoop;           /* scoop: the swept saw's pitch rising into the note, semitones */
    float env, fenv, vel, gate_left;
    int stage;                           /* 0 idle, 1 attack, 2 decay / sustain, 3 release */
    int note;
    uint32_t age;
    svf_t f1, f2;
} syn_t;
static syn_t Y[NSYN];
static uint32_t syn_age;
static float pwm_ph;
#define CHO_LEN 2048u                    /* the chorus: 46 ms */
#define CHO_MASK (CHO_LEN - 1u)
static PLAT_POOL float cho_mem[CHO_LEN];
static uint32_t cho_w;
static float cho_ph;

/* the duck: the kick's envelope, which the melodic tracks and the returns give way to */
static float duck_env;

/* the clock and the sequencer */
static int playing;
static float tick_frac;                  /* the part of a tick owed (single precision: the FM-1's FPU) */
static uint32_t tick_i;                  /* whole ticks done */
static uint32_t sn[TUN_TRACKS];          /* steps started a track */
static int step_now[TUN_TRACKS];
static uint8_t rat_left[TUN_TRACKS];
static uint32_t rat_next[TUN_TRACKS], rat_gap[TUN_TRACKS];
static float rat_vel[TUN_TRACKS];
static int rat_note[TUN_TRACKS];
static uint32_t srng = 0x13579BDFu;
static int clock_seen;
static uint32_t midi_ticks;              /* MIDI clocks owed (4 ticks each) */
static uint32_t last_f8_t, sample_t;
#define RTQ 64
static volatile uint8_t rtq[RTQ];
static volatile uint32_t rt_w, rt_r;
static uint32_t out_div;                 /* ticks to the next clock out */
static float lfo_ph, lfo_val, lfo_sh;
/* the keys' arpeggiator: the notes held, played a sixteenth each on the chosen track, up */
static volatile int8_t arp_notes[16];
static volatile uint8_t arp_n, arp_track, arp_on;
static float arp_acc;
static int arp_pos;

/* live notes from the keys: the event queue */
#define EVQ 64
static volatile uint32_t evq[EVQ];
static volatile uint32_t ev_w, ev_r;
enum { EV_ON = 1, EV_OFF, EV_PLAY, EV_STOP, EV_RT, EV_PANIC };
static void ev_push(uint32_t t, uint32_t a, uint32_t b)
{
    uint32_t w = ev_w;
    if (w - ev_r >= EVQ)
        return;
    evq[w % EVQ] = t | (a & 0xFFFu) << 8 | (b & 0xFFu) << 20 | 0u;
    ev_w = w + 1u;
}

static void rt_out(uint8_t b)
{
    uint32_t w = rt_w;
    if (w - rt_r >= RTQ)
        return;
    rtq[w % RTQ] = b;
    rt_w = w + 1u;
}

static int use_midi(void) { return F.clock_src == 1 || (F.clock_src == 2 && clock_seen); }
static float bpm(void)
{
    return (float)(P.g[G_BPM] < 60 ? 60 : P.g[G_BPM]);
}
static uint32_t step_ticks(int div)
{
    static const uint8_t T[4] = {24, 48, 12, 16};
    return T[div & 3];
}

/* ------------------------------------------------------------------ triggers --- */
static float vel_of(int v) { static const float V[4] = {0.45f, 0.72f, 1.0f, 1.0f}; return V[v & 3]; }

static void kick_hit(float vel)
{
    drum_t *d = &D[TK_KICK];
    const uint8_t *s = P.t[TK_KICK].snd;
    d->vel = vel;
    d->f0 = 32.0f * fm_exp2f(kn(s[0]) * 1.6f);              /* 32 .. 97 Hz */
    d->env = 1.0f;
    d->envm = decay_mul(t_of(s[1], 0.08f, 1.6f));
    d->penv = kn(s[2]) * 5.0f;                              /* octaves above */
    d->penvm = decay_mul(t_of(s[3], 0.008f, 0.25f));
    d->click = kn(s[4]);
    d->ph = 0.0f;
    d->tail = kn(s[7]) * 0.5f;
    d->burst_t = 0.0f;
    d->tailm = decay_mul(t_of(s[1], 0.3f, 3.0f));
    if (vel > 0.3f)                      /* a ghost kick doesn't pump */
        duck_env = 1.0f;
}
static void snare_hit(float vel)
{
    drum_t *d = &D[TK_SNARE];
    const uint8_t *s = P.t[TK_SNARE].snd;
    d->vel = vel;
    d->mode = s[0];
    d->f0 = 150.0f * fm_exp2f(kn(s[1]) * 1.5f);
    d->env = 1.0f;
    d->envm = decay_mul(t_of(s[2], d->mode == 2 ? 0.02f : 0.06f, d->mode == 1 ? 1.2f : 0.7f));
    d->penv = 1.0f;
    d->penvm = decay_mul(0.03f);
    d->burst_t = 0.0f;
    d->ph = 0.0f;
}
static void hat_hit(int tr, float vel)
{
    drum_t *d = &D[tr];
    const uint8_t *s = P.t[tr].snd;
    int ride = tr == TK_OH && s[0] == 1;
    d->vel = vel;
    d->mode = ride;
    d->env = 1.0f;
    if (tr == TK_CH) {
        d->envm = decay_mul(t_of(s[1], 0.02f, 0.4f));
        D[TK_OH].envm *= 0.0f;           /* the closed hat chokes the open one */
        D[TK_OH].env *= 0.05f;
    } else {
        d->envm = decay_mul(t_of(s[2], ride ? 0.4f : 0.12f, ride ? 4.0f : 1.6f));
    }
    d->penv = 1.0f;                      /* the stick's tick */
    d->penvm = decay_mul(0.004f);
}
static void perc_hit(float vel, int note)
{
    drum_t *d = &D[TK_PERC];
    const uint8_t *s = P.t[TK_PERC].snd;
    static const float BASE[6] = {80.0f, 220.0f, 900.0f, 540.0f, 400.0f, 160.0f};
    d->vel = vel;
    d->mode = s[0];
    d->f0 = BASE[d->mode % 6] * fm_exp2f(kn(s[1]) * 2.0f - 1.0f + (float)note * (1.0f / 12.0f));
    d->env = 1.0f;
    d->envm = decay_mul(t_of(s[2], d->mode == 2 ? 0.01f : 0.04f, d->mode == 0 ? 1.4f : 0.6f));
    d->penv = kn(s[3]) * 2.0f;
    d->penvm = decay_mul(d->mode == 4 ? 0.02f : 0.05f);
    d->ph = d->hp = 0.0f;
    d->tail = 0.0f;
}

static float key_note(int track, int note) { return 36.0f + (float)P.g[G_KEY] + (float)P.t[track].root + (float)note; }

static void bass_hit(float vel, int note, int slide, float gate)
{
    const uint8_t *s = P.t[TK_BASS].snd;
    float n = key_note(TK_BASS, note);
    B.target = n;
    B.vel = vel;
    B.acc = vel > 0.9f ? kn(s[5]) : 0.0f;
    B.gate_left = gate;
    if (slide && B.on) {
        B.glide = 0.004f + kn(s[7]) * 0.01f;
        return;                          /* a slide: the pitch moves, the envelopes go on */
    }
    B.glide = kn(s[7]) > 0.02f ? kn(s[7]) * 0.02f : 1.0f;
    if (!B.on)
        B.pitch = n;
    B.on = 1;
    B.aenv = 1.0f;
    B.fenv = 1.0f;
}

/* chords: semitones above the root */
static const int8_t CHORDS[8][4] = {
    {0, 3, 7, 12},                       /* minor */
    {0, 3, 7, 10},                       /* minor 7 */
    {0, 3, 10, 14},                      /* minor 9, no fifth */
    {0, 5, 7, 12},                       /* sus 4 */
    {0, 7, 12, 19},                      /* fifths */
    {0, 4, 7, 11},                       /* major 7 */
    {0, 3, 7, 13},                       /* minor, flat-9 detuned top: the alien one */
    {0, 12, 19, 24},                     /* octaves */
};
static void stab_hit(float vel, int note)
{
    const uint8_t *s = P.t[TK_STAB].snd;
    int i, ch = s[0] & 7, oct = s[7] - 2;
    float det = kn(s[5]) * 0.35f, base = key_note(TK_STAB, note) + 12.0f * (float)oct + 12.0f;
    for (i = 0; i < NSTAB; i++) {
        stab_t *v = &S[i];
        float n = base + (float)CHORDS[ch][i];
        v->inc[0] = mtof(n - det) * INV_SR;
        v->inc[1] = mtof(n + det) * INV_SR;
        v->env = 1.0f;
        v->envm = decay_mul(t_of(s[4], 0.06f, 1.5f) * (0.6f + 0.4f * vel));
        v->fenv = 1.0f;
        v->fenvm = decay_mul(t_of(s[4], 0.03f, 0.8f) * (0.5f + 0.5f * vel));
        v->vel = vel;
        v->on = 1;
    }
}
static void bell_hit(float vel, int note)
{
    const uint8_t *s = P.t[TK_BELL].snd;
    int i, best = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    bell_t *v;
    for (i = 0; i < NBELL; i++)
        if (L[i].env < 0.0005f) {
            best = i;
            oldest = 0;
            break;
        } else if (L[i].age < oldest) {
            oldest = L[i].age;
            best = i;
        }
    v = &L[best];
    v->note = note;
    v->age = ++bell_age;
    v->inc = mtof(key_note(TK_BELL, note) + 12.0f * (float)(s[6] - 1) + 12.0f) * INV_SR;
    v->env = 1.0f;
    v->envm = decay_mul(t_of(s[3], 0.08f, 3.0f));
    v->ienv = 1.0f;
    v->ienvm = decay_mul(t_of(s[3], 0.03f, 1.0f) * (0.4f + 0.6f * vel));
    v->vel = vel;
    v->pan = (rnd(&srng)) * kn(s[7]);
    v->cph = v->mph = 0.0f;
}

static void synth_hit(float vel, int note, float gate)
{
    const uint8_t *s = P.t[TK_SYNTH].snd;
    int i, best = 0;
    uint32_t oldest = 0xFFFFFFFFu;
    syn_t *v;
    for (i = 0; i < NSYN; i++)           /* a free voice, else the oldest */
        if (!Y[i].stage) {
            best = i;
            break;
        } else if (Y[i].age < oldest) {
            oldest = Y[i].age;
            best = i;
        }
    v = &Y[best];
    v->note = note;
    v->age = ++syn_age;
    v->inc = mtof(key_note(TK_SYNTH, note) + 12.0f) * INV_SR;
    v->scoop = s[0] == 3 ? -5.0f : 0.0f;   /* the swept saw rises into its note */
    v->vel = vel;
    v->gate_left = gate;
    v->stage = 1;
    v->fenv = 0.0f;
    if (v->env < 0.0001f) {
        v->ph = 0.0f;
        v->sub = 0.0f;
    }
}
static void synth_release(int note)
{
    int i;
    for (i = 0; i < NSYN; i++)
        if (Y[i].stage && Y[i].stage < 3 && Y[i].note == note)
            Y[i].gate_left = 0.0f;
}

static void trigger(int tr, float vel, int note, int slide, float gate)
{
    switch (tr) {
    case TK_KICK: kick_hit(vel); break;
    case TK_SNARE: snare_hit(vel); break;
    case TK_CH: case TK_OH: hat_hit(tr, vel); break;
    case TK_PERC: perc_hit(vel, note); break;
    case TK_BASS: bass_hit(vel, note, slide, gate); break;
    case TK_STAB: stab_hit(vel, note); break;
    case TK_SYNTH: synth_hit(vel, note, gate); break;
    default: bell_hit(vel, note); break;
    }
}

/* ---------------------------------------------------------------- sequencer --- */
static uint32_t swing_of(uint32_t n, uint32_t dur, int div)
{
    int sw = P.g[G_SWING];
    if (div == 2 || div == 3 || !(n & 1u) || sw <= 50)
        return 0;
    return (uint32_t)((float)dur * (float)(sw - 50) * 0.01f);
}

static void step_fire(int tr)
{
    tun_track_t *t = &P.t[tr];
    int len = t->len < 1 ? 1 : t->len > TUN_STEPS ? TUN_STEPS : t->len, idx = (int)(sn[tr] % (uint32_t)len);
    const tun_step_t *st = &t->st[idx];
    uint32_t dur = step_ticks(t->div);
    step_now[tr] = idx;
    if (!(st->f & ST_ON) || (mutes >> tr & 1u))
        return;
    {
        static const float PR[4] = {1.0f, 0.75f, 0.5f, 0.25f};
        if (PR[ST_PROB(st->f)] < 1.0f && 0.5f + 0.5f * rnd(&srng) > PR[ST_PROB(st->f)])
            return;
    }
    {
        float vel = vel_of((int)ST_VEL(st->f)), gate;
        int rat = (int)ST_RAT(st->f) + 1;
        const tun_step_t *nx = &t->st[(idx + 1) % len];
        gate = (float)dur * 0.55f * 60.0f / (bpm() * PPQN);   /* seconds */
        if (tr == TK_BASS && (nx->f & ST_SLIDE) && (nx->f & ST_ON))
            gate *= 1.9f;
        if (tr == TK_SYNTH) {            /* the synth holds its note until the next step that plays */
            int k = 1;
            while (k < len && !(t->st[(idx + k) % len].f & ST_ON))
                k++;
            gate = ((float)(k * (int)dur) - 4.0f) * 60.0f / (bpm() * PPQN) / (float)((int)ST_RAT(st->f) + 1);
        }
        trigger(tr, vel, st->note, (st->f & ST_SLIDE) != 0, gate);
        if (rat > 1) {
            rat_left[tr] = (uint8_t)(rat - 1);
            rat_gap[tr] = dur / (uint32_t)rat;
            rat_next[tr] = tick_i + rat_gap[tr];
            rat_vel[tr] = vel * 0.8f;
            rat_note[tr] = st->note;
        }
    }
}

static void deck_apply(void)
{
    static const uint8_t FIRST[NDECKS] = {0, 5, 7}, LAST[NDECKS] = {4, 6, 8};
    int d, t;
    if (preset_wait) {
        P = preset_next;
        preset_wait = 0;
        for (d = 0; d < NDECKS; d++)
            deck_wait[d] = 0;
        return;
    }
    for (d = 0; d < NDECKS; d++)
        if (deck_wait[d]) {
            for (t = FIRST[d]; t <= LAST[d]; t++)
                P.t[t] = deck_next[d].t[t];
            deck_wait[d] = 0;
        }
}

static void tick_one(void)               /* one sequencer tick: the steps due now */
{
    int tr;
    if (tick_i % BAR == 0u)
        deck_apply();
    for (tr = 0; tr < TUN_TRACKS; tr++) {
        tun_track_t *t = &P.t[tr];
        uint32_t dur = step_ticks(t->div), due = sn[tr] * dur + swing_of(sn[tr], dur, t->div);
        if (rat_left[tr] && tick_i >= rat_next[tr]) {
            trigger(tr, rat_vel[tr], rat_note[tr], 0, (float)rat_gap[tr] * 0.5f * 60.0f / (bpm() * PPQN));
            rat_left[tr]--;
            rat_next[tr] += rat_gap[tr];
        }
        if (tick_i >= due) {
            step_fire(tr);
            sn[tr]++;
        }
    }
    if (!use_midi() && F.clock_out && out_div++ % 4u == 0u)
        rt_out(0xF8);
    tick_i++;
}

static void transport(int on)
{
    int i;
    if (on && !playing) {
        tick_frac = 1.0f;                /* the first tick at once */
        tick_i = 0;
        out_div = 0;
        for (i = 0; i < TUN_TRACKS; i++) {
            sn[i] = 0;
            rat_left[i] = 0;
        }
        if (!use_midi() && F.clock_out)
            rt_out(0xFA);
    } else if (!on && playing) {
        for (i = 0; i < TUN_TRACKS; i++)
            step_now[i] = -1;
        B.gate_left = 0.0f;
        if (!use_midi() && F.clock_out)
            rt_out(0xFC);
        if (preset_wait || deck_wait[0] || deck_wait[1] || deck_wait[2])
            deck_apply();                /* stopped: what waits for the bar comes now */
    }
    playing = on;
}

static void events(void)
{
    while (ev_r != ev_w) {
        uint32_t e = evq[ev_r % EVQ], t = e & 0xFFu, a = (e >> 8) & 0xFFFu, b = (e >> 20) & 0xFFu;
        ev_r++;
        switch (t) {
        case EV_ON: {
            int tr = (int)(a >> 8), note = (int)(a & 0xFFu) - 64;
            trigger(tr, (float)b * (1.0f / 127.0f), note, 0, 30.0f);
            break;
        }
        case EV_OFF:
            if ((int)(a >> 8) == TK_BASS)
                B.gate_left = 0.0f;
            else if ((int)(a >> 8) == TK_SYNTH)
                synth_release((int)(a & 0xFFu) - 64);
            break;
        case EV_PLAY: transport(1); break;
        case EV_STOP: transport(0); break;
        case EV_RT:
            if (b == 0xF8) {
                clock_seen = 1;
                last_f8_t = sample_t;
                if (use_midi() && playing)
                    midi_ticks += 4;
            } else if (b == 0xFA) {
                if (use_midi()) {
                    transport(0);
                    transport(1);
                }
            } else if (b == 0xFB) {
                if (use_midi())
                    playing = 1;
            } else if (b == 0xFC) {
                if (use_midi())
                    transport(0);
            }
            break;
        case EV_PANIC: {
            int i;
            transport(0);
            for (i = 0; i < 5; i++)
                D[i].env = 0.0f;
            B.aenv = 0.0f;
            B.on = 0;
            for (i = 0; i < NSTAB; i++)
                S[i].env = 0.0f;
            for (i = 0; i < NBELL; i++)
                L[i].env = 0.0f;
            for (i = 0; i < NSYN; i++) {
                Y[i].env = 0.0f;
                Y[i].stage = 0;
            }
            duck_env = 0.0f;
            break;
        }
        default: break;
        }
    }
}

/* ---------------------------------------------------------------- the LFO --- */
static void lfo_run(void)
{
    static const float BEATS[10] = {8.0f, 4.0f, 2.0f, 1.5f, 1.0f, 0.75f, 0.5f, 0.375f, 0.25f, 0.125f};
    float beats = BEATS[P.g[G_LFO_RATE] % 10], per = beats * 60.0f / bpm(), was = lfo_ph;
    if (playing) {                       /* locked to the bar while playing */
        uint32_t len = (uint32_t)(beats * (float)PPQN);
        lfo_ph = (float)(tick_i % len) / (float)len;
    } else {
        lfo_ph += CR_DT / per;
        if (lfo_ph >= 1.0f)
            lfo_ph -= 1.0f;
    }
    switch (P.g[G_LFO_SHAPE] & 3) {
    case 0: lfo_val = lfo_ph < 0.5f ? 4.0f * lfo_ph - 1.0f : 3.0f - 4.0f * lfo_ph; break;   /* triangle */
    case 1: lfo_val = 1.0f - 2.0f * lfo_ph; break;                                           /* falling saw */
    case 2: lfo_val = lfo_ph < 0.5f ? 1.0f : -1.0f; break;                                   /* square */
    default:
        if (lfo_ph < was)
            lfo_sh = rnd(&srng);
        lfo_val = lfo_sh;
        break;                           /* sample and hold */
    }
}
static float lfo_to(int dest)            /* the LFO's octaves of cutoff for one destination */
{
    int d = P.g[G_LFO_DEST];
    if (d == 0 || !(d == dest || (d == 4 && ((dest >= 1 && dest <= 3) || dest == 7))))
        return 0.0f;
    return lfo_val * kn(P.g[G_LFO_DEPTH]) * 3.0f;
}

/* ---------------------------------------------------------------- the voices --- */
static float bufL[CR], bufR[CR], dlyL[CR], dlyR[CR], revS[CR];
static float lvl_now[TUN_TRACKS];
static float duck_g[CR];                 /* the duck's gain, each sample of the block: 1 none */

static void mix_out(int tr, float x, int i)
{
    const tun_track_t *t = &P.t[tr];
    float lv = kn(t->level) * (tr >= TK_BASS ? duck_g[i] : 1.0f), p = ((float)t->pan - 64.0f) * (1.0f / 64.0f), gl = lv * (1.0f - fm_maxf(p, 0.0f)),
          gr = lv * (1.0f + fm_minf(p, 0.0f));
    bufL[i] += x * gl;
    bufR[i] += x * gr;
    dlyL[i] += x * lv * kn(t->dly);
    revS[i] += x * lv * kn(t->rev);
}

static void kick_render(int ns)
{
    drum_t *d = &D[TK_KICK];
    const uint8_t *s = P.t[TK_KICK].snd;
    float drive = 1.0f + kn(s[5]) * 6.0f, lc = 1.0f - kn(s[6]) * 0.9f;
    int i;
    if (d->env < 0.0001f && d->tail < 0.0001f)
        return;
    for (i = 0; i < ns; i++) {
        float f = d->f0 * fm_exp2f(d->penv), x;
        d->ph += f * INV_SR;
        if (d->ph >= 1.0f)
            d->ph -= 1.0f;
        x = fm_sinf(FM_TWO_PI * d->ph) * d->env;
        if (d->burst_t < 0.003f) {       /* the click: 3 ms of bright noise */
            x += d->click * 0.6f * rnd(&nrng) * (1.0f - d->burst_t * 333.0f);
            d->burst_t += INV_SR;
        }
        x += d->tail * fm_sinf(FM_TWO_PI * d->ph) * lc;
        x = sat(x * drive) * (1.0f / fm_minf(drive, 2.2f));
        d->env *= d->envm;
        d->penv *= d->penvm;
        d->tail *= d->tailm;
        mix_out(TK_KICK, x * d->vel * 1.1f, i);
    }
    lvl_now[TK_KICK] = d->env;
}

static void snare_render(int ns)
{
    drum_t *d = &D[TK_SNARE];
    const uint8_t *s = P.t[TK_SNARE].snd;
    float noise = kn(s[3]), snap = kn(s[4]), tone = 1500.0f * fm_exp2f(kn(s[5]) * 2.5f), drive = 1.0f + kn(s[6]) * 4.0f;
    float fq = svf_f(tone), fhp = svf_f(400.0f + 3000.0f * snap), fclap = svf_f(1100.0f * (0.7f + kn(s[1])));
    int i;
    if (d->env < 0.0001f)
        return;
    for (i = 0; i < ns; i++) {
        float n = rnd(&nrng), x;
        if (d->mode == 1) {              /* clap: three bursts 10 ms apart, then the tail */
            float t = d->burst_t, b = t < 0.03f ? (t - 0.0105f * fm_floorf(t * (1.0f / 0.0105f)) < 0.004f ? 1.0f : 0.15f) : 1.0f;
            x = svf_bp(&d->f1, n, fclap, 0.8f) * b * d->env * 2.2f;
            d->burst_t += INV_SR;
        } else if (d->mode == 2) {       /* rim: a short tone and a click */
            d->ph += d->f0 * 3.2f * INV_SR;
            if (d->ph >= 1.0f)
                d->ph -= 1.0f;
            x = (fm_sinf(FM_TWO_PI * d->ph) * 0.8f + svf_bp(&d->f1, n, fhp, 0.5f) * 0.6f) * d->env;
        } else {                         /* snare: two tones and noise */
            float body;
            d->ph += d->f0 * INV_SR;
            if (d->ph >= 1.0f)
                d->ph -= 1.0f;
            body = fm_sinf(FM_TWO_PI * d->ph) + 0.6f * fm_sinf(FM_TWO_PI * d->ph * 1.62f);
            body *= d->penv;
            n = svf_lp(&d->f2, n, fq, 1.0f);
            {
                float hp = n - d->hp;
                d->hp += (n - d->hp) * fm_minf(fhp * 0.5f, 0.9f);
                n = hp;
            }
            x = (body * (1.0f - noise * 0.6f) * 0.7f + n * noise * 1.6f) * d->env;
            d->penv *= d->penvm;
        }
        x = sat(x * drive) / fm_minf(drive, 2.0f);
        d->env *= d->envm;
        mix_out(TK_SNARE, x * d->vel, i);
    }
    lvl_now[TK_SNARE] = d->env;
}

/* the hats' metal: six squares at inharmonic ratios, shared by both hats */
static const float HAT_F[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};
static void hats_render(int ns)
{
    drum_t *c = &D[TK_CH], *o = &D[TK_OH];
    const uint8_t *cs = P.t[TK_CH].snd, *os = P.t[TK_OH].snd;
    float ct = fm_exp2f(kn(cs[0]) * 1.2f - 0.4f), ot = fm_exp2f(kn(os[1]) * 1.2f - 0.4f) * (o->mode ? 1.35f : 1.0f);
    float cf = svf_f(6000.0f + 6000.0f * kn(cs[2])), of = svf_f(5000.0f + 6000.0f * kn(os[3]));
    float cm = kn(cs[3]), om = kn(os[4]), cn = kn(cs[4]), bell = kn(os[5]), fbell = svf_f(2400.0f * ot);
    int i, k;
    float inc[6];
    if (c->env < 0.0001f && o->env < 0.0001f)
        return;
    for (k = 0; k < 6; k++)
        inc[k] = HAT_F[k] * INV_SR;
    for (i = 0; i < ns; i++) {
        float mc = 0.0f, mo = 0.0f, n = rnd(&nrng), x, y;
        for (k = 0; k < 6; k++) {
            float sq;
            hat_ph[k] += inc[k] * ct;
            if (hat_ph[k] >= 1.0f)
                hat_ph[k] -= 1.0f;
            sq = hat_ph[k] < 0.5f ? 1.0f : -1.0f;
            mc += sq;
            mo += (k & 1) ? sq : -sq;
        }
        (void)ot;
        if (c->env > 0.0001f) {
            x = mc * 0.18f * cm + n * (1.0f - cm * 0.7f) * (0.4f + cn);
            x = svf_bp(&c->f1, x, cf, 0.6f);
            y = x - c->hp;
            c->hp += (x - c->hp) * 0.25f;
            mix_out(TK_CH, y * c->env * c->vel * 1.6f, i);
            c->env *= c->envm;
        }
        if (o->env > 0.0001f) {
            x = mo * 0.18f * om + n * (1.0f - om * 0.7f) * 0.5f;
            x = svf_bp(&o->f1, x, of, o->mode ? 0.35f : 0.6f);
            y = x - o->hp;
            o->hp += (x - o->hp) * 0.25f;
            if (o->mode)                 /* the ride's bell: the metal's low partials, ringing */
                y += svf_bp(&o->f2, mo, fbell, 0.12f) * bell * 0.12f;
            mix_out(TK_OH, y * o->env * o->vel * 1.4f, i);
            o->env *= o->envm;
        }
    }
    lvl_now[TK_CH] = c->env;
    lvl_now[TK_OH] = o->env;
}

static void perc_render(int ns)
{
    drum_t *d = &D[TK_PERC];
    const uint8_t *s = P.t[TK_PERC].snd;
    float tone = kn(s[4]), drive = 1.0f + kn(s[5]) * 5.0f, fb, fcb;
    int i;
    if (d->env < 0.0001f)
        return;
    fb = svf_f(d->f0 * fm_exp2f(d->penv));          /* (the band-passes follow the pitch a block at a time) */
    fcb = svf_f(d->f0 * fm_exp2f(d->penv) * 1.2f + 800.0f * tone);
    for (i = 0; i < ns; i++) {
        float f = d->f0 * fm_exp2f(d->penv), x;
        d->ph += f * INV_SR;
        if (d->ph >= 1.0f)
            d->ph -= 1.0f;
        switch (d->mode) {
        case 0: x = fm_sinf(FM_TWO_PI * d->ph) + tone * 0.3f * rnd(&nrng) * d->penv; break;         /* tom */
        case 1: x = fm_sinf(FM_TWO_PI * d->ph) * (1.0f + tone * 0.5f * fm_sinf(FM_TWO_PI * d->ph * 2.0f)); break;
        case 2: x = svf_bp(&d->f1, rnd(&nrng) + fm_sinf(FM_TWO_PI * d->ph), fb, 0.15f); break;  /* rim */
        case 3: {                        /* cowbell: two squares, band-passed */
            float a = d->ph < 0.5f ? 1.0f : -1.0f, b;
            d->tail += f * 1.48f * INV_SR;
            if (d->tail >= 1.0f)
                d->tail -= 1.0f;
            b = d->tail < 0.5f ? 1.0f : -1.0f;
            x = svf_bp(&d->f1, (a + b) * 0.5f, fcb, 0.4f) * 1.6f;
            break;
        }
        case 4: x = (d->ph < 0.5f ? 1.0f : -1.0f) * 0.5f; break;                                   /* blip */
        default: x = fm_sinf(FM_TWO_PI * d->ph) * (1.0f + tone * 0.6f * d->penv); break;           /* sine */
        }
        x = sat(x * drive) / fm_minf(drive, 2.0f);
        d->env *= d->envm;
        d->penv *= d->penvm;
        mix_out(TK_PERC, x * d->env * d->vel, i);
    }
    lvl_now[TK_PERC] = d->env;
}

static void bass_render(int ns)
{
    const uint8_t *s = P.t[TK_BASS].snd;
    int i, wave = s[0];
    float res = kn(s[2]), k = 4.1f * res, envamt = kn(s[3]) * (1.0f + B.acc), drive = 1.0f + kn(s[6]) * 5.0f;
    float am = decay_mul(t_of(s[4], 0.08f, 2.0f) * (1.0f + B.acc)), fm = decay_mul(t_of(s[4], 0.03f, 1.0f));
    float cut_oct, fc, g, G, G2, G3, G4, D_, inc;
    if (B.aenv < 0.0001f)
        return;
    if (B.gate_left > 0.0f)
        B.gate_left -= (float)ns * INV_SR;
    else
        am = decay_mul(0.03f);           /* released */
    B.pitch += (B.target - B.pitch) * fm_minf(1.0f, B.glide * (float)ns * 0.5f);
    inc = mtof(B.pitch) * INV_SR;
    cut_oct = 5.0f + kn(s[1]) * 6.5f + envamt * 5.0f * B.fenv + lfo_to(1) + B.acc * 1.5f * B.fenv - morph * 2.5f;
    fc = 15.0f * fm_exp2f(fm_clampf(cut_oct, 0.0f, 10.4f));
    g = fm_tanf(FM_PI * fm_minf(fc, 0.42f * TUN_SR) * INV_SR);
    G = g / (1.0f + g);
    G2 = G * G;
    G3 = G2 * G;
    G4 = G2 * G2;
    D_ = 1.0f / (1.0f + k * G4);
    for (i = 0; i < ns; i++) {
        float saw, sq, x, sig, y4, u, y;
        B.ph += inc;
        if (B.ph >= 1.0f)
            B.ph -= 1.0f;
        B.sub += inc * 0.5f;
        if (B.sub >= 1.0f)
            B.sub -= 1.0f;
        saw = 2.0f * B.ph - 1.0f - blep(B.ph, inc);
        sq = (B.ph < 0.5f ? 1.0f : -1.0f) + blep(B.ph, inc) - blep(wrap1(B.ph + 0.5f), inc);
        if (wave >= 3) {                 /* the sines: pure, or a sine with a saw's edge */
            float sn = fm_sinf(FM_TWO_PI * B.ph);
            x = wave == 3 ? 1.3f * sn : sn + 0.45f * saw;
        } else {
            x = wave == 0 ? saw : wave == 1 ? sq : 0.6f * (saw + sq);
        }
        x += 0.5f * fm_sinf(FM_TWO_PI * B.sub);
        sig = (1.0f - G) * (G3 * B.s[0] + G2 * B.s[1] + G * B.s[2] + B.s[3]);
        y4 = (G4 * x + sig) * D_;
        u = sat(x - k * y4);
        y = G * (u - B.s[0]) + B.s[0]; B.s[0] = 2.0f * y - B.s[0]; u = y;
        y = G * (u - B.s[1]) + B.s[1]; B.s[1] = 2.0f * y - B.s[1]; u = y;
        y = G * (u - B.s[2]) + B.s[2]; B.s[2] = 2.0f * y - B.s[2]; u = y;
        y = G * (u - B.s[3]) + B.s[3]; B.s[3] = 2.0f * y - B.s[3];
        y = sat(y * drive) / fm_minf(drive, 2.0f);
        mix_out(TK_BASS, y * B.aenv * (0.55f + 0.45f * B.vel) * (1.0f + 0.4f * B.acc) * 1.6f, i);
        B.aenv *= am;
        B.fenv *= fm;
    }
    if (B.aenv < 0.0001f)
        B.on = 0;
    for (i = 0; i < 4; i++)
        B.s[i] = fm_flush(B.s[i]);
    lvl_now[TK_BASS] = B.aenv;
}

static void stab_render(int ns)
{
    const uint8_t *s = P.t[TK_STAB].snd;
    int i, j, wave = s[6];
    float q = 2.0f - 1.85f * kn(s[2]), env = kn(s[3]);
    float base = 4.5f + kn(s[1]) * 6.5f + lfo_to(2) - morph * 3.0f;
    float lv = 0.0f;
    for (j = 0; j < NSTAB; j++) {
        stab_t *v = &S[j];
        float f;
        if (v->env < 0.0002f)
            continue;
        f = svf_f(15.0f * fm_exp2f(fm_clampf(base + env * 5.5f * v->fenv * (0.5f + 0.5f * v->vel), 0.0f, 10.2f)));
        for (i = 0; i < ns; i++) {
            float a, b, x;
            v->ph[0] += v->inc[0];
            if (v->ph[0] >= 1.0f)
                v->ph[0] -= 1.0f;
            v->ph[1] += v->inc[1];
            if (v->ph[1] >= 1.0f)
                v->ph[1] -= 1.0f;
            if (wave == 1) {             /* pulse */
                a = (v->ph[0] < 0.5f ? 1.0f : -1.0f) + blep(v->ph[0], v->inc[0]) - blep(wrap1(v->ph[0] + 0.5f), v->inc[0]);
                b = (v->ph[1] < 0.5f ? 1.0f : -1.0f) + blep(v->ph[1], v->inc[1]) - blep(wrap1(v->ph[1] + 0.5f), v->inc[1]);
            } else if (wave == 2) {      /* triangle-ish: soft */
                a = 1.0f - 4.0f * fm_fabsf(v->ph[0] - 0.5f);
                b = 1.0f - 4.0f * fm_fabsf(v->ph[1] - 0.5f);
            } else if (wave == 3) {      /* sines, and a sine an octave up: an organ's stab */
                a = fm_sin_turns(v->ph[0]) * 1.3f + 0.35f * fm_sin_turns(wrap1(2.0f * v->ph[0]));
                b = fm_sin_turns(v->ph[1]) * 1.3f + 0.35f * fm_sin_turns(wrap1(2.0f * v->ph[1]));
            } else {
                a = 2.0f * v->ph[0] - 1.0f - blep(v->ph[0], v->inc[0]);
                b = 2.0f * v->ph[1] - 1.0f - blep(v->ph[1], v->inc[1]);
            }
            x = svf_lp(&v->f, (a + b) * 0.35f, f, q);
            mix_out(TK_STAB, x * v->env * v->vel * 0.55f, i);
            v->env *= v->envm;
        }
        v->fenv *= fm_expf(fm_logf(v->fenvm) * (float)ns);
        v->f.lp = fm_flush(v->f.lp);
        v->f.bp = fm_flush(v->f.bp);
        if (v->env > lv)
            lv = v->env;
    }
    lvl_now[TK_STAB] = lv;
}

static void bell_render(int ns)
{
    const uint8_t *s = P.t[TK_BELL].snd;
    static const float RATIO[16] = {1.0f, 1.41f, 2.0f, 2.76f, 3.0f, 3.5f, 4.0f, 5.19f, 6.0f, 7.0f, 0.5f, 1.5f, 2.5f,
                                    3.14f, 8.0f, 11.0f};
    int i, j, fmmode = s[0] == 0;
    float ratio = RATIO[s[1] & 15], idx0 = kn(s[2]) * 6.0f, wob, lv = 0.0f;
    float cut = 15.0f * fm_exp2f(fm_clampf(5.0f + kn(s[4]) * 5.5f + lfo_to(3) - morph * 2.5f, 0.0f, 10.2f));
    float fq = svf_f(cut);
    warble_ph += CR_DT * 5.3f;
    if (warble_ph >= 1.0f)
        warble_ph -= 1.0f;
    wob = 1.0f + kn(s[5]) * 0.012f * fm_sinf(FM_TWO_PI * warble_ph);
    for (j = 0; j < NBELL; j++) {
        bell_t *v = &L[j];
        float inc = v->inc * wob, gl = 1.0f - fm_maxf(v->pan, 0.0f), gr = 1.0f + fm_minf(v->pan, 0.0f);
        if (v->env < 0.0002f)
            continue;
        for (i = 0; i < ns; i++) {
            float x;
            v->cph += inc;
            if (v->cph >= 1.0f)
                v->cph -= 1.0f;
            v->mph += inc * (fmmode ? ratio : 2.0f);
            if (v->mph >= 1.0f)
                v->mph -= 1.0f;
            if (fmmode) {
                x = fm_sinf(FM_TWO_PI * v->cph + idx0 * v->ienv * v->vel * fm_sinf(FM_TWO_PI * v->mph));
            } else {                     /* the pluck: a dark triangle and a bright one an octave up */
                float a = 1.0f - 4.0f * fm_fabsf(v->cph - 0.5f), b = 1.0f - 4.0f * fm_fabsf(v->mph - 0.5f);
                x = svf_lp(&v->f, a * 0.8f + b * (0.3f + 0.5f * kn(s[2])), fq * (0.4f + 0.6f * v->ienv), 1.0f);
            }
            x *= v->env * v->vel * 0.5f;
            {                            /* each voice its own place in the stereo field */
                const tun_track_t *t = &P.t[TK_BELL];
                float l = kn(t->level) * duck_g[i];
                bufL[i] += x * l * gl;
                bufR[i] += x * l * gr;
                dlyL[i] += x * l * kn(t->dly);
                revS[i] += x * l * kn(t->rev);
            }
            v->env *= v->envm;
            v->ienv *= v->ienvm;
        }
        v->f.lp = fm_flush(v->f.lp);
        v->f.bp = fm_flush(v->f.bp);
        if (v->env > lv)
            lv = v->env;
    }
    lvl_now[TK_BELL] = lv;
}

static inline float saw_bl(float ph, float inc) { return 2.0f * ph - 1.0f - blep(ph, inc); }
static inline float pulse_bl(float ph, float inc, float pw)
{
    return (ph < pw ? 1.0f : -1.0f) + blep(ph, inc) - blep(wrap1(ph + 1.0f - pw), inc);
}

static void synth_render(int ns)
{
    static const float PWM_DEPTH[SYN_NWAVE] = {0.0f, 0.0f, 0.38f, 0.42f, 0.0f, 0.0f, 0.0f, 0.0f};
    const tun_track_t *t = &P.t[TK_SYNTH];
    const uint8_t *s = t->snd;
    int i, j, wave = s[0] % SYN_NWAVE;
    float q = 2.0f - 1.92f * kn(s[2]), envamt = kn(s[3]), sus = kn(s[6]), cho = kn(s[7]);
    float ainc = INV_SR / t_of(s[4], 0.002f, 3.0f), dk = decay_mul(t_of(s[5], 0.06f, 8.0f)),
          rk = decay_mul(t_of(s[5], 0.05f, 6.0f));
    float base = 4.0f + kn(s[1]) * 6.8f + lfo_to(7) - morph * 2.5f, pw, lv = 0.0f, mono[CR];
    int any = 0;
    static int quiet;
    pwm_ph += CR_DT * 0.8f;              /* the pulse's sweep: a slow 0.8 Hz */
    if (pwm_ph >= 1.0f)
        pwm_ph -= 1.0f;
    pw = 0.5f + PWM_DEPTH[wave] * fm_sin_turns(pwm_ph);
    for (i = 0; i < ns; i++)
        mono[i] = 0.0f;
    for (j = 0; j < NSYN; j++) {
        syn_t *v = &Y[j];
        float f, pm, inc, kt;
        if (!v->stage)
            continue;
        any = 1;
        if (v->stage < 3) {
            v->gate_left -= (float)ns * INV_SR;
            if (v->gate_left <= 0.0f)
                v->stage = 3;
        }
        pm = v->scoop < -0.01f ? fm_exp2f(v->scoop * (1.0f / 12.0f)) : 1.0f;
        v->scoop *= 0.93f;
        inc = v->inc * pm;
        kt = fm_log2f(v->inc * TUN_SR * (1.0f / 261.6f)) * 0.5f;   /* half key tracking, octaves from middle C */
        f = svf_f(15.0f * fm_exp2f(fm_clampf(base + kt + envamt * 6.5f * v->env * (0.6f + 0.4f * v->vel), 0.0f, 10.3f)));
        for (i = 0; i < ns; i++) {
            float x, y;
            v->ph += inc;
            if (v->ph >= 1.0f)
                v->ph -= 1.0f;
            v->sub += inc * 0.5f;
            if (v->sub >= 1.0f)
                v->sub -= 1.0f;
            switch (wave) {
            case 0: x = saw_bl(v->ph, inc); break;
            case 1: x = pulse_bl(v->ph, inc, 0.5f); break;
            case 2: x = pulse_bl(v->ph, inc, pw); break;
            case 3: x = 0.62f * (saw_bl(v->ph, inc) + saw_bl(wrap1(v->ph + pw), inc)); break;   /* the swept saw */
            case 4: x = saw_bl(v->ph, inc) + 0.6f * pulse_bl(v->sub, inc * 0.5f, 0.5f); break;
            case 5: x = pulse_bl(v->ph, inc, 0.5f) + 0.6f * pulse_bl(v->sub, inc * 0.5f, 0.5f); break;
            case 6: x = 1.3f * fm_sin_turns(v->ph) + 0.5f * fm_sin_turns(v->sub); break;
            default: x = fm_sin_turns(v->ph) + 0.4f * saw_bl(v->ph, inc); break;
            }
            y = svf_lp(&v->f1, x, f, 1.414f);
            y = svf_lp(&v->f2, y, f, q);
            y = sat(y * 1.2f);
            mono[i] += y * v->env * (0.45f + 0.55f * v->vel) * 0.42f;
            if (v->stage == 1) {
                v->env += ainc;
                if (v->env >= 1.0f) {
                    v->env = 1.0f;
                    v->stage = 2;
                }
            } else if (v->stage == 2) {
                v->env = sus + (v->env - sus) * dk;
            } else {
                v->env *= rk;
            }
        }
        if (v->stage == 3 && v->env < 0.0001f) {
            v->stage = 0;
            v->env = 0.0f;
        }
        v->f1.lp = fm_flush(v->f1.lp);
        v->f1.bp = fm_flush(v->f1.bp);
        v->f2.lp = fm_flush(v->f2.lp);
        v->f2.bp = fm_flush(v->f2.bp);
        if (v->env > lv)
            lv = v->env;
    }
    lvl_now[TK_SYNTH] = lv;
    if (any)
        quiet = 0;
    else if (++quiet > 80)               /* silent: the chorus line has emptied, rest */
        return;
    {                                    /* the chorus: two taps swept in opposite phase, one each side */
        float rate = 0.4f + 0.5f * cho, depth = (0.0012f + 0.0026f * cho) * TUN_SR, mid = 0.0055f * TUN_SR;
        float wet = cho > 0.001f ? 0.55f + 0.25f * cho : 0.0f, dry = 1.0f - 0.25f * wet;
        float p = kn(t->pan) * 2.0f - 1.0f, lv2 = kn(t->level), gl = lv2 * (1.0f - fm_maxf(p, 0.0f)),
              gr = lv2 * (1.0f + fm_minf(p, 0.0f));
        for (i = 0; i < ns; i++) {
            float x = mono[i], l = x * dry, r = x * dry;
            cho_mem[cho_w & CHO_MASK] = x;
            if (wet > 0.0f) {
                float m = fm_sin_turns(cho_ph), d1 = mid + depth * m, d2 = mid - depth * m, a, b;
                uint32_t i1 = (uint32_t)d1, i2 = (uint32_t)d2;
                float f1 = d1 - (float)i1, f2 = d2 - (float)i2;
                a = cho_mem[(cho_w - i1) & CHO_MASK] * (1.0f - f1) + cho_mem[(cho_w - i1 - 1u) & CHO_MASK] * f1;
                b = cho_mem[(cho_w - i2) & CHO_MASK] * (1.0f - f2) + cho_mem[(cho_w - i2 - 1u) & CHO_MASK] * f2;
                l += a * wet;
                r += b * wet;
            }
            cho_w++;
            cho_ph += rate * INV_SR;
            if (cho_ph >= 1.0f)
                cho_ph -= 1.0f;
            bufL[i] += l * gl * duck_g[i];
            bufR[i] += r * gr * duck_g[i];
            dlyL[i] += (l + r) * 0.5f * lv2 * kn(t->dly) * duck_g[i];
            revS[i] += (l + r) * 0.5f * lv2 * kn(t->rev) * duck_g[i];
        }
    }
}

static void duck_run(int ns)             /* the duck's gain for this block */
{
    float depth = kn(P.g[G_DUCK]), rel = (0.25f + 1.75f * kn(P.g[G_DUCK_REL])) * 60.0f / bpm(),
          m = decay_mul(rel * 1.5f);
    static float sm;
    int i;
    for (i = 0; i < ns; i++) {
        sm += (duck_env - sm) * 0.02f;   /* a millisecond in: no click */
        duck_env *= m;
        duck_g[i] = 1.0f - depth * sm;
    }
}

/* ---------------------------------------------------------------- effects --- */
#define DLY_LEN 32768u                   /* 0.74 s each side */
#define DLY_MASK (DLY_LEN - 1u)
static PLAT_POOL int16_t dly_mem[2][DLY_LEN];
static uint32_t dly_w;
static float dly_now, dly_lp[2];
#define REV_LEN 16384u
#define REV_MASK (REV_LEN - 1u)
static PLAT_POOL int16_t rev_mem[REV_LEN];
static const uint16_t REV_D[4] = {1557, 1617, 1491, 1422}, REV_AP[2] = {225, 341};
static uint32_t rev_w;
static float rev_lp[4];
static svf_t sweep_f[2];
static float hp_x[2], hp_y[2];

static inline int16_t to16(float x)
{
    x *= 8192.0f;
    return (int16_t)(x > 32767.0f ? 32767.0f : x < -32768.0f ? -32768.0f : x);
}
static inline float from16(int16_t s) { return (float)s * (1.0f / 8192.0f); }

static void fx_render(int ns)
{
    static const float DIV[8] = {0.125f, 0.25f, 0.3333f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f};
    float target = DIV[P.g[G_DLY_TIME] & 7] * 60.0f / bpm() * TUN_SR, fb, lpc, size, damp, sweep, drive, vol;
    float comp, thr, slope, makeup;
    static float comp_env;
    const float comp_a = 0.0045f, comp_r = 0.00015f;   /* about 5 ms in, 150 ms out */
    int i;
    while (target > (float)DLY_LEN - 4.0f)
        target *= 0.5f;
    if (dly_now <= 0.0f)
        dly_now = target;
    fb = fm_minf(kn(P.g[G_DLY_FB]) * 0.9f + morph * 0.35f, 0.95f);
    lpc = 1.0f - fm_expf(-FM_TWO_PI * (1200.0f + 9000.0f * kn(P.g[G_DLY_TONE])) * INV_SR);
    size = 0.7f + 0.27f * kn(P.g[G_REV_SIZE]) + morph * 0.02f;
    if (size > 0.985f)
        size = 0.985f;
    damp = 0.15f + 0.7f * kn(P.g[G_REV_DAMP]);
    sweep = ((float)P.g[G_SWEEP] - 64.0f) * (1.0f / 63.0f);
    drive = 1.0f + kn(P.g[G_DRIVE]) * 3.0f;
    vol = P.g[G_VOLUME] ? fm_exp2f(((float)P.g[G_VOLUME] - 100.0f) * (0.45f / 6.02f)) : 0.0f;
    comp = kn(P.g[G_COMP]);
    thr = -26.0f * comp;                 /* the threshold falls, the ratio rises (1.5 .. 6), half made up */
    slope = 1.0f - 1.0f / (1.5f + 4.5f * comp);
    makeup = -thr * slope * 0.5f;
    {                                    /* the sweep and the TUNNEL: a low-pass that closes, a high-pass that opens */
        float lpoct = sweep < 0.0f ? 10.4f + sweep * 5.5f : 10.4f, hpoct = sweep > 0.0f ? 3.0f + sweep * 6.0f : 0.0f;
        float lfo = P.g[G_LFO_DEST] == 5 ? lfo_val * kn(P.g[G_LFO_DEPTH]) * 2.0f : 0.0f;
        float lpf, hpc, q = 1.6f - morph * 1.0f;
        lpoct = fm_clampf(lpoct - morph * 4.2f + lfo, 4.0f, 10.4f);
        lpf = svf_f(15.0f * fm_exp2f(lpoct));
        hpc = hpoct > 0.0f ? 1.0f - fm_expf(-FM_TWO_PI * 15.0f * fm_exp2f(hpoct) * INV_SR) : 0.0f;
        for (i = 0; i < ns; i++) {
            float w, l, r, il, ir, rin, ro, rl, rr;
            /* the delay, ping-pong: left feeds right */
            dly_now += (target - dly_now) * 0.001f;
            {
                uint32_t di = (uint32_t)dly_now;
                il = from16(dly_mem[0][(dly_w - di) & DLY_MASK]);
                ir = from16(dly_mem[1][(dly_w - di) & DLY_MASK]);
            }
            dly_lp[0] += (il - dly_lp[0]) * lpc;
            dly_lp[1] += (ir - dly_lp[1]) * lpc;
            dly_mem[0][dly_w & DLY_MASK] = to16(dlyL[i] + dly_lp[1] * fb);
            dly_mem[1][dly_w & DLY_MASK] = to16(dly_lp[0] * fb);
            dly_w++;
            /* the reverb: four lines, mixed by a Hadamard matrix, damped */
            rin = revS[i] + 0.25f * (il + ir) * morph;
            {
                float a[4], m[4];
                int k;
                uint32_t base = 0;
                for (k = 0; k < 4; k++) {
                    a[k] = from16(rev_mem[(base + ((rev_w - REV_D[k]) & 4095u)) & REV_MASK]);
                    base += 4096u;
                }
                m[0] = a[0] + a[1] + a[2] + a[3];
                m[1] = a[0] - a[1] + a[2] - a[3];
                m[2] = a[0] + a[1] - a[2] - a[3];
                m[3] = a[0] - a[1] - a[2] + a[3];
                base = 0;
                for (k = 0; k < 4; k++) {
                    rev_lp[k] += (m[k] * 0.5f * size - rev_lp[k]) * (1.0f - damp);
                    rev_mem[(base + (rev_w & 4095u)) & REV_MASK] = to16(rev_lp[k] + (k < 2 ? rin : -rin) * 0.5f);
                    base += 4096u;
                }
                rev_w++;
                rl = a[0] + a[2] * 0.6f;
                rr = a[1] + a[3] * 0.6f;
                (void)REV_AP;
            }
            l = bufL[i] + (il * 0.8f + rl * 0.6f) * duck_g[i];
            r = bufR[i] + (ir * 0.8f + rr * 0.6f) * duck_g[i];
            /* the DJ filter */
            l = svf_lp(&sweep_f[0], l, lpf, q);
            r = svf_lp(&sweep_f[1], r, lpf, q);
            if (hpc > 0.0f) {
                hp_y[0] += (l - hp_y[0]) * hpc;
                hp_y[1] += (r - hp_y[1]) * hpc;
                l -= hp_y[0];
                r -= hp_y[1];
            }
            (void)hp_x;
            if (comp > 0.0f) {           /* the compressor: a peak follower, the gain in dB */
                float pk = fm_maxf(fm_fabsf(l), fm_fabsf(r)), over, gdb;
                comp_env += (pk - comp_env) * (pk > comp_env ? comp_a : comp_r);
                over = 6.0206f * fm_log2f(comp_env + 1e-6f) - thr;
                gdb = (over > 0.0f ? -over * slope : 0.0f) + makeup;
                gdb = fm_exp2f(gdb * 0.16609640f);
                l *= gdb;
                r *= gdb;
            }
            w = 1.0f / fm_minf(drive, 1.8f);
            l = sat(l * drive) * w * vol;
            r = sat(r * drive) * w * vol;
            ro = l;
            bufL[i] = ro;
            bufR[i] = r;
        }
    }
    for (i = 0; i < 4; i++)
        rev_lp[i] = fm_flush(rev_lp[i]);
}

/* ---------------------------------------------------------------- render --- */
void tun_render(float *out, uint32_t n)
{
    events();
    if (clock_seen && sample_t - last_f8_t > (uint32_t)TUN_SR)
        clock_seen = 0;
    while (n) {
        int i, ns = n < CR ? (int)n : CR;
        for (i = 0; i < ns; i++)
            bufL[i] = bufR[i] = dlyL[i] = dlyR[i] = revS[i] = 0.0f;
        /* the clock */
        if (playing) {
            if (use_midi()) {
                while (midi_ticks) {
                    tick_one();
                    midi_ticks--;
                }
            } else {
                tick_frac += bpm() * (float)PPQN / 60.0f * (float)ns * INV_SR;
                while (tick_frac >= 1.0f) {
                    tick_one();
                    tick_frac -= 1.0f;
                }
            }
        }
        lfo_run();
        if (arp_on && arp_n) {           /* sixteenths at the tempo, locked to the sequencer while it plays */
            int fire = 0;
            if (playing) {
                static uint32_t last16 = 0xFFFFFFFFu;
                uint32_t s16 = tick_i / 24u;
                if (s16 != last16) {
                    last16 = s16;
                    fire = 1;
                }
            } else {
                arp_acc += bpm() * 4.0f / 60.0f * (float)ns * INV_SR;
                if (arp_acc >= 1.0f) {
                    arp_acc -= 1.0f;
                    fire = 1;
                }
            }
            if (fire) {
                int n = arp_n;
                arp_pos = (arp_pos + 1) % (n > 0 ? n : 1);
                trigger(arp_track, 0.85f, arp_notes[arp_pos], 0, 0.08f);
            }
        } else {
            arp_acc = 1.0f;
            arp_pos = -1;
        }
        duck_run(ns);
        kick_render(ns);
        snare_render(ns);
        hats_render(ns);
        perc_render(ns);
        bass_render(ns);
        stab_render(ns);
        bell_render(ns);
        synth_render(ns);
        fx_render(ns);
        for (i = 0; i < ns; i++) {
            float l = bufL[i], r = bufR[i];
            if (fm_fabsf(l) > 0.8f)
                l = (l > 0.0f ? 1.0f : -1.0f) * (0.8f + 0.2f * fm_tanhf((fm_fabsf(l) - 0.8f) * 5.0f));
            if (fm_fabsf(r) > 0.8f)
                r = (r > 0.0f ? 1.0f : -1.0f) * (0.8f + 0.2f * fm_tanhf((fm_fabsf(r) - 0.8f) * 5.0f));
            out[2 * i] = l;
            out[2 * i + 1] = r;
        }
        out += 2 * ns;
        n -= (uint32_t)ns;
        sample_t += (uint32_t)ns;
    }
}

/* ------------------------------------------------------------------- API --- */
void tun_init(void)
{
    int i;
    for (i = 0; i < 6; i++)
        hat_ph[i] = 0.17f * (float)i;
    for (i = 0; i < TUN_TRACKS; i++)
        step_now[i] = -1;
    for (i = 0; i < (int)CHO_LEN; i++)
        cho_mem[i] = 0.0f;
    (void)coef;
}
void tun_set_preset(const tun_preset_t *p)
{
    if (playing) {
        preset_next = *p;
        preset_wait = 1;
    } else {
        P = *p;
    }
}
void tun_deck_load(int deck, const tun_preset_t *p)
{
    if (deck < 0 || deck >= NDECKS)
        return;
    deck_next[deck] = *p;
    deck_wait[deck] = 1;
    if (!playing)
        deck_apply();
}
int tun_deck_pending(int deck) { return deck >= 0 && deck < NDECKS ? deck_wait[deck] || preset_wait : 0; }
tun_preset_t *tun_preset(void) { return &P; }
void tun_touch(int track) { (void)track; }
void tun_set_perf(const tun_perf_t *f) { F = *f; }
void tun_play(int on) { ev_push(on ? EV_PLAY : EV_STOP, 0, 0); }
int tun_playing(void) { return playing; }
void tun_mute(int track, int on)
{
    if (track >= 0 && track < TUN_TRACKS)
        mutes = (uint16_t)(on ? mutes | 1u << track : mutes & ~(1u << track));
}
int tun_muted(int track) { return track >= 0 && track < TUN_TRACKS ? mutes >> track & 1 : 0; }
int tun_step_now(int track) { return track >= 0 && track < TUN_TRACKS && playing ? step_now[track] : -1; }
int tun_beat(void) { return playing ? (int)(tick_i / 24u % 16u) : -1; }
float tun_level(int track) { return track >= 0 && track < TUN_TRACKS ? lvl_now[track] : 0.0f; }
void tun_morph(int v) { morph = (float)(v < 0 ? 0 : v > 127 ? 127 : v) * (1.0f / 127.0f); }
void tun_note_on(int track, int note, int vel)
{
    ev_push(EV_ON, (uint32_t)(track << 8 | ((note + 64) & 0xFF)), (uint32_t)vel);
}
void tun_note_off(int track, int note) { ev_push(EV_OFF, (uint32_t)(track << 8 | ((note + 64) & 0xFF)), 0); }
void tun_clock(int rt) { ev_push(EV_RT, 0, (uint32_t)rt); }
int tun_clock_out(uint8_t *rt)
{
    if (rt_r == rt_w)
        return 0;
    *rt = rtq[rt_r % RTQ];
    rt_r++;
    return 1;
}
void tun_panic(void) { ev_push(EV_PANIC, 0, 0); }
void tun_arp(int track, const int8_t *notes, int n, int on)
{
    int i;
    if (n > 16)
        n = 16;
    for (i = 0; i < n; i++)
        arp_notes[i] = notes[i];
    arp_n = (uint8_t)n;
    arp_track = (uint8_t)(track >= 0 && track < TUN_TRACKS ? track : 0);
    arp_on = (uint8_t)(on != 0);
}
