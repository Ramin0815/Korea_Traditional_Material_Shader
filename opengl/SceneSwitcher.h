#pragma once

#include <glad/glad.h>
#include "DirectionalLight.h"
#include "JagaeLight.h"
#include "EnvironmentLighting.h"
#include <string>

enum class SceneSelection
{
    Gat,
    Jagae
};

enum class JagaeBaseColor
{
    Black,
    White,
    // Compatibility alias for existing nacre diagnostics; the UI exposes White.
    Nacre = White
};

constexpr float JagaeStructuralMix = 0.55f;
constexpr float JagaeNacreStructuralMix = JagaeStructuralMix;
constexpr float JagaePatternLightBackground = 0.80f;
constexpr float JagaePatternDarkBackground = 0.025f;

enum class ModelSelection
{
    Sphere,
    Square,
    Cylinder
};

struct RenderViewport
{
    int width = 1;
    int height = 1;
    bool hovered = false;
    bool resetView = false;
};

// Fixed three-column ImGui workspace used by the material debugger. The
// renderer remains independent: this class only edits selections and displays
// the supplied OpenGL render-target texture.
class SceneSwitcher
{
public:
    SceneSwitcher() = default;

    RenderViewport draw(int framebufferWidth, int framebufferHeight,
                        GLuint renderTexture, SceneSelection& material,
                        ModelSelection& model, DirectionalLight& jagaeLight,
                        JagaeBaseColor& baseColor, bool& clearCoatEnabled,
                        EnvironmentPreset& environment, const std::string& environmentError,
                        float nacreStructuralMix = JagaeNacreStructuralMix,
                        JagaeLightMode* lightMode = nullptr,
                        bool* patternEnabled = nullptr) const;
};
