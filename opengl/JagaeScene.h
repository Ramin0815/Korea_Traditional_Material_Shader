#pragma once

#include "SceneSwitcher.h"
#include "DirectionalLight.h"

#include <glm/glm.hpp>
#include <memory>
#include <iosfwd>

// A self-contained rendering pipeline for the revised Modify_Jagae material.
// Its mesh, shader program, material parameters and draw path are intentionally
// isolated from the Gat renderer.
class JagaeScene
{
public:
    JagaeScene();
    ~JagaeScene();
    JagaeScene(const JagaeScene&) = delete;
    JagaeScene& operator=(const JagaeScene&) = delete;

    void draw(const glm::mat4& viewProjection, const glm::vec3& cameraWorld,
              float seconds, ModelSelection model, const DirectionalLight& light,
              JagaeBaseColor baseColor = JagaeBaseColor::Black,
              bool thicknessBaseline = false, float nacreMixTest = -1.0f,
              bool clearCoatEnabled = true, bool detailBaseline = false, bool opticsBaseline = false,
              bool continuousBaseline = false, bool waveBaseline = false,
              JagaeLightMode lightMode = JagaeLightMode::Directional, float lightGain = 1.0f,
              bool smoothBaseline = false, bool patternEnabled = false) const;
    bool lightUniformsMatch(const DirectionalLight& light, JagaeLightMode mode=JagaeLightMode::Directional,
                            float gain=1.0f) const;
    bool environmentSelfTest(std::ostream& output) const;
    bool setEnvironment(EnvironmentPreset preset, std::string& error);
    EnvironmentPreset environmentPreset() const;
    bool baseColorSelfTest(std::ostream& output) const;
    bool thicknessBenchmark(std::ostream& output, bool optics = false, bool continuous = false,
                            bool area = false) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
