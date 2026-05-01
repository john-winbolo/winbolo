/*********************************************************
 * BrainTest Shot-Sim panel — ImGui sub-window rendered
 * inside the main map window (uses the main ImGui context).
 *
 * The user picks an Origin + Target in WU, optionally picks
 * a shooter type (TANK / PILL), and presses Run to invoke
 * brainPathfinderSimulateShot. The result tiles get drawn
 * on the main map by braintest_main.c.
 *
 * Endpoints can be set three ways:
 *   - "Pick on map"        — arms the next left-click on the
 *                            main map to fill that endpoint
 *   - "From tank"          — current followed-tank position
 *   - <brain POI buttons>  — registered via the
 *                            braintest_shotsim_poi_register
 *                            Lua binding; greys out when the
 *                            POI's lua_expr returns nil
 *********************************************************/

#ifndef BRAINTEST_SHOTSIMPANEL_H
#define BRAINTEST_SHOTSIMPANEL_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SHOTSIM_PICK_NONE   = 0,
    SHOTSIM_PICK_ORIGIN = 1,
    SHOTSIM_PICK_TARGET = 2,
} ShotSimPick;

/* Host callbacks. All take a userdata pointer the host passed
 * into shotSimPanelRender. */
typedef void (*ShotSimRunFn)(int originWX, int originWY,
                              int targetWX, int targetWY,
                              int shooterType, void *ud);

/* Clear the host-rendered shot result. Called when the user presses
 * the panel's Clear button — the panel resets its own endpoint state
 * inline, this fires so the host can drop its cached tile list too. */
typedef void (*ShotSimClearFn)(void *ud);

/* "Pick from followed tank" handler — fills outWX/outWY with the
 * current tank's WU position. Returns true on success, false if no
 * tank is followed. */
typedef bool (*ShotSimTankPosFn)(int *outWX, int *outWY, void *ud);

/* Poll a POI by registry index. Returns true (and fills out coords)
 * if the POI is currently available. The panel calls this every
 * frame for every registered POI — keep it cheap; the host should
 * cache POI lua_expr results between brain ticks. */
typedef bool (*ShotSimPoiPollFn)(int poiIdx, int *outWX, int *outWY,
                                  void *ud);

/* Render one ImGui frame inside the *currently active* ImGui
 * context. Caller owns Begin/EndFrame; this just emits ImGui
 * windows. No-op if visible == false. */
void shotSimPanelRender(bool visible,
                        ShotSimRunFn runCb,
                        ShotSimClearFn clearCb,
                        ShotSimTankPosFn tankCb,
                        ShotSimPoiPollFn poiCb,
                        void *ud);

/* Visibility toggle (host's 'S' hotkey calls Toggle). */
void shotSimPanelToggle(void);
bool shotSimPanelIsVisible(void);

/* Pick-on-map integration. The host's left-click handler reads
 * GetPick(); if non-NONE, calls SetClickedWU(wx,wy) — the panel
 * stores the coords on the corresponding endpoint and disarms.
 * The host's "is the click consumed" check should be:
 *   if (shotSimPanelGetPick() != SHOTSIM_PICK_NONE) { ... consumed }  */
ShotSimPick shotSimPanelGetPick(void);
void        shotSimPanelSetClickedWU(int wx, int wy);

/* Returns the current shooter type — needed for the host's pill-
 * center snap on origin clicks (BRAIN_SHOT_SHOOTER_TANK / _PILL). */
int  shotSimPanelGetShooterType(void);

/* Read the panel's current endpoint state (WU). Returns true when
 * the endpoint is set (used for drawing the green origin / red
 * target markers on the map even before a Run / between Runs). */
bool shotSimPanelGetOrigin(int *outWX, int *outWY);
bool shotSimPanelGetTarget(int *outWX, int *outWY);

#ifdef __cplusplus
}
#endif

#endif /* BRAINTEST_SHOTSIMPANEL_H */
