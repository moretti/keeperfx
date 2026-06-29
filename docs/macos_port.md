# KeeperFX on macOS (Apple Silicon)

KeeperFX runs natively on Apple Silicon Macs (M1 and newer) from a self-contained
`KeeperFX.app`. This guide covers two paths:

- **[Install the app](#1-install-the-app-recommended)** — download the `.dmg`, add your
  original Dungeon Keeper files, and play.
- **[Build from source](#2-build-from-source)** — compile it yourself with Homebrew + `make`.

> **Requirements:** an **Apple Silicon** Mac (M1/M2/M3/…). Intel Macs are not supported.
> You also need a copy of the original **Dungeon Keeper** files (see step 3) — KeeperFX is a
> standalone game but requires the original data as proof of ownership.

> **A note on signing:** the app is **ad-hoc signed**, not notarized by Apple (notarization
> requires a paid Apple Developer account, which this open-source project does not have). macOS
> Gatekeeper will therefore warn the first time you open it — [step 4](#4-allow-it-through-gatekeeper)
> shows how to allow it. This is a one-time step.

---

## 1. Install the app (recommended)

### Step 1 — Download `KeeperFX.dmg`

Grab `KeeperFX.dmg` from a published release if one is available. If there is no release yet, you
can download it from the latest **macOS CI build**: open the repository's **Actions** tab → the
most recent *"Build macOS (Apple Silicon)"* run → the `…-dmg` artifact (downloading a workflow
artifact requires being signed in to GitHub), or [build it yourself](#2-build-from-source).

### Step 2 — Install KeeperFX

1. Open `KeeperFX.dmg` (double-click).
2. Drag **KeeperFX** onto the **Applications** folder in the window.
3. Eject the disk image.

### Step 3 — Add the original Dungeon Keeper files

KeeperFX ships its own data but needs a handful of original DK files (copyright, not
redistributable). The easiest source is the **GOG "Dungeon Keeper Gold"** offline installer:

1. Buy/own *Dungeon Keeper Gold* on [GOG](https://www.gog.com/game/dungeon_keeper).
2. From your GOG library, download the **offline backup installer** — the file named
   `setup_dungeon_keeper_gold_*.exe`. (You only need the main `setup_…exe`, **not** any
   separate `patch_…exe`.) Leave it in your **Downloads** folder.
3. Open **Terminal** (Applications → Utilities → Terminal.app) and run:

   ```sh
   /Applications/KeeperFX.app/Contents/Resources/install_dk_data.sh
   ```

   The bundled installer finds the `setup_…exe` in `~/Downloads` (or `~/Desktop`), extracts only
   the `DATA/` and `SOUND/` it needs, and copies them into KeeperFX's data folder at
   `~/Library/Application Support/KeeperFX/`. Nothing else is installed — it uses a self-contained
   `innoextract` inside the app.

   If your installer is somewhere else, pass its path explicitly:

   ```sh
   /Applications/KeeperFX.app/Contents/Resources/install_dk_data.sh ~/path/to/setup_dungeon_keeper_gold_1.01_fix_*.exe
   ```

   You should see `Done. Original Dungeon Keeper data installed.` Without these files the game stops
   at *"Installation file not found"*.

### Step 4 — Allow it through Gatekeeper

Because the app is ad-hoc signed (not notarized), macOS blocks it the first time. Pick **one**:

- **Easiest (Terminal):** remove the quarantine flag, then open it normally afterwards:

  ```sh
  xattr -dr com.apple.quarantine /Applications/KeeperFX.app
  ```

- **Via System Settings:** double-click KeeperFX once (it gets blocked), then go to
  **System Settings → Privacy & Security**, scroll to the message about *KeeperFX*, and click
  **Open Anyway**. (On older macOS you can instead Control-click the app → **Open** → **Open**.)

This is a **one-time** step. After it, launch KeeperFX normally from Applications or Launchpad.

### Step 5 — Play in fullscreen (optional)

**Launch KeeperFX at least once first** — the data folder and `keeperfx.cfg` are created when the app
seeds them on first launch, so the path below won't exist until then.

First, read your display's native size from the command line:

```sh
system_profiler SPDisplaysDataType | grep Resolution
#   Resolution: 3456 x 2234 Retina
```

Then open `keeperfx.cfg` (in the data dir) in TextEdit:

```sh
open -e ~/Library/Application\ Support/KeeperFX/keeperfx.cfg
```

Two lines control resolution — `FRONTEND_RES` (menus/movies) and `INGAME_RES` (the in-game list;
**ALT+R** cycles it in-game, and the **first** entry is used at launch):

```
FRONTEND_RES=640x480w32 DESKTOP DESKTOP
INGAME_RES=DESKTOP DESKTOP_FULL 3456x2234x32
```

Each value is `WxHx32` (exclusive fullscreen) or `WxHw32` (windowed), or one of the keywords
`DESKTOP` (borderless fullscreen window — recommended on macOS, handles Retina scaling for you) and
`DESKTOP_FULL` (exclusive fullscreen at your desktop resolution). **For fullscreen, make the first
`INGAME_RES` entry `DESKTOP`.** To pin the exact resolution you read above instead, use it as
`WxHx32` — e.g. for `3456 x 2234`, `INGAME_RES=3456x2234x32`. Save in TextEdit and relaunch.

### Where your files live

Saves, screenshots, config (`keeperfx.cfg`), logs and game data all live in
`~/Library/Application Support/KeeperFX/`. The app seeds this folder from inside the bundle on
first launch, so it's safe to delete and let it regenerate (you'll just need to re-run the DK-data
installer from step 3).

---

## 2. Build from source

### Step 1 — Install build dependencies (Homebrew)

These are needed only on the **build machine** — people who install the `.dmg` never need Homebrew.

```sh
brew install pkg-config cmake \
             sdl2 sdl2_image sdl2_mixer sdl2_net \
             ffmpeg@6 openal-soft luajit libspng minizip miniupnpc libnatpmp \
             dylibbundler innoextract
```

`dylibbundler` and `innoextract` are only used for packaging (`dist`). `ffmpeg@6` is pinned on
purpose — newer ffmpeg removed an API the video code uses.

### Step 2 — Build

```sh
# (a) Compile only → bin/macos-arm64/keeperfx  (the raw arm64 binary)
make -f macos.mk -j"$(sysctl -n hw.ncpu)"

# (b) OR build the distributable: compiles, fetches the latest game data, assembles
#     KeeperFX.app (icon + dylibs + ad-hoc sign), and makes the styled .dmg.
make -f macos.mk dist -j"$(sysctl -n hw.ncpu)"
#     → bin/macos-arm64/KeeperFX.app  and  bin/macos-arm64/KeeperFX.dmg
```

Useful targets:

| Target | What it does |
|---|---|
| `all` (default) | Compile → `bin/macos-arm64/keeperfx` |
| `dist` | `app` + `dmg` (the full distributable) |
| `app` | Assemble `KeeperFX.app` (binary + game data + bundled dylibs + ad-hoc sign) |
| `dmg` | Re-wrap the existing `.app` into the styled `.dmg` (no rebuild) |
| `data` | Fetch the latest release's game data into the `.app` bundle's `Resources/` |
| `clean` | Remove everything the build creates (`obj/`, `bin/`, vendored deps, download cache) |
| `clean-macos` | Remove just `bin/macos-arm64` |

Notes:
- The build fetches the **latest** KeeperFX release's game data automatically. Pin a specific
  release with `KFX_DATA_TAG=vX.Y.Z` (e.g. `make -f macos.mk dist KFX_DATA_TAG=v1.3.2`).
- Small vendored deps (`centijson`, `astronomy`, `enet6`) are cloned + built from pinned source
  into a git-ignored `deps/` the first time; later builds reuse them.
- For a dev run, build and launch the bundle: `make -f macos.mk app` then open
  `bin/macos-arm64/KeeperFX.app`. It writes saves, config and logs to
  `~/Library/Application Support/KeeperFX` (the runtime dir is relocated there for any
  build launched from inside a `.app`).

### Debugging with AddressSanitizer

arm64 is far less forgiving than x86: an out-of-bounds write that x86 silently absorbs can land in
unrelated heap and crash somewhere unrelated, far from the real bug. AddressSanitizer (ASan) catches
the heap/global overflow *at the offending access*, with the allocation site, so the cause is
obvious. Reach for it whenever a crash doesn't point straight at its cause.

**Build** with the `ASAN=1` knob:

```sh
make -f macos.mk clean      # toggling ASAN reuses the same obj/ paths, so clean first
make -f macos.mk ASAN=1 -j"$(sysctl -n hw.ncpu)"
```

**Run** the raw binary directly (no `.app` needed). `cd` into the Application Support data dir
**first** — a flat dev build uses the current directory as its runtime/data dir (only the `.app`
relocates itself there). Two env vars matter: `DYLD_FALLBACK_LIBRARY_PATH` so the Homebrew
dylibs that get `dlopen`ed by name resolve, and `ASAN_OPTIONS` to stop cleanly on the first error.
ASan reports go to **stderr**. Point `$KFX` at your checkout:

```sh
KFX="$(git -C path/to/your/keeperfx rev-parse --show-toplevel)"   # or just hardcode the checkout path
cd ~/Library/Application\ Support/KeeperFX
DYLD_FALLBACK_LIBRARY_PATH="$(brew --prefix)/lib:$(brew --prefix)/opt/openal-soft/lib:$(brew --prefix)/opt/ffmpeg@6/lib" \
ASAN_OPTIONS=abort_on_error=1:detect_leaks=0 \
"$KFX"/bin/macos-arm64/keeperfx 2>~/kfx-asan.log
```

The ASan runtime dylib resolves on its own via the Xcode toolchain rpath baked into the binary.
`detect_leaks=0` is a no-op on macOS (LeakSanitizer doesn't run on Darwin) but is harmless. When ASan
trips it prints the access (READ/WRITE + size), the offending stack, and the buffer's allocation
stack to `~/kfx-asan.log`, then aborts — that report is the starting point for the fix.

The ASan binary is **dev-only**: ~2-3× slower, larger, and not distributable. Rebuild a normal binary
the same way (`make -f macos.mk clean && make -f macos.mk …`, without `ASAN=1`) when you're done.

### Continuous integration

`.github/workflows/build-macos-arm64.yml` builds the same `.dmg`/`.app`/binary on a
`macos-14` (Apple Silicon) runner and uploads them as workflow artifacts. Like the project's
Windows/Linux CI, it produces artifacts only — publishing a download to the Releases page stays a
manual maintainer step.

---

## 3. Under the hood (developer notes)

Not needed to install or play — kept so the load-bearing decisions aren't re-litigated. All source
changes are `__APPLE__`-guarded (no Windows/Linux impact).

- **`-Wl,-no_fixup_chains` is required.** The codebase is `#pragma pack(1)` everywhere, which puts
  pointers at unaligned offsets in static-init globals; macOS chained fixups reject those. This flag
  falls back to classic relocations (which tolerate them). **Do not** try to selectively un-pack
  headers — `#pragma pack` leaks across `#include`s and made `struct Game` differ between translation
  units → SIGBUS. Keep everything packed + this flag.
- **`libSDL3.dylib` is bundled explicitly.** Homebrew's `sdl2` is sdl2-compat (SDL2 ABI atop SDL3) and
  `dlopen`s `libSDL3` by name, so `dylibbundler` (which follows `otool -L`) never copies it. `dist`
  copies it into `Contents/Frameworks` and sets its id to `@rpath/libSDL3.dylib`.
- **Writable data lives in Application Support.** A double-clicked `.app` starts with CWD `/`, but the
  engine expects its data dir to be the CWD (`INSTALL_PATH=./`). `macos_app_support_dir()` seeds
  `~/Library/Application Support/KeeperFX/` from the bundle and `chdir`s into it. Flat dev builds are
  unaffected (it returns NULL and the executable-relative dir is used). The seed runs `mkdir`/`cp` via
  `posix_spawn` (no shell, so a `$HOME` with spaces/quotes can't break or inject the command) and is
  gated on a `.seeded` marker written only after a successful copy, so an interrupted copy retries on
  the next launch instead of leaving a half-populated tree.
- **High-DPI (Retina) render sizing.** On a Retina display SDL's window surface is larger in pixels
  than the requested mode (point) size, and `LbScreenSwap()` blits the draw surface onto it 1:1 — so
  the engine must render at the *surface* size or the image is cropped. **Two places must agree:**
  (1) `LbScreenSetup()` derives its working `setup_width/height` (draw/secondary/physical-screen sizes)
  from the live `lbScreenSurface` on every call; and (2) `setup_screen_mode*()` feeds those same live
  dimensions (`lbDisplay.PhysicalScreenWidth/Height`) into `update_screen_mode_data()`, so
  `MyScreenWidth/Height` — and with them every scaling factor and the engine window the polygon
  rasterizer clamps its spans to — match the surface. Doing only (1) is the subtle trap: the engine
  then renders at the *mode* size into the smaller live surface and the rasterizer writes past it,
  corrupting adjacent heap. **Do not** "fix" any of this by writing the size back into `mdinfo`
  (`&lbScreenModeInfo[mode]`): that table is shared, persistent mode config that gets re-registered
  across the menu↔game transition, so the patched value is reverted (the crop returns) and re-doubles
  the surface on each toggle.
- **`_XOPEN_SOURCE` in `bflib_crash.c`.** macOS `<ucontext.h>` `#error`s unless `_XOPEN_SOURCE` is
  defined, so it is defined immediately before that one include. **Do not** hoist it to the top of the
  file — defining it before `<execinfo.h>`/`<dlfcn.h>` puts macOS into strict X/Open mode and hides the
  BSD extensions the crash handler uses (`backtrace()`, `dladdr()`).
- **System (not bundled) libs:** `libcurl`, `libz`, `libiconv` ship in the macOS SDK and are linked
  with `-lcurl -lz -liconv` (like Windows' system DLLs). Everything else (SDL family, ffmpeg@6,
  openal, luajit, spng, minizip, miniupnpc, libnatpmp) is bundled into `Contents/Frameworks`.
- **Game data** comes from the upstream `*_complete` release archive (platform-independent). It is
  *not* rebuilt from the FXGraphics/FXSounds source repos — those tools have no macOS prebuilds.

### Packaging-only files

`tools/macos_Info.plist.in` (bundle plist template), `tools/macos_make_dmg.sh` (styled-dmg builder),
`tools/macos_dmg_background.py` (regenerates the dmg background), `res/dmg-background.tiff`
(committed Retina background), `tools/macos_install_dk_data.sh` (the DK-data installer bundled into
the app). The app icon is built from the existing `res/keeperfx_icon*.png` via `iconutil`.

### Remaining / optional work

- **Notarization** — would remove the Gatekeeper prompt in [step 4](#4-allow-it-through-gatekeeper),
  but needs a paid Apple Developer ID ($99/yr); there is no free tier for open-source.
- **First-run UX** — the DK-data installer is currently a Terminal command (step 3); a GUI
  file-picker on first launch would be friendlier.
- **Data/code version drift.** `make data` fetches the latest *release* archive for the binary
  assets (graphics, sound banks, levels, music — not in git) and overlays the working tree's
  `config/` on top, so configs match the code. But the *binaries* stay frozen at that release while
  the source tracks master, so a post-release commit that needs a new sprite/sample/level can still
  surface a missing or wrong asset that no config overlay can fix — and the gap widens the further
  master runs ahead of the last release. The config overlay also doesn't prune files removed
  upstream, nor cover `levels/`/`campgns/` configs. For a reproducible, zero-drift release `.dmg`,
  build from a release tag and pin the data to it (check out `vX.Y.Z` + `make … KFX_DATA_TAG=vX.Y.Z`)
  so code, configs and binaries all match. Master-tracking is fine for dev, with this caveat.
- **Minor, non-blocking:** missing unifont files only affect DBCS languages (harmless for English);
  a missing multiplayer map-order file logs a warning.
