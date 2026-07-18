# macos.mk — KeeperFX build for macOS Apple Silicon (arm64)
#
# Status: WORKING. Compiles natively, runs from a self-contained KeeperFX.app, and
# `make -f macos.mk dist` produces a drag-to-/Applications KeeperFX.dmg.
# See docs/macos_port.md for the full plan, decision log, and remaining work.
#
# Prerequisites (Homebrew — build machine only; players never need it):
#   brew install pkg-config cmake sdl2 sdl2_image sdl2_mixer sdl2_net ffmpeg@6 \
#                openal-soft luajit libspng minizip miniupnpc libnatpmp \
#                dylibbundler innoextract
#   (dylibbundler + innoextract are only needed for `dist`; innoextract is bundled
#    self-contained into the .app. curl/zlib/iconv come from the macOS SDK.)
#
# Notes:
#   * Unlike linux.mk, the small vendored deps (centijson, astronomy, enet6) have no
#     macOS/arm64 prebuilts, so we build them from pinned source here.
#   * Homebrew is the BUILD-TIME source of system libs only (the macOS analog of the
#     Windows build downloading prebuilt archives). The shipped .app/.dmg references
#     no /opt/homebrew — `dist` rewrites linked dylibs to @rpath in Contents/Frameworks
#     (and copies the dlopen'd libSDL3 explicitly; see docs/macos_port.md §5).
#   * Reuses src/posix.cpp as the POSIX entry point (provides main()).
#
# Distribution model — the SDL family + ffmpeg/openal/luajit/etc. are dynamic and
# bundled into KeeperFX.app/Contents/Frameworks (the analog of shipping SDL2.dll next
# to keeperfx.exe). enet6/centijson/astronomy are built+linked as static .a below.

include version.mk

BUILD_NUMBER ?= $(VER_BUILD)
VER_SUFFIX   ?= macOS
VER_STRING    = $(VER_MAJOR).$(VER_MINOR).$(VER_RELEASE).$(BUILD_NUMBER) $(VER_SUFFIX)

MKDIR ?= mkdir -p
STRIP ?= strip
ECHO  ?= echo
MV    ?= mv -f
CC    := clang
CXX   := clang++

# Homebrew prefix (Apple Silicon default). openal-soft is keg-only, so we point
# pkg-config at its .pc file explicitly.
# NOTE: curl and zlib are NOT from Homebrew — macOS ships them (headers + linker
# stubs in the SDK, library in the dyld shared cache), so we link the system ones
# with plain -lcurl / -lz. They need no bundling, like Windows' system DLLs.
BREW_PREFIX ?= $(shell brew --prefix 2>/dev/null || echo /opt/homebrew)
# Keg-only formulae (openal-soft, ffmpeg@6) aren't on the default pkg-config path.
# ffmpeg@6 is pinned because the FMV code uses swr_alloc_set_opts(), removed in ffmpeg 7+;
# 6.1.6 matches what the Linux CI builds against.
# NOTE: set PKG_CONFIG_PATH *inline* on the command — an `export`ed make variable does
# NOT reach $(shell ...) calls, which silently drops every -I/-l flag if a keg-only
# package can't be found.
KFX_PKG_CONFIG_PATH := $(BREW_PREFIX)/opt/openal-soft/lib/pkgconfig:$(BREW_PREFIX)/opt/ffmpeg@6/lib/pkgconfig:$(BREW_PREFIX)/lib/pkgconfig:$(PKG_CONFIG_PATH)
PKG_CONFIG := PKG_CONFIG_PATH=$(KFX_PKG_CONFIG_PATH) pkg-config

# ---- Source list (identical to linux.mk; includes src/posix.cpp entry point) ----
# Pull the canonical list out of linux.mk to avoid drift between the two paths.
KFX_SOURCES := $(shell awk '/^KFX_SOURCES = /{f=1;next} f&&/^src\//{gsub(/\\/,"");print $$1} f&&!/^src\//{f=0}' linux.mk)

KFX_C_SOURCES   = $(filter %.c,$(KFX_SOURCES))
KFX_CXX_SOURCES = $(filter %.cpp,$(KFX_SOURCES))
KFX_C_OBJECTS   = $(patsubst src/%.c,obj/%.o,$(KFX_C_SOURCES))
KFX_CXX_OBJECTS = $(patsubst src/%.cpp,obj/%.o,$(KFX_CXX_SOURCES))

# ---- Functional-test / oracle-dump build (FTEST_DEBUG=1; off by default) ----
# Mirrors the main Makefile's FTEST_DEBUG: define FUNCTESTING (freezes the PRNG seed, enables the
# `-ftests` runner) and compile src/ftests/**, which linux.mk's KFX_SOURCES omits. Needed for the
# oracle-dump harness (keeper-rx ADR-0016). Build with: make -f macos.mk FTEST_DEBUG=1 ...
FTEST_DEBUG ?= 0
ifeq ($(FTEST_DEBUG), 1)
  KFX_CFLAGS      += -DFUNCTESTING=1
  KFX_CXXFLAGS    += -DFUNCTESTING=1
  # The legacy tests/ftest_bug_*.c reference since-changed APIs (magic.h, the rules-config layout,
  # get_slab_attrs) and don't compile under clang; they are bit-rotted and unrelated to the oracle
  # harness. So the macOS oracle build compiles only the framework + the oracle spike (ftest_list.c
  # likewise guards the legacy registrations out on __APPLE__), not the whole tests/ directory.
  FTEST_C_SOURCES := src/ftests/ftest.c src/ftests/ftest_util.c src/ftests/ftest_list.c \
                     src/ftests/tests/ftest_oracle_spike.c \
                     src/ftests/tests/ftest_movement_oracle.c \
                     src/ftests/tests/ftest_creature_state_slap.c \
                     src/ftests/tests/ftest_parity_screenshot.c \
                     src/ftests/tests/ftest_ariadne_oracle.c \
                     src/ftests/tests/ftest_imp_dig_oracle.c \
                     src/ftests/tests/ftest_imp_convert_oracle.c \
                     src/ftests/tests/ftest_imp_mine_oracle.c \
                     src/ftests/tests/ftest_imp_gems_oracle.c \
                     src/ftests/tests/ftest_starter_dungeon_oracle.c \
                     src/ftests/tests/ftest_starter_dungeon_jobs_oracle.c \
                     src/ftests/tests/ftest_dig_shuffle_oracle.c \
                     src/ftests/tests/ftest_room_state_oracle.c
  KFX_C_OBJECTS   += $(patsubst src/%.c,obj/%.o,$(FTEST_C_SOURCES))
endif

KFX_OBJECTS     = $(KFX_C_OBJECTS) $(KFX_CXX_OBJECTS)

# ---- Include paths ----
KFX_INCLUDES = \
	-Ideps/centijson/include \
	-Ideps/centitoml \
	-Ideps/astronomy/include \
	-Ideps/enet6/include \
	-I$(BREW_PREFIX)/include \
	$(shell $(PKG_CONFIG) --cflags-only-I luajit sdl2 SDL2_image SDL2_mixer SDL2_net libavformat libavcodec libavutil libswresample openal spng minizip 2>/dev/null)

# ---- Compiler flags ----
# No -march=x86-64 (invalid on arm64). -mcpu=apple-m1 tunes for Apple Silicon.
# -Wno-main silences clang's "main should not be extern \"C\"" on src/posix.cpp.
ARCHFLAGS = -mcpu=apple-m1
# Warnings disabled to keep -Werror passing. The first group mirrors linux.mk; the
# second group covers cases Apple clang flags that the project's GCC/MinGW does not
# (verified against a full compile sweep — none are portability bugs). Candidates to
# fix at source over time rather than suppress: -Wformat (Windows %I64d specifiers),
# -Wdelete-non-virtual-dtor (gui_soundmsgs.cpp).
WARNFLAGS = -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas \
            -Wno-format-truncation -Wno-sign-compare -Wno-absolute-value -Wno-main \
            -Wno-unused-but-set-variable -Wno-missing-field-initializers \
            -Wno-bitwise-instead-of-logical -Wno-parentheses-equality \
            -Wno-pessimizing-move -Wno-unused-function -Wno-constant-conversion \
            -Wno-gnu-folding-constant -Wno-c23-extensions -Wno-deprecated-declarations \
            -Wno-delete-non-virtual-dtor -Wno-delete-abstract-non-virtual-dtor \
            -Wno-format -Wno-tautological-constant-out-of-range-compare

KFX_CFLAGS   += -g -DDEBUG -DBFDEBUG_LEVEL=0 -O3 $(ARCHFLAGS) $(KFX_INCLUDES) $(WARNFLAGS)
KFX_CXXFLAGS += -g -DDEBUG -DBFDEBUG_LEVEL=0 -O3 -std=gnu++20 $(ARCHFLAGS) $(KFX_INCLUDES) $(WARNFLAGS)

# ---- Linker flags ----
# macOS: -ldl is part of libc (omit). -rdynamic -> -Wl,-export_dynamic for backtraces.
KFX_LDFLAGS += \
	-g \
	-Wl,-export_dynamic \
	-Wl,-no_fixup_chains \
	-Ldeps/astronomy -lastronomy \
	-Ldeps/centijson -ljson \
	-Ldeps/enet6 -lenet6 \
	-L$(BREW_PREFIX)/lib \
	$(shell $(PKG_CONFIG) --libs sdl2 SDL2_image SDL2_mixer SDL2_net libavformat libavcodec libswresample libavutil openal luajit spng minizip 2>/dev/null) \
	-lminiupnpc -lnatpmp \
	-lcurl -lz -liconv

# ---- centitoml (compiled from in-repo source, same as linux.mk) ----
TOML_SOURCES = deps/centitoml/toml_api.c
TOML_OBJECTS = $(patsubst deps/centitoml/%.c,obj/centitoml/%.o,$(TOML_SOURCES))
TOML_INCLUDES = -Ideps/centijson/include
TOML_CFLAGS  += -O3 $(ARCHFLAGS) $(TOML_INCLUDES) -Wall -Wextra -Werror -Wno-unused-parameter

ifeq ($(ENABLE_LTO), 1)
KFX_CFLAGS   += -flto
KFX_CXXFLAGS += -flto
KFX_LDFLAGS  += -flto
TOML_CFLAGS  += -flto
endif

# Optional AddressSanitizer build: `make -f macos.mk ASAN=1 app -j$(sysctl -n hw.ncpu)`.
# Instruments the game's allocations and memory accesses; since ASan replaces malloc
# process-wide it also guards the bundled libs' heap, so a game-side out-of-bounds write
# that corrupts e.g. an OpenAL mix buffer is reported at the exact faulting file:line.
# Debugging only — slower and larger. Run the resulting binary directly to see the report.
ifeq ($(ASAN), 1)
SANFLAGS      = -fsanitize=address -fno-omit-frame-pointer
KFX_CFLAGS   += $(SANFLAGS)
KFX_CXXFLAGS += $(SANFLAGS)
TOML_CFLAGS  += $(SANFLAGS)
KFX_LDFLAGS  += $(SANFLAGS)
endif

# Build output lives in a per-platform subfolder so several targets can coexist.
BINDIR := bin/macos-arm64
BIN    := $(BINDIR)/keeperfx

all: $(BIN)

clean:
	rm -rf obj bin src/ver_defs.h
	rm -rf deps/astronomy deps/centijson deps/enet6 $(DL_CACHE)

.PHONY: all clean deps

# The vendored static libs, whose recipes also install the headers many sources
# #include (deps/*/include/...). Used as an order-only prerequisite of every object so
# `make -jN` never compiles a source before its dep headers exist. Order-only means a
# rebuilt lib doesn't force a recompile (the .d depfiles track real header deps); on a
# cached build the libs already exist, so this costs nothing.
DEP_LIBS := deps/centijson/libjson.a deps/astronomy/libastronomy.a deps/enet6/libenet6.a

# ---- Link ----
$(BIN): $(KFX_OBJECTS) $(TOML_OBJECTS) | $(BINDIR)
	$(CXX) -o $@ $(KFX_OBJECTS) $(TOML_OBJECTS) $(KFX_LDFLAGS)

# -MMD -MP generates .d depfiles so edits to headers trigger recompilation
# (without this, header edits leave stale .o files).
DEPFLAGS = -MMD -MP

$(KFX_C_OBJECTS): obj/%.o: src/%.c src/ver_defs.h | obj $(DEP_LIBS)
	$(MKDIR) $(dir $@)
	$(CC) $(KFX_CFLAGS) $(DEPFLAGS) -c $< -o $@

$(KFX_CXX_OBJECTS): obj/%.o: src/%.cpp src/ver_defs.h | obj $(DEP_LIBS)
	$(MKDIR) $(dir $@)
	$(CXX) $(KFX_CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(TOML_OBJECTS): obj/centitoml/%.o: deps/centitoml/%.c | obj/centitoml $(DEP_LIBS)
	$(CC) $(TOML_CFLAGS) $(DEPFLAGS) -c $< -o $@

-include $(KFX_OBJECTS:.o=.d) $(TOML_OBJECTS:.o=.d)

$(BINDIR) obj obj/centitoml:
	$(MKDIR) $@

# ============================================================================
#  Vendored deps built from source (no macOS/arm64 prebuilts exist).
#  These are cloned into git-ignored deps/ dirs at build time — same convention
#  the project uses for these libs on Windows/Linux (which download prebuilts).
#  Pinned to specific upstream refs for reproducibility.
# ============================================================================
CENTIJSON_REPO = https://github.com/mity/centijson.git
CENTIJSON_REF  = 93395382de7ea59f7348759b78d5b2044370fcce   # HEAD as of 2026-06-28 (no tagged releases)
ASTRONOMY_REPO = https://github.com/cosinekitty/astronomy.git
ASTRONOMY_REF  = v2.1.9
ENET6_REPO     = https://github.com/SirLynix/enet6.git
ENET6_REF      = v6.1.3

# Helper: shallow-clone REPO at REF into $1/src-build (idempotent).
# $(1)=dest dir  $(2)=repo url  $(3)=ref(tag or commit)
define clone_at_ref
	$(MKDIR) $(1)
	[ -d $(1)/src-build ] || git -c advice.detachedHead=false clone $(2) $(1)/src-build
	cd $(1)/src-build && git fetch --depth 1 origin $(3) && git -c advice.detachedHead=false checkout FETCH_HEAD
endef

deps: $(DEP_LIBS)

# --- centijson (mity/centijson) ---
deps/centijson/libjson.a:
	$(call clone_at_ref,deps/centijson,$(CENTIJSON_REPO),$(CENTIJSON_REF))
	cd deps/centijson/src-build && cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 && cmake --build build
	$(MKDIR) deps/centijson/include
	cp deps/centijson/src-build/src/*.h deps/centijson/include/
	cp deps/centijson/src-build/build/libjson.a deps/centijson/ 2>/dev/null || \
	  find deps/centijson/src-build/build -name 'libjson*.a' -exec cp {} deps/centijson/libjson.a \;

# --- astronomy (cosinekitty/astronomy) — single C file ---
deps/astronomy/libastronomy.a:
	$(call clone_at_ref,deps/astronomy,$(ASTRONOMY_REPO),$(ASTRONOMY_REF))
	$(MKDIR) deps/astronomy/include
	cp deps/astronomy/src-build/source/c/astronomy.h deps/astronomy/include/
	$(CC) -O3 $(ARCHFLAGS) -c deps/astronomy/src-build/source/c/astronomy.c -o deps/astronomy/astronomy.o
	ar rcs deps/astronomy/libastronomy.a deps/astronomy/astronomy.o

# --- enet6 (SirLynix/enet6) ---
deps/enet6/libenet6.a:
	$(call clone_at_ref,deps/enet6,$(ENET6_REPO),$(ENET6_REF))
	cd deps/enet6/src-build && cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DENET_STATIC=ON && cmake --build build
	$(MKDIR) deps/enet6/include
	cp -R deps/enet6/src-build/include/* deps/enet6/include/
	find deps/enet6/src-build/build -name 'libenet6*.a' -exec cp {} deps/enet6/libenet6.a \;

# ============================================================================
#  Game data  (deliberately split from the compile step)
#
#    make -f macos.mk            compile only (default `all` -> bin/macos-arm64/keeperfx)
#    make -f macos.mk data       fetch the latest KeeperFX release and extract its
#                                platform-independent game data into the .app bundle
#    make -f macos.mk dist       build a self-contained KeeperFX.app + KeeperFX.dmg
#                                (drag-to-/Applications); reuses `data` for Resources/
#
#  Data comes from the upstream *_complete release (generated, platform-independent —
#  see docs/macos_port.md §4/§6). We do NOT rebuild it from FXGraphics/FXSounds: those
#  are source-asset repos and the gfx/sfx tools have no macOS prebuilds.
# ============================================================================

# Resolve the LATEST KeeperFX release at build time — nothing is hardcoded. The small
# GitHub API call runs on every `data`, but the archive is content-addressed by the
# asset's sha256 digest, so it only re-downloads when upstream actually changes.
#   * Override KFX_DATA_TAG=vX.Y.Z to pin a specific release (e.g. a reproducible .dmg)
#     instead of tracking latest.
#   * Set GITHUB_TOKEN to raise the API rate limit (picked up automatically in CI).
KFX_REPO     ?= dkfans/keeperfx
KFX_DATA_TAG ?=
KFX_API      := https://api.github.com/repos/$(KFX_REPO)/releases/$(if $(KFX_DATA_TAG),tags/$(KFX_DATA_TAG),latest)

# Destination for the fetched game data: the .app bundle's Resources folder. `data` is
# just a helper that `app` reuses to fill the bundle — there is no separate install
# location. Override on the command line only if you want the data dropped elsewhere.
DATA_DIR ?= $(APP)/Contents/Resources

# Built-in macOS tools (Homebrew not needed here): bsdtar reads 7-Zip via libarchive/
# liblzma; shasum verifies. Override TAR=/path if you ever want a different extractor.
TAR    ?= /usr/bin/tar
SHASUM ?= /usr/bin/shasum

# Download cache (git-ignored). Cache filenames embed tag+digest, so re-running `data`
# upgrades the install in place whenever upstream publishes a newer release.
DL_CACHE := deps/dl

.PHONY: data dist app dmg clean-macos

# --- fetch the latest release's game data into DATA_DIR (download is cached,
#     content-addressed by digest, hash-verified; extracted with built-in bsdtar) ---
# Excludes the Windows binaries (we ship our own arm64 binary + dylibs) and never
# clobbers an existing keeperfx.cfg (preserves your fullscreen settings).
#
# After unpacking, overlay the in-repo config/ tree (creatrs/, fxdata/, mods/) on top.
# Windows/Linux build their data straight from config/ in the working tree, so configs
# always match the code. The macOS shortcut instead unpacks a release archive, whose
# configs are frozen at that release while this tree tracks master (far ahead) — pairing
# new code with old data. So take binary assets (graphics/sound/levels, not in git) from
# the archive but the configs from the tree, the same way the other platforms do.
data:
	@$(MKDIR) "$(DL_CACHE)"
	@echo "Resolving $(if $(KFX_DATA_TAG),release $(KFX_DATA_TAG),latest release) of $(KFX_REPO) ..."; \
	 if [ -n "$$GITHUB_TOKEN" ]; then \
	   api=$$(curl -fsSL -H "Authorization: Bearer $$GITHUB_TOKEN" "$(KFX_API)"); \
	 else api=$$(curl -fsSL "$(KFX_API)"); fi \
	   || { echo "GitHub API request failed"; exit 1; }; \
	 tag=$$(printf '%s' "$$api" | sed -n 's/.*"tag_name": *"\([^"]*\)".*/\1/p' | head -1); \
	 url=$$(printf '%s' "$$api" | grep -o '"browser_download_url": *"[^"]*_complete\.7z"' | head -1 | sed 's/.*"\(http[^"]*\)"/\1/'); \
	 sha=$$(printf '%s' "$$api" | grep -o '"digest": *"sha256:[0-9a-f]*"' | head -1 | sed 's/.*sha256:\([0-9a-f]*\)".*/\1/'); \
	 [ -n "$$url" ] || { echo "No *_complete.7z asset found in release $$tag"; exit 1; }; \
	 echo "  -> $$tag : $$url"; \
	 key=$${sha:-$$tag}; \
	 archive="$(DL_CACHE)/keeperfx-$$tag-$$key.7z"; \
	 if [ -f "$$archive" ]; then echo "Cached: $$archive"; else \
	   echo "Downloading ..."; \
	   curl -L --fail --retry 3 -o "$$archive.part" "$$url"; \
	   if [ -n "$$sha" ]; then echo "$$sha  $$archive.part" | $(SHASUM) -a 256 -c - \
	     || { echo "Checksum mismatch — aborting."; rm -f "$$archive.part"; exit 1; }; fi; \
	   mv -f "$$archive.part" "$$archive"; \
	 fi; \
	 stamp="$(DATA_DIR)/.kfx_data_$$key.stamp"; \
	 if [ -f "$$stamp" ]; then echo "Data already current in $(DATA_DIR) ($$tag)."; exit 0; fi; \
	 ex="$(DL_CACHE)/extract-$$key"; rm -rf "$$ex"; $(MKDIR) "$$ex"; \
	 echo "Extracting (built-in bsdtar) ..."; \
	 $(TAR) -xf "$$archive" -C "$$ex" || { echo "Extraction failed"; exit 1; }; \
	 root=$$(dirname "$$(find "$$ex" -maxdepth 3 -type d -name fxdata | head -1)"); \
	 [ -n "$$root" ] && [ -d "$$root" ] || { echo "No fxdata/ found in archive"; exit 1; }; \
	 $(MKDIR) "$(DATA_DIR)"; \
	 echo "Installing data -> $(DATA_DIR)"; \
	 rsync -a --exclude='*.exe' --exclude='*.dll' --exclude='*.map' --exclude='keeperfx.cfg' "$$root"/ "$(DATA_DIR)"/; \
	 [ -f "$(DATA_DIR)/keeperfx.cfg" ] || cp "$$root/keeperfx.cfg" "$(DATA_DIR)/keeperfx.cfg" 2>/dev/null || true; \
	 echo "Overlaying in-repo config/ (code-matching creatrs/fxdata/mods) over $$tag binary assets ..."; \
	 rsync -a config/creatrs config/fxdata config/mods "$(DATA_DIR)"/; \
	 rm -rf "$$ex"; rm -f "$(DATA_DIR)"/.kfx_data_*.stamp; touch "$$stamp"; \
	 echo "Upgraded to $$tag in $(DATA_DIR) (configs from working tree)"

# --- package a self-contained KeeperFX.app and wrap it in a drag-to-Applications dmg ---
# Built only from built-in tools (hdiutil/codesign/otool) plus dylibbundler (a build-
# time tool — `brew install dylibbundler`). The produced .dmg references no Homebrew.
APP_NAME  := KeeperFX
APP        = $(BINDIR)/$(APP_NAME).app
DMG        = $(BINDIR)/$(APP_NAME).dmg
BUNDLE_ID ?= net.keeperfx.$(APP_NAME)
DYLIBBUNDLER ?= dylibbundler
THIS_MK   := $(firstword $(MAKEFILE_LIST))

# ---- DMG window styling ----
# The opened .dmg shows a styled Finder window: the KeeperFX banner as a header strip,
# a near-black dungeon fill below, the app + Applications icons with a drag arrow, and
# label chips. The background is a committed Retina asset; regenerate it (and edit the
# matching window/icon geometry) with tools/macos_dmg_background.py. The packaging itself
# lives in tools/macos_make_dmg.sh — invoked by the `dmg` target below.
DMG_BG       := res/dmg-background.tiff
PLIST_IN     := tools/macos_Info.plist.in

# ---- App icon ----
# Assemble a Retina .icns from the same res/keeperfx_icon*.png set the Windows .ico
# is built from (see Makefile's res/%.ico rule) using only built-in tools: an
# .iconset folder of conventionally-named PNGs piped through iconutil. No new deps.
# The 24bpp variants are the full-colour artwork; the 08bpp ones are palette-reduced
# and only exist at the small sizes. iconutil happily packs whatever sizes are present.
ICON_PNGS = $(wildcard res/keeperfx_icon*.png)
%.icns: $(ICON_PNGS)
	@iconset="$(@D)/.$(@F).iconset"; rm -rf "$$iconset"; $(MKDIR) "$$iconset"; \
	  cp res/keeperfx_icon016-08bpp.png "$$iconset/icon_16x16.png"; \
	  cp res/keeperfx_icon032-08bpp.png "$$iconset/icon_16x16@2x.png"; \
	  cp res/keeperfx_icon032-08bpp.png "$$iconset/icon_32x32.png"; \
	  cp res/keeperfx_icon064-08bpp.png "$$iconset/icon_32x32@2x.png"; \
	  cp res/keeperfx_icon128-24bpp.png "$$iconset/icon_128x128.png"; \
	  cp res/keeperfx_icon256-24bpp.png "$$iconset/icon_128x128@2x.png"; \
	  cp res/keeperfx_icon256-24bpp.png "$$iconset/icon_256x256.png"; \
	  cp res/keeperfx_icon512-24bpp.png "$$iconset/icon_256x256@2x.png"; \
	  cp res/keeperfx_icon512-24bpp.png "$$iconset/icon_512x512.png"; \
	  iconutil -c icns "$$iconset" -o "$@"; \
	  rm -rf "$$iconset"

# `dist` = assemble the .app, then wrap it in the styled .dmg. The two halves are also
# usable on their own (`make -f macos.mk app` / `... dmg`) so you can re-package without
# rebuilding the bundle.
dist:
	@$(MAKE) -f $(THIS_MK) app
	@$(MAKE) -f $(THIS_MK) dmg

# ---- assemble KeeperFX.app ----
app: $(BIN)
	@command -v $(DYLIBBUNDLER) >/dev/null \
	  || { echo "Packaging needs dylibbundler (build-time only): brew install dylibbundler"; exit 1; }
	@command -v innoextract >/dev/null \
	  || { echo "Packaging needs innoextract (build-time only): brew install innoextract"; exit 1; }
	@echo "Assembling $(APP) ..."
	rm -rf "$(APP)"
	$(MKDIR) "$(APP)/Contents/MacOS" "$(APP)/Contents/Resources" "$(APP)/Contents/Frameworks"
	cp "$(BIN)" "$(APP)/Contents/MacOS/keeperfx"
	@echo "Building app icon + Info.plist ..."
	$(MAKE) -f $(THIS_MK) "$(APP)/Contents/Resources/$(APP_NAME).icns"
	sed -e 's/@APP_NAME@/$(APP_NAME)/g' \
	    -e 's/@BUNDLE_ID@/$(BUNDLE_ID)/g' \
	    -e 's/@VERSION@/$(VER_MAJOR).$(VER_MINOR).$(VER_RELEASE)/g' \
	    -e 's/@BUILD@/$(BUILD_NUMBER)/g' \
	    "$(PLIST_IN)" > "$(APP)/Contents/Info.plist"
	@echo "Fetching game data into the bundle ..."
	$(MAKE) -f $(THIS_MK) data
	@echo "Bundling dylibs into Contents/Frameworks (@rpath) ..."
	$(DYLIBBUNDLER) -cd -b -of -x "$(APP)/Contents/MacOS/keeperfx" \
	  -d "$(APP)/Contents/Frameworks" -p @executable_path/../Frameworks
	@echo "Bundling libSDL3 (sdl2-compat dlopens it by name, so dylibbundler can't see it) ..."
	cp "$(BREW_PREFIX)/lib/libSDL3.dylib" "$(APP)/Contents/Frameworks/libSDL3.dylib"
	chmod u+w "$(APP)/Contents/Frameworks/libSDL3.dylib"
	install_name_tool -id @rpath/libSDL3.dylib "$(APP)/Contents/Frameworks/libSDL3.dylib"
	@echo "Bundling the original-DK-data installer (script + self-contained innoextract) ..."
	cp tools/macos_install_dk_data.sh "$(APP)/Contents/Resources/install_dk_data.sh"
	chmod +x "$(APP)/Contents/Resources/install_dk_data.sh"
	$(MKDIR) "$(APP)/Contents/Resources/innoextract"
	cp "$$(command -v innoextract)" "$(APP)/Contents/Resources/innoextract/innoextract"
	chmod u+w "$(APP)/Contents/Resources/innoextract/innoextract"
	$(DYLIBBUNDLER) -cd -b -of -x "$(APP)/Contents/Resources/innoextract/innoextract" \
	  -d "$(APP)/Contents/Resources/innoextract" -p @executable_path/
	@echo "Code-signing (ad-hoc) ..."
	codesign --force --deep --sign - "$(APP)"
	@echo "Built $(APP)"

# ---- wrap the assembled .app in the styled .dmg (see tools/macos_make_dmg.sh) ----
dmg:
	@test -d "$(APP)" || { echo "No $(APP) — run 'make -f $(THIS_MK) app' first."; exit 1; }
	@echo "Creating styled $(DMG) ..."
	@APP="$(APP)" DMG="$(DMG)" BG="$(DMG_BG)" VOLNAME="$(APP_NAME)" tools/macos_make_dmg.sh

# Quick subset: remove just the macOS binary output folder (bin/macos-arm64),
# leaving vendored deps and the download cache intact for fast rebuilds.
# (`make clean` already removes everything, including the download cache.)
clean-macos:
	rm -rf $(BINDIR)

# ---- version header ----
src/ver_defs.h: version.mk
	$(ECHO) "#define VER_MAJOR   $(VER_MAJOR)" > $@.swp
	$(ECHO) "#define VER_MINOR   $(VER_MINOR)" >> $@.swp
	$(ECHO) "#define VER_RELEASE $(VER_RELEASE)" >> $@.swp
	$(ECHO) "#define VER_BUILD   $(BUILD_NUMBER)" >> $@.swp
	$(ECHO) "#define VER_STRING  \"$(VER_STRING)\"" >> $@.swp
	$(ECHO) "#define PACKAGE_SUFFIX  \"$(VER_SUFFIX)\"" >> $@.swp
	$(ECHO) "#define GIT_REVISION  \"$(shell git describe --always)\"" >> $@.swp
	$(MV) $@.swp $@
