/* SPDX-License-Identifier: MPL-2.0 */
/* TUNNEL FM-1: a hard, hypnotic Detroit techno machine for the M-VAVE FM-1. Nine synthesized tracks (kick, snare /
 * clap, closed hat, open hat / ride, percussion, bass, chord stab, bell, a four-voice synth), each with its own loop
 * length and rate, so loops roll against each other; three decks that load their tracks from any preset on the next bar; a delay, a
 * reverb, a DJ filter and the TUNNEL macro. No samples: every sound is made here. Pure C, single-precision float, no
 * allocation: runs on the FM-1 and on a computer alike. */
#pragma once
#include <stdint.h>

#define TUN_SR 44100.0f
#define TUN_TRACKS 9
#define TUN_STEPS 32                     /* a track's loop: 1 .. 32 steps */
#define TUN_NAME_LEN 12
#define TUN_NSND 8                       /* sound parameters a track */

enum { TK_KICK, TK_SNARE, TK_CH, TK_OH, TK_PERC, TK_BASS, TK_STAB, TK_BELL, TK_SYNTH };
/* the decks: A the drums (tracks 0-4), B bass and stab (5, 6), C the bell and the synth (7, 8) */
enum { DECK_A, DECK_B, DECK_C, NDECKS };

/* a step, two bytes: flags (on, velocity 0..3, ratchet 0..3 = 1..4 hits, probability 0..3 = 100 75 50 25 %, slide)
 * and a note, semitones from the track's root */
#define ST_ON 0x01u
#define ST_VEL(f) (((f) >> 1) & 3u)
#define ST_RAT(f) (((f) >> 3) & 3u)
#define ST_PROB(f) (((f) >> 5) & 3u)
#define ST_SLIDE 0x80u
#define ST_MAKE(vel, rat, prob) (ST_ON | (uint8_t)((vel) << 1 | (rat) << 3 | (prob) << 5))
typedef struct {
    uint8_t f;
    int8_t note;
} tun_step_t;

typedef struct {
    uint8_t snd[TUN_NSND];               /* the sound: tun_snd_name / TUN_SND_MAX */
    uint8_t level, pan, dly, rev;        /* mix: 0..127, pan 64 centre, the delay and reverb sends */
    uint8_t len, div;                    /* the loop: 1..32 steps; rate 0 1/16, 1 1/8, 2 1/32, 3 1/16 triplet */
    int8_t root;                         /* the track's transposition, semitones */
    uint8_t rsv;
    tun_step_t st[TUN_STEPS];
} tun_track_t;                           /* 80 bytes */

/* the global settings a preset carries */
enum { G_BPM, G_SWING, G_KEY,            /* BPM 60..200, swing 0..100 (50: straight .. 100: 75 %), key 0..11 */
       G_DLY_TIME, G_DLY_FB, G_DLY_TONE, /* delay: 0..7 = 1/32 1/16 1/16T 1/8 3/16 1/4 3/8 1/2; feedback; tone */
       G_REV_SIZE, G_REV_DAMP,           /* reverb */
       G_SWEEP, G_DRIVE, G_VOLUME,       /* master: the DJ filter (64 open; below low-pass, above high-pass) */
       G_LFO_RATE, G_LFO_DEPTH, G_LFO_DEST, G_LFO_SHAPE,   /* the motion LFO (sync); DEST 0 off 1 bass 2 stab 3 bell
                                                           * 4 all four cutoffs 5 sweep 6 hats' tone 7 synth */
       G_COMP,                           /* the master compressor: 0 off .. 127 squashed */
       G_DUCK, G_DUCK_REL,               /* the kick ducks the bass, stab, bell, synth and the returns: depth, and
                                          * its recovery (a 16th .. half a bar) */
       G_N };

typedef struct {
    char name[TUN_NAME_LEN];
    uint8_t g[G_N];
    uint8_t rsv[32 - TUN_NAME_LEN - G_N];   /* (0.2's COMP, DUCK and DUCK REL came from here: 0 in a 0.1 preset) */
    tun_track_t t[TUN_TRACKS];
} tun_preset_t;                          /* 752 bytes */
#define TUN_PRESET_01 (sizeof(tun_preset_t) - sizeof(tun_track_t))   /* a 0.1 preset: the same, without the synth */

/* each kind of track's sound parameters: name and top value (bottom 0) */
extern const char *const TUN_SND_NAME[TUN_TRACKS][TUN_NSND];
extern const uint8_t TUN_SND_MAX[TUN_TRACKS][TUN_NSND];
extern const char *const TUN_TRACK_NAME[TUN_TRACKS];
extern const uint8_t TUN_G_MAX[G_N], TUN_G_MIN[G_N];
extern const char *const TUN_G_NAME[G_N];

/* settings that are not the preset's */
typedef struct {
    uint8_t clock_src;                   /* 0 internal, 1 MIDI, 2 auto */
    uint8_t clock_out;                   /* 1: MIDI clock and start / stop out */
} tun_perf_t;

void tun_init(void);
void tun_set_preset(const tun_preset_t *p);   /* the whole machine (on the next bar while playing) */
void tun_deck_load(int deck, const tun_preset_t *p);   /* a deck's tracks from p, on the next bar */
int tun_deck_pending(int deck);          /* 1: a load waits for the bar */
tun_preset_t *tun_preset(void);          /* the one playing (edit it, then tun_touch) */
void tun_touch(int track);               /* a track's sound changed: -1 all */
void tun_set_perf(const tun_perf_t *f);

void tun_play(int on);                   /* the transport */
int tun_playing(void);
void tun_mute(int track, int on);
int tun_muted(int track);
int tun_step_now(int track);             /* the step playing, -1 stopped */
int tun_beat(void);                      /* 0..15 sixteenths in the bar, for the screen */
float tun_level(int track);              /* each track's envelope now, for the screen */

void tun_morph(int v);                   /* the TUNNEL macro 0..127 */
void tun_note_on(int track, int note, int vel);   /* live: the melodic tracks (bass, stab, bell, synth), any drum */
void tun_note_off(int track, int note);
void tun_clock(int rt);                  /* MIDI real time in: 0xF8 0xFA 0xFB 0xFC */
int tun_clock_out(uint8_t *rt);          /* the next real-time byte to send, 0 none */
void tun_panic(void);
void tun_arp(int track, const int8_t *notes, int n, int on);   /* the keys' arpeggiator: the notes held, sorted */

void tun_render(float *out, uint32_t n); /* n frames, L R L R ..., about +-1 */
