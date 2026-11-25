#pragma once
#include <glad/glad.h>
#include <iosfwd>
#include <vector>

// Gat historically wrote display code values. Jagae writes linear radiance.
enum class OutputMode { LegacyDisplay, LinearScene };
enum class ToneMode { None = 0, Neutral = 1 };
struct PostSettings
{
    ToneMode tone = ToneMode::None;
    float bloomIntensity = 0.0f;
    float threshold = 0.9f; // gamma-space volume parameter
    float scatter = 0.7f;
    float bloomClamp = 65472.0f;
    int maxIterations = 6;
    float vignetteIntensity = 0.0f;
    float vignetteSmoothness = 0.2f;
    static PostSettings inspectionProfile();
};

// RGBA8 contains explicitly sRGB-encoded display values. It is NOT GL_SRGB8:
// ImGui samples code values unchanged; GL_FRAMEBUFFER_SRGB must stay disabled.
class PostProcess
{
public:
    PostProcess();
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;
    void render(GLuint linearScene, int width, int height, OutputMode mode,
                const PostSettings& settings);
    GLuint displayTexture() const { return display_.texture; }
    void bindDisplayForRead() const;
    bool selfTest(std::ostream& output);
private:
    struct Target { GLuint framebuffer = 0, texture = 0; int width = 0, height = 0; };
    static void resize(Target& target, int width, int height, GLenum format);
    static void destroy(Target& target);
    void pass(Target& target, GLuint source, GLuint low, int stage, const PostSettings& settings);
    Target display_;
    std::vector<Target> down_, up_;
    GLuint program_ = 0, vao_ = 0;
};
