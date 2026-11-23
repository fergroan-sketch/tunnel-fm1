# TUNNEL FM-1 0.3, stage S1: implementation notes

Status: branch prepared under reviewer decision #9. No S1 code yet. S1 code starts only after the owner's
0.2 hardware test evidence is accepted. Nothing on this branch is to be merged, flashed or released.

Baseline: `7ebf36a` (v0.2-synth). Design: the 0.3 spec, amendments 1 to 4 and erratum 1 (all accepted in
decision #9). This file resolves the three implementation conditions I01 to I03. It is not a spec amendment.

## I01: the live-note timeout

- One engine block is 32 samples at 44,100 Hz. The timeout is exactly 41,345 blocks:
  41,345 x 32 / 44,100 = 30.00091 s.
- A record is stamped with the block count `t` of the block in which the engine processes its note-on.
  Events are read at a block boundary, once per 256-frame render call (tun.c, line 1268), so `t` is exact.
- Every block, before the voices render, each live record is checked. It expires in the first block where
  `(uint32_t)(now - t) >= 41345`. So it is still held at `t + 41,344` and released at `t + 41,345`, with
  no window: the expiry block is fixed.
- The 30 s is measured from engine processing, not from physical arrival. A MIDI or key event reaches the
  engine up to one render call (5.8 ms) after it arrives, plus any transport delay before that.
- Constant: `#define TUN_LIVE_TIMEOUT_BLOCKS 41345u`, with a static assert that it equals
  ceil(30 x 44,100 / 32) = ceil(41,343.75) + 1, so a later sample-rate change fails the build.
- Tests: T18b (41,344 held, 41,345 released), T18c (the same across the 32-bit wrap), T30b (MIDI and
  KEYS records with the same `t`, each released with its own source).

## I02: the REC hold for Bluetooth is not a keyboard-mode change

In 0.2, REC toggles `ui.keys_mode` on press (app.c, line 755). The 1-second hold then forces
`keys_mode = 0` and toggles Bluetooth (line 787). So a hold always ends with the keys out of REC, and from
REC-off it switches the mode twice.

S1 changes this:

- On builds with Bluetooth (`FM1B_BLE`), a REC press only starts the timer. The mode toggle happens on
  release, and only if REC was held under 1 s. A hold of 1 s or more toggles Bluetooth and leaves
  `keys_mode` exactly as it was. Without Bluetooth, REC toggles on press as now.
- `keys_input_set(mode)` is the only writer of `ui.keys_mode`. It returns at once if `mode` equals the
  current mode, so it never clears `key_live` or marks keys stale when nothing changes.
- The Bluetooth branch never calls `keys_input_set`.
- Cost: a short REC tap now takes effect on release, typically under 0.2 s later. That is the price of
  telling a tap from a hold.
- T34, corrected: in REC with C4 held (live), hold REC for 1 s. Bluetooth toggles, `keys_mode` is
  unchanged, C4 keeps sounding, no key is marked stale. Release REC: nothing else changes. Release C4: its
  note-off is sent. Repeat from REC off with a step key held: no step toggle, mode stays off.

## I03: one snapshot per input scan

`input()` reads the 27 keys once per scan (`keys = touch_keys()`, app.c line 688) and derives `down` and
`up` from that one sample. S1 keeps that and adds:

- Order inside a scan: read keys, then compute `down` and `up`, then strip stale keys from both
  (`down &= ~stale; up_stale = up & stale; up &= ~stale; stale &= ~up_stale`), then handle key edges in
  the current mode, then handle buttons.
- `keys_input_set()` takes its snapshot from the same `keys` sample: `stale = keys`. Every key down at
  that moment is stale, including one pressed in this scan (that key's note was already handled in the
  old mode, and is released by the leave step if it was live).
- A stale key's bit is cleared only by a real up edge in a later scan, and that up does nothing: no
  note-off, no step toggle, no mute or select. The same key's next down is a fresh press.
- Because `down` and `up` come from one level sample, a key cannot be both in a scan. With the strip done
  before any handling, no edge can reach a handler for a stale key.
- `keys_arp_update()` uses `keys & ~stale`, so keys held into ARP add no ARP notes (T35).
- Tests: T33 to T36 as in erratum 1, plus T37: a mode change and a press of the same key in one scan give
  one note in the old mode, released by the change, and nothing more until that key is up and down again.
  T38: a key that goes up in the scan right after a change sends nothing.

## S1 evidence report (to fill in after the hardware gate)

Commit hashes, host test results (0.2's 42 checks plus the new ones), RAM and pool use against 0.2
(RAM 82,968 / 98,304, pool 290,088 / 344,064, image 360,804 B), and any deviation from the accepted design.
