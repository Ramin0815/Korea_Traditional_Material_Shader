#pragma once
#include "DirectionalLight.h"
#include <array>
#include <cmath>

enum class JagaeLightMode { Directional, Area };

// Finite, one-sided, uniform-radiance rectangle in world space. The existing
// azimuth/elevation aim its center at the model origin, never at the camera.
struct JagaeRectangleLight {
    glm::vec3 center, normal, halfU, halfV;
    std::array<glm::vec3,9> points;
    std::array<float,9> patchAreas;
    explicit JagaeRectangleLight(const DirectionalLight& direction) {
        const glm::vec3 d=direction.direction();
        const float azimuth=direction.azimuthDegrees()*.017453292519943295f;
        center=2.5f*d;normal=-d;
        halfU=1.25f*glm::vec3(std::cos(azimuth),0,-std::sin(azimuth));
        halfV=glm::normalize(glm::cross(d,halfU));
        const float positions[3]={-.7745966692f,0,.7745966692f};
        const float weights[3]={5.0f/9,8.0f/9,5.0f/9};
        for(int y=0;y<3;++y)for(int x=0;x<3;++x) {
            const int i=y*3+x;
            points[i]=center+positions[x]*halfU+positions[y]*halfV;
            patchAreas[i]=1.25f*weights[x]*weights[y];
        }
    }
};
