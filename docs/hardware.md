# Hardware

PCM Studio is not a portable UI toolkit: the ordered dither, the shadow buffer, the 25 ms tick, the touch-target sizes and the lookup tables all exist because
of properties of this machine. Statements below are **measured** (bench unit or car), **derived** (from constants in the source) or **inferred** (not closed on
hardware).

## 1. The box

| Part | What it is |
|---|---|
| CPU | Renesas **SH7785** (SH-4A), little-endian. Disassemble with `objdump -m sh4 -EL`. |
| GPU | **Fujitsu Carmine MB86297** (PCI `10cf:202b`), config string `graphicChip=CARMINE16` |
| VRAM | PCI BAR2 @ `0xd0000000`, 256 MB aperture, **only 128 MB backed** (`carmine.conf vsize=0x8000000`) |
| Display | **800 × 480**; gf reports `nlayers=8 main_layer=2` |
| OS | QNX 6.3.2; graphics = QNX **gf**, driver `devg-carmine.so`, server `gdcServerCarmine` |
| Surface format | **RGBA5551**, 16 bpp (`studio/sys/pcm_sys.h:12`, `studio/platform/plat_pcm.c:19`) |
| Panel input | Resistive touchscreen. Observed behaviour — no datasheet or part number found. |

No sourced figure exists for CPU clock, DRAM size, Carmine core clock or panel refresh rate, so none appears here. There is **no `libgf.so.1` on the device** —
link against `libgdcApiCarmine.so`. We build freestanding against a stub libc: no `stddef.h`, no libm (`plat_pcm.c:28-42`, `gfx.c:524`).

## 2. RGBA5551: 32 levels per channel

gf reports format `0x1710` (`GF_FORMAT_PACK_ARGB1555`), but that constant only selects bytes-per-pixel = 2; the hardware mode is driver-written and gf cannot
change it. For 16 bpp the driver unconditionally programs `LnEC = 0b10` (`carmine_layer_set_surface@0x13020: or 0x80000000`) — direct-colour RGBA, Carmine
manual p.430, layout `R[15:11] G[10:6] B[5:1] A[bit 0]` (§7.3.2 p.364). 5-bit channels shift toward the MSB on output: measured, 15 → 120, 31 → 248. The pack
macro (`gfx.c:39`) matches; the unpack (`gfx.c:50`) uses bit replication, so the Mac preview runs slightly bright at channel maxima.

**Banding is guaranteed:** step = 8/255. Measured on a 520 px horizontal line through the centre of the background glow, **9 distinct colours, longest
same-colour run 280 px** (`gfx.c:56`).

**Fix: 8×8 ordered (Bayer) dither before quantisation** (`gfx.c:63`) — add a position-dependent 0..7 offset, then quantise. Exact multiples of 8 stay in the
same level (flat areas gain no noise); intermediate values alternate proportionally, correct mean, band edges dissolved. Cost: one lookup, three adds per pixel.
The dither index **must use screen coordinates**, not loop variables, or the cached background shifts its texture when blitted elsewhere (`gfx.c:62`); and
`gfx_blend` must dither too, since the glow is built from many blends (`gfx.c:86`). 32 levels also flatten small luminance gaps: the progress-bar knob needs a
background-colour ring under the white dot, because `C_AMBER` and `C_DISPLAY` are only 62 luma apart (`scene_btplay.c:747`).

## 3. Getting a layer: we are the second gf client

PCM Studio does not patch the stock framebuffer: it attaches to gf as a **second client**, takes a hardware layer the stock layer manager does not use, creates
its own 800 × 480 scanout surface and orders itself on top. **Studio's own installation rewrites no flash partition** — binary and font are ordinary files on
`/HBpersistence` (`main_pcm.c:63`), and everything Studio does to the stock software at runtime is
RAM-only, gone on power-cycle. That is a statement about *installing and running Studio*, not about
the project as a whole: two firmware patches have been flashed to the bench and Studio depends on
them for anything that *changes* stock state — the IFS1 puppet cave (volume, source, tuner) and the
IFS2 `preProcessKey` gate (stopping the stock acting on SOURCE). Both are raw-partition writes with
real bricking risk, and neither is undone by a power cycle.

**Layer numbering is inverted: `hardware layer = 7 − gf layer`** (`plat_pcm.c:438`). `devg-carmine.so` does `neg rN,r1 / add #7,r1` in three places:
`carmine_layer_query@0x12cec`, `set_surface@0x12f86`, `set_dest_viewport@0x13124`.

| gf | hw | Bench census (221 s) | Verdict |
|---|---|---|---|
| gf1 | L6 | 0 changes, "placeholder, unused" — the only idle RGB layer | **First choice** (`plat_pcm.c:372`) |
| gf5 | L2 | Stock transition/overlay layer; stock writes L3's full-screen surface here on page changes | Fallback |
| gf6 | L1 | **Video capture layer.** Driver writes `L1EM bits[1:0]`, *Reserved* on L1 (manual p.412) → top 6 bits die, red never appears | **Do not use** |
| gf7 | L0 | Stock content, 800 × 480, 8 changes | Last-resort fallback |

The fallback list is `int pref[3] = {1,5,7}` (`plat_pcm.c:372`). Both `layermanager.cfg` images we hold carry `lastAvailableLayer=0-4`, so the LM pool is
gf4..gf7 = hw L3..L0: gf5 and gf7 sit inside it, only gf1 is outside. Those two images (a 911 and a bench-variant build) are byte-identical ⇒ **no evidence the
layer map varies by model**, but no live census has run on a car — which is what §7 covers.

**Z-order: append yourself last.** `gf_display_set_layer_order` takes an 8-entry array, last = topmost, and it must be fully initialised (the library reads all
8). With our gf layer appended last, a full-screen opaque surface covers the stock page: 24 000 sampled points, zero stock pixels (`plat_pcm.c:427`).

**`enable` is gated, not unconditional.** `push_layer()` returns 0 and refuses to light the layer
while we are covering but the first frame is not yet in the surface — lighting it early shows whatever
the stock wrote into that memory while we were yielding, which decodes as a ~198 ms green flash. The
enable is deferred to `swap_bank()`, so the screen holds a complete stock frame until ours is ready.
The gate lives at the single exit rather than at each call site, because there are several paths that
light the layer and the symptom of missing one is an occasional green flicker — the hardest kind to
reproduce or attribute. It is falsified on every boot by a deliberate fake "frame not ready".

**The push sequence is otherwise fixed**; `push_layer` and `swap_bank` are the only places that talk to the layer, and no shorter
sequence works.

```c
set_surfaces → set_blending → set_src_viewport → set_dst_viewport → set_layer_order → enable → update
```

`set_surfaces` wipes the layer's other bindings, and `set_blending` must be called even when you do not want blending: layer configuration lives in the
*server*, keyed by hardware layer, with no ownership and no release, so an inherited alpha setting can leave correct pixels on an invisible layer. We pass an
all-zero `gf_alpha_t`, mode 0 (`plat_pcm.c:439`). dst viewport width *and* height are `x2−x1+1`. **Never call `gf_layer_set_chroma`** — in the RGBA (`LnEC==10`)
branch `carmine_layer_program@0x13640` packs the key as ARGB1555, always misaligned. **`gf_layer_update` must not run in a tight loop** — it REPLY-blocks in
`gdcServerCarmine` (`pidin` shows `REPLY 4104`) and deadlocks the process; keep-alive re-assert is throttled to 1 Hz, the flip to 20 Hz with
`GF_LAYER_UPDATE_NO_WAIT_VSYNC`.

**Only `disable + update` gives the layer back.** `gf_layer_attach`/`gf_layer_detach` contain zero `jsr`/PLT — the refcount is a per-process heap array,
invisible across processes — so a killed process does **not** release the layer (enable bit measured still set) and a `slay`ed PCM Studio leaves its last frame
on screen until power-off. Only stop it with **`touch /tmp/studio.stop`** → `plat_shutdown()`, which
disarms the touch gate, cancels MME event registration and parks the puppet cave *before* it looks at
the layer at all — those three matter even when no layer was ever attached — then, only if there is
one, resets blending, disables and updates it, and finally releases the single-instance lock.

**Two studios at once has the same unrecoverable outcome**, by a different route: the second one's
startup `disable + update` pulls the layer out from under the first, whose next `gf` call blocks on
the graphics server; the survivor no longer answers `/tmp/studio.stop` and the unit needs a power
cycle. Studio takes a lock at `/tmp/studio.lock` as its first action, and the launcher refuses to
start over a running instance.

## 4. Alpha: one bit, on purpose

RGBA5551 gives one alpha bit per pixel; with no blending configured, `0x0000` is genuine per-pixel binary transparency. PCM Studio is a full-screen opaque
layer, so every pixel it writes has A = 1 (`|1u` in `gfx.c:39`). Eight-bit per-pixel alpha *is* obtainable — `GF_ALPHA_M1_MAP` (mode `0x00080102`) plus a
`GF_FORMAT_BYTE` alpha-plane surface, stride 64-byte aligned (512 → clean, 544 → full-screen striping) — but **we deliberately do not use it.**

The alpha blend planes (LA0..LA3) are a pool of four in `gdcServerCarmine`: the allocator `FUN_0804f8ce` hands out indices 8..11, while the release guard at
`0x080502ee` (`mov #3,r1 / cmp/hi r1,r2`) frees only when the index is ≤ 3. The ranges do not intersect ⇒ **the release path is unreachable; a plane handed out
is never returned until power-off.** Bench ledger readout from the server's own `.bss` table: stock holds LA0/LA1/LA2 at boot; `panel_alpha=255` + two draws
took no plane; `panel_alpha=240` + one draw moved **LA3 from `12` (free) to `6`** — our layer — still bound after the overlay was hidden. On a real 911 this
showed up as the PDC radar detection zone going solid black for the rest of the ignition cycle; the fix is to never request a plane (`ui.def panel_alpha=255`).
Still **inferred**: that PDC needs the fourth plane, and that `set_surfaces`/`enable`/layer-order are not co-conspirators.

## 5. Writing VRAM is a bus-transaction budget, not a bandwidth budget

Bench benchmark, 800 × 480 × 2 = **768 000 bytes**, 3 runs averaged: `bench_copy()` (`plat_pcm.c:748`), triggered by `>/tmp/studio_bench`, logs to
`/tmp/studio.log`.

| Method | Time | Effective rate |
|---|---|---|
| ① `u16` per pixel | **259 ms** | 3.0 MB/s |
| ② `u32` (two pixels per store) | **130 ms** | 5.9 MB/s |
| ③ `u32` + 8× unrolled | **130 ms** | 5.9 MB/s |
| ④ row compare only, no VRAM writes (plain RAM) | **7 ms** | 110 MB/s |

Doubling store width halved the time ⇒ **the bottleneck is transaction count, not bytes**; SH-4 store queues burst 32 bytes, which *projects* a further 8× (130
ms → ~16 ms), untried. Unrolling bought nothing ⇒ the CPU is not busy. RAM comparison is nearly free ⇒ dirty-region tracking costs nothing fixed.

**Shadow buffer + per-row dirty spans** (`present_diff`, `plat_pcm.c:695`): scenes draw into one contiguous 800 × 480 system buffer; on present each row is
compared as `u32` against a shadow copy and only the span between the first and last differing word goes to VRAM. Measured: **present 7–8 ms, 40/480 rows
moved** (258 ms → 7 ms), scene code untouched.

The `u32` path carries a **runtime alignment guard** (`plat_pcm.c:470`, `blit_span` `plat_pcm.c:674`): a misaligned `u32` access on SH-4 is an address error →
SIGBUS → process death → last frame stuck on screen (§3), so it falls back to `u16`. It tests `g_vaddr`, `g_fb`, `g_shadow` and `stride*2`, but *not* the second
bank's `g_va2` or `g_shadowB`, which are also cast.

**Double buffering** (`swap_bank`, `plat_pcm.c:720`) — without it the page is visibly written top-to-bottom, since the panel scans out while we write. **Each
surface carries its own shadow**: the back buffer is one frame stale, so the delta must be "current frame vs *this* buffer's previous frame", keeping copy
volume identical to single buffering. A full re-copy is forced on first frame, return from a yield, leaving mirror mode and any surface change; the flag is a
**counter initialised to 2** because both surfaces need repainting (`plat_pcm.c:153`). Escape hatch `>/tmp/studio_nodbuf`.

## 6. Frame budget

| Item | Cost |
|---|---|
| Main loop tick | **25 ms** sleep (40 Hz poll ceiling) — `main_pcm.c:200` |
| Scene render, full page (384 000 px) | **27 ms** — `gfx.c:19` |
| Present, steady state (dirty spans) | **7 ms** |
| Present, forced full screen | 130 ms copy + 7 ms compare |
| Full redraw budget used by scene scheduling | **≈ 47 ms** = 27 render + 20 present |

The 20 ms present predates the dirty-span path (7 ms warm), so treat 47 ms as a conservative upper bound. Before the background cache a full-page render cost 83
ms (`gfx.c:149`).

The shell is **dirty-driven**: nothing is redrawn unless an event, state change, animation timer or overlay says so. Scenes declare their animation interval via
`anim_ms()`, not "every frame" — a 47 ms redraw against a 25 ms tick would saturate the CPU; the BT page returns 80 ms playing, 0 paused, 50 ms during a press
glow (`scene_btplay.c:812`). Scenes also declare a **dirty rectangle** via `anim_rect()`; the shell sets the gfx clip and runs *the same* render function
(`pcm_shell.c:234`), so partial and full redraws cannot diverge, and every primitive intersects its bounding box with the clip (`gfx.c:24`). Rect sizes are
**derived** from the layout constants (`btplay_anim_rect()`): narrow (only the vinyl disc turns) = (109,153)–(283,327), 174 × 174 = 30 276 px = **7.9 %** of
the page; wide (the second also ticked, so progress bar and time row too) = (109,153)–(766,352), 657 × 199 = 130 743 px = **34.0 %**. At 80 ms that is ~12.5
redraws/s, one of which takes the wide rect; the press glow is in neither rect, so `anim_rect` returns 0 during a press and the shell goes full-page. The
**background is cached** by its 13 parameters and memcpy'd per frame (`gfx.c:152-157`); on a partial redraw only the clipped rows **and columns** are restored —
full-width row restores wipe whatever sits left and right of the clip box (`gfx.c:186`).

**No full-screen transitions.** A cross-fade needs 6+ consecutive full-screen frames at 137 ms each — "flash to black, then jump", worse than a hard cut — so it
is compiled off: `CAP_SCREEN_TRANSITION 0` (`pcm_caps.h:84`), `plat_can_animate()` returns 0 on PCM (`pcm_sys.h:202`), plus a runtime drop if `tick*2 >=
SCENE_FADE_MS`. Any animation that ends must repaint one final frame on the falling edge, or a cleared dirty flag leaves the last (black) frame on screen.

## 7. The yield protocol: hands off, never `disable`

We take a hardware layer whose runtime ownership on a car has never been surveyed. Rule: **when stock starts using this layer we stop touching it; when it has
been idle again we resume** — no model-specific layer map needed. Detection reads the gdc shared record for our hardware layer every tick (`yield_check`,
`plat_pcm.c:296`, from `plat_tick_watch`, `plat_pcm.c:613`): 4 uncached `u32` reads per tick, zero syscalls. Resume takes **20 idle ticks** (`plat_pcm.c:308`)
at `usleep(25000)` = **≈ 0.5 s**.

```
shm_open("/gdc_shm_inform") + mmap
rec = base + 0xe28 + disp*0x5a0 + hw_layer*120
fingerprint = (u32[1] << 16 | u32[3] & 0xffff , u32[4] << 16 | (u32[5] >> 12) & 0xffff)
```

⚠️ The field labels are not settled (the code's `bytes/pixel, pitch` vs `color, width` from the library's own `printf` strings). They serve only as a change
fingerprint, so behaviour is unaffected.

**Yielding must never `disable` / go dark**: stock lights some layers once, at feature start, so a yield landing after that would kill the layer for the rest of
the ignition cycle — and doing nothing is safe, because when displacement is detected what is on screen is already stock's content. **It must run every tick,
not inside `plat_present`**, which is behind the dirty gate and never called on a static screen. **Your own render pipeline must not trip your own detector**:
the fingerprint's low half is `phys_addr >> 12`, which double buffering changes on every flip, so a single baseline from bank 0 makes the first flip read as
"stock stole the layer" → yield → we stop flipping → permanently stuck; learn one fingerprint **per bank**, right after pushing it (`plat_pcm.c:272-294`).

**On a real car this protocol has never fired** — no genuine `[yield]` line exists in any log, and the PDC blackout it was written to explain was re-attributed
to the alpha-plane pool (§4); it is an untested safety net. `set_cover()` (`plat_pcm.c:580`) is separate: it hides us on purpose when the stock page is one we
do not own, and *does* `disable + update` our own layer, never a stock one.

## 8. Touch: resistive screen, mirrored input

We never take the input event stream: polling the shared IPC/IOC input channels hung both the bench and a real car. Touch is **mirrored read-only** out of
PCM3Reload's `CHBKey2MSMEventMapper` via `/proc/<pid>/as` (`plat_pcm.c:1017` onward), the same path used for page id, source and volume. Coordinates arrive
**already in 800 × 480 pixels** (stock applies TouchCalib first) but are not clamped, so an edge press extrapolates out of range and we clamp
(`plat_pcm.c:1079`). The 24-byte record is read in one `read()` to avoid tearing (`plat_pcm.c:1074`). **Debounce must wait for a real release**: a resistive
panel jitters and stock reports intermediate states (one press seen as 2–4), so a `type==2` counts as a new press only after `type==3`.

Resistive panel plus a moving car ⇒ generous hit targets, and the BT page derives drawing, hit-testing and dirty rects from **one** set of constants
(`scene_btplay.c:681-697`): control centres `CTL_XS[5] = { 408, 486, 576, 666, 744 }`, `CTL_HIT_Y0/Y1 = 356 / 480` (124 px tall for 34 px icons), `CTL_HIT_EDGE
= 52` outward at both ends. Horizontal boundaries are midpoints between neighbours, so **adjacent zones touch — no dead gaps**; derived zone widths are 84–91 px
by 124 px, with the upper edge 8 px clear of the time-row baseline. A self-check at `enter()` asserts every drawn centre hit-tests to its own index
(`scene_btplay.c:864-899`). The progress bar takes no touch: `mme_seektotime` (MME subtype 9) is *expected* to return ENODATA on a Bluetooth stream the way
`mme_next/prev` (10/11) do, but nobody has tried it, so `CAP_SEEK` stays 0 (`pcm_caps.h:36-45`).

**The touch gate.** Our layer is a hardware overlay and does **not** intercept touch — the same press that hits our button also reaches stock — so we set a
touch early-return in stock's firmware (`plat_pcm.c:1691-1703`):

```
0x085D20B4:  q = *(mapper+0x40);  r0 = 0x085D1EAA(q);  xor #1,r0
0x085D1EAA:  S = *(q+0x28);  return (*(S+0x1a0) == 2) ? *(S+0x9c) : 0
```

Writing `*(S+0x9c) = 1` makes stock drop touch frames. Hard keys take a different path and this word
alone does not touch them — which is why blocking a key needs its own firmware patch; on a unit with
the IFS2 gate flashed, arming this same word *also* makes the stock drop SOURCE, because the cave
re-tests the stock's own predicate rather than keeping separate state. The gate is `xor #1`, not a logical negate — **only odd values
work; write 1**. It needs **both** `*(S+0x9c)==1` **and** `*(S+0x1a0)==2`, so reading back `0x9c == 1` alone is a false green; a per-tick watchdog re-checks
`0x1a0` and writes it back to 2 (`plat_pcm.c:1860-1884`). **Effectiveness is not settled**: on the bench the settings page (id 3475) held, the BT page (id 375)
failed once and then held for 8 presses after the watchdog, with no evidence either way about the watchdog's role. The only admissible test is to press where
the stock page *definitely* has a control and check that stock does not act — "the page id did not change" and "we received the touch" are structurally always
true. `S` has ~20 holders and the semantics of `+0x9c` are unknown.

The write can outlive us, so arming is an engine invariant, not a scene-callable switch: covered ⟺ armed, held in `set_cover()` both directions
(`plat_pcm.c:580`), re-checked once per second, disarmed first thing in `plat_shutdown()` (`plat_pcm.c:495`), fenced off from scene code by `tools/build.sh:28`.
Being pure RAM, a power cycle always restores stock behaviour.

## 9. SH-4 arithmetic

**There is no integer divide instruction.** Division by a runtime variable compiles to a `__sdivsi3_i4i` `div1` chain; the home page's glow alone ran ~360k
divisions per frame (`gfx.c:241`). So the glow falloff is a **257-entry lookup table** built on first use (`gfx.c:248`); the vignette multiplies **two
precomputed 1-D falloff tables**, no per-pixel sqrt or divide (`gfx.c:217`); and loop ranges are **intersected with the screen first**, since the glow centre
often sits at the screen edge and 26 % (home) / 34 % (radio) of iterations were computing a division and a blend for pixels the bounds check would discard
(`gfx.c:245`). Reciprocal multiplication instead of the divide was tried and rejected: it shifted 926 of 384 000 pixels (`gfx.c:251`).

`progress_width` is computed in **seconds**, not milliseconds (`scene_btplay.c:732`): with `W = 368` the millisecond product leaves int32 above ~97 min, and
double truncation (`pct = pos*100/dur`, then `w = W*pct/100`) gives only 101 steps — on a 219 s track the bar stepped `0,0,3,0,4,…` px/s instead of
`1,2,2,1,2,…`. **The SH-4 does have an FPU** and we use it where pixel counts are small — the icon rasteriser (distance-field shapes, ~900 px per icon, ~5 per
frame) is float; every full-screen loop is integer (`gfx.c:509-539`).

## 10. Text

Glyphs are baked offline to an anti-aliased coverage bitmap (`tools/bake_font.py`); there is no runtime TTF rasteriser. **The blob is external**
(`/HBpersistence/dev/share/studio.fnt`), not linked in — it is the bulk of the binary and almost never changes, and splitting it cut a serial push from 12 min
to 2.5 (`gfx.c:501-509`). **Scaling is 1/16 fixed point**, not integer multiples (`gfx.c:634`): bilinear plus edge reconstruction, since pixel replication gives
2×2 stair-steps that read as blurry.

**Character coverage is a real, user-visible limit** — track titles are arbitrary text from the phone. The recipe of record (`tools/bake_font.py`, the `--charset` handling) bakes
`--charset gb2312` = GB2312 levels 1+2, **6763 hanzi / 6875 glyphs**, ~3.76 MB. Level 1 only (3755) was rejected because real track titles need level-2
characters; the full CJK set (12792 glyphs, 7.3 MB) does not fit on the bench, and `bake_font.py` offers no Big5 charset ⇒ **traditional-only characters outside
GB2312 render as placeholder boxes.** The loader tries three candidate paths and validates each by *content*, not by `open()` succeeding (`main_pcm.c:56-95`).

`/HBpersistence` is **f3s NOR flash**: metadata updates immediately after a large write while data blocks are still committing, and there is no `sync` on the
bench — a freshly copied 3.9 MB font read back as 2.29 MB and was complete minutes later, so a short read is not a corrupt file (`main_pcm.c:77-84`). The font
must live here, not on the read-only navigation disk.

## Operational cautions

Beyond the rules stated inline — never `slay` the process, never `gf_layer_set_chroma`, never a per-pixel alpha plane, never `gf_layer_update` in a tight loop,
never push large binaries over serial:

| Do not | Why |
|---|---|
| Read `gdc`'s `/dev/mem` mappings through `/proc/<pid>/as` | Live Carmine MMIO; floods `CGOnOffDevCtrl.cpp:221 ASSERTION failed` and wedges the console. |
| Poll the shared IPC/IOC input channels | Hung a real car and the bench. |
| Enumerate `/fs/avrcp0` | Directory traversal SIGSEGVs `io-fs-media`; open the exact path (`plat_pcm.c:520-536`). |
| Open `/proc/<pid>/as` `O_RDWR` for state reads | A read path should carry no write capability; state reads use `O_RDONLY` (`plat_pcm.c:854`). |

Studio holds **one** `O_RDWR` fd on PCM3Reload while the touch gate is armed (`plat_pcm.c:870`); QNX permits only one `O_RDWR` opener on `/proc/<pid>/as`, so
`tsoff` can only take over after studio exits.
