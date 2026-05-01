/*********************************************************
 * braintest_shotsimpanel.cpp — ImGui panel UI for the shot
 * simulator. Renders inside the currently-active ImGui
 * context (the main window's, set up by braintest_main.c).
 *********************************************************/

#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <cmath>

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
    /* One-shot collapse: when an endpoint just got set via any
     * picker (map pick, From/To tank, POI), force the corresponding
     * section closed on the next render. The user can click the
     * disclosure triangle to re-open it any time after that. */
    bool        collapseOriginNext = false;
    bool        collapseTargetNext = false;
    /* "Shoot from tank" mode: target endpoint is synthesized from
     * the tank's current float angle each frame instead of being
     * set explicitly. Auto-run uses simulate_shot_angle for a
     * bit-exact match to the engine's actual flight. */
    bool        targetUseTankAngle = false;
    float       lastTankAngle      = 0.0f;
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

static void endpointPickRow(const char *epLabel, bool isOrigin,
                            ShotSimTankPosFn tankCb,
                            ShotSimTankAngleFn tankAngleCb,
                            ShotSimPoiPollFn poiCb, void *ud) {
    int  *destWX     = isOrigin ? &s.originWX  : &s.targetWX;
    int  *destWY     = isOrigin ? &s.originWY  : &s.targetWY;
    bool *destSet    = isOrigin ? &s.originSet : &s.targetSet;
    bool *collapseFlag = isOrigin ? &s.collapseOriginNext
                                   : &s.collapseTargetNext;
    ShotSimPick myPick = isOrigin ? SHOTSIM_PICK_ORIGIN : SHOTSIM_PICK_TARGET;

    /* Header label = current value (or "unset"). Section starts open
     * by default; we collapse it once on each successful set so the
     * user just sees a one-line summary. Disclosure triangle re-opens.
     * Special "Shoot from tank" mode for the target shows the live
     * float tank angle instead of coords since target is dynamic. */
    char header[128];
    if (!isOrigin && s.targetUseTankAngle) {
        snprintf(header, sizeof(header),
                 "%s: shoot from tank @ %.2f brad###ep_t",
                 epLabel, s.lastTankAngle);
    } else if (*destSet) {
        snprintf(header, sizeof(header),
                 "%s: (%d, %d)  tile (%d, %d)###ep_%s",
                 epLabel, *destWX, *destWY,
                 *destWX >> 8, *destWY >> 8,
                 isOrigin ? "o" : "t");
    } else {
        snprintf(header, sizeof(header), "%s: (unset)###ep_%s",
                 epLabel, isOrigin ? "o" : "t");
    }

    if (*collapseFlag) {
        ImGui::SetNextItemOpen(false, ImGuiCond_Always);
        *collapseFlag = false;
    } else {
        /* Default-open on first use — the FirstUseEver condition
         * means the user's manual collapse via the triangle sticks
         * across frames. */
        ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    }

    if (!ImGui::CollapsingHeader(header)) return;

    /* "Pick on map" toggle. Highlighted while armed; clicking again
     * cancels. Only ever one endpoint armed at a time. */
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
            *destWX        = wx;
            *destWY        = wy;
            *destSet       = true;
            *collapseFlag  = true;
            if (!isOrigin) s.targetUseTankAngle = false;
        }
    }
    if (!tankAvail) ImGui::EndDisabled();

    /* Target-only "Shoot from tank" — synthesizes the target from
     * the tank's current float angle each frame and runs the sim
     * via simulate_shot_angle for a bit-exact engine match. The
     * synthetic target gets refreshed every frame in the render
     * loop below so it tracks the tank as it rotates. */
    if (!isOrigin && tankAngleCb) {
        ImGui::SameLine();
        bool active = s.targetUseTankAngle;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImVec4(0.20f, 0.55f, 0.85f, 1.0f));
        if (ImGui::Button("Shoot from tank##sft")) {
            float a = 0.0f;
            if (tankAngleCb(&a, ud)) {
                s.targetUseTankAngle = true;
                s.targetSet          = true;
                s.collapseTargetNext = true;
            }
        }
        if (active) ImGui::PopStyleColor();
    }

    /* "From list" — brain-registered POIs. Poll every POI once up
     * front so the section is only shown when at least one is
     * currently available; a section with all-disabled rows is just
     * clutter. Each row is a two-line button: name on top,
     * "(wx, wy)  tile (mx, my)" greyed underneath. */
    int npoi = shotSimPoiCount();
    if (npoi > 0) {
        struct Resolved { int wx, wy; bool ok; };
        Resolved cache[SHOTSIM_POI_REG_MAX];
        int liveCount = 0;
        for (int i = 0; i < npoi && i < SHOTSIM_POI_REG_MAX; i++) {
            int wx = 0, wy = 0;
            bool ok = poiCb && poiCb(i, &wx, &wy, ud);
            cache[i].wx = wx;
            cache[i].wy = wy;
            cache[i].ok = ok;
            if (ok) liveCount++;
        }
        if (liveCount > 0) {
            ImGui::Spacing();
            ImGui::TextDisabled("From list");
            for (int i = 0; i < npoi && i < SHOTSIM_POI_REG_MAX; i++) {
                if (!cache[i].ok) continue;
                const ShotSimPoiEntry *e = shotSimPoiGet(i);
                if (!e) continue;
                int wx = cache[i].wx, wy = cache[i].wy;
                char label[160];
                snprintf(label, sizeof(label),
                         "%s\n(%d, %d)  tile (%d, %d)##poi_%s_%d",
                         e->name, wx, wy, wx >> 8, wy >> 8,
                         isOrigin ? "o" : "t", i);
                if (ImGui::Button(label, ImVec2(-FLT_MIN, 0))) {
                    *destWX        = wx;
                    *destWY        = wy;
                    *destSet       = true;
                    *collapseFlag  = true;
                    if (!isOrigin) s.targetUseTankAngle = false;
                }
            }
        }
    }
}

void shotSimPanelRender(bool visible, ShotSimRunFn runCb,
                        ShotSimClearFn clearCb,
                        ShotSimTankPosFn tankCb,
                        ShotSimTankAngleFn tankAngleCb,
                        ShotSimPoiPollFn poiCb, void *ud) {
    if (!visible || !s.visible) {
        /* Honor either the host hint or the persistent toggle being
         * off. We track our own visible bool so the user can close
         * the window via the [X] without the host knowing. */
        s.visible = s.visible && visible;
        return;
    }
    bool open = true;
    /* Initial size only — user can resize, drag, etc. We keep auto-
     * resize off so a tall POI list doesn't make the panel grow off-
     * screen each tick. */
    ImGui::SetNextWindowSize(ImVec2(360, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Shot simulator", &open)) {
        ImGui::End();
        if (!open) s.visible = false;
        return;
    }

    /* Refresh the synthesized "Shoot from tank" target each frame so
     * it tracks the live tank angle. We project the synthetic target
     * far enough to be off-map so the rendered line/X marker on the
     * main map look like a clean direction indicator; the actual
     * trajectory comes from simulate_shot_angle, not from this
     * synthetic endpoint. */
    if (s.targetUseTankAngle && tankAngleCb) {
        float a = 0.0f;
        if (tankAngleCb(&a, ud)) {
            s.lastTankAngle = a;
            /* Bolo: 0 = N, sin → +x, -cos → +y. Project ~16 tiles
             * out (well past max shell range) so the X marker sits
             * along the direction, not on top of the spawn dot. */
            float rad   = a * (3.14159265358979323846f / 128.0f);
            float far_  = 16.0f * 256.0f;
            int   tx    = (int)(s.originWX + sinf(rad) * far_ + 0.5f);
            int   ty    = (int)(s.originWY - cosf(rad) * far_ + 0.5f);
            s.targetWX  = tx;
            s.targetWY  = ty;
            s.targetSet = true;
        } else {
            /* Tank gone — drop the mode rather than render stale. */
            s.targetUseTankAngle = false;
            s.targetSet          = false;
        }
    }

    /* Origin / Target sections — each is a CollapsingHeader whose
     * label IS the current value, so a closed section still shows
     * the user where the endpoint sits. Sections auto-collapse the
     * frame after a value gets set; click the disclosure triangle
     * to re-open and change. */
    endpointPickRow("Origin", true,  tankCb, tankAngleCb, poiCb, ud);
    endpointPickRow("Target", false, tankCb, tankAngleCb, poiCb, ud);

    /* Shooter type radio. Affects pill-center snap on origin click
     * + which physics brainPathfinderSimulateShot uses. */
    ImGui::Spacing();
    ImGui::SeparatorText("Shooter");
    ImGui::RadioButton("Tank##sht", &s.shooterType, SHOOTER_TANK);
    ImGui::SameLine();
    ImGui::RadioButton("Pill##shp", &s.shooterType, SHOOTER_PILL);

    /* Auto-run: as soon as both endpoints are set the sim fires.
     * Dedupes on (origin, target, shooter, useTankAngle, tankAngle)
     * so dragging the panel doesn't re-invoke the pathfinder every
     * frame; only meaningful param changes do. */
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
                  s.shooterType,
                  s.targetUseTankAngle, s.lastTankAngle,
                  ud);
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
        s.originSet          = false;
        s.targetSet          = false;
        s.pickArmed          = SHOTSIM_PICK_NONE;
        s.lastRunValid       = false;
        s.targetUseTankAngle = false;
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
        s.originWX           = wx;
        s.originWY           = wy;
        s.originSet          = true;
        s.collapseOriginNext = true;
    } else if (s.pickArmed == SHOTSIM_PICK_TARGET) {
        s.targetWX           = wx;
        s.targetWY           = wy;
        s.targetSet          = true;
        s.collapseTargetNext = true;
        s.targetUseTankAngle = false;
    }
    s.pickArmed = SHOTSIM_PICK_NONE;
}

int shotSimPanelGetShooterType(void) { return s.shooterType; }

void shotSimPanelClearAll(void) {
    s.originSet          = false;
    s.targetSet          = false;
    s.pickArmed          = SHOTSIM_PICK_NONE;
    s.lastRunValid       = false;
    s.targetUseTankAngle = false;
}

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
