#include "ftest_parity_screenshot.h"

#ifdef FUNCTESTING

#include "../../pre_inc.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "../ftest.h"
#include "../ftest_util.h"

#include "../../game_legacy.h"
#include "../../game_merge.h"
#include "../../keeperfx.hpp"
#include "../../thing_objects.h"
#include "../../light_data.h"
#include "../../game_lghtshdw.h"
#include "../../player_data.h"
#include "../../player_instances.h"
#include "../../engine_camera.h"
#include "../../local_camera.h"
#include "../../engine_redraw.h"
#include "../../engine_render.h"
#include "../../thing_list.h"
#include "../../thing_data.h"
#include "../../gui_msgs.h"
#include "../../gui_frontmenu.h"
#include "../../bflib_vidsurface.h"
#include "../../bflib_video.h"
#include "../../bflib_mouse.h"
#include "../../config_settings.h"
#include "../../ver_defs.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>

#include "../../post_inc.h"

#ifdef __cplusplus
extern "C" {
#endif

// Map coords are 256 per subtile; used to report the camera focus in fractional subtiles.
#define PARITY_COORDS_PER_SUBTILE 256.0

// Fixed capture zoom (DK's code-default camera zoom, == keeper-rx's KEEPER_GAME_ZOOM default). Forcing
// it every shot makes the frame independent of the persisted/drifted zoom and of any live mouse-wheel
// input, and lines the shot up with our matched render.
#define PARITY_ZOOM 8192

// The game turns we grab a frame at — spanning one heart beat (~36 turns at 20 Hz) so a keeper-rx diff
// exercises the beat's phases, not a single still. Starts at 9 (not 0): the first turns are still
// warming up the render (texture upload, first draw), so turn 0 captures a partial frame. The last
// entry is the terminating tick.
static const GameTurn PARITY_SHOT_TICKS[] = { 9, 18, 27, 36 };
#define PARITY_SHOT_COUNT ((int)(sizeof(PARITY_SHOT_TICKS) / sizeof(PARITY_SHOT_TICKS[0])))
#define PARITY_LAST_TICK (PARITY_SHOT_TICKS[PARITY_SHOT_COUNT - 1])

struct ftest_parity_screenshot__variables
{
    TbBool setup_done;
};

static struct ftest_parity_screenshot__variables ftest_parity_screenshot__vars = {
    .setup_done = false,
};

// forward declaration
FTestActionResult ftest_parity_screenshot_action001__capture(struct FTestActionArgs* const args);

// The parity scene this run captures — a numbered, meaningful name describing the behaviour under test
// (e.g. "001-dungeon-heart-beat", "002-game-start"). keeper-rx groups a scene's frames in a folder of
// this name, so the number orders them and the words say what it is.
static const char* parity_scene(void)
{
    const char* s = getenv("KEEPERFX_PARITY_SCENE");
    return (s != NULL && s[0] != '\0') ? s : "001-dungeon-heart-beat";
}

// Optional: KEEPERFX_PARITY_NO_CREATURES=1 removes every creature before the shots, for a scene that
// isolates the static world (heart/terrain/light) from wandering, self-lit creatures.
static TbBool parity_no_creatures(void)
{
    const char* v = getenv("KEEPERFX_PARITY_NO_CREATURES");
    return v != NULL && v[0] == '1';
}

// Optional: KEEPERFX_PARITY_NO_CURSOR_LIGHT=1 turns off the player's cursor light — a dynamic light
// that follows the mouse pointer (created in init_player_as_single_keeper, player_utils.c). Our
// renderer never draws it, so for a clean world-only parity shot it is a spurious light we can drop.
static TbBool parity_no_cursor_light(void)
{
    const char* v = getenv("KEEPERFX_PARITY_NO_CURSOR_LIGHT");
    return v != NULL && v[0] == '1';
}

static TbBool parity_delete_thing(struct Thing* thing)
{
    delete_thing_structure(thing, 0);
    return false; // keep visiting the rest of the list
}

// Where the png+json pairs are written: $KEEPERFX_PARITY_OUT, else $KEEPERFX_ORACLE_OUT, else CWD.
static const char* parity_out_dir(void)
{
    const char* dir = getenv("KEEPERFX_PARITY_OUT");
    if (dir == NULL || dir[0] == '\0')
        dir = getenv("KEEPERFX_ORACLE_OUT");
    if (dir == NULL || dir[0] == '\0')
        return ".";
    mkdir(dir, 0755); // best-effort; ignore EEXIST
    return dir;
}

// Save the current frame as an opaque RGB PNG. The engine's take_screenshot() saves lbDrawSurface
// verbatim — an 8-bit paletted surface whose in-game transparent colour-key becomes a PNG tRNS chunk,
// so a viewer draws the (mostly dark) dungeon as transparent and the frame looks blank. Converting to
// RGB24 first flattens the palette and drops the transparency, giving a plain, viewable image.
static TbBool parity_save_png_opaque(const char* path)
{
    if (lbDrawSurface == NULL)
        return false;
    SDL_Surface* rgb = SDL_ConvertSurfaceFormat(lbDrawSurface, SDL_PIXELFORMAT_RGB24, 0);
    if (rgb == NULL)
    {
        FTESTLOG("parity: SDL_ConvertSurfaceFormat failed: %s", SDL_GetError());
        return false;
    }
    TbBool ok = (IMG_SavePNG(rgb, path) == 0);
    SDL_FreeSurface(rgb);
    return ok;
}

static TbBool tick_is_a_shot(GameTurn tick)
{
    for (int i = 0; i < PARITY_SHOT_COUNT; i++)
        if (PARITY_SHOT_TICKS[i] == tick)
            return true;
    return false;
}

// Write the metadata sidecar that lets keeper-rx reproduce this exact shot (schema:
// keeper-rx/docs/design/parity-snapshot-harness.md). Every value is read live from the engine — the
// camera especially, since its zoom drifts run to run and must be recorded, not assumed.
static void parity_write_metadata(const char* path, LevelNumber map, GameTurn tick,
                                  const struct Camera* cam, int width, int height)
{
    FILE* f = fopen(path, "w");
    if (f == NULL)
    {
        FTESTLOG("parity: failed to open '%s'", path);
        return;
    }
    // rotate_mode 0 = classic wibble, anything else (1 straight / 2 front) is captured straight.
    const char* view_mode = (settings.video_rotate_mode == 0) ? "wibble" : "straight";
    double focus_x = (double)cam->mappos.x.val / PARITY_COORDS_PER_SUBTILE;
    double focus_y = (double)cam->mappos.y.val / PARITY_COORDS_PER_SUBTILE;
    fprintf(f,
        "{\n"
        "  \"engine\": \"keeperfx\",\n"
        "  \"version\": \"%s\",\n"
        "  \"scene\": \"%s\",\n"
        "  \"map\": %ld,\n"
        "  \"gameTurn\": %ld,\n"
        "  \"resolution\": [%d, %d],\n"
        "  \"camera\": { \"focusSubtile\": [%.3f, %.3f], \"zoom\": %d, \"rotation\": [%d, %d, %d] },\n"
        "  \"viewMode\": \"%s\",\n"
        "  \"flicker\": false\n"
        "}\n",
        VER_STRING, parity_scene(), (long)map, (long)tick, width, height,
        focus_x, focus_y, (int)cam->zoom,
        (int)cam->rotation_angle_x, (int)cam->rotation_angle_y, (int)cam->rotation_angle_z,
        view_mode);
    fclose(f);
    FTESTLOG("parity: wrote '%s'", path);
}

TbBool ftest_parity_screenshot_init(void)
{
    // Fire every turn from the start; the action self-terminates after the last shot tick.
    ftest_append_action(ftest_parity_screenshot_action001__capture, 0, &ftest_parity_screenshot__vars);
    return true;
}

FTestActionResult ftest_parity_screenshot_action001__capture(struct FTestActionArgs* const args)
{
    struct ftest_parity_screenshot__variables* const vars = args->data;

    struct Thing* heartng = get_player_soul_container(PLAYER0);
    if (thing_is_invalid(heartng))
    {
        FTEST_FAIL_TEST("parity: PLAYER0 has no dungeon heart (is -campaign classic set?)");
        return FTRs_Go_To_Next_Action;
    }

    if (!vars->setup_done)
    {
        // Freeze the heart light's per-frame flicker so a frozen frame is deterministic (matches
        // keeper-rx's flicker:false / KEEPER_NO_FLICKER). Same bit clear as the numeric oracle.
        struct Light* lgt = &game.lish.lights[heartng->light_id];
        lgt->flags2 &= ~0xFE;
        // Suspend the mouse so live pointer movement can't pan/zoom the camera mid-capture — the shot is
        // driven only by this ftest, not by whatever the physical mouse is doing.
        LbMouseSuspend();
        // Draw the camera we force, not a smoothed copy of it. The isometric view is rendered from a
        // separate "local camera" — get_local_camera() (local_camera.c) returns local_cameras[Iso],
        // whose zoom/position are a filter chasing the real camera over several frames
        // (interpolate_local_cameras, run inside redraw_display). For a single frozen parity shot we want
        // the exact forced pose with no lag, so switch the local-camera subsystem off: get_local_camera()
        // then returns the real player->cameras[Iso] we set below, and interpolation is skipped entirely.
        local_camera_ready = false;
        // The documented default parity view: whole map revealed. The camera pose is forced per shot
        // (below), so it is deterministic regardless of any earlier input.
        ftest_util_reveal_map(PLAYER0);
        // Strip the UI so a parity shot is the world only: dismiss the level-intro briefing and the
        // sub-panels, then also turn off the main panel itself (GMnu_MAIN — the minimap, zoom buttons
        // and tab icons, which turn_off_all_panel_menus deliberately keeps), and hide the GUI flags
        // (matching keeper-rx's HUD-off capture). Messages (incl. the ftest's own "Initializing…"
        // banner) are cleared per-shot below.
        turn_off_all_panel_menus();
        turn_off_menu(GMnu_MAIN);
        game.operation_flags &= ~(GOF_ShowGui | GOF_ShowPanel);
        // Optional scene variant: strip creatures so only the static world remains.
        if (parity_no_creatures())
            do_to_all_things_of_class_and_model(TCls_Creature, -1, parity_delete_thing);
        vars->setup_done = true;
    }

    GameTurn tick = get_gameturn();
    if (tick_is_a_shot(tick))
    {
        struct PlayerInfo* player = get_player(PLAYER0);
        struct Camera* cam = get_player_active_camera(player);

        // Force a deterministic camera every shot (mouse-proof): centre on the heart at the fixed zoom.
        // The render copies player->isometric_view_zoom_level into cam->zoom during view setup
        // (engine_camera.c:423) — i.e. the persisted settings.toml zoom, drifted by the mouse wheel — so
        // set THAT source (not just cam->zoom, which would be overwritten) to pin the actual drawn zoom.
        ftest_util_move_camera_to_thing(heartng, PLAYER0);
        player->isometric_view_zoom_level = PARITY_ZOOM;
        settings.isometric_view_zoom_level = PARITY_ZOOM;
        set_camera_zoom(cam, PARITY_ZOOM);
        // Full-screen the 3D viewport so the world fills the frame and the heart is centred (with the
        // HUD hidden the panel-inset window would otherwise leave the world off-centre with a black bar).
        player->engine_window_x = 0;
        player->engine_window_y = 0;
        player->engine_window_width = MyScreenWidth;
        player->engine_window_height = MyScreenHeight;

        // Clear any on-screen messages (the ftest banner, event popups) so they don't overlay the shot.
        zero_messages();
        // Hide the tile-selection box (the green cursor outline) — it follows the pointer and is UI our
        // renderer never draws, so it would be a spurious diff.
        map_volume_box.visible = 0;
        // Optionally kill the pointer's dynamic cursor light (the warm glow under the mouse). Done per
        // shot, not once in setup, so a mid-run view/possession transition can't turn it back on before
        // the grab (see the light_turn_light_on calls in player_instances.c).
        if (parity_no_cursor_light())
            light_turn_light_off(player->cursor_light_idx);
        // The ftest fires in gameplay_loop_logic, before gameplay_loop_draw, so the on-screen frame is
        // still the previous turn's. Force a redraw so the captured PNG is exactly this turn's state.
        keeper_screen_redraw();

        LevelNumber map = get_loaded_level_number();
        int width = (lbDrawSurface != NULL) ? lbDrawSurface->w : 0;
        int height = (lbDrawSurface != NULL) ? lbDrawSurface->h : 0;

        // Files are named by tick only; the scene folder (and the json's "scene"/"map") identify the rest.
        char base[256];
        snprintf(base, sizeof(base), "%s/t%ld", parity_out_dir(), (long)tick);
        char png_path[300];
        char json_path[300];
        snprintf(png_path, sizeof(png_path), "%s.png", base);
        snprintf(json_path, sizeof(json_path), "%s.json", base);

        if (parity_save_png_opaque(png_path))
            FTESTLOG("parity: shot tick %ld -> '%s'", (long)tick, png_path);
        else
            FTESTLOG("parity: FAILED screenshot at tick %ld", (long)tick);

        parity_write_metadata(json_path, map, tick, cam, width, height);
    }

    if (tick < PARITY_LAST_TICK)
        return FTRs_Repeat_Current_Action;

    FTESTLOG("parity: capture complete at tick %ld", (long)tick);
    return FTRs_Go_To_Next_Action; // completes the test -> game auto-exits
}

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
