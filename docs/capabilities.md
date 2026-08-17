# Capabilities and Hard Limits

What PCM Studio can do on a 2009 Porsche PCM 3.1 head unit (SH-4A, QNX 6.3.2, 800×480), what
it cannot, and why not. The "why not" matters because the reasons are not interchangeable:

| Kind of "no" | Meaning | Can it change? |
|---|---|---|
| **Protocol-absent** | The concept does not exist in the protocol the head unit speaks. | Only by replacing the transport (new stack, new flash image). Not a config switch. |
| **Hardware/architecture** | The SH-4A never sees the data, or the write path physically refuses. | No. |
| **Untested** | The code path exists and looks reachable; nobody has run it on hardware. | Yes — by running it once and recording the result. |
| **Not implemented** | Writable; not written. | Yes, by writing it. |

Evidence strength is marked as **measured** (run on a bench unit), **read out of the
firmware** (static analysis of the stock binaries), or **inferred** (never executed).

⚠️ **On `file:line` references.** They point into `studio/` in this repository, but they are a
*snapshot*: `plat_pcm.c` alone is over 3000 lines and changes most days, so a line number can drift
by hundreds of lines between edits of this document. **When a citation and a symbol name disagree,
the symbol name is the authoritative one** — grep for it. Newer passages cite symbols precisely
because those do not rot. A sync-time check rejects citations that point past the end of a file, but
nothing can mechanically catch a number that is merely stale.

---

## 1. The capability contract

Boundaries are compile-time constants in [`studio/sys/pcm_caps.h`](../studio/sys/pcm_caps.h).
`CAP_X = 1` → the engine provides it, comment says how. `CAP_X = 0` → it cannot be done **or**
has not been tried, and the comment must say which. Flipping a `0` to a `1` requires evidence
from real hardware, cited in the comment. The header offers `#if CAP_X` and
`PCM_REQUIRE_CAP(CAP_X)` (`pcm_caps.h:133-134`) to make a `0` bite at compile time, but
**neither is used in `studio/` today** — the contract is held by review. What does bite at
build time is the command enum (§2), the accessor discipline (§3.1), and the build lints.

| Capability | Value | Kind | Basis |
|---|---|---|---|
| `CAP_PLAY_PAUSE` | 1 | — | MME `set_speed` (subtype 5), body 1000/0. Measured. `pcm_caps.h:24-25`, `plat_pcm.c:2731-2732` |
| `CAP_NEXT_PREV` | 1 | — | MME `mme_button` (subtype 18), body 0/1. Measured — track name in PCM3Reload changed. `pcm_caps.h:26-31`, `plat_pcm.c:2675-2682` |
| `CAP_SHUFFLE_REPEAT` | 1 | — | MME subtypes 12/13 write, 14/15 read; 0..4 clamped by us. Measured. `pcm_caps.h:32-34`, `plat_pcm.c:2695-2701` |
| `CAP_SEEK` | 0 | **Untested (expected to fail)** | `mme_seektotime` exists as MME subtype 9. A Bluetooth stream has no track session — the same reason `mme_next`/`mme_prev` return `ENODATA`. Never run. §5. `pcm_caps.h:36-45` |
| `CAP_ALBUM_ART` | 0 | Protocol-absent | AVRCP 1.3 (BlueSDK 2010) has no cover-art transfer. Read out of the firmware. `pcm_caps.h:47-52` |
| `CAP_LYRICS` | 0 | Protocol-absent | `pcm_caps.h:54-55` |
| `CAP_FAVORITE_SYNC` | 0 | Protocol-absent | No rating/favorite PDU in the stack's AVRCP command table; `SendVendorDependent` has zero call sites; the OBEX target list is a 5-way whitelist with no Apple iAP. Read out of the firmware. `pcm_caps.h:57-61` |
| `CAP_AUDIO_SPECTRUM` | 0 | Hardware | For Bluetooth the SH-4A is control plane only and never sees PCM samples — §3.3. `pcm_caps.h:63-68` |
| `CAP_BROWSE_LIBRARY` | 0 | **Untested** | Browse code is complete on the stack side; reachability can only be decided on hardware. §5. `pcm_caps.h:70-75` |
| `CAP_FULLSCREEN_OVERLAY` | 1 | — | Second gf client claims a free hardware layer, 800×480. Measured. `pcm_caps.h:79-80`, `plat_pcm.c:6-16` |
| `CAP_PARTIAL_REDRAW` | 1 | — | Clip rect + dirty-span blit. `pcm_caps.h:81-82`, `pcm_shell.c:231-244` |
| `CAP_SCREEN_TRANSITION` | 0 | Hardware (budget) | A full-screen present costs more than a whole transition's frame budget. Measured. `pcm_caps.h:84-89`, `plat_pcm.c:2743-2749` |
| `CAP_ROTATE_BLIT` | 0 | Not implemented | `gfx` is pure software per-pixel; no such primitive. `pcm_caps.h:91-94` |
| `CAP_GAUSSIAN_BLUR` | 0 | Not implemented | same. `pcm_caps.h:91-95` |
| `CAP_READ_INPUT` | 1 | — | Read the stock state mirror; never steal the event stream. `pcm_caps.h:99-100` |
| `CAP_BLOCK_STOCK_TOUCH` | 1 | — effectiveness unresolved, §4.3 and §5 | Touch gate `*(S+0x9c)=1`, requires `*(S+0x1a0)==2`. `pcm_caps.h:101-105`, `plat_pcm.c:1688-1703` |
| `CAP_RUNTIME_CODE_INJECT` | 0 | Hardware | `/proc/<pid>/as` writes RW pages only; RO code pages refuse. Measured. `pcm_caps.h:107-111` |

---

## 2. The command enum

`plat_command()` is the only way a scene changes anything, and its enum
(`pcm_sys.h:185-188`) **is the capability list** — a command that cannot be done is not in
the enum, so a scene cannot write the call.

```c
enum { CMD_PLAY=1, CMD_PAUSE, CMD_NEXT, CMD_PREV, CMD_SET_VOLUME, CMD_SET_SOURCE,
       CMD_TUNE, CMD_SET_SHUFFLE, CMD_SET_REPEAT };
```

`CMD_SEEK` is deliberately absent (see `CAP_SEEK`). A comment-guarded constant is not enough:
as long as the constant exists, someone wires a draggable progress bar to it.

**A second, stronger rule sits in front of the whole enum: if Studio is not visibly covering the
screen, no event is delivered to a scene and no command is sent at all.** `cover_owns_input()`
(`plat_pcm.c:2901`) gates both chokepoints — `plat_poll_event()` drops every touch (`:2919`) and
`plat_command()` refuses outright (`:3311-3319`). It is enforced at those two places rather than
per scene because the failure it prevents is *invisible*: in mirror mode a scene that was not on
screen interpreted a real touch with its own layout and drove `CMD_SET_SOURCE` through the puppet
cave, actually switching the car's audio source. The SOURCE hard key is the one deliberate
exemption, because it is a key **code** rather than a coordinate and it is the only way back once
we have yielded the screen — and it only navigates; it never reaches `plat_command()`.

### 2.1 Backend per command

| Command | Transport | Primitive | Needs flashed firmware? |
|---|---|---|---|
| `CMD_PLAY` / `CMD_PAUSE` | MME | `set_speed` subtype 5, body 1000 / 0 (`plat_pcm.c:2731-2732`) | No |
| `CMD_NEXT` / `CMD_PREV` | MME | `mme_button` subtype 18, body `MMB_NEXT=0` / `MMB_PREV=1` (`plat_pcm.c:2458-2459`, `:2675-2682`) | No |
| `CMD_SET_SHUFFLE` / `CMD_SET_REPEAT` | MME | subtype 12 / 13, clamped to 0..4 (`plat_pcm.c:2695-2701`) | No |
| `CMD_SET_VOLUME` | puppet cave | `pup_arm_op(PUP_OP_VOL_UP/DOWN, …)` (`plat_pcm.c:2715-2722`) | **Yes** |
| `CMD_SET_SOURCE` | puppet cave | `PUP_OP_ENTERT_SOURCE_CHANGED`; arg is a Studio-side `SRC_*`, **not** a stock slot — `src_to_slot()` in the platform layer maps it and rejects unknown values | **Yes** |
| `CMD_TUNE` | puppet cave | `PUP_OP_TUNER_FREQUENCY`, arg = kHz (`plat_pcm.c:2726-2728`) | **Yes** |

The puppet-cave path arms a stub at fixed addresses inside a flashed image; those addresses
come from a generated header, `platform/puppet_addr.h`, emitted by the flash packager so the
binary and the flashed cave cannot disagree (`plat_pcm.c:48`). **The header is published** (the
build needs it) **but the cave it describes is not** — the packager that produces both lives in an
unpublished workspace, and the addresses are valid for exactly one firmware build on one unit. The media transport controls need none of it: they are a second client on
`/dev/mme/default` alongside the stock HMI. Measured — `GETCLIENTCOUNT` returned 8 with the
stock processes unharmed.

### 2.2 MME wire format

24-byte `_IO_MSG` header, common to every command (`plat_pcm.c:2374-2379`, built at
`:2611-2617`): `+0` u16 `0x0113` (`_IO_MSG`) · `+2` u16 `combine_len` · `+4` u16 `19`
(`_IOMGR_MEDIA`) · `+6` u16 subtype · `+8` body (integers only for every command we use — no
pointers).

**`clen` is per-command and is not `sbytes`. Copy it from the table, do not compute it**
(`plat_pcm.c:2553-2558`): 8/8 pure header for `next` (10), `prev` (11), `stop` (2); 24/24 for
`set_speed` (5), `get_speed` (6), `setrandom` (12), `setrepeat` (13), `getrandom` (14),
`getrepeat` (15), `getclientcount` (16), `button` (18), `getregstatus` (95), `getevent`
(103); 32/32 for `regevent` (53), which has its own function; 72/8 for `play_get_info`, which
we do not send.

### 2.3 Safety rails

The danger is not a malformed packet — it is a **well-formed packet carrying a different
command**. Subtype 24 (`SHUTDOWN`), 27/28 (track session), 60, 62, 66 (`ZONE_DELETE`), 89
(`DELETE_MEDIASTORES`) have the same 8-byte pure-header shape as `next`/`prev`
(`plat_pcm.c:2589-2590`). Hence: a 14-subtype whitelist checked at send time
(`:2591-2609`); subtypes as compile-time constants only, with `mme_set_mode()` taking a
*selector* rather than a caller-supplied subtype (`:2695-2701`); mode values clamped to 0..4
**before** sending, because the server does no upper-bound check and would write an
out-of-range value into stock persistent settings (`:2693-2701`); a `TimerTimeout` on every
`MsgSend` — 2 s for commands, 500 ms for the per-tick event drain — so a stuck server cannot
hang the render loop (`:2604`, `:2620`); one `open()` for the whole process lifetime, since
each open costs the server a 1720-byte OCB plus a new qdb connection (`:2382-2383`,
`:2472-2483`); and a permanent ban on subtype 48 (`set_notification_interval`), whose handler
writes the **shared** attr and silently overwrites the stock setting (`:2436-2441`).

Failure diagnosis separates *rejected* (instant return) from *timed out* (≈2000 ms) and
prints the exact bytes sent, once per subtype (`plat_pcm.c:2633-2651`). Error codes decoded
against the unit's own `sys_errlist` (`plat_pcm.c:2569-2586`):

| errno | Meaning here |
|---|---|
| 9 `EBADF` | **The server** has no device fd — not our fd. `mme_button` returns this when nothing is playing; `get_speed` on the same fd still succeeds immediately after. |
| 61 `ENODATA` | No track session. `mme_next`/`mme_prev` (10/11) return this on Bluetooth in both stopped and playing states. |
| 89 `ENOSYS` | Server does not recognise mgrid/subtype — the packet is wrong. |

So `next`/`prev` (`plat_pcm.c:2675-2682`) tries subtype 10/11 once, latches the failure, and
uses `mme_button(18)` from then on. **Precondition: audio must actually be playing** —
`button` on a stopped stream returns errno 9.

---

## 3. Reading state

### 3.1 Unknown is not zero

A value that could not be read is not drawn at all — no empty battery icon, no `0:00`, no
invented station name (`plat_pcm.c:2085-2093`). Every unreliable field in `PcmState`
(`pcm_sys.h:63-110`) carries a `u_` prefix and is read through an accessor that **returns 0
for "no value"**, forcing the caller to handle that branch (`pcm_sys.h:113-146`):
`pcm_clock` (0 when the RTC is unset — an unconfigured bench unit reports -1), `pcm_battery`
(phone did not report), `pcm_track_index` (phone reports 0/0, common on iPhone),
`pcm_shuffle` / `pcm_repeat` (mode not read yet → icon greyed, not "off"), `pcm_freq_am`.
Usage: `scene_btplay.c:711`, `:718`. Phone battery in particular is a **five-state enum, not a
percentage** (`pcm_sys.h:59-61`), copied from the AVRCP enum in `libbssservice.so` at file
offset `0x15e1c0` — hence an icon and never a number (`scene_btplay.c:163-182`).

### 3.2 What the PCM backend populates

| Field | On hardware |
|---|---|
| `title` / `artist` / `album` / `genre` | Read from PCM3Reload; refresh gated on a 16-byte fingerprint of the four string pointers (`plat_pcm.c:2239-2245`) |
| `pos_ms` / `dur_ms` | MME gives **seconds**; converted (`plat_pcm.c:2263-2269`) |
| `play_state` | `plat_pcm.c:2259-2262` |
| `u_shuffle` / `u_repeat` | MME read, throttled: 2 s polling, 10 s once the event subscription is up (`plat_pcm.c:2270-2289`) |
| `device` | `/fs/avrcp0/.FS_info./info.xml`, single-file open only, rate-limited to 5 s (`plat_pcm.c:2298-2313`) |
| `stock_page` / `stock_src_slot` / `stock_src_app` | Read-only mirror of stock state (`plat_pcm.c:2182-2197`) |
| `u_battery`, `u_trk_cur`, `u_trk_total` | **Not populated.** The PCM backend only clears them, in the "not playing over Bluetooth" branch (`plat_pcm.c:2290-2296`); the Mac backend fills them for preview (`plat_mac.c:59-60`, `:147-149`). On the head unit the accessors return 0 and nothing is drawn. |

Two operational constraints: **never traverse `/fs/avrcp0`** — a directory listing crashes
`io-fs-media` (measured; srvstarter restarts it), while single-file reads are safe; and
metadata reads must pass the **source gate** first, otherwise stale Bluetooth data is mirrored
while FM is playing (`plat_pcm.c:2211-2225`).

### 3.3 No spectrum or waveform

For Bluetooth the SH-4A is a pure control plane and never sees PCM samples. SBC decoding runs
on the CSR BlueCore BC04 baseband (config flag `BSS_A2DP.UseInternalSbcDecoder=NO`), then
DRA300 DSP → OS8104 MOST NIC → amplifier. Read out of the firmware. Two qualifications, so a
later `grep` does not read as a contradiction: a software SBC decoder **does exist** in the
BlueSDK stack (`Thread_for_SBC_decoding` @ `0x151d84` in `libbssservice.so`), but that config
flag switches it off; and the SH-4A **does** touch PCM for locally generated audio (nav / TTS
/ prompts / mic, via `io-audio` + `deva-ctrl-hb_fpga.so`) and for forwarding compressed DAB
MSC bytes — neither is an entertainment source. Anything drawn as a waveform or spectrum is
therefore synthetic; synthetic decoration is allowed, drawing it so that it reads as *this
track's audio* is not (§6).

---

## 4. Display and input

### 4.1 Layer, format, cost

* Independent gf hardware layer, full 800×480, over the stock UI. **The overlay itself needs no
  flash** (it is the commands in §2.1 that do): layer claim,
  pixel writes and layer order are all runtime, and a power cycle restores stock state
  (`plat_pcm.c:6-16`).
* gf1 (hardware L6) is the preferred free RGB layer; the driver inverts the index
  (`hw = 7 - gf`, `plat_pcm.c:438`). Layer allocation **varies by car model** — on a 911, gf1
  is the PDC graphics layer — so the overlay ships with a yield protocol rather than betting
  on a free layer (`plat_pcm.c:8-15`, `yield_check()` at `:294`).
* Pixel format is **RGBA5551** (`plat_pcm.c:19`, `gfx.c:39`): 32 levels per channel, single
  alpha bit. Wide gradients band visibly — measured on one 520-pixel line through a soft-light
  centre, only 9 distinct colours with a 280-pixel longest run. Fix: ordered 8×8 Bayer dither
  before quantisation (`gfx.c:54-75`). In-frame alpha blending is software (`gfx.c:80-88`).
* Layer config is stored per hardware layer and inherited from the previous client, so
  blending is reset with a zeroed `gf_alpha_t` (mode = 0) on every claim and on shutdown
  (`plat_pcm.c:335`, `:355`, `:506`); and since `set_surfaces` clears blending, both viewports
  and the layer order, re-assert the full sequence after it — `set_surfaces → set_blending →
  src_vp → dst_vp → order → enable → update` (`:333-334`).

Measured cost, 800×480×2 = 768000 bytes:

| Operation | Time |
|---|---|
| Full screen, u16 per pixel, into VRAM | 258 ms (`plat_pcm.c:661`) |
| Full screen, u32 (two pixels per store) | 130 ms |
| Full screen, u32 + 8× unrolled | 130 ms — unrolling buys nothing |
| Row-by-row shadow compare only (plain RAM) | 7 ms |
| Present with dirty-span blit (40 of 480 rows) | 8 ms (`main_pcm.c:199-204`) |
| Full-page scene render | ~27 ms (`pcm_shell.c:39`) |
| One full-page redraw, end to end | ~47 ms = 27 render + 20 present (`pcm_shell.c:28`) |
| Main loop period | 25 ms (`main_pcm.c:199-204`) |

The first three rows are why the engine has this shape: **doubling the store width halves the
time and unrolling changes nothing — the bottleneck is bus transactions, not bytes or CPU.**

### 4.2 No transitions; partial redraw instead

`plat_can_animate()` returns 0 on PCM, 1 on Mac (`plat_pcm.c:2749`, `pcm_sys.h:202-206`). A
220 ms fade needs 6+ full-screen frames while the budget covers 1–2; the result is one black
flash then a jump. The shell adds a measured guard: if a tick is at least half the fade
length, the fade is dropped for that tick (`pcm_shell.c:215-219`). The SH-4 store queue is
**not** the way out: with the MMU on, the target physical address comes from a
kernel-installed UTLB entry, so a user-mode access faults, and QEMU does not model it — which
would break the project's sim == car rule (`plat_pcm.c:2746-2748`; inferred, not run). The
remaining untried lever is the Carmine 2D hardware blit (`libgdcApiCarmine.so` has real
`gf_draw_*` bodies), which needs the drawing lock.

A scene may instead implement `anim_rect()` and get a clipped redraw (`pcm_shell.c:40`,
`:231-244`), applied whenever nothing this tick set the *full-page* dirty flag and no
transition or overlay animation is running (`:238`). Two things reach it: the scene's own
`anim_ms` interval (`:199-205`), and the once-per-second `pos_ms/1000` advance (`:196`), in
steady state the most frequent redraw reason. `scene_btplay.c:846-863` widens its rectangle on
that tick to cover the progress bar and time row: 7.9% of the screen per frame
(`scene_btplay.c:821`). Everything else forces a full page: input events
(`pcm_shell.c:182`), scene switches (`:118`), `shell_invalidate()` (`:137`), changes to
volume / source / play state / minute / frequency (`:186-190`), and the catch-up frame at the
end of an animation (`:228`). Scenes without `anim_rect()` are unaffected. The comment at
`pcm_shell.c:33` describes an older, stricter rule; the code at `:238` is authoritative.
Clipping pays off because every primitive bounding-box tests against the clip rectangle and
skips entirely on no intersection — that early-out, not the per-pixel bounds check, is where
the time goes (`gfx.c:18-37`).

### 4.3 Input: read, do not steal

The stock software keeps interpreting user input and switching its own pages while we cover
the screen. We mirror the **result** — current page id, source slot, source app — instead of
intercepting the event stream. Page ownership is a **whitelist**: 855/873 → radio, 375 →
Bluetooth playback, 387 → AUX; anything unrecognised gives the screen back to the stock UI
(`main_pcm.c:208-210`). Each whitelisted page is additionally gated on its own "Studio takes
over" setting, so turning a source off in settings shows the stock page for it instead.

There is deliberately **no page id for the source page** — the stock's SOURCE control is a popup
that never changes the page id, so a source/home page id does not exist. Ours is reached by the
SOURCE hard key instead (§4.5). An earlier version of this document mapped 387 to "home"; that was
wrong twice over, and the bench proved it.

Measured ids: FM = slot 11 / app 1, AUX = 26 / 6, Bluetooth = 40 / 7; `app` is the more stable
discriminator. Page id `0xFFFE` is a page-transition sentinel and must be ignored. **A whitelist,
not a pattern** — "three digits means media" was disproved by page 913.

**Studio does not cover the screen until it has read a stock page id.** `g_cover` starts at 0, so a
freshly started instance shows nothing and the stock UI is untouched; the page router turns covering
on once it recognises a page. This matters most at boot: handing back the screen requires a valid
page id, so an instance that never anchors would otherwise cover the display for the whole ignition
cycle — including while the driver is reversing. For the same reason the anchoring retry no longer
gives up permanently after ~30 s; it backs off to one attempt every 30 s and keeps trying.

Yield protection is also **fail-closed**: if the layer record cannot be read there is no way to tell
whether the stock has taken the layer, and startup aborts rather than running blind.

`plat_poll_event()` sources touch from the read-only mirror, the SOURCE hard key (§4.5), and a
debug injection file (`echo "<type> <which> <arg> <x> <y>" > /tmp/studio_ev`). The FPGA/IPC path
is still **not connected** — polling shared IPC channels has hung both a car and a bench unit —
so rotary knobs and every hard key other than SOURCE do not reach us.

**The interaction model is touch, and only touch.** No scene draws a focus ring, a selection
state or a cursor, and no scene handles `EV_ROTARY`, `K_UP`/`K_DOWN`/`K_LEFT`/`K_RIGHT` or
`K_OK`. This is a product decision, not a limitation waiting to be lifted: without rotary
selection there is no "currently selected item", so a highlight that means *where the cursor is*
would be describing something that does not exist. Highlights are therefore reserved for facts —
on the source page the one highlighted card is the source that is actually playing. The volume
knob is the single exception and is handled globally, never by a scene.

Hit targets are sized for a **resistive** touchscreen: five transport zones, adjacent regions
touching at their midpoints, extended to the bottom of the screen for a height of 124 px and
widened by 52 px at the outer edges — minimum 84 × 124 px, inscribed radius 42
(`scene_btplay.c:682-701`). A scene-entry self-check asserts that each control's drawn centre
falls inside its own hit region and that the dirty rectangle covers everything that animates
(`scene_btplay.c:864-905`).

### 4.4 The touch gate

Writing `*(S+0x9c) = 1` in PCM3Reload puts the stock HMI's own touch early-return into
effect. Hard keys take a different branch of `CHBKey2MSMEventMapper::processEvent`
(`0x085D3D94`) and are unaffected by this word — that is the escape hatch, and it is also why
blocking a key needs its own patch (§4.5) (`plat_pcm.c:1688-1703`).
**Whether this reliably stops pass-through is unresolved; see §5.** Our own receipt of the
touch proves nothing either way: `read_touch` reads a cache *upstream* of the gate.

The gate is guarded by an invariant maintained in exactly one place — *we cover the screen ⟺
the gate is armed* (`set_cover()`, `plat_pcm.c:578-608`) — plus a re-check once a second,
because edge-triggered enforcement cannot fix a wrong initial state (`plat_tick_watch()`,
`plat_pcm.c:611-636`). Scenes cannot touch it: those functions live in `sys/plat_internal.h`,
not `pcm_sys.h`, and a build lint enforces that (`build.sh:25-33`).

Constraints on any change here:

* The gate is `xor #1`, not a logical negation: **write odd values only** (canonically 1).
  Writing 2 yields 3, still non-zero, still passing touch through (`plat_pcm.c:1694`, `:1816`).
* It has **two** conditions. The disassembled getter is
  `return (*(S+0x1a0)==2) ? *(S+0x9c) : 0`, then inverted. Watching only `0x9c` produces a
  false green; the per-tick watchdog checks `0x1a0` too and writes 2 back on drift (`:1863-1877`).
* `set_cover(0)` **disarms the gate automatically** (`:583`). When diagnosing a "the gate did
  not hold" report, first establish whether the gate was still armed at that moment.
* Arming refuses if the original value is non-zero (stock code is using the field), if a
  finger is down, if the touch source was never anchored, or if the pointer chain does not
  validate against the expected vptrs (`:1805-1820`).
* Every **catchable** exit path restores it: handlers for 9 signals plus a `SIGALRM` +
  `alarm(3)` watchdog, because a blocking hang raises no signal at all (`:1822-1831`). Not
  covered: `SIGKILL` — hence stop with `touch /tmp/studio.stop`, never `slay`
  (`main_pcm.c:123-128`); a write-back whose read-back fails (`:1845-1849`); and the case
  where the field no longer holds our value (`:1840-1844`, which deliberately does **not**
  write back). The backstop for all three is the same: the change is pure RAM, and a power
  cycle restores stock behaviour.

### 4.5 Hard keys, and the key gate

Reading a hard key needs no patch. The stock key mapper's payload carries two fields
(`plat_pcm.c:2846-2854`):

* `+0x64` — the key code, **latched**. SOURCE is `0x0d`, MEDIA is `0x07`. Pressing the same key
  twice does not change it, so a value alone is not an event.
* `+0x6c` — the key **edge**, which pulses `3 → 0 → 3` on every press, repeats included.

So the criterion is: *read `+0x64` on the tick where `+0x6c` reads 0*. A differential probe is
permanently blind to this — the pulse is transient and both endpoints of a diff show `3`; it took
a per-tick watcher to see it at all. Only whitelisted key codes are turned into events, today
just `0x0d`.

Reading is where "no flash" ends. With Studio covering the screen, the stock still receives
SOURCE and still cycles its own source list underneath, and the touch gate does not cover keys —
they take a different branch. Stopping that needs a firmware patch: `preProcessKey()` is **vtable
slot 5 (`+0x14`)** of the mapper, and returning non-zero makes the stock drop the key. The vtable
is in a read-only segment, so the patch is made in the IFS2 file: slot 5 is redirected to a code
cave that walks a table of key codes and returns 1 on a hit, otherwise tail-jumps to the real
`preProcessKey`.

Two properties make that safe enough to flash:

* **Disarmed, it is byte-for-byte stock.** The cave is armed by the *same word as the touch
  gate*, checking the same two conditions (including `*(S+0x1a0)==2`). So the invariant grows
  from *we cover the screen ⟺ the stock sees no touch* to *⟺ it also sees no SOURCE*, and Studio
  needed no change at all to get it.
* **It is table-driven.** Adding a key edits the table, not the code.

Verified on the bench three ways, because any one alone proves nothing: with the gate armed the
stock logged no page or source change across four presses while Studio received all four; MEDIA,
which is not in the table, still worked normally; and stopping Studio restored stock behaviour
completely.

### 4.6 The status bar

Every page draws the same bar (`shell_draw.c:127`): clock, volume value, volume level, speaker,
laid out **right to left** from a caller-supplied right edge, returning the width it consumed so
the page can lay out around it. The volume lives here because it used to be drawn by the
Bluetooth page, and turning the knob made that page visibly jump.

The row is defined by its **baseline**, not by a box. An earlier version positioned text from an
approximated mid-line, and every element sat on a slightly different line; the constants now
derive from `SB_BL` and a cap-height band (`shell_draw.c:109-119`), and the residual spread
measured on rendered pixels is about 1 px. Two bugs only became visible by measuring the output
rather than looking at it: a hand-guessed baseline offset that was 4.5 px out, and a helper whose
coordinate was a centre while the caller passed a top-left.

The numbers came from phone status bars — icons and text are near the same height there, so an
icon noticeably taller than the digits reads as wrong. That relationship is checked at startup
against the text cap height, and the check was falsified before being trusted: restoring the
previously rejected icon size makes it fire.

### 4.7 Languages

UI strings live in one X-macro table (`pcm_i18n.h:29`) that expands to both an id enum and the
per-language arrays, so a new string cannot be added to one language and forgotten in the other.
`T(id)` reads the current language on **every call** (`:90-93`) rather than caching a pointer,
which is what makes switching language redraw immediately instead of at next boot. English is the
default; Simplified Chinese is the other. A build lint rejects a **Chinese** literal that reaches
a drawing call (`build.sh:32-42`) — it catches the common way a translated string gets hard-coded,
but note it cannot see an English literal drawn directly, so that class still needs review.

---

## 5. Open questions (untested, not impossible)

| Question | State | Why it is open |
|---|---|---|
| **Browse the phone's library** (`CtGetFolderItems` / `CtPlayItem`) | `CAP_BROWSE_LIBRARY = 0` | All four layers are present in the firmware: the `avrcpbrws.c` compile unit exists, the `CT_BROWSING` feature bit is set in `AVRCP_CT`, all 8 browse commands in the 27-entry command table have real handler addresses, and the media plugin exports `mediaBt_BrowseFolder` / `mediaBt_PlayItem`. "L2CAP PSM 0x1B has zero hits ⇒ browsing is dead" is **not** valid reasoning: we are the controller, and that PSM is advertised by the phone's target role. Off-line analysis cannot settle it. Until it is settled, **do not reserve layout space for it** (`pcm_caps.h:70-75`). |
| **Does the touch gate stop pass-through?** | `CAP_BLOCK_STOCK_TOUCH = 1`, effectiveness unresolved | Three bench runs, same code, same session: settings page 3475 — 5 taps, no effect on the stock UI; Bluetooth page 375 — one tap passed through (stock UI navigated to 377); Bluetooth page 375 with the `0x1a0` monitor added — 8 taps, no effect, and the monitor never fired, so it is neither proven cause nor proven cure. Acceptance criterion: only a tap **on a spot where the stock UI has a real control**, with a pre-arm control run at the same spot, counts. "The page id did not change" and "we received the touch" are always true and prove nothing. |
| **Phone battery level** | Field and accessor exist; backend does not fill them | Whether MME exposes an outlet for the AVRCP battery status has not been traced to a conclusion. |
| **Fast-forward / rewind** | Not implemented | Read out of io-media: subtype 5 with `speed > 1000` → FASTFWD, `speed < 0` → FASTRWD; sending 1000 restores normal play. Never run. |
| **Seek** (`mme_seektotime`, subtype 9) | `CAP_SEEK = 0` | The command exists in MME — subtype 9, 64-bit time at body +16/+20 — and PCM3Reload registers `MmeDevCtrl_playback_seek`. What is missing is a track session on an A2DP stream, the same failure mode as `mme_next`/`mme_prev`, so failure is the expectation. It stays 0 until hardware says otherwise; the UI consequence (§6: the progress bar registers no touch handler) holds either way until then. |

---

## 6. Deliberate non-goals

Not hardware limits — decisions:

* **No album-art placeholder.** An always-empty frame reads as broken. Either a no-art layout
  or a with-art layout, never one layout with a placeholder. A generated abstract backdrop
  exists in `gfx` (`gfx_artbg`, `gfx.c:350`; `gfx_art_tile`, `gfx.c:415`; no scene calls
  either today) — decoration, never presented as *this track's* cover.
* **No synthetic waveform in the progress bar's place.** A waveform hashed from the track
  title implies "this is what this song looks like". The code survives behind `-DBT_WAVEBAR`
  (`scene_btplay.c:568-591`, `:736-743`) and is off by default.
* **No draggable progress bar.** It registers no touch handler at all. Looking draggable and
  not responding is worse than not offering it (`scene_btplay.c:788`).
* **No repeat modes 3/4** (folder / subfolder). The UI cycles 0→1→2 only; the other two are
  meaningless for a Bluetooth stream (`scene_btplay.c:799-801`).

---

This document covers the Bluetooth playback page and the engine. The other scenes
(`scene_source`, `scene_radio`, `scene_aux`, `scene_settings`) exist and use the same
accessors; their end-to-end status on the head unit is not covered here.
