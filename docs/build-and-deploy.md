# Build and deploy

One C source tree, two backends: a Mac backend for offline iteration and an SH-4 / QNX backend that runs on the head unit (SH-4A, QNX 6.3.2, 800×480). Everything below is bench procedure — it does not cover vehicle installation or the boot autostart hook.

**"Zero-flash" is about deploying Studio, and nothing more.** Installing and running it reprograms no raw partition and patches no firmware image: it copies ordinary files into a mounted filesystem and claims a display layer at runtime. Two things it does *not* mean. (1) Nothing is written to persistent storage — `/HBpersistence` is NOR flash, and Studio's own settings file lives there, which matters in Trap ②. (2) The project needs no flashing — it depends on two flashed code caves for anything that changes stock state (IFS1: volume, source, tuner; IFS2: the SOURCE key gate). Those are separate, genuinely risky operations, and they are not covered by this document.

**Published:** `studio/sys/`, `studio/scenes/*.c`, `studio/platform/`, both entry points, `studio/tools/{build.sh,bake_font.py,studio_server.py}`. **Also published because the build needs them:** `studio/platform/puppet_addr.h` and `coexist-app/mvp/gf_defs.h` — `plat_pcm.c` includes both, and for the repo's first two commits neither was here, so nobody could build the PCM backend from a clean clone. **Not published:** the baked `.fnt` fonts, the fallback atlas `studio/scenes/studio_font.h` (rasterised from a proprietary system font; the build does not need it — `build.sh mac` passes `-DSTUDIO_FONT_EXTERNAL`, which loads a `.fnt` at runtime instead), the launcher `dev/bench/gostudio`, the SH-4 link script `dev/build_coexist_vol.sh`, the bench serial tooling.

So `build.sh mac` builds from this repo as-is, but **`build.sh pcm` still does not** — not for the old reason (both headers are here now), but because it shells out to `dev/build_coexist_vol.sh`, which in turn wants a set of SH-4 stub sources under `dev/sh4tools/` and a `sh4build` Docker image. That is a cross-toolchain, not part of the product, and it is not published. What *is* published is every line of the program itself.

The sync script that produces this repo ends with a **smoke build of the published tree**, run with the same flags as `build.sh mac`. It compiles the maintainer's own tree first as a control, so a red result means the repo is broken rather than the check — the first version of that check had the flags wrong and reported two failures that did not exist.

## 0. Quick reference

| What | Command |
|---|---|
| Build Mac preview | `./studio/tools/build.sh mac` → `/tmp/pcm_studio_run` |
| Build device binary | `./studio/tools/build.sh pcm` → `studio/pcm_studio.stripped`, prints `ck=` |
| Build both | `./studio/tools/build.sh` |
| Preview one page | `STUDIO_SCENE=btplay /tmp/pcm_studio_run` (from repo root) |
| Browser console for the preview | `python3 studio/tools/studio_server.py` → <http://localhost:8770> |
| Bake the font | §3 — the `.fnt` is **not** in the repo |
| Ferry files to the unit | USB stick, §4 |
| Start on the unit | `/HBpersistence/dev/bin/gostudio` |
| Stop on the unit | `echo x > /tmp/studio.stop` — **never `slay`** |
| Device log | `/tmp/studio.log` (`studio/platform/plat_pcm.c:85`) |

`build.sh` `cd`s to the repository root itself (`studio/tools/build.sh:7`), so it runs from anywhere. The Mac binary finds the font by *relative* path (`studio/main_mac.c:22`), so run `/tmp/pcm_studio_run` from the repository root.

Prerequisites: a host `cc` (`-lm`, no third-party deps); Python 3; Pillow for `bake_font.py`; pyserial for the bench serial tooling; a TrueType font for baking (the deployed bake uses Noto Sans SC, SIL OFL); a FAT32 USB stick; a USB-serial adapter at 57600 8N1 with **ground connected** — without ground you get zero bytes *and* every process on the unit that writes to the console blocks forever, which looks like "some library call hangs". The SH-4 build needs Docker with an image tagged `sh4build:latest` providing `sh4-linux-gnu-{gcc,ld,strip}` (Debian `gcc-sh4-linux-gnu` + `binutils-sh4-linux-gnu` suffices); no Dockerfile is checked in.

## 1. One source, two backends

Scenes (`studio/scenes/`) and the shell (`studio/sys/`) contain no platform `#ifdef`. The entry points differ only in which backend they `#include`, and both are single translation units — every `.c` is `#include`d by the entry point, no link step across scene files.

```
studio/main_mac.c  →  platform/plat_mac.c  +  scenes/*.c      (host)
studio/main_pcm.c  →  platform/plat_pcm.c  +  scenes/*.c      (SH-4 / QNX)
```

Shared code carries three build-time switches, and only one of them is about the platform: `BT_WAVEBAR` and `BT_TONEARM` in `scene_btplay.c` are opt-in decorations, off by default. The platform-relevant one is the font-source switch at `studio/scenes/gfx.c:583`: `#ifdef STUDIO_FONT_EXTERNAL` loads a runtime blob, `#else` (`gfx.c:608`) compiles in `studio/scenes/studio_font.h`. `studio/main_pcm.c:10` hard-defines the macro and the Mac build passes `-DSTUDIO_FONT_EXTERNAL` (`build.sh:43`), so both platforms take the external-blob branch.

`studio/sys/pcm_caps.h` is a capability contract: anything the hardware or the Bluetooth stack cannot deliver is `#define CAP_X 0` with its evidence attached. Read it before adding a feature. It is documentation, **not** compile-time enforcement — there are zero `#if CAP_X` call sites and `PCM_REQUIRE_CAP` is unused (see the note at the end of `pcm_caps.h`).

## 2. Building

```bash
./studio/tools/build.sh mac        # -> /tmp/pcm_studio_run
./studio/tools/build.sh pcm        # -> studio/pcm_studio.stripped, prints ck=0x........
```

**Mac:** `cc -O2 -DSTUDIO_FONT_EXTERNAL -o /tmp/pcm_studio_run studio/main_mac.c -lm` (`build.sh:43`). The flag is not optional: without it `gfx.c` uses the compiled-in atlas `studio/scenes/studio_font.h` (585 glyphs, 474 CJK — they are there because `bake_font.py` scans source string literals, §3). Hard-coded UI strings still render; **arbitrary text arriving from the phone** — track and artist names — does not. With the flag the Mac loads the same `.fnt` blob the device loads. The old artifact is deleted before the build (`:40`) and again if `cc` fails (`:44`).

**Device:** delegates to `dev/build_coexist_vol.sh` (not published), building inside `sh4build:latest`. The link recipe is not negotiable:

| Element | Why |
|---|---|
| `-nostdlib` + a minimal `_start.S` | no QNX SDK exists |
| SONAME stubs for `libc.so.2`, `libm.so.2`, `libz.so`, `libecpp-ne.so.4`, `libgdcApiCarmine.so` | they only satisfy the static linker; `ldqnx.so.2` rebinds every import to the unit's real libraries at load |
| `-Wl,--no-as-needed` on all five | the graphics library is BIND_NOW; if its deps are not `NEEDED` by *our* process the loader kills us before `main()` — no log, nothing on screen |
| `libgdcApiCarmine.so`, never `libgf.so.1` | the stock device does not ship `libgf.so.1` (`plat_pcm.c:28`) |
| `-lgcc` | SH-4 software division helper `__sdivsi3_i4i`, used by the per-pixel renderer |
| `-Wl,--dynamic-linker=/usr/lib/ldqnx.so.2` | QNX interpreter path |

Deploy the **stripped** output; `readelf -d` should show exactly five `NEEDED` entries. Both `studio/pcm_studio` and `studio/pcm_studio.stripped` are deleted before the build (`build.sh:66`) and again on failure (`:70`), `set -o pipefail` is on (`:65`), and the output filter drops only `warning:` lines (`:68`). On failure: `‼️ 编译失败 -> 两个产物都已删除, 别推台架`, and no artifacts. See Trap ④.

### 2.1 Build-time lint

`lint_guards()` runs before both backends (`build.sh:10-33`, called at `:36` and `:50`):

| Rule | Scope | What it catches |
|---|---|---|
| `cond ? C_X : C_Y` (`:16-17`) | `studio/scenes/` | Colour constants are three comma-separated components (`#define C_AMBER 239,181,74`, `scene_btplay.c:59`). A ternary between two of them re-associates through the comma operator, keeps the argument count unchanged, compiles silently, and lets only the first channel follow the condition. Use `if/else` (`scene_btplay.c:584-585`). |
| `read(...) > 0` loops (`:21-23`) | `studio/main_pcm.c` | `read()` returning **-1** is an error, not EOF; a hand-rolled loop treats it as EOF and truncates at a varying offset. Use `read_all()` (`main_pcm.c:40-53`), which separates `0` from `<0` and reports the retry count. |
| `plat_ts_*` / `plat_peek2` / `plat_touchgate` (`:28-30`) | `studio/scenes/` | Scenes must not touch engine internals (`plat_internal.h`). The touch gate enforces the "covering ⟺ gate armed" invariant; if a scene arms or disarms it, nobody can reason about it. |
| Chinese literals inside `gfx_text*` / `shell_toast` (`:39-43`) | `studio/scenes/`, `studio/sys/` | A translated string hard-coded at the call site instead of going through the `pcm_i18n.h` table, which would leave it stuck in one language. Note the inverse is **not** caught: an English literal drawn directly still compiles. |

All four greps skip lines whose match sits in a comment (`grep -vE ':[[:space:]]*[*/]'`, `:16`, `:22`, `:29`), so keep the comment marker at line start when documenting a bad pattern.

### 2.2 The self-check `ck`

The unit has no `cksum`, so at startup the binary FNV-1a-32-hashes the file at `/HBpersistence/dev/bin/studio`, falling back to `/tmp/studio`, and prints it first (`main_pcm.c:97-111`): `自校验 ck=0x…… size=……`. It hashes that path, not its own loaded image, so running from a third location makes the two diverge. `build.sh pcm` computes the same hash over `studio/pcm_studio.stripped` (`build.sh:72-77`). **The two must match** — otherwise the file on the unit is not the file you built. `ck` is per build; read it from your own build, never copy one from a document. Compare `size=` against the file size on disk too: a short read shortly after a copy is normal (Trap ②).

## 3. Font baking

The font is generated: a TrueType face rasterised offline into a greyscale coverage atlas, so the SH-4 does table lookup plus alpha blending at runtime. It stays out of git because most bakes are not redistributable (Arial Unicode, PingFang), because the blobs are 3–7 MB and change on every tweak, and because one command regenerates it. `studio/scenes/studio_font.h` is itself an Arial Unicode bake — re-bake from Noto before publishing it.

```bash
python3 studio/tools/bake_font.py \
    --body-font references/fonts/NotoSansSC-var.ttf \
    --weight Medium --body-px 24 --charset gb2312 \
    --bin studio/studio_notosc.fnt
```

That produces 6,875 glyphs at 24 px, 3,938,152 bytes, redistributable (SIL OFL). `--charset cjk` on the same face gives 12,792 glyphs / 7,285,918 bytes.

- **`--weight Medium` is not optional.** Variable Noto Sans SC defaults to Thin (100); omit it and you bake hairline glyphs (`bake_font.py:63-64`). The tool prints `可变字重 -> Medium` (`:111`).
- **Charset.** `gb2312` is levels 1+2 = 6,763 hanzi (`bake_font.py:66-68`). Level 1 alone (≈2.1 MB) misses characters that occur in real artist names (奕 is level 2); the full `cjk` range is too large for the persistence partition. Level 1+2 gave zero missing characters across 39 real track/artist names and rendered pixel-identically to the full bake in the text area.
- **Open point:** phones also deliver **traditional** characters, which GB2312 does not contain. An older bake used GB2312 ∪ Big5, a set none of the current `--charset` options reproduces. The loader takes whatever blob is at the path, so swapping is a file copy.
- The script scans `studio/**/*.{c,h}` string literals and adds the characters it finds (`bake_font.py:35-55`), so hard-coded UI strings render. Limits: the predicate is `U+4E00 ≤ ch ≤ U+9FFF` plus the punctuation set `·—…、。，：；？！《》（）℃°` (`:53`), and it only helps **after you re-bake** — new Chinese in the source without a re-bake renders as boxes. Without `--bin` the script writes the C header to stdout instead; that is the fallback path only.

**Loading.** Device candidate order (`main_pcm.c:63-68`): `/HBpersistence/dev/share/studio.fnt` → `/tmp/studio.fnt` → `/fs/usb0/studio.fnt`. Opening successfully is **not** enough: content is validated (`SFNT` magic plus header/length consistency, `gfx.c:589-602`) and a rejected file falls through (`main_pcm.c:56-94`) — see Trap ③. The load buffer is 7,168 KiB (`main_pcm.c:55`); a larger font is truncated and then correctly rejected, so it never renders half a font. The loader reads the real length with `lseek` and reports read-vs-actual separately (`main_pcm.c:71-84`):

```
  (只读到 <n>/3938152 —— 文件是全的, 多半是刚写完还没落盘, 过会儿再试)
字库已加载 3938152 字节, 6875 字形  <- /HBpersistence/dev/share/studio.fnt
```

The Mac side loads the same file from the repo (`main_mac.c:22-23`) and prints `[studio] 字库 studio/studio_notosc.fnt  3938152 字节`. If you instead see `⚠ 字库没加载上 -> 中文会是方块, **版式判断不作数**` (`main_mac.c:36`), stop — any layout judgement from that frame is void.

## 4. Deploying to a bench unit (USB)

| Source | Destination | Note |
|---|---|---|
| `studio/pcm_studio.stripped` | `/HBpersistence/dev/bin/studio` | `chmod +x` |
| `studio/studio_notosc.fnt` | `/HBpersistence/dev/share/studio.fnt` | 3,938,152 B, changes rarely |
| `gostudio` | `/HBpersistence/dev/bin/gostudio` | `chmod +x` |

A fourth persistent file is **not** ferried — Studio writes it itself: `/HBpersistence/dev/etc/studio.conf`, holding the per-source "Studio takes over" toggles and the UI language, rewritten immediately on every settings change. `mkdir -p /HBpersistence/dev/etc` must exist or settings silently fail to persist and revert to defaults (Bluetooth on, FM and AUX off, English) at next start.

`/HBpersistence/dev/` is a development area laid out as `bin/ lib/ share/ pkg/ etc/ var/`. Use it rather than `/tmp`, which is a RAM disk and is empty after every power cycle. `/tmp` is a fallback in both candidate lists, not an override: the persistent copy wins whenever it is present and passes content validation.

```bash
# host
./studio/tools/build.sh pcm
cp studio/pcm_studio.stripped /Volumes/<STICK>/studio
cp studio/studio_notosc.fnt   /Volumes/<STICK>/studio.fnt
cp dev/bench/gostudio        /Volumes/<STICK>/gostudio
sync
```

> ⚠️ **The stick must not contain `copie_scr.sh`.** If it does, inserting it after boot makes `proc_scriptlauncher` XOR-decode it and run `run.sh` from the stick root automatically. That is how flashing packages are delivered, so a ferry stick carrying an armed flashing `run.sh` will flash the unit the moment you plug it in. Keep ferry sticks free of both files, or keep the flashing script renamed (`run_flash_puppet.sh.OFF`). This is the only step here that can brick a unit, and it is a property of the stick, not of Studio.

On the unit (serial console), **after it has fully booted** — the autorun mechanism fires only on an insert event after boot; a stick present at power-on is treated as ordinary media storage:

```sh
mount | grep umass                 # mount point varies: /fs/usb0 or /mnt/umass<xxxx>
U=/fs/usb0                         # set to what you actually saw
mount -uw $U                       # only if it mounted read-only
echo x > /tmp/studio.stop          # 🚨 STOP IT FIRST — see the trap below
pidin -P studio | grep studio      # must print nothing before you copy
cp $U/studio      /HBpersistence/dev/bin/studio
cp $U/studio.fnt  /HBpersistence/dev/share/studio.fnt
cp $U/gostudio /HBpersistence/dev/bin/gostudio
chmod +x /HBpersistence/dev/bin/studio /HBpersistence/dev/bin/gostudio
```

Verify with the `ck` line at startup (§2.2), not `ls`.

Bench shell limits, worth re-checking on your own unit before scripting: `head`, `wc`, `cat` (in PATH), `touch`, `dd`, `od`, `base64` and `cksum` have been observed absent. `grep -E` with multiple patterns or Chinese has SIGSEGV'd — use `sed -n 'a,bp'`. A trailing `&` at end of line is a syntax error in this ksh; `on -d ... & echo x` parses. `slay` asks `y/N` interactively and `slay -f` is not reliable — processes have survived it and accumulated; scripts must answer `y` themselves. For Studio, `slay` is the wrong tool regardless (§5).

## 5. Traps

**① Never push binaries over the serial console.** It is 57600 8N1 (≈5.7 KB/s) and the unit echoes everything you type, so a 132 KB binary sent as hex is 264 KB of characters ≈ 46 s of saturated console. During that window any other process writing to the console blocks — an undrained `devc-sersci` buffer makes a console write block *forever* in `REPLY` state — and the stock OnOff service, which writes on a timer, hits its IPC timeout and asserts until it dies; then the console is mute and the only exit is a power cycle. Binaries and fonts go by USB; serial is for commands and logs. Keep a resident drainer reading the port, because stock tools log via `slog` and `slog` writes to the console.

**② After copying a large file, a short read is expected.** `/HBpersistence` is f3s on NOR flash: metadata updates immediately after a large write while data blocks are still committing, and reads return EOF at the committed boundary. There is no `sync` on the bench. Measured: a freshly copied 3.9 MB font read back as 2.29 MB, then read complete a few minutes later. A short read or a low `size=` right after a push means "wait", not "corrupt"; the loader prints read-vs-actual separately so the two are distinguishable (`main_pcm.c:71-84`).

**③ A broken leftover file will shadow a good one.** Both candidate lists (font, self-checksum) are ordered by path and `open()` succeeding on the first candidate is enough to win — a half-written 2.8 MB remnant at `/HBpersistence/dev/share/studio.fnt` was loaded forever while the good copy in `/tmp` was never touched. Acceptance must be by **content**, not by `open()`: `load_font()` treats `gfx_font_use_blob()` accepting the blob as the test and logs the path and size of every rejected candidate (`main_pcm.c:56-94`). When something behaves like an old version, check whether an earlier candidate path still holds a file.

**④ A build script that prints a stale checksum will get a stale binary onto the unit.** Two shapes to close: an output filter that swallows the inner script's failure line (without `pipefail` the pipeline's exit status comes from `grep`, so the script prints the previous `ck` and exits 0), and `strip` running before the error check, which regenerates a `.stripped` from the *previous* unstripped binary — old bytes, fresh timestamp. `build.sh` deletes both artifacts before and after a failed build, sets `pipefail`, filters `warning:` lines and the source-echo lines that follow them, and requires the artifact to exist (`build.sh:65-71`). Verify any such guard by injecting a deliberate syntax error and checking that it goes red.

**⑤ `cp` over a *running* binary fails silently.** On the bench it does not error; it just does not replace the file, and the install script goes on to report success. It is worse than it sounds: two consecutive builds can be **exactly the same size** (132232 B, observed), so `ls -l` shows nothing wrong and the timestamp is fresh. Half an hour was lost debugging an old binary that way, and the only thing that eventually gave it away was a probe command answering "unknown level 20". Two consequences: stop Studio *before* copying (`echo x > /tmp/studio.stop`, then confirm with `pidin -P studio` that it is gone, and refuse to copy if it is not — the installer script does this itself), and treat only the startup `ck` line as proof of which binary is running. **`ls -l` is not evidence.**

**⑥ Never run two studios.** The second one's startup `disable + update` pulls the layer out from under the first, whose next `gf` call then blocks on the graphics server; the survivor cannot be stopped by `/tmp/studio.stop` and the unit needs a power cycle. It is worse than a crash because the symptoms look like something else entirely: a command file that empties with nothing responding, or a startup log with the `ck` line missing, because `plat_log` truncates per process and the refusing instance would otherwise wipe the live one's log. Studio now takes a single-instance lock at `/tmp/studio.lock` as its very first action and `gostudio` refuses to launch over a running instance — but only if you start it through `gostudio`.

## 5.5 Starting at boot

The stock process table already contains an entry that runs `/proc/boot/ksh /HBpersistence/debugTools.sh` in the background, and **that file does not exist on a stock unit** — the boot log has always said so. Creating it is the entire install; deleting it is the entire uninstall. No firmware image, no checksum, no raw-partition write. The repository copy is `dev/bench/studio_autostart.sh` (deliberately *not* named `debugTools.sh`, because the old volume-OSD engine ships a file by that name and one machine can only have one of them — they are mutually exclusive by construction).

The hook runs as process #34 while `layermanager` is #46 and `gdcServerCarmine` is #56, so the script polls for gdc before touching anything graphical. It also writes **both** `/tmp/p3pid` and `/tmp/rlpid`: the stock hook writes only the first, and an instance missing the second comes up looking healthy while its page id stays −1 forever. A kill switch at `/HBpersistence/studio/DISABLE` stops it without deleting anything, and it lives on persistent storage on purpose — a switch in `/tmp` would un-set itself at the next power cycle.

The respawn guard has one deliberate dead end worth knowing about. Because layer selection skips any 800×480 candidate that has a real buffer, and Studio is itself 800×480, a crashed instance's residue reads as "occupied" — so the respawned instance finds no free layer, logs it, and exits. The guard then backs off to 30 s and keeps trying without effect until the next power cycle. That is the intended trade: not stealing a layer beats recovering the UI.

**Verified on the bench on 2026-08-17**, end to end: power on → the hook waits for `gdcServerCarmine` → Studio starts about ten seconds in, takes gf1 while it is still untouched, registers its claim, and leaves the screen to the stock UI. One press of the physical SOURCE key hands the screen over in 221 ms. The old volume-OSD engine never starts, because one machine can only have one `debugTools.sh`.

The same session found three real defects that only a running unit could have shown, all fixed and re-verified: a watchdog that killed the process silently because it was armed unconditionally but only refreshed while the touch gate was armed; a layer-occupancy check that could not recognise its own residue, so Studio could start exactly once per power cycle; and an init-failure path that idled forever without honouring the stop switch, which held the singleton lock and made the respawn guard believe Studio was alive.

**Still true: none of this has run in a car.** Everything above is a bench result. Three of the guards added that day — the yield gate, fail-closed yield startup, and the anchoring backoff — are covered only by their boot self-checks and build lints; no normal code path has triggered them.

## 6. Running and stopping

```sh
/HBpersistence/dev/bin/gostudio
```

`gostudio` is the **only** supported way to start it. It resolves the PIDs of `PCM3Root` and `PCM3Reload` with `pidin` into `/tmp/p3pid` and `/tmp/rlpid` (**never hard-code PIDs** — a stale one silently reads another process; `/tmp` is cleared on every boot, and the stock autostart hook writes only `p3pid`, so a studio started any other way comes up half-blind with page id stuck at −1); **refuses to start if a studio is already running**; clears `/tmp/studio.stop`; takes an optional mode argument (`gostudio 2` for mirror mode); and launches with all three standard streams redirected: `on -d /HBpersistence/dev/bin/studio </dev/null >/dev/null 2>/dev/null`. The redirection is mandatory — the graphics library prints to stdout on connect, and with an undrained console that single write blocks the process forever and looks exactly like "the gf call hangs" (Trap ①).

**The compiled default is mode 1 (the real UI).** It used to be mode 0, the full-screen colour test
pattern — which was fine while the only way to start Studio was by hand, and a latent disaster for
booting it automatically: `/tmp` is empty at boot, so a missing `/tmp/studio_mode` meant the car
would come up showing colour bars. What appears on the screen should not depend on whether an
external file happens to exist. The self-test is now opt-in (`echo 0 > /tmp/studio_mode`).

Startup mode comes from `/tmp/studio_mode`, re-read **every tick** from the main loop (`main_pcm.c:198`, `plat_pcm.c:178-185`); read inside `plat_present()` it would never run once the screen goes idle, because the shell only presents when dirty. Mode `0` (default) is a self-test image — corner primaries, white border, moving square, which proves layer, colour channels and geometry before you debug content. Mode `1` is the real UI. Mode `2` is mirror mode: it reads stock state and logs it but **does not take the screen**, so you can watch the stock UI while calibrating page ids and sources.

```sh
echo 1 > /tmp/studio_mode          # takes effect on the next frame
echo btplay > /tmp/studio_scene    # optional: pin one scene instead of following stock pages
echo x > /tmp/studio.stop          # stop (bench ksh has no `touch`)
```

**Never `slay` the process.** Four things happen only on the graceful path (`plat_shutdown()`, `plat_pcm.c:495-510`): the touch gate is disarmed (`:499`), MME event registration is cancelled (`:502`), the puppet cave is parked back to `LEVEL=0` (`:503`), and the layer's blending is reset (`:506`) before `disable` + `update` actually turns it off (`:507-508`). The first three run *before* the `if(!g_layer) return` guard (`:504`), so they happen even with no layer to give back.

- **The layer.** `gdc` does not clear a layer's enable bit when a client dies; measured on the bench, after `slay -f` enable stayed 1 for more than 8 s, leaving the last frame on the display. Starting Studio again clears it — `plat_init()` does an explicit `gf_layer_disable` + `gf_layer_update` right after attach and logs `[清残留] 已 disable+update 真关一次` (`plat_pcm.c:407-409`). A power cycle also clears it.
- **The touch gate.** Whether it actually blocks stock touch is **not settled** — bench results were inconsistent across pages (`pcm_caps.h:101-105`). The engine maintains it as an invariant anyway, so leaving it armed risks leaving stock touch dead. It is pure RAM; a power cycle clears it.

## 7. Verifying on the real unit

**Read the log first** — `/tmp/studio.log` (`plat_pcm.c:85`). A healthy start:

| Line | Meaning | Source |
|---|---|---|
| `自校验 ck=0x… size=…` | must equal the `ck` from your build (§2.2) | `main_pcm.c:110` |
| `[自检] 防绿屏闸门有效 ✓` | every boot deliberately fakes "first frame not ready" and checks the gate refuses to light the layer. If this line is missing or says otherwise, the anti-green-screen guard is gone | `plat_pcm.c`, `plat_init()` |
| `[层] 首帧还没就位, 拒绝点亮(防绿屏)` | the same gate firing for real — expected once during takeover, not repeatedly | `push_layer()` |
| `字库已加载 … 字节, … 字形  <- …` | which font file won the candidate race | `main_pcm.c:86-87` |
| `display 800x480  nlayers=…` | display attached | `plat_pcm.c:389-390` |
| `gf_layer_attach(1,PASSIVE) r=0` | got the layer | `plat_pcm.c:396-397` |
| `surf vaddr=0x… stride=1600 (px=800)` | surface created, no padding | `plat_pcm.c:422-424` |
| `双缓冲: 开 (第二块 va=0x…)` | double buffering active | `plat_pcm.c:461` |
| `控件/脏矩形自检 ✓` | scene-entry assertions passed | `scene_btplay.c:904` |
| `[覆盖] 接管屏幕` / `[覆盖] 让开, 显示原厂` | we took the screen / handed it back for an unrecognised stock page | `plat_pcm.c:588`, `:606` |
| `[让出] 原厂在用这层 -> 纯停手(不 disable!)` | stock started using our layer | `plat_pcm.c:300` |

Double buffering failing is not fatal and not silent: it falls back to one buffer and logs `双缓冲: 关 (第二块建不出来 r=…)` (`:467`), `双缓冲: 关 (第二块 stride 不一致)` (`:463`), or `双缓冲: 关 (/tmp/studio_nodbuf 存在)` (`:451`, an escape switch that avoids re-pushing a binary). `btplay_enter()` (`scene_btplay.c:878-905`) re-derives every control's hit box from the constants used to draw it and asserts that each button's centre hits itself, that the dirty rectangle covers everything that changes, and that the touch band does not overlap the time row.

**Take a real screenshot; do not photograph the screen.** `gf_display_snapshot` captures the *composited* output, so the stock UI and our hardware layer are both in it. The capture helper (`pcmshot`, not published) writes hex to a file on the unit; pull it over serial and decode on the host. Stop the resident drainer before pulling and restart it afterwards — it holds the port exclusively, and a second opener gets `multiple access on port`.

```sh
/HBpersistence/dev/bin/pcmshot /tmp/s.hex                 # whole screen, step=2 (~40 s over serial)
/HBpersistence/dev/bin/pcmshot /tmp/s.hex 392 280 368 80  # region, full resolution (~25-60 s)
```

The display is BGRA8888, fmt `0x1420`, stride 3200; `pcmshot` emits 8 hex digits per pixel (the 16-bit P5HEX form is 4). At `step=2` CJK glyphs smear — **do not judge the font from a half-resolution capture**, re-shoot the region at full resolution. Because the capture is a composite, padding, radii, colour, layout and opacity can all be checked offline pixel-exactly; what it cannot show is anything happening during hardware scan-out (e.g. striping from a misaligned stride), which is the one case where a photograph is genuinely required. Diffing a "hidden" and a "shown" frame gives the overlay's real bounding box — that is how the destination viewport was confirmed to be `y2-y1+1`.

**Mac preview.** `plat_mac.c` writes `/tmp/pcm_studio/frame.ppm` every frame (P6, RGB after three header lines) with an atomic rename, so a reader never sees a partial frame (`plat_mac.c:85-91`). Crop and magnify 8–12× nearest-neighbour before judging icons; at 1:1 the page hides 2×2 stair-stepping and invisible press highlights. Wait a couple of seconds after a scene change before grabbing. Feed events by appending to the events file (`plat_mac.c:94-98`): `echo '2 0 1 0 0' >> /tmp/pcm_studio/events` (fields: type, which, arg, x, y).

## 8. Mac preview vs. the real machine

The Mac is for layout, colour, typography and logic. Timing, bandwidth, pixel format and real data must be confirmed on hardware.

| | Mac | PCM 3.1 |
|---|---|---|
| Tick | 33 ms (`main_mac.c:51`) | 25 ms poll (`main_pcm.c:204`); a full-page redraw measured ≈27 ms render + 12–25 ms present |
| Full-screen push | free (writes a PPM) | 768,000 B: **259 ms** per-pixel u16, **130 ms** with u32, **130 ms** with u32 + 8× unrolling (bench-measured, 3 runs averaged) |
| Redraw | dirty-driven | dirty-driven **plus** dirty spans; on the Bluetooth page the animation rectangle is 1.4 % of the screen for 11 of every 12.5 frames, 9.6 % on the second boundary |
| Transitions | `plat_can_animate()` = 1 | 0 — `CAP_SCREEN_TRANSITION 0` (`pcm_caps.h:84-89`); a 6-frame fade cannot exist at 130 ms/frame |
| Colour | PPM → RGB888 | RGBA5551: 32 levels per channel, so gentle gradients band. Banding invisible on the Mac is real on the device |
| Data | `state.json` | real MME + stock-state mirror; titles are arbitrary text from a phone, including traditional Chinese |
| Input | events file | stock mirror (read-only) + touch gate |
| Layer politics | none | yield protocol, double buffering, periodic re-assert, `gf_layer_update` rate limits |

Do not turn the VRAM numbers into a bandwidth figure: doubling the store width halved the time and unrolling changed nothing, so the bottleneck is the number of bus transactions, not bytes. Widening stores helps; micro-optimising the loop does not. SH-4 store queues (32-byte bursts) could plausibly go below 130 ms — untested.

`gf_layer_update` has two throttles, neither a 1 Hz ceiling on updates: page flip at **20 Hz** — `swap_bank()` returns early if under 50 ms have passed (`plat_pcm.c:726`) and updates with `GF_LAYER_UPDATE_NO_WAIT_VSYNC` (`:735`) — and the keep-alive re-assert at **1 Hz** (`:824`). The hazard is calling `gf_layer_update` in a tight unthrottled loop, which REPLY-blocks against `gdcServerCarmine` (seen as `REPLY 4104` in `pidin`).

Failure classes only the real machine produces: a transition drawing its first frame at alpha 255 (black) and expiring before the next tick, because one device tick can exceed a whole animation; a control switch read from inside `plat_present()`, which a dirty-driven shell stops calling once the screen is static; a periodic layer re-assert pushing a hard-coded surface index and dragging the just-swapped frame back to the older bank (invisible single-buffered, obvious double-buffered); and missing glyphs, because mock strings are covered by the atlas and real phone titles are not.

## 9. Troubleshooting

| Symptom | Most likely cause |
|---|---|
| `ck` in the log ≠ `ck` from the build | old or truncated file on the unit — re-ferry, do not debug the code |
| `size=` < the file size on disk | data blocks still committing after a copy; wait and restart (Trap ②) |
| CJK renders as boxes | font not loaded (check the candidate line), new Chinese added without re-baking (§3), or the Mac build lost `-DSTUDIO_FONT_EXTERNAL` |
| `pidin \| grep studio` shows `REPLY <gdc pid>` | blocked in a `gf` call: `gf_layer_update` in an unthrottled loop (§8), or an undrained console blocking a stdout write (Trap ①) |
| Process killed, screen frozen on our last frame | `gdc` does not clear the enable bit when a client dies (≥8 s observed). Restarting Studio clears it, so does a power cycle. Use `/tmp/studio.stop` |
| Stock touch odd after we exit | killed before `plat_shutdown()` disarmed the gate. Gate effectiveness is undetermined (`pcm_caps.h:101-105`); it is RAM-only, so a power cycle clears it |
| Console mute, picture still updating | console wedged, unit fine — power-cycle (Trap ①) |
| Build "succeeded" but behaviour unchanged | verify with `ck` (Trap ④) |
| `multiple access on port` | the serial drainer and a direct-open script both hold the device — stop the drainer first (§7) |

Line references were checked against the tree; they drift, so grep before trusting one.
