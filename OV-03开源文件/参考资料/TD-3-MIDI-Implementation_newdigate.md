# Behringer TD-3 — MIDI Implementation (Technical Reference)

A practical, byte-level reference for the Behringer TD-3's MIDI implementation, as
implemented by this library (`include/td3/`). It is **unofficial and reverse-engineered**;
it is not affiliated with or endorsed by Behringer / Music Tribe. Where a detail has been
checked against real hardware it is marked **(verified)**; verification was done against a
**TD-3-MO** on 2026-06-26.

DFU / firmware-update mode is intentionally out of scope.

---

## 1. Conventions

### 1.1 SysEx framing

Every configuration / identity / pattern message is a MIDI System Exclusive frame:

```
F0 00 20 32 00 01 0A <opcode> <payload…> F7
└┬┘ └────┬────┘ └─┬─┘ └──┬───┘            └┬┘
 │   manufacturer │   opcode (byte 7)      end
 │   00 20 32     │
 │   (Behringer)  device/model id 00 01 0A (TD-3)
 SysEx start
```

- **Bytes 0–6** — fixed 7-byte header: `F0 00 20 32 00 01 0A`.
  `00 20 32` is Music Tribe / Behringer; `00 01 0A` identifies the TD-3.
- **Byte 7** — opcode (see the [reference table](#9-opcode-quick-reference)).
- **Bytes 8…** — opcode-specific payload.
- **Last byte** — `F7` (End of Exclusive).

A frame is recognized as TD-3 SysEx when it starts with the 7-byte header, ends with
`F7`, and is at least 9 bytes long.

### 1.2 8-bit values over 7-bit MIDI (nibble split)

MIDI data bytes must stay ≤ `0x7F`, so 8-bit values are split into two bytes carrying one
nibble each, **most-significant nibble first**:

```
value = (MSB << 4) | (LSB & 0x0F)        e.g.  01 0F → 0x1F
```

A few fields use a single `(00, value)` pair where only the second byte matters; these are
noted where they occur.

### 1.3 Channels

Realtime channel messages (notes, etc.) use the configured MIDI channel. SysEx is
channel-independent.

---

## 2. Identity & Firmware

| Request | Frame | Response | Payload |
|---|---|---|---|
| Model code | `…0A 04 F7` | `…0A 05 <ascii…> F7` | ASCII string from byte 8 until `00`/`F7` |
| Product name | `…0A 06 F7` | `…0A 07 <ascii…> F7` | ASCII string from byte 8 |
| Firmware | `…0A 08 00 F7` | `…0A 09 <00> <maj> <min> <rev> F7` | byte 8 = `00`; bytes 9/10/11 = major/minor/revision |

---

## 3. Configuration

### 3.1 Get / response

**Get:** `…0A 75 F7`. **Response:** opcode `0x76`, payload at bytes 8–17:

| Byte | Field | Encoding |
|---|---|---|
| `[8]`  | MIDI out channel | 0–15 |
| `[9]`  | MIDI in channel | 0–15 |
| `[10]` | transpose | stored as value + 12 → −12…+12 |
| `[11]` | pitch-bend range | 0–12 semitones |
| `[12]` | key priority | `0` Low · `1` High · `2` Last |
| `[13]` | multi-trigger | `0` off · `1` on |
| `[14]` | clock-trigger polarity | `0` Fall · `1` Rise |
| `[15]` | clock-trigger rate | `0` 1 PPS · `1` 2 PPQ · `2` 24 PPQ · `8` 48 PPQ |
| `[16]` | clock source | `0` Internal · `1` MIDI DIN · `2` MIDI USB · `3` Trigger |
| `[17]` | accent velocity threshold | 0–127 |

**Reset to defaults:** `…0A 7D F7` (no payload).

### 3.2 Setters

Each setter is acknowledged by the device with an **ACK** (`…0A 01 F7`).

| Setting | Opcode | Payload |
|---|---|---|
| MIDI channels | `0x0E` | `<out> 00 <in>` |
| Transpose | `0x0F` | `<semitones + 12>` (−12…+12 → 0…24) |
| Pitch-bend range | `0x11` | `<semitones> 00` |
| Key priority | `0x12` | `0` Low / `1` High / `2` Last |
| Multi-trigger | `0x14` | `<0|1> 00` |
| Clock-trigger polarity | `0x19` | `0` Fall / `1` Rise |
| Clock-trigger rate | `0x1A` | `0` 1 PPS / `1` 2 PPQ / `2` 24 PPQ / `3` 48 PPQ |
| Clock source | `0x1B` | `0` Internal / `1` MIDI DIN / `2` MIDI USB / `3` Trigger |
| Accent velocity threshold | `0x1C` | `0`–`127` |

> **Quirk — clock-rate encoding is direction-specific.** 48 PPQ is reported as `0x08` in the
> *get-config* response (byte 15) but set with `0x03` via opcode `0x1A`. The other three
> rates share encodings (`0/1/2`).

---

## 4. Patterns

The TD-3 holds **64 patterns**: 4 groups (`0–3`) × 16 patterns per group (bank A 1–8 =
index `0–7`, bank B 1–8 = index `8–15`).

### 4.1 Request

`…0A 77 <group> <index> F7` — `group` 0–3, `index` 0–15.

### 4.2 Dump / Put (opcode `0x78`)

The device answers a request with a 123-byte dump. **The same frame can be sent back
verbatim to store a pattern** ("Put Pattern"). Byte offsets, relative to frame start:

| Offset | Field | Notes |
|---|---|---|
| `[8]` | group | 0–3 |
| `[9]` | index | 0–15 |
| `[10..11]` | reserved | `00 00` |
| `[12..43]` | pitches | 16 steps × `(MSB, LSB)`; note = `((MSB<<4)|LSB) & 0x7F`. One octave below MIDI standard; the inactive/filler pitch is conventionally `0x18` (C2). |
| `[44..75]` | accent | 16 × `(00, bool)` — accent flag in the second byte of each pair **(verified)** |
| `[76..107]` | slide | 16 × `(00, bool)` |
| `[108..109]` | triplet | `(00, bool)` |
| `[110..111]` | step count | `(MSB, LSB)` → 1–16 (e.g. `01 00` = 16) |
| `[112..113]` | reserved | `00 00` |
| `[114..117]` | **gate / trigger bitmask** | 4 nibble bytes — see below **(verified)** |
| `[118..121]` | **rest bitmask** | 4 nibble bytes — see below |
| `[122]` | `F7` | |

### 4.3 Gate / rest / tie semantics **(verified on hardware)**

Two 4-byte bitmasks at the end of the dump describe, per step, whether it fires:

- **`[114..117]` — gate / trigger.** A **set** bit means the step **articulates a new note**
  (an onset). A **clear** bit means the step is **tied / legato** — it holds the previous
  note instead of retriggering.
- **`[118..121]` — rest.** A **set** bit **silences** the step.

So each step is one of three states:

| State | gate bit `[114..117]` | rest bit `[118..121]` |
|---|---|---|
| Onset (new note) | 1 | 0 |
| Tie (legato/slide) | 0 | 0 |
| Rest (silent) | 0 | 1 |

> ⚠️ **Errata.** The widely-circulated unofficial doc labels `[114..117]` as a *tie* mask
> with inverted polarity. It is actually the **gate**. Writing ties there (and leaving
> onsets clear) makes every step tie — nothing retriggers — and the pattern plays silent.
> Set the gate on every onset.

### 4.4 Bitmask step→bit layout

Both masks use only the **low nibble** of each byte; the 16 steps map across the four bytes
as:

```
byte0: ____ 7654   byte1: ____ 3210   byte2: ____ FEDC   byte3: ____ BA98
       steps 8–5          steps 4–1          steps 16–13        steps 12–9
```

(bit 0 = the lowest-numbered step in each group; step numbers above are 1-based.) Example:
gate `01 01 01 01` = onsets on steps 1, 5, 9, 13 (a four-on-the-floor); rest `0E 0E 0E 0E`
= the other twelve steps silent.

### 4.5 Storing patterns — pace bulk writes **(verified)**

A Put-Pattern dump is **not acknowledged**: the device stores it and replies with nothing.
Each dump is 123 bytes, and the TD-3's MIDI input cannot absorb them at full rate — sent
back-to-back, only the first handful (≈ 7) are stored and the rest are **silently dropped**
(no error). When programming many slots, **pace the dumps**: leave roughly **50 ms** between
them (empirically reliable for a full 64-slot write; tune to taste). With no handshake to
wait on, a fixed inter-message delay is the simplest reliable approach. (`td3batch` paces by
default and exposes `--delay <ms>`.)

---

## 5. Test Mode

**Enter:** `…0A 50 01 F7`. **Exit:** `…0A 50 00 F7`.

While in test mode the device streams panel activity as **3-byte channel-style messages**
(status `0xA0`–`0xA4`, *not* SysEx):

| Status | Meaning | data1 | data2 |
|---|---|---|---|
| `0xA0` | Tempo knob | value low 7 bits | value high bits |
| `0xA1` | Track knob | value low 7 bits | value high bits |
| `0xA2` | Mode knob | value low 7 bits | value high bits |
| `0xA4` | Button | button code | pressed (`≠0`) |

Knob value = `data1 | (data2 << 7)`.

Button codes:

| Code | Button | Code | Button | Code | Button |
|---|---|---|---|---|---|
| `0x00` | Clear | `0x08` | E | `0x10` | C1 |
| `0x01` | Start/Stop | `0x09` | F | `0x11` | Step |
| `0x02` | Pitch Mode | `0x0A` | F♯ | `0x12` | Triplet |
| `0x03` | Bar | `0x0B` | G | `0x13` | Pattern A |
| `0x04` | C0 | `0x0C` | G♯ | `0x14` | Slide |
| `0x05` | C♯ | `0x0D` | A | `0x15` | Pattern B |
| `0x06` | D | `0x0E` | A♯ | `0x16` | Back |
| `0x07` | D♯ | `0x0F` | B | `0x17` | Write/Next |

---

## 6. Loopback

`…0A 7E F7` — the device echoes a loopback frame (opcode `0x7E`); the returned payload is
bytes `[8 .. len-2]`. Useful as a round-trip / connectivity check.

---

## 7. Realtime & Performance (standard MIDI, not SysEx)

| Message | Bytes |
|---|---|
| Note On | `9n <note> <velocity>` (`n` = channel) |
| Note Off | `8n <note> 00` |
| Timing clock | `F8` |
| Start | `FA` |
| Continue | `FB` |
| Stop | `FC` |

Slides between sequenced notes are produced by overlapping note-ons (note-on of the next
note before note-off of the current); the library exposes raw note on/off and leaves slide
timing to the caller.

> ⚠️ **Errata.** The unofficial doc lists **Start** as `FB`, which is **Continue** in
> standard MIDI. Start is `FA`.

---

## 8. TD-3-MO differences **(verified)**

The **TD-3-MO** exposes **8 pattern groups** on its panel, but its SysEx still uses the
TD-3's four groups. It **mirrors each SysEx group onto a pair of panel groups**:

| SysEx group | Panel groups |
|---|---|
| 0 | 1 & 2 |
| 1 | 3 & 4 |
| 2 | 5 & 6 |
| 3 | 7 & 8 |

A pattern written once to, say, SysEx group 0 therefore appears in **both** panel groups 1
and 2 — the MO duplicates on receive; it is not a double write. **SysEx groups 4–7 are not
addressable** (the MO returns nothing to reads and ignores writes), so software should keep
the group in `0–3`.

Everything else in this document was verified against the TD-3-MO and matches the TD-3
protocol.

---

## 9. Opcode quick reference

SysEx opcodes (byte 7). All frames are `F0 00 20 32 00 01 0A <op> … F7`.

| Op | Name | Direction |
|---|---|---|
| `0x01` | ACK | device → host |
| `0x04` / `0x05` | Get model / response | host → / device → |
| `0x06` / `0x07` | Get product / response | host → / device → |
| `0x08` / `0x09` | Get firmware / response | host → / device → |
| `0x0E` | Set MIDI channels | host → (ACK) |
| `0x0F` | Set transpose | host → (ACK) |
| `0x11` | Set pitch-bend range | host → (ACK) |
| `0x12` | Set key priority | host → (ACK) |
| `0x14` | Set multi-trigger | host → (ACK) |
| `0x19` | Set clock-trigger polarity | host → (ACK) |
| `0x1A` | Set clock-trigger rate | host → (ACK) |
| `0x1B` | Set clock source | host → (ACK) |
| `0x1C` | Set accent velocity threshold | host → (ACK) |
| `0x50` | Enter / exit test mode | host → |
| `0x75` / `0x76` | Get config / response | host → / device → |
| `0x77` | Request pattern | host → |
| `0x78` | Pattern dump / put | device → / host → |
| `0x7D` | Reset config | host → |
| `0x7E` | Loopback | host ↔ device |

Non-SysEx: realtime `F8`/`FA`/`FB`/`FC`, channel `9n`/`8n`, and test-mode status
`0xA0`–`0xA4`.

---

*This reference is maintained alongside the `td3` C++ library; the authoritative byte
layouts live in [`include/td3/Protocol.h`](../include/td3/Protocol.h) and
[`include/td3/Codec.h`](../include/td3/Codec.h).*
