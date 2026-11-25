#pragma once
#include "DirectionalLight.h"

// Platform-independent viewport keyboard repeater. Call update once per frame.
class DirectionalLightController
{
public:
    struct Keys { bool left=false, right=false, up=false, down=false, reset=false; };
    void update(DirectionalLight& light, const Keys& keys, bool focused,
                bool control, bool shift, float deltaSeconds);
private:
    float heldSeconds_[4]{};
    bool keyWasDown_[4]{};
    bool blockedUntilRelease_[4]{};
    bool resetWasDown_ = false;
    bool resetBlockedUntilRelease_ = false;
};
