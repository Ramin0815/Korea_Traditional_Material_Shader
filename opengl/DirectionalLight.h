#pragma once

#include <glm/glm.hpp>

class DirectionalLight
{
public:
    static constexpr float DefaultAzimuthDegrees = 203.50963f;
    static constexpr float DefaultElevationDegrees = 52.35937f;

    DirectionalLight();

    float azimuthDegrees() const { return azimuthDegrees_; }
    float elevationDegrees() const { return elevationDegrees_; }
    void setAzimuthDegrees(float value);
    void setElevationDegrees(float value);
    void rotate(float azimuthDelta, float elevationDelta);
    glm::vec3 direction() const;
    glm::vec3 radiance() const;
    void reset();

private:
    float azimuthDegrees_ = DefaultAzimuthDegrees;
    float elevationDegrees_ = DefaultElevationDegrees;
};
