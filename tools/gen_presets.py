# TUNNEL FM-1 preset generator: writes app/factory.h and PRESETS.md.
# Every preset is made here from a recipe for its style and a seed: sounds, loops, tempo and effects. None copies a
# record or another instrument's presets. Run from tunnel-fm1/: python3 tools/gen_presets.py
import random, json, os

NAME_LEN = 11
STEPS = 32
KICK, SNARE, CH, OH, PERC, BASS, STAB, BELL, SYNTH = range(9)
NTRACKS = 9
# globals, in tun.h's order
G = ("bpm swing key dly_time dly_fb dly_tone rev_size rev_damp sweep drive volume lfo_rate lfo_depth lfo_dest lfo_shape "
     "comp duck duck_rel").split()
MINOR_PENT = [0, 3, 5, 7, 10]
MINOR = [0, 2, 3, 5, 7, 8, 10]
PHRYG = [0, 1, 3, 5, 7, 8, 10]

def clamp(v, lo, hi): return max(lo, min(hi, int(round(v))))

class Pre:
    def __init__(self, name, rng):
        self.name = name
        self.r = rng
        self.g = dict(bpm=134, swing=50, key=9, dly_time=4, dly_fb=50, dly_tone=60, rev_size=70, rev_damp=60, sweep=64,
                      drive=20, volume=100, lfo_rate=4, lfo_depth=0, lfo_dest=0, lfo_shape=0, comp=0, duck=0,
                      duck_rel=40)
        self.t = []
        for i in range(NTRACKS):
            self.t.append(dict(snd=[64] * 8, level=100, pan=64, dly=0, rev=0, len=16, div=0, root=0,
                               st=[(0, 0)] * STEPS))
    def snd(self, tr, *v):
        self.t[tr]["snd"] = [clamp(x, 0, 127) for x in (list(v) + [0] * 8)[:8]]
    def mix(self, tr, level, pan=64, dly=0, rev=0):
        self.t[tr].update(level=clamp(level, 0, 127), pan=clamp(pan, 0, 127), dly=clamp(dly, 0, 127),
                          rev=clamp(rev, 0, 127))
    def loop(self, tr, length, steps, div=0, root=0):
        """steps: {index: (vel 0..3, note, ratchet 0..3, prob 0..3, slide)}"""
        st = [(0, 0)] * STEPS
        for i, s in steps.items():
            vel, note, rat, prob, slide = (tuple(s) + (0, 0, 0, 0, 0))[:5] if isinstance(s, tuple) else (s, 0, 0, 0, 0)
            f = 1 | (vel & 3) << 1 | (rat & 3) << 3 | (prob & 3) << 5 | (0x80 if slide else 0)
            st[i % STEPS] = (f, max(-24, min(36, note)))
        self.t[tr].update(len=length, div=div, root=root, st=st)

# --- pattern pieces ------------------------------------------------------------------------------------------------
def four(vel=2): return {0: vel, 4: vel, 8: vel, 12: vel}
def hats16(r, accent_off=True, prob_ghosts=True):
    s = {}
    for i in range(16):
        if i % 4 == 2:
            s[i] = (2,)
        elif i % 2 == 1:
            s[i] = (1, 0, 0, 1 if prob_ghosts and r.random() < 0.3 else 0)
        else:
            s[i] = (0, 0, 0, 2 if prob_ghosts and r.random() < 0.5 else 0)
    return s
def offbeats(vel=2): return {2: vel, 6: vel, 10: vel, 14: vel}
def eighths(r):
    return {i: (2 if i % 4 == 2 else 1, 0, 0, 0) for i in range(0, 16, 2)}
def euclid(k, n, rot=0):
    pat = [((i * k) % n) < k for i in range(n)]
    return [i for i in range(n) if pat[(i - rot) % n]]
def scale_walk(r, n, scale, span=2, start=0):
    notes, idx = [], start
    for _ in range(n):
        idx += r.choice([-2, -1, 0, 1, 1, 2, 3])
        idx = max(0, min(len(scale) * span - 1, idx))
        notes.append(scale[idx % len(scale)] + 12 * (idx // len(scale)))
    return notes

# --- sounds ---------------------------------------------------------------------------------------------------------
def kit(p, hard=0.5, ride=False, clap=True):
    r = p.r
    p.snd(KICK, 40 + r.randint(-10, 15), 50 + 30 * hard + r.randint(-10, 10), 70 + 30 * hard, 40 + r.randint(-10, 20),
          40 + 50 * hard, 20 + 80 * hard + r.randint(-10, 10), 20, r.randint(0, 50) * hard)
    p.snd(SNARE, 1 if clap else r.choice([0, 2]), 50 + r.randint(-15, 15), 45 + r.randint(-10, 25), 80, 60, 60,
          20 + 40 * hard, 40)
    p.snd(CH, 60 + r.randint(-15, 15), 25 + r.randint(0, 30), 70 + r.randint(-20, 30), 70 + r.randint(-20, 30),
          30 + r.randint(0, 40))
    p.snd(OH, 1 if ride else 0, 60 + r.randint(-15, 15), 55 + r.randint(-15, 30), 60 + r.randint(-15, 30),
          70 + r.randint(-20, 30), 60 + r.randint(0, 50))
    p.snd(PERC, r.choice([0, 0, 1, 2, 3, 4]), 60 + r.randint(-25, 25), 40 + r.randint(-10, 30), 40 + r.randint(0, 50),
          50, 20 + 40 * hard)
    p.mix(KICK, 112, 64, 0, 4)
    p.mix(SNARE, 88, 64 + r.randint(-6, 6), 10, 28)
    p.mix(CH, 70, 64 + r.randint(-20, 20), 0, 10)
    p.mix(OH, 66, 64 + r.randint(-20, 20), 8, 18)
    p.mix(PERC, 74, 64 + r.randint(-30, 30), 25, 25)

def bass(p, wave=None, cut=40, res=60, env=70, dec=40, acc=60, drive=40, glide=20, level=96):
    r = p.r
    p.snd(BASS, r.choice([0, 1, 2]) if wave is None else wave, cut + r.randint(-8, 8), res + r.randint(-10, 10),
          env + r.randint(-10, 10), dec + r.randint(-10, 10), acc, drive, glide)
    p.mix(BASS, level, 64, 0, 0)

def stab(p, chord=None, cut=50, res=40, env=70, dec=35, det=40, wave=0, octv=2, level=82, dly=50, rev=40):
    r = p.r
    p.snd(STAB, r.choice([0, 1, 2, 6]) if chord is None else chord, cut + r.randint(-8, 8), res + r.randint(-10, 10),
          env + r.randint(-10, 10), dec + r.randint(-8, 8), det + r.randint(-10, 10), wave, octv)
    p.mix(STAB, level, 64 + r.randint(-10, 10), dly, rev)

def bell(p, mode=None, ratio=None, tone=60, dec=50, cut=70, warble=40, octv=2, spread=60, level=74, dly=60, rev=50):
    r = p.r
    p.snd(BELL, r.choice([0, 1]) if mode is None else mode, r.choice([2, 3, 4, 7]) if ratio is None else ratio,
          tone + r.randint(-15, 15), dec + r.randint(-10, 15), cut + r.randint(-15, 15), warble, octv, spread)
    p.mix(BELL, level, 64, dly, rev)

# the synth: WAVE 0 saw 1 pulse 2 swept pulse 3 swept saw ("hoover") 4 saw+sub 5 square+sub 6 sines 7 sine+saw
def synth(p, wave=0, cut=60, res=30, env=50, att=0, dec=50, sus=60, cho=80, level=80, pan=64, dly=30, rev=40):
    r = p.r
    p.snd(SYNTH, wave, cut + r.randint(-6, 6), res + r.randint(-6, 6), env + r.randint(-8, 8), att, dec + r.randint(-6, 6),
          sus, cho)
    p.mix(SYNTH, level, pan, dly, rev)

# --- styles ---------------------------------------------------------------------------------------------------------
def style_hard(p):
    r = p.r
    p.g.update(bpm=r.randint(134, 142), swing=r.choice([50, 50, 52]), dly_time=r.choice([3, 4]), drive=r.randint(30, 60),
               rev_size=r.randint(50, 80))
    kit(p, hard=0.8, ride=False, clap=r.random() < 0.7)
    k = four(2)
    if r.random() < 0.5: k[15] = (0, 0, 0, 1)
    p.loop(KICK, 16, k)
    p.loop(SNARE, 16, {4: 2, 12: 2} if r.random() < 0.7 else {4: 2, 12: 2, 14: (0, 0, 0, 2)})
    p.loop(CH, 16, hats16(r))
    p.loop(OH, 16, offbeats(2))
    perc_len = r.choice([3, 5, 6, 7])
    p.loop(PERC, perc_len, {i: (1, r.choice([0, 3, 7, -5])) for i in euclid(r.choice([1, 2]), perc_len)})
    bass(p, cut=30, res=40, env=60, dec=30, drive=60)
    n = r.choice([0, 0, -2, 3])
    bl = r.choice([8, 16, 7])
    p.loop(BASS, bl, {i: (r.choice([1, 2]), n + r.choice([0, 0, 12, 0, -12 if n > 0 else 0])) for i in
                      [x for x in range(bl) if x % 4 != 0 and r.random() < 0.55]})
    stab(p, dec=25, level=70)
    p.loop(STAB, 16, {r.choice([3, 6, 10, 11, 14]): (1, 0)} if r.random() < 0.6 else {})
    bell(p, level=60)
    p.loop(BELL, 16, {})

def style_hypnotic(p):
    r = p.r
    p.g.update(bpm=r.randint(128, 136), swing=r.choice([50, 52, 54]), dly_time=r.choice([2, 4, 3]),
               dly_fb=r.randint(55, 80), lfo_rate=r.choice([0, 1, 2]), lfo_depth=r.randint(30, 70),
               lfo_dest=r.choice([2, 3, 4]), lfo_shape=r.choice([0, 1]), rev_size=r.randint(70, 100))
    kit(p, hard=0.55)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {4: 1, 12: 1} if r.random() < 0.5 else {12: 1})
    p.loop(CH, 16, hats16(r))
    p.loop(OH, r.choice([16, 12]), offbeats(1))
    pl = r.choice([5, 7, 9])
    p.loop(PERC, pl, {i: (1, r.choice([0, 5, 7])) for i in euclid(r.choice([2, 3]), pl)})
    bass(p, cut=34, res=50, env=50, dec=50)
    p.loop(BASS, 16, {i: (1, 0) for i in [2, 6, 10, 14] if r.random() < 0.8})
    scale = r.choice([MINOR, PHRYG])
    sl = r.choice([7, 7, 5, 9, 11])                # the loop that rolls against the bar
    notes = scale_walk(r, sl, scale)
    stab(p, chord=r.choice([1, 2, 6]), dec=30, dly=70, rev=50)
    hits = euclid(r.choice([2, 3]), sl, r.randint(0, sl - 1))
    p.loop(STAB, sl, {i: (r.choice([1, 2]), notes[i] - 12 if notes[i] > 7 else notes[i]) for i in hits})
    bl = r.choice([5, 7, 9, 11, 13])
    bnotes = scale_walk(r, bl, scale, span=3)
    bell(p, mode=1, dec=40, warble=60)
    p.loop(BELL, bl, {i: (r.choice([1, 2]), bnotes[i] + (12 if r.random() < 0.2 else 0)) for i in range(bl)
                      if r.random() < 0.7}, div=r.choice([0, 0, 1]))

def style_stab(p):
    r = p.r
    p.g.update(bpm=r.randint(125, 132), swing=r.randint(54, 62), dly_time=4, dly_fb=r.randint(40, 60),
               rev_size=r.randint(60, 90))
    kit(p, hard=0.4, ride=r.random() < 0.4, clap=r.random() < 0.5)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {4: 1, 12: 2})
    p.loop(CH, 16, {i: (1 if i % 4 else 0, 0, 0, 1 if r.random() < 0.3 else 0) for i in range(2, 16, 2)})
    p.loop(OH, 16, offbeats(1))
    p.loop(PERC, 16, {r.choice([3, 7, 11]): (1, 0), r.choice([13, 15]): (0, 5)})
    bass(p, cut=30, res=40, env=40, dec=45)
    p.loop(BASS, 16, {i: (1, 0) for i in [3, 7, 11, 14] if r.random() < 0.7})
    chord = r.choice([1, 2, 1, 3])
    stab(p, chord=chord, cut=40, res=35, env=75, dec=r.randint(25, 45), det=30, level=86, dly=60, rev=45)
    seq = [0, 0, r.choice([-2, 3, 5]), 0]
    hits = sorted(r.sample([1, 3, 6, 7, 10, 11, 13, 14], r.randint(3, 5)))
    p.loop(STAB, r.choice([16, 16, 12]), {h: (r.choice([1, 2]), seq[j % 4]) for j, h in enumerate(hits)})
    bell(p, level=50)
    p.loop(BELL, 16, {})

def style_ride(p):
    r = p.r
    p.g.update(bpm=r.randint(126, 134), swing=r.randint(55, 64), dly_time=3, rev_size=r.randint(60, 90))
    kit(p, hard=0.45, ride=True, clap=False)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {4: (1,), 12: (1,), 7: (0, 0, 0, 2), 15: (0, 0, 0, 2)})
    p.loop(CH, 16, {i: (1, 0, 0, 1) for i in [2, 6, 10, 14]})
    p.loop(OH, 16, {i: (2 if i % 4 == 2 else 1,) for i in range(0, 16, 2)})
    pl = r.choice([6, 7, 12])
    p.snd(PERC, 1, 50 + r.randint(-20, 20), 40, 30, 50, 20)
    p.loop(PERC, pl, {i: (1, r.choice([0, 7, 12])) for i in euclid(r.choice([3, 4, 5]), pl)})
    bass(p, cut=28, res=35, env=45, dec=50)
    p.loop(BASS, 16, {i: (1, r.choice([0, 0, 0, 3, -2])) for i in [2, 5, 10, 13] if r.random() < 0.8})
    stab(p, chord=r.choice([1, 2]), dec=35, level=70, dly=55)
    p.loop(STAB, 16, {r.choice([6, 14]): (1, 0)})
    bell(p, level=56)
    p.loop(BELL, 16, {})

def style_bells(p):
    r = p.r
    p.g.update(bpm=r.randint(132, 140), swing=50, dly_time=r.choice([4, 3, 6]), dly_fb=r.randint(45, 70),
               rev_size=r.randint(70, 105), lfo_rate=1, lfo_depth=r.randint(20, 50), lfo_dest=3)
    kit(p, hard=0.6)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {4: 1, 12: 2})
    p.loop(CH, 16, hats16(r))
    p.loop(OH, 16, offbeats(1))
    p.snd(PERC, 0, 40 + r.randint(-10, 30), 50, 60, 40, 30)
    tl = r.choice([3, 6, 12])
    p.loop(PERC, tl, {i: (1, r.choice([0, 5, 7, 12])) for i in euclid(r.choice([2, 3]), tl)})
    bass(p, cut=30, res=40, env=50, dec=40)
    p.loop(BASS, 16, {i: (1, 0) for i in [2, 6, 10, 14]})
    scale = r.choice([MINOR, MINOR_PENT, PHRYG])
    # two interlocking bell loops: the bell track, and the stab played as a soft second bell
    bl = r.choice([5, 7, 9, 11, 13])
    bn = scale_walk(r, bl, scale, span=3, start=3)
    bell(p, mode=r.choice([0, 1]), ratio=r.choice([2, 3, 4]), tone=r.randint(40, 90), dec=r.randint(35, 70),
         warble=r.randint(20, 70), level=80, dly=70, rev=60)
    p.loop(BELL, bl, {i: (r.choice([1, 2, 2]), bn[i], 1 if r.random() < 0.1 else 0) for i in range(bl)
                      if r.random() < 0.75}, div=r.choice([0, 0, 2]))
    sl = r.choice([7, 9, 11]) if bl != 7 else 9
    stab(p, chord=7, cut=60, res=20, env=40, dec=20, det=10, wave=2, octv=3, level=58, dly=70, rev=50)
    sn = scale_walk(r, sl, scale, start=2)
    p.loop(STAB, sl, {i: (1, sn[i]) for i in euclid(r.choice([2, 3]), sl)})

def style_acid(p):
    r = p.r
    p.g.update(bpm=r.randint(130, 140), swing=r.choice([50, 54, 56]), dly_time=4, dly_fb=r.randint(30, 55),
               drive=r.randint(30, 60), lfo_rate=r.choice([0, 1]), lfo_depth=r.randint(30, 60), lfo_dest=1)
    kit(p, hard=0.6)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {4: 2, 12: 2})
    p.loop(CH, 16, hats16(r))
    p.loop(OH, 16, offbeats(2))
    p.loop(PERC, 16, {})
    bass(p, wave=r.choice([0, 1]), cut=r.randint(25, 45), res=r.randint(85, 115), env=r.randint(70, 100),
         dec=r.randint(25, 45), acc=r.randint(70, 110), drive=r.randint(40, 80), glide=40, level=90)
    bl = r.choice([16, 16, 12, 13])
    scale = r.choice([MINOR_PENT, PHRYG])
    notes = scale_walk(r, bl, scale, span=2)
    s = {}
    for i in range(bl):
        if r.random() < 0.72:
            s[i] = (r.choice([1, 1, 2, 3]), notes[i] - (12 if r.random() < 0.25 else 0), 0, 0, r.random() < 0.25)
    p.loop(BASS, bl, s)
    stab(p, level=60)
    p.loop(STAB, 16, {})
    bell(p, level=50)
    p.loop(BELL, 16, {})

def style_scifi(p):
    r = p.r
    p.g.update(bpm=r.randint(128, 138), swing=50, dly_time=r.choice([2, 4, 6]), dly_fb=r.randint(55, 85),
               dly_tone=r.randint(60, 110), rev_size=r.randint(85, 120), lfo_rate=r.choice([6, 7, 8, 9]),
               lfo_depth=r.randint(40, 90), lfo_dest=r.choice([3, 4, 6]), lfo_shape=3)
    kit(p, hard=0.5)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {12: 1})
    p.loop(CH, 16, hats16(r))
    p.loop(OH, 16, offbeats(1))
    p.snd(PERC, 4, 70 + r.randint(-20, 30), 20, 80, 50, 30)
    pl = r.choice([5, 7, 11])
    p.loop(PERC, pl, {i: (1, r.choice([0, 7, 12, 19, 24])) for i in euclid(r.choice([2, 3, 4]), pl)}, div=2)
    bass(p, cut=30, res=50, env=50, dec=40)
    p.loop(BASS, 16, {i: (1, 0) for i in [2, 6, 10, 14]})
    bell(p, mode=0, ratio=r.choice([7, 13, 15, 9]), tone=r.randint(60, 110), dec=r.randint(25, 50), warble=90,
         octv=r.choice([2, 3]), level=74, dly=80, rev=70)
    bl = r.choice([7, 9, 11, 13])
    p.loop(BELL, bl, {i: (r.choice([1, 2]), r.choice([0, 7, 12, 19, 24, 31]), r.choice([0, 0, 1]))
                      for i in range(bl) if r.random() < 0.6}, div=r.choice([0, 2]))
    stab(p, chord=6, cut=60, res=70, env=80, dec=20, det=80, level=60, dly=80, rev=70)
    p.loop(STAB, 16, {r.choice([3, 11]): (1, 0)})

def style_broken(p):
    r = p.r
    p.g.update(bpm=r.randint(130, 140), swing=r.choice([50, 56]), dly_time=4, dly_fb=r.randint(40, 65))
    kit(p, hard=0.6, clap=False)
    k = {0: 2, 3: (1,), 6: (2,), 10: (2,), 13: (1, 0, 0, 1)}
    p.loop(KICK, 16, k)
    p.loop(SNARE, 16, {4: 2, 12: 2, 14: (0, 0, 2, 1)})
    p.loop(CH, 16, hats16(r))
    p.loop(OH, 16, {6: 1, 14: 1})
    p.snd(PERC, 0, 40 + r.randint(-20, 30), 55, 70, 40, 40)
    p.loop(PERC, 16, {i: (r.choice([1, 2]), r.choice([0, 3, 5, 7, 12]), r.choice([0, 0, 1, 2]))
                      for i in sorted(r.sample(range(16), r.randint(5, 8)))})
    bass(p, cut=30, res=50, env=60, dec=35)
    p.loop(BASS, 16, {i: (1, r.choice([0, 0, 3, -2])) for i in [0, 3, 6, 10, 13] if r.random() < 0.7})
    stab(p, dec=25, level=68)
    p.loop(STAB, 16, {r.choice([7, 15]): (1, 0)})
    bell(p, level=56)
    p.loop(BELL, 16, {})

def style_warehouse(p):
    r = p.r
    p.g.update(bpm=r.randint(138, 146), swing=50, dly_time=r.choice([1, 3]), dly_fb=r.randint(40, 70),
               drive=r.randint(55, 95), rev_size=r.randint(70, 100))
    kit(p, hard=1.0, clap=r.random() < 0.6)
    p.snd(KICK, 40 + r.randint(-10, 10), 70 + r.randint(-10, 20), 100, 50, 90, 110, 30, 60)
    p.loop(KICK, 16, four(3))
    p.loop(SNARE, 16, {4: 2, 12: 2})
    p.loop(CH, 16, {i: (2 if i % 2 else 1,) for i in range(16)})
    p.loop(OH, 16, offbeats(2))
    pl = r.choice([3, 6])
    p.loop(PERC, pl, {i: (1, 0) for i in euclid(1, pl)})
    bass(p, wave=0, cut=24, res=30, env=50, dec=55, drive=100, level=90)
    p.loop(BASS, 16, {i: (1, 0) for i in [1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15] if r.random() < 0.6})
    stab(p, chord=r.choice([0, 4, 6]), dec=20, level=72, dly=40)
    p.loop(STAB, r.choice([16, 7]), {r.choice([2, 3, 5]): (2, 0)})
    bell(p, level=50)
    p.loop(BELL, 16, {})

def style_dub(p):
    r = p.r
    p.g.update(bpm=r.randint(120, 128), swing=r.randint(52, 58), dly_time=4, dly_fb=r.randint(65, 90),
               dly_tone=r.randint(30, 60), rev_size=r.randint(95, 125), rev_damp=80)
    kit(p, hard=0.3, ride=r.random() < 0.5, clap=False)
    p.snd(SNARE, 2, 60, 40, 40, 40, 50, 10, 40)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {r.choice([7, 11]): (1,), 15: (0, 0, 0, 2)})
    p.loop(CH, 16, {i: (1, 0, 0, 1) for i in range(2, 16, 4)})
    p.loop(OH, 16, offbeats(1))
    p.loop(PERC, 16, {})
    bass(p, wave=0, cut=22, res=30, env=30, dec=60, drive=20)
    p.loop(BASS, 16, {i: (1, 0) for i in [0, 3, 8, 11]})
    stab(p, chord=r.choice([1, 2]), cut=36, res=50, env=60, dec=40, det=40, level=84, dly=110, rev=80)
    p.loop(STAB, 16, {r.choice([3, 4, 6]): (2, 0), r.choice([10, 11]): (1, r.choice([0, -2, 3]))})
    bell(p, level=50)
    p.loop(BELL, 16, {})

def style_synth(p):
    """the synth up front: hoover riffs, brass stabs, swept-pulse pads, plucks and sine leads over a stripped kit"""
    r = p.r
    kind = p.idx % 5
    scale = r.choice([MINOR, PHRYG])
    p.g.update(bpm=r.randint(130, 140), swing=r.choice([50, 52]), dly_time=r.choice([3, 4, 6]), dly_fb=r.randint(35, 60),
               rev_size=r.randint(65, 100))
    kit(p, hard=0.7 if kind == 0 else 0.5, ride=kind == 2, clap=r.random() < 0.5)
    p.loop(KICK, 16, four(2))
    p.loop(SNARE, 16, {4: 1, 12: 2} if kind != 2 else {12: 1})
    p.loop(CH, 16, hats16(r))
    p.loop(OH, 16, offbeats(1))
    p.loop(PERC, 16, {})
    bass(p, wave=r.choice([3, 3, 4, 0]), cut=30, res=30, env=40, dec=45, drive=30)
    p.loop(BASS, 16, {i: (1, 0) for i in [2, 6, 10, 14] if r.random() < 0.85})
    stab(p, level=60)
    p.loop(STAB, 16, {})
    bell(p, level=50)
    p.loop(BELL, 16, {})
    if kind == 0:                       # the hoover: a swept saw, chorus full, a riff that leaps octaves
        synth(p, wave=3, cut=88, res=18, env=30, att=0, dec=55, sus=95, cho=120, level=78, dly=40, rev=45)
        sl = r.choice([16, 16, 12])
        riff = {0: (2, 0), 3: (1, 0), 6: (2, 12), 8: (1, r.choice([-2, 3])), 11: (2, 0), 14: (1, r.choice([7, 10, 12]))}
        p.loop(SYNTH, sl, {k: v for k, v in riff.items() if k < sl})
    elif kind == 1:                     # brass stabs: saw and sub, the envelope snapping the filter open
        synth(p, wave=4, cut=34, res=30, env=82, att=6, dec=36, sus=30, cho=70, level=76, dly=50, rev=40)
        hits = sorted(r.sample([0, 3, 6, 7, 10, 11, 13, 14], r.randint(3, 4)))
        p.loop(SYNTH, 16, {h: (r.choice([1, 2]), r.choice([0, 0, 3, -2])) for h in hits})
    elif kind == 2:                     # a pad: swept pulse, slow in and out, one chord tone a bar
        synth(p, wave=2, cut=58, res=20, env=25, att=80, dec=95, sus=110, cho=110, level=74, dly=20, rev=90)
        tones = r.choice([[0, 3, 7, -2], [0, -2, -4, -5], [0, 5, 3, 0]])
        p.loop(SYNTH, 32, {8 * j: (1, tones[j]) for j in range(4)}, div=1)
    elif kind == 3:                     # a pluck: a square and its sub, short, rolling at an odd length
        synth(p, wave=5, cut=40, res=55, env=70, att=0, dec=28, sus=0, cho=50, level=74, dly=70, rev=50)
        sl = r.choice([5, 7, 9, 11])
        notes = scale_walk(r, sl, scale, span=2)
        p.loop(SYNTH, sl, {i: (r.choice([1, 2]), notes[i]) for i in range(sl) if r.random() < 0.7})
    else:                               # a sine lead: pure and glassy, through the delay
        synth(p, wave=r.choice([6, 7]), cut=80, res=10, env=20, att=12, dec=45, sus=70, cho=60, level=72, dly=80, rev=60)
        sl = r.choice([7, 9, 12])
        notes = scale_walk(r, sl, scale, span=2, start=4)
        p.loop(SYNTH, sl, {i: (1, notes[i]) for i in euclid(r.choice([3, 4]), sl)}, div=r.choice([0, 1]))

STYLES = [  # name, function, count, name words
    ("Hard tools", style_hard, 13, "STEEL TORQUE MOTOR PISTON FORGE RIVET ANVIL GIRDER BOLT FURNACE DYNAMO TURBINE "
     "CRANK CHASSIS GEAR LATHE"),
    ("Hypnotic loops", style_hypnotic, 13, "TUNNEL SPIRAL ORBIT HELIX VORTEX LOOP CIRCUIT RELAY MOBIUS GYRE COIL "
     "RECURSE DRIFT TRANCE PENDULUM CYCLE"),
    ("Stab grooves", style_stab, 12, "SIGNAL CHORD MEMO PULSE TRANSIT CROSSING AVENUE GRID BLOCK SUBWAY CORNER "
     "STATION DISTRICT SECTOR"),
    ("Ride grooves", style_ride, 11, "CYMBAL FLOAT GLIDE CRUISE SWAY TIDE RIPPLE SHIMMER RIDE BREEZE CURRENT FLOW"),
    ("Bell polymeters", style_bells, 12, "CHIME BEACON LATTICE PRISM CRYSTAL TOWER CLOCK QUARTZ ASTRAL CELESTE "
     "HALO RINGS TEMPLE SPIRE"),
    ("Acid and slides", style_acid, 11, "ACIDITY CORRODE ETCH SOLVENT REACTOR CATALYST ION ALKALI OXIDE ENZYME "
     "TOXIN VAPOR"),
    ("Sci-fi", style_scifi, 12, "QUASAR NEUTRON PULSAR NEBULA ZENITH VECTOR PROBE ORBITER TELEMETRY RADAR "
     "SATELLITE COSMOS"),
    ("Broken and toms", style_broken, 12, "FRACTURE SPLINTER RUPTURE FAULT TREMOR SEISMIC JOLT SHATTER STUTTER "
     "SKIP SCATTER BREAK"),
    ("Warehouse", style_warehouse, 10, "BUNKER CONCRETE SILO HANGAR DOCK BASEMENT VAULT SHAFT DEPOT CELLAR "
     "TERMINAL BOILER"),
    ("Dub and deep", style_dub, 7, "DEPTH ABYSS FATHOM SONAR UNDERTOW MIST HAZE"),
    ("Synth", style_synth, 14, "HOOVER BRASS VAPOUR PLUCK GLASS SIREN HORN AURORA FILAMENT MIRAGE ENGINE FANFARE "
     "NIMBUS FLARE"),
]

def init_preset():
    p = Pre("INIT", random.Random(0))
    p.g.update(bpm=130, dly_fb=40, rev_size=60, drive=10, comp=20, duck=30)
    kit(p, hard=0.5)
    p.loop(KICK, 16, four(2))
    p.loop(CH, 16, offbeats(1))
    bass(p, wave=0)
    stab(p, chord=0)
    bell(p, mode=1)
    synth(p, wave=0)
    for tr in (SNARE, OH, PERC, BASS, STAB, BELL, SYNTH):
        p.loop(tr, 16, {})
    return p

def dynamics(p, cat):
    """the compressor and the kick's duck, after the style's recipe (their own random numbers, so the recipes'
    sounds and loops are what they were in 0.1)"""
    r = random.Random(sum(ord(c) for c in p.name) * 131 + 17)
    hard = cat in ("Hard tools", "Warehouse", "Acid and slides")
    p.g["comp"] = r.randint(40, 70) if hard else r.randint(20, 45)
    p.g["duck"] = r.randint(35, 70) if cat not in ("Dub and deep",) else r.randint(20, 40)
    p.g["duck_rel"] = r.randint(25, 55)

def build():
    bank = [("INIT", init_preset())]
    used = set()
    for cat, fn, n, words in STYLES:
        ws = words.split()
        assert len(ws) >= n, cat
        for i in range(n):
            name = ws[i][:NAME_LEN]
            assert name not in used, name
            used.add(name)
            p = Pre(name, random.Random(hash(name) & 0xFFFFFFFF if False else sum(ord(c) * (k + 7) for k, c in enumerate(name)) * 7919 + i))
            p.g["key"] = p.r.choice([9, 9, 2, 4, 7, 0, 5, 10])
            p.idx = i
            fn(p)
            dynamics(p, cat)
            bank.append((cat, p))
    assert len(bank) == 128, len(bank)
    return bank

LEVELS = {}
if os.path.exists("tools/levels.json"):
    LEVELS = json.load(open("tools/levels.json"))   # each preset's VOLUME, from renders (tools/level_match.py)

def c_preset(p):
    g = [clamp(p.g[k], 0, 255) for k in G]
    if p.name in LEVELS:
        g[G.index("volume")] = LEVELS[p.name]
    tr = []
    for t in p.t:
        steps = ", ".join("{%d, %d}" % s for s in t["st"])
        tr.append("{{%s}, %d, %d, %d, %d, %d, %d, %d, 0, {%s}}" % (", ".join(str(x) for x in t["snd"]), t["level"],
                  t["pan"], t["dly"], t["rev"], t["len"], t["div"], t["root"], steps))
    return '{"%s", {%s}, {0}, {%s}},' % (p.name, ", ".join(str(x) for x in g), ",\n  ".join(tr))

def main():
    bank = build()
    with open("app/factory.h", "w") as f:
        f.write("/* written by tools/gen_presets.py: TUNNEL FM-1's factory presets, every one made by its recipes */\n")
        for cat, p in bank:
            f.write(c_preset(p) + "\n")
    with open("PRESETS.md", "w") as f:
        f.write("# TUNNEL FM-1 presets\n\n128 presets, 000 to 127, by style. Each is a whole machine: the nine "
                "tracks' sounds and loops, the tempo, the swing, the key and the effects. tools/gen_presets.py makes "
                "them from a recipe for each style; none is copied from a record or another instrument.\n")
        last = None
        for i, (cat, p) in enumerate(bank):
            if cat != last:
                f.write("\n## %s\n\n" % cat)
                last = cat
            lens = [p.t[k]["len"] for k in range(NTRACKS)]
            poly = sorted(set(l for l in lens if l not in (16, 8, 32)))
            f.write("- %03d %s (%d BPM%s)\n" % (i, p.name, p.g["bpm"],
                    ", loops of " + " and ".join(str(x) for x in poly) + " steps" if poly else ""))
    print(len(bank), "presets")

if __name__ == "__main__":
    main()
