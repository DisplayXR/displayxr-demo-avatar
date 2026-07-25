// Copyright 2026, Leia Inc / DisplayXR
// SPDX-License-Identifier: Apache-2.0
//
// Dynamic-recenter pin control (shared, header-only).
//
// Two orthogonal concepts:
//   1) INITIAL centering — the display rig is framed on the animation-swept
//      AABB at load (model_renderer getRobustSceneBounds). Universal; not here.
//   2) DYNAMIC recentering — per frame, the rig position tracks the smoothed
//      skeleton centroid (model_renderer getAnimatedAnchor). THIS is what the
//      pins control.
//
// The pin logic is universal:
//
//     pos[axis] = (pin[axis] ? anchor[axis] : fitCentre[axis]) + userOffset[axis]
//
// where userOffset is a PER-APP, INDEPENDENT keyboard pan/dolly that adds on top
// (avatar: A/D->X, W/S->Z; modelviewer: none). Each app owns its offset; the
// pins here decide only anchor-vs-fitCentre per axis.
//
// Per-app DEFAULT pins reproduce each demo's historical behaviour exactly:
//   - modelviewer: X Y Z  (hard-pin all three; no user offset)
//   - avatar:      X Y .   (pin X+Y, leave Z as the free W/S dolly)
// so enabling this is a no-op on the reference (Windows) platform and brings
// Linux/macOS to parity.
//
// UI: press P to arm, then X / Y / Z toggles that axis' pin; any other key (or a
// timeout the caller enforces) disarms. DXR_RECENTER_PIN=XYZ|XY|Z|- sets the
// initial pins headless (overrides the app default) — the env path is how the
// modes are exercised on hardware without a keyboard in the loop.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace dxr {

struct RecenterPins {
    bool x = false;
    bool y = false;
    bool z = false;
};

class RecenterControl {
public:
    // Set the app default, then let DXR_RECENTER_PIN override it if present.
    void init(bool defX, bool defY, bool defZ) {
        RecenterPins p{defX, defY, defZ};
        if (const char* e = std::getenv("DXR_RECENTER_PIN")) {
            p.x = p.y = p.z = false;  // explicit env replaces the default set
            for (const char* c = e; *c != '\0'; ++c) {
                if (*c == 'x' || *c == 'X') p.x = true;
                else if (*c == 'y' || *c == 'Y') p.y = true;
                else if (*c == 'z' || *c == 'Z') p.z = true;
                // '-', 'none', 'off', digits, spaces -> no axis; leaves all off
            }
        }
        pins_.store(pack(p));
        armed_.store(false);
    }

    RecenterPins pins() const { return unpack(pins_.load()); }
    bool armed() const { return armed_.load(); }

    // Feed one key character (upper- or lower-case). Returns true if consumed.
    //   - not armed: 'P' arms and consumes; anything else is ignored (not ours).
    //   - armed:     'X'/'Y'/'Z' toggle that pin and disarm (consumed); any other
    //                key disarms WITHOUT consuming, so the caller still handles it.
    bool onKey(char c) {
        const char u = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        if (!armed_.load()) {
            if (u == 'P') { armed_.store(true); return true; }
            return false;
        }
        RecenterPins p = unpack(pins_.load());
        switch (u) {
            case 'X': p.x = !p.x; break;
            case 'Y': p.y = !p.y; break;
            case 'Z': p.z = !p.z; break;
            default:  armed_.store(false); return false;  // disarm, let caller keep the key
        }
        pins_.store(pack(p));
        armed_.store(false);
        return true;
    }

    // Caller-driven disarm (e.g. a timeout since the last P).
    void disarm() { armed_.store(false); }

    // Compact ASCII HUD label, e.g. "PIN XYZ", "PIN XY-", "PIN --Z", or when
    // armed "PIN ?" prompting for the axis key.
    void hudLabel(char* buf, std::size_t n) const {
        if (armed_.load()) { std::snprintf(buf, n, "PIN press X/Y/Z"); return; }
        const RecenterPins p = unpack(pins_.load());
        std::snprintf(buf, n, "PIN %c%c%c",
                      p.x ? 'X' : '-', p.y ? 'Y' : '-', p.z ? 'Z' : '-');
    }

private:
    static int pack(RecenterPins p) { return (p.x ? 1 : 0) | (p.y ? 2 : 0) | (p.z ? 4 : 0); }
    static RecenterPins unpack(int v) { return RecenterPins{(v & 1) != 0, (v & 2) != 0, (v & 4) != 0}; }

    std::atomic<int> pins_{0};
    std::atomic<bool> armed_{false};
};

}  // namespace dxr
