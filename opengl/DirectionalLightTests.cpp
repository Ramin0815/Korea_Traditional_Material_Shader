#include "DirectionalLightTests.h"
#include "DirectionalLight.h"
#include "DirectionalLightController.h"
#include <cmath>
#include <iostream>

namespace { bool near(float a, float b, float e = 0.02f) { return std::abs(a-b) <= e; } }

int runDirectionalLightSelfTests()
{
    DirectionalLight light;
    if (!near(glm::length(light.direction()), 1.0f, 0.0001f) ||
        !near(light.direction().x, -0.24361293f, 0.00001f) ||
        !near(light.direction().y, 0.79185680f, 0.00001f) ||
        !near(light.direction().z, -0.56001389f, 0.00001f)) return 1;
    light.setAzimuthDegrees(-10.0f); light.setElevationDegrees(100.0f);
    if (!near(light.azimuthDegrees(), 350.0f) || !near(light.elevationDegrees(), 89.0f)) return 2;
    if (light.radiance() != glm::vec3(1.0f)) return 3;
    light.setAzimuthDegrees(12.0f); light.setElevationDegrees(-30.0f);
    light.reset();
    if (!near(light.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees) ||
        !near(light.elevationDegrees(), DirectionalLight::DefaultElevationDegrees)) return 5;

    DirectionalLight zeroDelta;
    DirectionalLightController zeroDeltaController;
    DirectionalLightController::Keys keys; keys.right = true;
    zeroDeltaController.update(zeroDelta, keys, true, false, false, 0.0f);
    zeroDeltaController.update(zeroDelta, keys, true, false, false, 0.1f);
    if (!near(zeroDelta.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees + 1.0f)) return 6;

    DirectionalLight normalA, normalB;
    DirectionalLightController normalControllerA, normalControllerB;
    normalControllerA.update(normalA, keys, true, false, false, 0.0f);
    normalControllerB.update(normalB, keys, true, false, false, 0.0f);
    for (int i=0; i<10; ++i) normalControllerA.update(normalA, keys, true, false, false, 0.1f);
    for (int i=0; i<100; ++i) normalControllerB.update(normalB, keys, true, false, false, 0.01f);
    if (!near(normalA.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees + 23.5f) ||
        !near(normalA.azimuthDegrees(), normalB.azimuthDegrees(), 0.03f)) return 7;

    DirectionalLight ctrlLight, shiftLight;
    DirectionalLightController ctrlController, shiftController;
    ctrlController.update(ctrlLight, keys, true, true, false, 0.0f);
    shiftController.update(shiftLight, keys, true, false, true, 0.0f);
    for (int i=0; i<5; ++i) {
        ctrlController.update(ctrlLight, keys, true, true, false, 0.1f);
        shiftController.update(shiftLight, keys, true, false, true, 0.1f);
    }
    if (!near(ctrlLight.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees + 1.6f) ||
        !near(shiftLight.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees + 27.5f)) return 8;

    DirectionalLight gated;
    DirectionalLightController gatedController;
    gatedController.update(gated, keys, false, false, false, 0.1f); // key held off viewport
    gatedController.update(gated, keys, true, false, false, 0.1f);  // focus/material re-entry
    for (int i=0; i<10; ++i) gatedController.update(gated, keys, true, false, false, 0.1f);
    if (!near(gated.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees)) return 9;
    keys.right = false; gatedController.update(gated, keys, true, false, false, 0.0f);
    keys.right = true; gatedController.update(gated, keys, true, false, false, 0.0f);
    if (!near(gated.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees + 1.0f)) return 10;
    gatedController.update(gated, keys, false, false, false, 0.1f); // block held right on re-entry
    gatedController.update(gated, keys, true, false, false, 0.1f);
    keys.left = true; gatedController.update(gated, keys, true, false, false, 0.1f);
    const float oppositeStart = gated.azimuthDegrees();
    for (int i=0; i<5; ++i) gatedController.update(gated, keys, true, false, false, 0.1f);
    if (!near(gated.azimuthDegrees(), oppositeStart)) return 11;
    keys.right = false; gatedController.update(gated, keys, true, false, false, 0.0f);
    const float afterOppositeRelease = gated.azimuthDegrees();
    for (int i=0; i<5; ++i) gatedController.update(gated, keys, true, false, false, 0.1f);
    if (!near(gated.azimuthDegrees(), afterOppositeRelease - 7.5f, 0.03f)) return 14;

    keys.right = false; keys.left = false; keys.reset = true;
    gated.setAzimuthDegrees(200.0f); gated.setElevationDegrees(-40.0f);
    gatedController.update(gated, keys, false, false, false, 0.1f);
    gatedController.update(gated, keys, true, false, false, 0.1f); // R still held; must not reset
    for (int i=0; i<10; ++i) gatedController.update(gated, keys, true, false, false, 0.1f);
    if (!near(gated.azimuthDegrees(), 200.0f)) return 12;
    keys.reset = false; gatedController.update(gated, keys, true, false, false, 0.0f);
    keys.reset = true; gatedController.update(gated, keys, true, false, false, 0.0f);
    if (!near(gated.azimuthDegrees(), DirectionalLight::DefaultAzimuthDegrees) ||
        !near(gated.elevationDegrees(), DirectionalLight::DefaultElevationDegrees)) return 13;

    std::cout << "Directional light self-tests passed.\n";
    return 0;
}
