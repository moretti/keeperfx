/**
 * @file ftest_ariadne_oracle.h
 * @brief Ariadne navigation oracle for the keeper-rx pathfinding port (keeper-rx ADR-0024, pathfinding.md).
 *
 * Loads a synthetic copyright-free level (mapNNNNN.* authored by keeper-rx) and dumps the navmesh
 * layers for keeper-rx to diff bottom-up. This slice emits D0 — the per-subtile NavColour raster
 * (get_navigation_colour) — as a compact little-endian binary. Later layers (mesh, regions, route,
 * follow) are appended as the keeper-rx build reaches them.
 */

#pragma once

#include "../../globals.h"

#ifdef FUNCTESTING

#ifdef __cplusplus
extern "C" {
#endif

TbBool ftest_ariadne_oracle_init();

#ifdef __cplusplus
}
#endif

#endif // FUNCTESTING
