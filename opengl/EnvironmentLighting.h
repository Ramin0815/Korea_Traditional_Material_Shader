#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <array>
#include <iosfwd>
#include <vector>

enum class EnvironmentPreset { Studio, UnityReference };
constexpr float BlackStudioFillRadiance = .06f;
constexpr float WhiteStudioFillRadiance = .8f;

// Fixed world-space HDR illumination. SH and reflection mips share one source.
class EnvironmentLighting
{
public:
    explicit EnvironmentLighting(EnvironmentPreset preset = EnvironmentPreset::Studio,
                                 float studioFill = BlackStudioFillRadiance);
    ~EnvironmentLighting();
    EnvironmentLighting(const EnvironmentLighting&) = delete;
    EnvironmentLighting& operator=(const EnvironmentLighting&) = delete;
    void bind(GLenum unit) const;
    void bindAlbedo(GLenum unit) const;
    const std::array<glm::vec3, 9>& diffuseSH() const { return sh_; }
    float maxMip() const { return 6.0f; }
    bool selfTest(std::ostream& output) const;
private:
    EnvironmentPreset preset_;
    float studioFill_;
    GLuint cube_ = 0;
    GLuint albedo_ = 0;
    std::array<glm::vec3, 9> sh_{};
    std::array<double, 7> mipMeans_{};
};
