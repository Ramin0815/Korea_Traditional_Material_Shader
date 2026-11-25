#include "DirectionalLight.h"
#include <algorithm>
#include <cmath>

namespace { constexpr float Pi = 3.14159265358979323846f; }

DirectionalLight::DirectionalLight() = default;
void DirectionalLight::setAzimuthDegrees(float value)
{
    if (!std::isfinite(value)) value = 0.0f;
    azimuthDegrees_ = std::fmod(value, 360.0f);
    if (azimuthDegrees_ < 0.0f) azimuthDegrees_ += 360.0f;
}
void DirectionalLight::setElevationDegrees(float value)
{ elevationDegrees_ = std::isfinite(value) ? std::clamp(value, -89.0f, 89.0f) : 0.0f; }
void DirectionalLight::rotate(float azimuthDelta, float elevationDelta)
{ setAzimuthDegrees(azimuthDegrees_ + azimuthDelta); setElevationDegrees(elevationDegrees_ + elevationDelta); }
glm::vec3 DirectionalLight::direction() const
{
    const float a = azimuthDegrees_ * Pi / 180.0f, e = elevationDegrees_ * Pi / 180.0f;
    const float c = std::cos(e);
    return {c * std::sin(a), std::sin(e), c * std::cos(a)};
}
glm::vec3 DirectionalLight::radiance() const { return glm::vec3(1.0f); }
void DirectionalLight::reset()
{ azimuthDegrees_ = DefaultAzimuthDegrees; elevationDegrees_ = DefaultElevationDegrees; }
