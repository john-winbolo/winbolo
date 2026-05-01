/*********************************************************
 * braintest_shotsimpanel.cpp — ImGui panel UI for the shot
 * simulator. Renders inside the currently-active ImGui
 * context (the main window's, set up by braintest_main.c).
 *********************************************************/

#include "imgui.h"
#include <cstdio>
#include <cstring>

extern "C" {
#include "braintest_shotsimpanel.h"
#include "braintest_shotsim_poi_registry.h"
}

#define SHOOTER_TANK 0   /* mirrors BRAIN_SHOT_SHOOTER_TANK without the include */
#define SHOOTER_PILL 1

/* Persistent panel state. Lives across frames (BrainTest never
 * destroys the panel), defaults are zero-init friendly. */
struct PanelState {
    bool        visible      = false;
    int         originWX     = 0;
    int         originWY     = 0;
    int         targetWX     = 0;
    int         targetWY     = 0;
    bool        originSet    = false;
    bool        targetSet    = false;
    int         shooterType  = SHOOTER_TANK;
    ShotSimPick pickArmed    = SHOTSIM_PICK_NONE;
    /* Auto-run dedupe: re-run only when one of these inputs has
     * changed since the last run, so the panel doesn't fire the
     * sim every frame at 50Hz. */
    bool        lastRunValid = false;
    int         lastRunOWX   = 0;
    int         lastRunOWY   = 0;
    int         lastRunTWX   = 0;
    int         lastRunTWY   = 0;
    int         lastRunShoot = SHOOTER_TANK;
};
static PanelState s;

static void coordCell(const char *label, bool isSet, int wx, int wy) {
    /* Single-line "label: (wx, wy)" with greyed (unset) state. The
     * panel never lets the user type — every value comes from a
     * pick / button click — so we render coords as text, not Input. */
    if (isSet) {
        ImGui::Text("%s: (%d, %d)  tile (%d, %d)",
                    label, wx, wy, wx >> 8, wy >> 8);
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.6f, 1.0f),
                           "%s: (unset)", label);
    }
}

static void endpointPickRow(const char *epLabel, bool isOrigin,
                            ShotSimTankPosFn tankCb,
                            ShotSimPoiPollFn poiCb, void *ud) {
    int *destWX     = isOrigin ? &s.originWX  : &s.targetWX;
    int *destWY     = isOrigin ? &s.originWY  : &s.targetWY;
    bool *destSet   = isOrigin ? &s.originSet : &s.targetSet;
    ShotSimPick myPick = isOrigin ? SHOTSIM_PICK_ORIGIN : SHOTSIM_PICK_TARGET;

    coordCell(epLabel, *destSet, *destWX, *destWY);

    /* "Pick on map" toggle. Highlighted while armed; clicking again
     * cancels. Only ever one endpoint armed at a time — picking
     * origin while target is armed flips the arm to origin. */
    bool armed = (s.pickArmed == myPick);
    if (armed) ImGui::PushStyleColor(ImGuiCol_Button,
                                     ImVec4(0.85f, 0.55f, 0.15f, 1.0f));
    char btnId[64];
    snprintf(btnId, sizeof(btnId), "%s##pick_%s",
             armed ? "Cancel pick" : "Pick on map",
             isOrigin ? "o" : "t");
    if (ImGui::Button(btnId)) {
        s.pickArmed = armed ? SHOTSIM_PICK_NONE : myPick;
    }
    if (armed) ImGui::PopStyleColor();

    ImGui::SameLine();
    /* Tank-position button. Origin reads "From tank" (the shot
     * comes FROM the tank); target reads "To tank" (the shot is
     * aimed AT the tank). */
    snprintf(btnId, sizeof(btnId), "%s##tank_%s",
             isOrigin ? "From tank" : "To tank",
             isOrigin ? "o" : "t");
    bool tankAvail = (tankCb != NULL);
    if (!tankAvail) ImGui::BeginDisabled();
    if (ImGui::Button(btnId) && tankCb) {
        int wx = 0, wy = 0;
        if (tankCb(&wx, &wy, ud)) {
            *destWX  = wx;
            *destWY  = wy;
            *destSet = true;
        }
    }
    if (!tankAvail) ImGui::EndDisabled();

    /* Per-POI buttons — one row per registered POI. Disabled while
     * the POI is currently unavailable (poiCb returns false). */
    int npoi = shotSimPoiCount();
    for (int i = 0; i < npoi; i++) {
        const ShotSimPoiEntry *e = shotSimPoiGet(i);
        if (!e) continue;
        int wx = 0, wy = 0;
        bool ok = poiCb && poiCb(i, &wx, &wy, ud);
        snprintf(btnId, sizeof(btnId), "%s##poi_%s_%d",
                 e->name, isOrigin ? "o" : "t", i);
        if (!ok) ImGui::BeginDisabled();
        if (ImGui::Button(btnId) && ok) {
            *destWX  = wx;
            *destWY  = wy;
            *destSet = true;
        }
        if (!ok) ImGui::EndDisabled();
    }
}

void shotSimPanelRender(bool visible, ShotSimRunFn runCb,
                        ShotSimClearFn clearCb,
                        ShotSimTankPosFn tankCb,
                        ShotSimPoiPollFn poiCb, void *ud) {
    if (!visible || !s.visible) {
        /* Honor either the host hint or the persistent toggle being
         * off. We track our own visible bool so the user can close
         * the window via the [X] without the host knowing. */
        s.visible = s.visible && visible;
        return;
    }
    bool open = true;
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Shot simulator", &open,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        if (!open) s.visible = false;
        return;
    }

    /* Origin section */
    ImGui::SeparatorText("Origin");
    endpointPickRow("Origin", true, tankCb, poiCb, ud);

    /* Target section */
    ImGui::Spacing();
    ImGui::SeparatorText("Target");
    endpointPickRow("Target", false, tankCb, poiCb, ud);

    /* Shooter type radio. Affects pill-center snap on origin click
     * + which physics brainPathfinderSimulateShot uses. */
    ImGui::Spacing();
    ImGui::SeparatorText("Shooter");
    ImGui::RadioButton("Tank##sht", &s.shooterType, SHOOTER_TANK);
    ImGui::SameLine();
    ImGui::RadioButton("Pill##shp", &s.shooterType, SHOOTER_PILL);

    /* Auto-run: as soon as both endpoints are set the sim fires.
     * Dedupes on (origin, target, shooter) so dragging the panel or
     * just re-rendering doesn't re-invoke the pathfinder every
     * frame; only param changes do. Clearing wipes lastRunValid so
     * setting the endpoints to the same coords as before still
     * fires once after a Clear. */
    if (s.originSet && s.targetSet && runCb) {
        bool changed = !s.lastRunValid
                    || s.lastRunOWX   != s.originWX
                    || s.lastRunOWY   != s.originWY
                    || s.lastRunTWX   != s.targetWX
                    || s.lastRunTWY   != s.targetWY
                    || s.lastRunShoot != s.shooterType;
        if (changed) {
            runCb(s.originWX, s.originWY,
                  s.targetWX, s.targetWY,
                  s.shooterType, ud);
            s.lastRunValid = true;
            s.lastRunOWX   = s.originWX;
            s.lastRunOWY   = s.originWY;
            s.lastRunTWX   = s.targetWX;
            s.lastRunTWY   = s.targetWY;
            s.lastRunShoot = s.shooterType;
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Clear", ImVec2(80, 0))) {
        s.originSet    = false;
        s.targetSet    = false;
        s.pickArmed    = SHOTSIM_PICK_NONE;
        s.lastRunValid = false;
        if (clearCb) clearCb(ud);
    }

    if (s.pickArmed != SHOTSIM_PICK_NONE) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f),
                           "Click on the map to set %s.",
                           s.pickArmed == SHOTSIM_PICK_ORIGIN ? "origin" : "target");
    }

    ImGui::End();
    if (!open) {
        s.visible   = false;
        s.pickArmed = SHOTSIM_PICK_NONE;
    }
}

void shotSimPanelToggle(void) {
    s.visible = !s.visible;
    if (!s.visible) s.pickArmed = SHOTSIM_PICK_NONE;
}

bool shotSimPanelIsVisible(void) { return s.visible; }

ShotSimPick shotSimPanelGetPick(void) { return s.pickArmed; }

void shotSimPanelSetClickedWU(int wx, int wy) {
    if (s.pickArmed == SHOTSIM_PICK_ORIGIN) {
        s.originWX  = wx;
        s.originWY  = wy;
        s.originSet = true;
    } else if (s.pickArmed == SHOTSIM_PICK_TARGET) {
        s.targetWX  = wx;
        s.targetWY  = wy;
        s.targetSet = true;
    }
    s.pickArmed = SHOTSIM_PICK_NONE;
}

int shotSimPanelGetShooterType(void) { return s.shooterType; }

bool shotSimPanelGetOrigin(int *outWX, int *outWY) {
    if (!s.originSet) return false;
    if (outWX) *outWX = s.originWX;
    if (outWY) *outWY = s.originWY;
    return true;
}

bool shotSimPanelGetTarget(int *outWX, int *outWY) {
    if (!s.targetSet) return false;
    if (outWX) *outWX = s.targetWX;
    if (outWY) *outWY = s.targetWY;
    return true;
}
