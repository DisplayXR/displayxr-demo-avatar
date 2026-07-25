// Copyright 2026, The DisplayXR Project and its contributors
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
// UI: press P to arm, then X / Y / Z toggles that axis' pin; any other key
// disarms, and so does kArmSeconds elapsing with no axis key (self-enforced —
// the caller does not need to tick anything). DXR_RECENTER_PIN=XYZ|XY|Z|- sets
// the initial pins headless (overrides the app default) — the env path is how
// the modes are exercised on hardware without a keyboard in the loop.
//
// Every state change is worth confirming on screen: pair this with
// dxr::ToastState (displayxr-common toast.h) and post hudLabel() on each
// accepted key, so the user sees the result whether or not the HUD is up.

#pragma once

#include <atomic>
#include <chrono>
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
    // How long P stays armed waiting for an axis key. Long enough to read the
    // prompt, short enough that a stray P can't silently swallow the next X.
    static constexpr float kArmSeconds = 3.0f;

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

    // Armed AND not yet timed out. Self-expiring, so a caller that only ever
    // reads this (e.g. the HUD/toast label) still sees the arm lapse.
    bool armed() const {
        if (!armed_.load()) return false;
        if (nowNanos() >= armExpiryNanos_.load()) { armed_.store(false); return false; }
        return true;
    }

    // Feed one key character (upper- or lower-case). Returns true if consumed.
    //   - not armed: 'P' arms and consumes; anything else is ignored (not ours).
    //   - armed:     'X'/'Y'/'Z' toggle that pin and disarm (consumed); any other
    //                key disarms WITHOUT consuming, so the caller still handles it.
    // An arm older than kArmSeconds has already lapsed, so the key is treated as
    // if unarmed — a stale P never steals a later X/Y/Z.
    bool onKey(char c) {
        const char u = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
        if (!armed()) {
            if (u == 'P') {
                armExpiryNanos_.store(nowNanos() +
                    static_cast<long long>(kArmSeconds * 1e9));
                armed_.store(true);
                return true;
            }
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
        if (armed()) { std::snprintf(buf, n, "PIN press X/Y/Z"); return; }
        const RecenterPins p = unpack(pins_.load());
        std::snprintf(buf, n, "PIN %c%c%c",
                      p.x ? 'X' : '-', p.y ? 'Y' : '-', p.z ? 'Z' : '-');
    }

private:
    static int pack(RecenterPins p) { return (p.x ? 1 : 0) | (p.y ? 2 : 0) | (p.z ? 4 : 0); }
    static RecenterPins unpack(int v) { return RecenterPins{(v & 1) != 0, (v & 2) != 0, (v & 4) != 0}; }

    static long long nowNanos() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    std::atomic<int> pins_{0};
    // `mutable`: armed() is a read-only query that also retires a lapsed arm.
    mutable std::atomic<bool> armed_{false};
    std::atomic<long long> armExpiryNanos_{0};
};

}  // namespace dxr
