#include "DirectionalLightController.h"
#include <algorithm>

void DirectionalLightController::update(DirectionalLight& light, const Keys& keys,
    bool focused, bool control, bool shift, float deltaSeconds)
{
    const bool physicalDown[4] = {keys.left, keys.right, keys.up, keys.down};
    if (!focused) {
        for (float& held : heldSeconds_) held = 0.0f;
        // Require a post-reentry release even if GLFW reports a synthetic key
        // release while the app/window is inactive.
        for (int i = 0; i < 4; ++i) { blockedUntilRelease_[i] = true; keyWasDown_[i] = physicalDown[i]; }
        resetBlockedUntilRelease_ = true;
        resetWasDown_ = keys.reset;
        return;
    }
    if (!keys.reset) resetBlockedUntilRelease_ = false;
    if (keys.reset && !resetBlockedUntilRelease_ && !resetWasDown_) light.reset();
    resetWasDown_ = keys.reset;
    for (int i = 0; i < 4; ++i)
        if (!physicalDown[i]) blockedUntilRelease_[i] = false;
    const bool available[4] = {physicalDown[0] && !blockedUntilRelease_[0],
                               physicalDown[1] && !blockedUntilRelease_[1],
                               physicalDown[2] && !blockedUntilRelease_[2],
                               physicalDown[3] && !blockedUntilRelease_[3]};
    const bool down[4] = {available[0] && !physicalDown[1], available[1] && !physicalDown[0],
                          available[2] && !physicalDown[3], available[3] && !physicalDown[2]};
    const float step = control ? 0.1f : (shift ? 5.0f : 1.0f);
    const float rate = control ? 6.0f : (shift ? 90.0f : 30.0f);
    float azimuth = 0.0f, elevation = 0.0f;
    const float dt = std::clamp(deltaSeconds, 0.0f, 0.1f);
    for (int i = 0; i < 4; ++i) {
        if (!down[i]) { heldSeconds_[i] = 0.0f; continue; }
        const float before = heldSeconds_[i];
        heldSeconds_[i] += dt;
        float amount = available[i] && !keyWasDown_[i] ? step : 0.0f;
        const float delay = 0.25f;
        const float first = std::max(before, delay), last = std::max(heldSeconds_[i], delay);
        if (last > first) amount += (last - first) * rate;
        if (i == 0) azimuth -= amount;
        if (i == 1) azimuth += amount;
        if (i == 2) elevation += amount;
        if (i == 3) elevation -= amount;
    }
    for (int i = 0; i < 4; ++i) keyWasDown_[i] = physicalDown[i];
    light.rotate(azimuth, elevation);
}
