#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/quaternion.hpp>

#include "JagaeScene.h"
#include "NacreTests.h"
#include "DirectionalLightController.h"
#include "DirectionalLightTests.h"
#include "SceneSwitcher.h"
#include "PostProcess.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr float Pi = 3.14159265358979323846f;

struct Vertex
{
    glm::vec3 position;
    glm::vec3 normal;
};

class Mesh
{
public:
    Mesh() = default;
    Mesh(const std::vector<Vertex>& vertices, const std::vector<unsigned int>& indices)
        : indexCount_(static_cast<GLsizei>(indices.size()))
    {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(Vertex)), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)), indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, normal)));
        glBindVertexArray(0);
    }

    ~Mesh() { reset(); }
    Mesh(const Mesh&) = delete;
    Mesh& operator=(const Mesh&) = delete;
    Mesh(Mesh&& other) noexcept { *this = std::move(other); }
    Mesh& operator=(Mesh&& other) noexcept
    {
        if (this != &other)
        {
            reset();
            vao_ = std::exchange(other.vao_, 0);
            vbo_ = std::exchange(other.vbo_, 0);
            ebo_ = std::exchange(other.ebo_, 0);
            indexCount_ = std::exchange(other.indexCount_, 0);
        }
        return *this;
    }

    void draw() const
    {
        glBindVertexArray(vao_);
        glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    }

private:
    void reset()
    {
        if (ebo_) glDeleteBuffers(1, &ebo_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
        vao_ = vbo_ = ebo_ = 0;
        indexCount_ = 0;
    }
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    GLsizei indexCount_ = 0;
};

std::string readTextFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("Cannot open shader: ") + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void captureFramebuffer(int width, int height, const std::string& filename)
{
    // Bounded render evidence, saved before the UI is composited.
    const unsigned int stride=(static_cast<unsigned int>(width)*3+3)&~3u;
    std::vector<unsigned char> pixels(static_cast<size_t>(width)*height*4);
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    std::ofstream output(filename,std::ios::binary);
    if(!output) throw std::runtime_error("Cannot write " + filename);
    const auto integer=[&](unsigned int value,int bytes)
    { for(int b=0;b<bytes;++b) output.put(static_cast<char>((value>>(8*b))&255)); };
    output.put('B'); output.put('M'); integer(54+stride*height,4); integer(0,4); integer(54,4);
    integer(40,4); integer(width,4); integer(height,4); integer(1,2); integer(24,2);
    integer(0,4); integer(stride*height,4); integer(2835,4); integer(2835,4); integer(0,4); integer(0,4);
    for(int y=0;y<height;++y)
    {
        for(int x=0;x<width;++x)
        {
            const size_t i=(static_cast<size_t>(y)*width+x)*4;
            output.put(pixels[i+2]); output.put(pixels[i+1]); output.put(pixels[i]);
        }
        for(unsigned int pad=width*3;pad<stride;++pad) output.put(0);
    }
    std::cout<<"Inspection frame saved: "<<filename<<" ("<<width<<"x"<<height<<")\n";
}

GLuint compileShader(GLenum stage, const char* path)
{
    const std::string source = readTextFile(path);
    const char* sourcePtr = source.c_str();
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &sourcePtr, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    std::cerr << "Shader compilation failed (" << path << "):\n" << log << '\n';
    glDeleteShader(shader);
    throw std::runtime_error("Shader compilation failed.");
}

class Program
{
public:
    Program(const char* vertexPath, const char* fragmentPath)
    {
        GLuint vertex = 0;
        GLuint fragment = 0;
        try
        {
            vertex = compileShader(GL_VERTEX_SHADER, vertexPath);
            fragment = compileShader(GL_FRAGMENT_SHADER, fragmentPath);
            program_ = glCreateProgram();
            if (!program_) throw std::runtime_error("OpenGL could not create a shader program.");
            glAttachShader(program_, vertex);
            glAttachShader(program_, fragment);
            glLinkProgram(program_);
            glDeleteShader(vertex); vertex = 0;
            glDeleteShader(fragment); fragment = 0;

            GLint success = GL_FALSE;
            glGetProgramiv(program_, GL_LINK_STATUS, &success);
            if (success == GL_TRUE) return;
            GLint length = 0;
            glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
            glGetProgramInfoLog(program_, length, nullptr, log.data());
            std::cerr << "Program link failed:\n" << log << '\n';
            throw std::runtime_error("Program link failed.");
        }
        catch (...)
        {
            if (vertex) glDeleteShader(vertex);
            if (fragment) glDeleteShader(fragment);
            if (program_) glDeleteProgram(program_);
            program_ = 0;
            throw;
        }
    }
    ~Program() { if (program_) glDeleteProgram(program_); }
    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;

    void use() const { glUseProgram(program_); }
    void set(const char* name, const glm::mat4& value) const { glUniformMatrix4fv(glGetUniformLocation(program_, name), 1, GL_FALSE, glm::value_ptr(value)); }
    void set(const char* name, const glm::vec3& value) const { glUniform3fv(glGetUniformLocation(program_, name), 1, glm::value_ptr(value)); }
    void set(const char* name, const glm::vec4& value) const { glUniform4fv(glGetUniformLocation(program_, name), 1, glm::value_ptr(value)); }
    void set(const char* name, float value) const { glUniform1f(glGetUniformLocation(program_, name), value); }
private:
    GLuint program_ = 0;
};

Mesh makeCylinder(float radius, float height, int segments = 192)
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    const float halfHeight = height * 0.5f;
    vertices.reserve(static_cast<size_t>(segments + 1) * 6 + 2);
    indices.reserve(static_cast<size_t>(segments) * 12);

    const unsigned int sideStart = static_cast<unsigned int>(vertices.size());
    for (int i = 0; i <= segments; ++i)
    {
        const float angle = Pi * 2.0f * static_cast<float>(i) / static_cast<float>(segments);
        const glm::vec3 normal(std::cos(angle), 0.0f, std::sin(angle));
        vertices.push_back({normal * radius + glm::vec3(0.0f, -halfHeight, 0.0f), normal});
        vertices.push_back({normal * radius + glm::vec3(0.0f, halfHeight, 0.0f), normal});
    }
    for (int i = 0; i < segments; ++i)
    {
        const unsigned int a = sideStart + static_cast<unsigned int>(i * 2);
        // theta then Y is an outward-facing frame: +theta cross +Y equals radial.
        indices.insert(indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }

    const unsigned int topCenter = static_cast<unsigned int>(vertices.size());
    vertices.push_back({glm::vec3(0.0f, halfHeight, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f)});
    const unsigned int topStart = static_cast<unsigned int>(vertices.size());
    for (int i = 0; i <= segments; ++i)
    {
        const float angle = Pi * 2.0f * static_cast<float>(i) / static_cast<float>(segments);
        vertices.push_back({glm::vec3(std::cos(angle) * radius, halfHeight, std::sin(angle) * radius), glm::vec3(0.0f, 1.0f, 0.0f)});
    }
    const unsigned int bottomCenter = static_cast<unsigned int>(vertices.size());
    vertices.push_back({glm::vec3(0.0f, -halfHeight, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)});
    const unsigned int bottomStart = static_cast<unsigned int>(vertices.size());
    for (int i = 0; i <= segments; ++i)
    {
        const float angle = Pi * 2.0f * static_cast<float>(i) / static_cast<float>(segments);
        vertices.push_back({glm::vec3(std::cos(angle) * radius, -halfHeight, std::sin(angle) * radius), glm::vec3(0.0f, -1.0f, 0.0f)});
    }
    for (int i = 0; i < segments; ++i)
    {
        indices.insert(indices.end(), {topCenter, topStart + static_cast<unsigned int>(i + 1), topStart + static_cast<unsigned int>(i)});
        indices.insert(indices.end(), {bottomCenter, bottomStart + static_cast<unsigned int>(i), bottomStart + static_cast<unsigned int>(i + 1)});
    }
    return Mesh(vertices, indices);
}

Mesh makeInspectionSphere(int slices = 160, int stacks = 96)
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    for (int y = 0; y <= stacks; ++y)
    {
        const float v = static_cast<float>(y) / static_cast<float>(stacks);
        const float latitude = Pi * v;
        for (int x = 0; x <= slices; ++x)
        {
            const float longitude = 2.0f * Pi * static_cast<float>(x) / static_cast<float>(slices);
            const glm::vec3 normal(std::sin(latitude) * std::cos(longitude), std::cos(latitude),
                                   std::sin(latitude) * std::sin(longitude));
            vertices.push_back({normal * 0.55f, normal});
        }
    }
    for (int y = 0; y < stacks; ++y)
    {
        for (int x = 0; x < slices; ++x)
        {
            const unsigned int a = static_cast<unsigned int>(y * (slices + 1) + x);
            const unsigned int b = a + 1;
            const unsigned int c = a + static_cast<unsigned int>(slices + 1);
            indices.insert(indices.end(), {a, b, c, b, c + 1, c});
        }
    }
    return Mesh(vertices, indices);
}

Mesh makeInspectionSquare()
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    const float h = 0.48f;
    const auto addFace = [&](const glm::vec3& normal, const glm::vec3& a,
                             const glm::vec3& b, const glm::vec3& c, const glm::vec3& d)
    {
        const unsigned int start = static_cast<unsigned int>(vertices.size());
        vertices.insert(vertices.end(), {{a, normal}, {b, normal}, {c, normal}, {d, normal}});
        indices.insert(indices.end(), {start, start + 1, start + 2, start, start + 2, start + 3});
    };
    addFace({0, 0, 1}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h});
    addFace({0, 0, -1}, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h});
    addFace({1, 0, 0}, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h});
    addFace({-1, 0, 0}, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h});
    addFace({0, 1, 0}, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h});
    addFace({0, -1, 0}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h});
    return Mesh(vertices, indices);
}

// A gat brim is a thin plate, not a short crown cylinder.  Give it an explicit
// radially tessellated top and bottom plus a real outer wall so that the plate
// has physical thickness, stable cap normals, and a silhouette of its own.
Mesh makeBrimDisc(float radius, float thickness, int around = 192, int radialRings = 24)
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    const float halfThickness = thickness * 0.5f;
    const int ringVertexCount = around + 1;
    vertices.reserve(static_cast<size_t>(radialRings + 1) * ringVertexCount * 2
                     + static_cast<size_t>(ringVertexCount) * 2);
    indices.reserve(static_cast<size_t>(radialRings) * around * 12
                    + static_cast<size_t>(around) * 6);

    const auto addFace = [&](float y, const glm::vec3& normal, bool top)
    {
        // Use one center vertex followed by nonzero-radius rings.  Duplicating a
        // zero-radius ring would create one degenerate triangle per sector.
        const unsigned int center = static_cast<unsigned int>(vertices.size());
        vertices.push_back({glm::vec3(0.0f, y, 0.0f), normal});
        const unsigned int ringsStart = static_cast<unsigned int>(vertices.size());
        for (int ring = 1; ring <= radialRings; ++ring)
        {
            const float currentRadius = radius * static_cast<float>(ring)
                                      / static_cast<float>(radialRings);
            for (int segment = 0; segment <= around; ++segment)
            {
                const float angle = Pi * 2.0f * static_cast<float>(segment)
                                  / static_cast<float>(around);
                vertices.push_back({glm::vec3(std::cos(angle) * currentRadius, y,
                                               std::sin(angle) * currentRadius), normal});
            }
        }

        // Center fan.
        for (int segment = 0; segment < around; ++segment)
        {
            const unsigned int current = ringsStart + static_cast<unsigned int>(segment);
            const unsigned int next = current + 1;
            if (top)
                indices.insert(indices.end(), {center, next, current});
            else
                indices.insert(indices.end(), {center, current, next});
        }

        // Annular quads between the remaining radial rings.
        for (int ring = 1; ring < radialRings; ++ring)
        {
            for (int segment = 0; segment < around; ++segment)
            {
                const unsigned int a = ringsStart + static_cast<unsigned int>((ring - 1) * ringVertexCount + segment);
                const unsigned int b = a + 1;
                const unsigned int c = a + static_cast<unsigned int>(ringVertexCount);
                const unsigned int d = c + 1;
                if (top)
                    indices.insert(indices.end(), {a, d, c, a, b, d});
                else
                    indices.insert(indices.end(), {a, c, d, a, d, b});
            }
        }
    };

    addFace(halfThickness, glm::vec3(0.0f, 1.0f, 0.0f), true);
    addFace(-halfThickness, glm::vec3(0.0f, -1.0f, 0.0f), false);

    const unsigned int sideStart = static_cast<unsigned int>(vertices.size());
    for (int segment = 0; segment <= around; ++segment)
    {
        const float angle = Pi * 2.0f * static_cast<float>(segment)
                          / static_cast<float>(around);
        const glm::vec3 normal(std::cos(angle), 0.0f, std::sin(angle));
        vertices.push_back({normal * radius + glm::vec3(0.0f, -halfThickness, 0.0f), normal});
        vertices.push_back({normal * radius + glm::vec3(0.0f, halfThickness, 0.0f), normal});
    }
    for (int segment = 0; segment < around; ++segment)
    {
        const unsigned int a = sideStart + static_cast<unsigned int>(segment * 2);
        indices.insert(indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }

    return Mesh(vertices, indices);
}

Mesh makeRims(float radius, float height, float rimHeight, float rimDepth, int around = 192, int crossSection = 16)
{
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    const auto addBand = [&](float centerY, float halfBandHeight)
    {
        const unsigned int start = static_cast<unsigned int>(vertices.size());
        for (int i = 0; i < around; ++i)
        {
            const float theta = Pi * 2.0f * static_cast<float>(i) / static_cast<float>(around);
            const glm::vec3 radial(std::cos(theta), 0.0f, std::sin(theta));
            for (int j = 0; j < crossSection; ++j)
            {
                const float phi = Pi * 2.0f * static_cast<float>(j) / static_cast<float>(crossSection);
                const float radialBlend = 0.5f * (1.0f + std::cos(phi));
                const float currentRadius = radius + rimDepth * radialBlend;
                const glm::vec3 position = radial * currentRadius + glm::vec3(0.0f, centerY + halfBandHeight * std::sin(phi), 0.0f);
                const glm::vec3 normal = glm::normalize(radial * (halfBandHeight * std::cos(phi)) + glm::vec3(0.0f, rimDepth * 0.5f * std::sin(phi), 0.0f));
                vertices.push_back({position, normal});
            }
        }
        for (int i = 0; i < around; ++i)
        {
            const int nextI = (i + 1) % around;
            for (int j = 0; j < crossSection; ++j)
            {
                const int nextJ = (j + 1) % crossSection;
                const unsigned int a = start + static_cast<unsigned int>(i * crossSection + j);
                const unsigned int b = start + static_cast<unsigned int>(nextI * crossSection + j);
                const unsigned int c = start + static_cast<unsigned int>(nextI * crossSection + nextJ);
                const unsigned int d = start + static_cast<unsigned int>(i * crossSection + nextJ);
                // This tube's outward normal is theta-tangent cross cross-section tangent.
                indices.insert(indices.end(), {a, c, b, a, d, c});
            }
        }
    };

    const float usableHeight = std::min(rimHeight, height);
    if (height <= rimHeight * 2.0f)
        addBand(0.0f, height * 0.5f);
    else
    {
        addBand(-height * 0.5f + usableHeight * 0.5f, usableHeight * 0.5f);
        addBand(height * 0.5f - usableHeight * 0.5f, usableHeight * 0.5f);
    }
    return Mesh(vertices, indices);
}

class RenderTarget
{
public:
    RenderTarget()
    {
        glGenFramebuffers(1, &framebuffer_);
        glGenTextures(1, &colorTexture_);
        glGenRenderbuffers(1, &depthBuffer_);
        resize(1, 1);
    }
    ~RenderTarget()
    {
        if (depthBuffer_) glDeleteRenderbuffers(1, &depthBuffer_);
        if (colorTexture_) glDeleteTextures(1, &colorTexture_);
        if (framebuffer_) glDeleteFramebuffers(1, &framebuffer_);
    }
    RenderTarget(const RenderTarget&) = delete;
    RenderTarget& operator=(const RenderTarget&) = delete;

    void resize(int width, int height)
    {
        width = std::max(width, 1);
        height = std::max(height, 1);
        if (width_ == width && height_ == height) return;
        width_ = width;
        height_ = height;

        GLint previousTexture, previousRenderbuffer, previousDraw, previousRead;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &previousRenderbuffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDraw);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousRead);
        glBindTexture(GL_TEXTURE_2D, colorTexture_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width_, height_, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer_);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width_, height_);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture_, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthBuffer_);
        const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDraw);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, previousRead);
        glBindTexture(GL_TEXTURE_2D, previousTexture);
        glBindRenderbuffer(GL_RENDERBUFFER, previousRenderbuffer);
        if (!complete)
            throw std::runtime_error("Render viewport framebuffer is incomplete.");
    }

    void bind() const
    {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
        glViewport(0, 0, width_, height_);
    }
    GLuint colorTexture() const { return colorTexture_; }

private:
    GLuint framebuffer_ = 0;
    GLuint colorTexture_ = 0;
    GLuint depthBuffer_ = 0;
    int width_ = 0;
    int height_ = 0;
};

struct Camera
{
    glm::vec3 position{0.0f, 0.0f, 2.25f};
    float yaw = -90.0f;
    float pitch = 0.0f;
    float zoom = 42.0f;
    void resetInspection()
    {
        // Source Modify_Jagae sphere's local transform plus identity Jagae parent.
        const glm::vec3 sourceTarget = glm::vec3(0.19235098f,-5.15262f,18.30627f)
            + glm::vec3(5.88f,6.14462f,-16.09927f);
        const glm::vec3 sourceCamera(3.02f,2.14f,-1.97f);
        position = glm::normalize(sourceCamera-sourceTarget)*2.25f;
        const glm::vec3 direction = glm::normalize(-position);
        yaw = glm::degrees(std::atan2(direction.z,direction.x));
        pitch = glm::degrees(std::asin(direction.y));
        zoom = 42.0f;
    }
    glm::vec3 forward() const
    {
        const float y = glm::radians(yaw), p = glm::radians(pitch);
        return glm::normalize(glm::vec3(std::cos(y) * std::cos(p), std::sin(p), std::sin(y) * std::cos(p)));
    }
    glm::mat4 view() const { return glm::lookAt(position, position + forward(), glm::vec3(0, 1, 0)); }
};

struct AppState
{
    Camera camera;
    DirectionalLight jagaeLight;
    JagaeLightMode jagaeLightMode=JagaeLightMode::Area;
    DirectionalLightController lightController;
    float deltaSeconds = 0.0f;
    double lastX = 640.0, lastY = 360.0;
    bool firstMouse = true;
    bool cursorCaptured = false;
    bool f1WasDown = false;
    bool viewportHovered = false;
    SceneSelection scene = SceneSelection::Gat;
    ModelSelection model = ModelSelection::Sphere;
    JagaeBaseColor jagaeBaseColor = JagaeBaseColor::Black;
    bool clearCoatEnabled = true;
    bool jagaePatternEnabled = false;
    EnvironmentPreset jagaeEnvironment = EnvironmentPreset::Studio;
    std::string environmentError;
};

bool acceptsViewportMouse(const AppState& state)
{
    if (state.cursorCaptured) return true;
    // The center image is an intentional hand-off from ImGui to the camera.
    // Elsewhere, respect ImGui's capture request so panel interaction cannot
    // rotate or zoom the scene.
    const bool imguiWantsMouse = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureMouse;
    if (imguiWantsMouse && !state.viewportHovered) return false;
    return state.viewportHovered;
}

void framebufferSizeCallback(GLFWwindow*, int width, int height) { glViewport(0, 0, width, height); }
void cursorPositionCallback(GLFWwindow* window, double x, double y)
{
    auto& state = *static_cast<AppState*>(glfwGetWindowUserPointer(window));
    const bool rotating = acceptsViewportMouse(state) &&
        (state.cursorCaptured || glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS);
    if (!rotating) { state.firstMouse = true; return; }
    if (state.firstMouse) { state.lastX = x; state.lastY = y; state.firstMouse = false; return; }
    const float dx = static_cast<float>(x - state.lastX) * 0.11f;
    const float dy = static_cast<float>(state.lastY - y) * 0.11f;
    state.lastX = x; state.lastY = y;
    state.camera.yaw += dx;
    state.camera.pitch = std::clamp(state.camera.pitch + dy, -89.0f, 89.0f);
}

void scrollCallback(GLFWwindow* window, double, double yOffset)
{
    auto& state = *static_cast<AppState*>(glfwGetWindowUserPointer(window));
    if (acceptsViewportMouse(state))
        state.camera.zoom = std::clamp(state.camera.zoom - static_cast<float>(yOffset) * 2.0f, 20.0f, 75.0f);
}
void APIENTRY debugCallback(GLenum, GLenum type, GLuint, GLenum severity, GLsizei, const GLchar* message, const void*)
{
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    std::cerr << "OpenGL " << (type == GL_DEBUG_TYPE_ERROR ? "error" : "message") << ": " << message << '\n';
}
void processInput(GLFWwindow* window, AppState& state)
{
    // Escape and F1 are application controls and intentionally remain active
    // even when an ImGui widget owns keyboard input.
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) glfwSetWindowShouldClose(window, GLFW_TRUE);
    const bool keyboardCaptured = ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantCaptureKeyboard;
    if (!keyboardCaptured)
    {
        if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS) state.scene = SceneSelection::Gat;
        if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS) state.scene = SceneSelection::Jagae;
        if (glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE &&
            (state.viewportHovered || state.cursorCaptured))
        {
            const glm::vec3 forward = state.camera.forward();
            const glm::vec3 right = glm::normalize(glm::cross(forward, glm::vec3(0, 1, 0)));
            const float speed = 1.5f * state.deltaSeconds *
                (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS ? 2.0f : 1.0f);
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) state.camera.position += forward * speed;
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) state.camera.position -= forward * speed;
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) state.camera.position -= right * speed;
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) state.camera.position += right * speed;
            const bool moveDown = glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS;
            const bool moveUp = glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS;
            if (moveUp != moveDown)
                state.camera.position.y += moveUp ? speed : -speed;
        }
    }
    DirectionalLightController::Keys lightKeys;
    lightKeys.left = glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
    lightKeys.right = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
    lightKeys.up = glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS;
    lightKeys.down = glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS;
    lightKeys.reset = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
    const bool lightFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED) == GLFW_TRUE &&
        !keyboardCaptured && state.scene == SceneSelection::Jagae &&
        (state.viewportHovered || state.cursorCaptured);
    state.lightController.update(state.jagaeLight, lightKeys, lightFocused,
        glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_CONTROL) == GLFW_PRESS,
        glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS,
        state.deltaSeconds);
    const bool f1Down = glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS;
    if (f1Down && !state.f1WasDown)
    {
        state.cursorCaptured = !state.cursorCaptured;
        state.firstMouse = true;
        glfwSetInputMode(window, GLFW_CURSOR, state.cursorCaptured ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
    }
    state.f1WasDown = f1Down;
}

void drawPart(const Program& shader, const Mesh& surface, const Mesh& rims, const glm::mat4& model,
              const glm::mat4& viewProjection, const Camera& camera, float height, float innerDiameter)
{
    shader.set("uModel", model);
    shader.set("uViewProjection", viewProjection);
    shader.set("uCameraWorld", camera.position);
    shader.set("uAxisMillimeters", glm::vec3(1000.0f));
    shader.set("uBoundsY", glm::vec4(-height * 0.5f, height * 0.5f, 0.0f, 0.0f));
    // 288 = 160 * 1.8.  One shared even density keeps cap rings/spokes and side
    // horizontal/vertical yarn at the same spatial frequency.
    shader.set("uGat", glm::vec4(innerDiameter, 288.0f, 0.00025f, 0.012f)); // inner diameter, even density, thread width, rim height (metres)
    shader.set("uRole", 0.0f);
    surface.draw();
    shader.set("uRole", 1.0f);
    rims.draw();
}

void drawInspectionMaterial(const Program& shader, const Mesh& mesh,
                            const glm::mat4& viewProjection, const Camera& camera)
{
    shader.set("uModel", glm::mat4(1.0f));
    shader.set("uViewProjection", viewProjection);
    shader.set("uCameraWorld", camera.position);
    shader.set("uAxisMillimeters", glm::vec3(1000.0f));
    shader.set("uBoundsY", glm::vec4(-0.55f, 0.55f, 0.0f, 0.0f));
    shader.set("uGat", glm::vec4(0.11f, 288.0f, 0.00025f, 0.012f));
    shader.set("uRole", 0.0f);
    mesh.draw();
}
}

int main(int argc, char* argv[])
{
    for (int argument = 1; argument < argc; ++argument)
    {
        if (std::string(argv[argument]) == "--light-self-test") return runDirectionalLightSelfTests();
        if (std::string(argv[argument]) == "--camera-self-test")
        {
            Camera camera; camera.resetInspection();
            const glm::quat sourceRotation(.99297583f,.11831733f,0,0);
            const glm::vec3 sourceForward=sourceRotation*glm::vec3(0,0,1);
            const glm::vec3 originalForward(0,-.234969f,.972f);
            const float visibleNdotL=glm::dot(glm::normalize(camera.position),DirectionalLight().direction());
            const bool ok=glm::length(camera.forward()+glm::normalize(camera.position))<.00001f
                && glm::length(sourceForward-originalForward)<.001f && visibleNdotL>0
                && std::abs(glm::length(camera.position)-2.25f)<.00001f;
            std::cout<<"Camera source +Z quaternion / centered target / distance / visible NdotL="
                <<visibleNdotL<<" "<<(ok?"passed":"FAILED")<<'\n';
            return ok?0:1;
        }
    }
    if (!glfwInit()) { std::cerr << "GLFW initialization failed.\n"; return 1; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(_DEBUG)
    glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
#endif
    GLFWwindow* window = glfwCreateWindow(1280, 720, "Korean Traditional Material - Gat", nullptr, nullptr);
    if (!window) { std::cerr << "GLFW window creation failed.\n"; glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
    {
        std::cerr << "GLAD failed to load OpenGL functions.\n";
        glfwDestroyWindow(window); glfwTerminate(); return 1;
    }
    std::cout << "OpenGL " << glGetString(GL_VERSION) << " | " << glGetString(GL_RENDERER) << std::endl;
    // A requested 3.3 context can expose KHR_debug without advertising OpenGL
    // 4.3.  GLAD only loads the core 4.3 entry point for a 4.3+ context, so load
    // it explicitly when the 3.3 extension supplies it.
    if (GLAD_GL_VERSION_4_3 || glfwExtensionSupported("GL_KHR_debug"))
    {
        const auto setDebugCallback = reinterpret_cast<PFNGLDEBUGMESSAGECALLBACKPROC>(
            glfwGetProcAddress("glDebugMessageCallback"));
        if (setDebugCallback)
        {
            glEnable(GL_DEBUG_OUTPUT); glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            setDebugCallback(debugCallback, nullptr);
        }
    }

    AppState state;
    int frameLimit = 0;
    bool lightRenderTest = false;
    bool captureFrame = false;
    bool postSelfTest = false;
    bool nacreSelfTest = false;
    bool baseColorSelfTest = false;
    bool thicknessBaseline = false;
    bool detailBaseline = false;
    bool opticsBaseline = false;
    bool continuousBaseline = false;
    bool continuousBenchmark = false;
    bool waveBaseline = false;
    bool smoothBaseline = false;
    bool areaBenchmark = false;
    bool lightingBaseline = false;
    bool opticsBenchmark = false;
    bool thicknessBenchmark = false;
    float nacreMixTest = -1.0f;
    std::string nacreMixLabel;
    PostSettings postSettings; // Actual linked default profile: None, Bloom/Vignette off.
    for (int argument = 1; argument < argc; ++argument)
    {
        const std::string value(argv[argument]);
        if (value == "--jagae") state.scene = SceneSelection::Jagae;
        if (value == "--base-black") { state.scene = SceneSelection::Jagae; state.jagaeBaseColor = JagaeBaseColor::Black; }
        if (value == "--base-white") { state.scene = SceneSelection::Jagae; state.jagaeBaseColor = JagaeBaseColor::White; }
        if (value == "--base-nacre")
        {
            state.scene = SceneSelection::Jagae;
            state.jagaeBaseColor = JagaeBaseColor::White;
            std::cerr << "--base-nacre is obsolete; using --base-white.\n";
        }
        if (value == "--pattern-on") { state.scene = SceneSelection::Jagae; state.jagaePatternEnabled = true; }
        if (value == "--pattern-off") { state.scene = SceneSelection::Jagae; state.jagaePatternEnabled = false; }
        if (value == "--pattern-reflection-inspect")
        {
            state.scene = SceneSelection::Jagae;
            state.jagaeLight.setAzimuthDegrees(144.0f);
            state.jagaeLight.setElevationDegrees(-12.5f);
            std::cout << "Pattern reflection inspection light: azimuth 144 deg, elevation -12.5 deg.\n";
        }
        if (value == "--unity-environment") state.jagaeEnvironment = EnvironmentPreset::UnityReference;
        if (value == "--no-clear-coat") state.clearCoatEnabled = false;
        // Deliberately bounded candidate ratios; ordinary presets keep their defaults.
        if (value == "--nacre-mix-test=0.35") { nacreMixTest = .35f; nacreMixLabel = "035"; }
        if (value == "--nacre-mix-test=0.50") { nacreMixTest = .50f; nacreMixLabel = "050"; }
        if (value == "--nacre-mix-test=0.55") { nacreMixTest = .55f; nacreMixLabel = "055"; }
        if (value == "--nacre-mix-test=0.60") { nacreMixTest = .60f; nacreMixLabel = "060"; }
        if (value.rfind("--nacre-mix-test", 0) == 0 && value != "--nacre-mix-test=0.35"
            && value != "--nacre-mix-test=0.50" && value != "--nacre-mix-test=0.55" && value != "--nacre-mix-test=0.60")
        {
            std::cerr << "Use --nacre-mix-test=0.35, =0.50, =0.55 or =0.60.\n";
            glfwDestroyWindow(window); glfwTerminate(); return 2;
        }
        if (value == "--nacre-thickness-baseline") thicknessBaseline = true;
        if (value == "--nacre-detail-baseline") detailBaseline = true;
        if (value == "--nacre-optics-baseline") opticsBaseline = true;
        if (value == "--nacre-continuous-baseline") continuousBaseline = true;
        if (value == "--nacre-wave-baseline") waveBaseline = true;
        if (value == "--nacre-smooth-baseline") smoothBaseline = true;
        if (value == "--directional-light") state.jagaeLightMode=JagaeLightMode::Directional;
        if (value == "--area-light") state.jagaeLightMode=JagaeLightMode::Area;
        if (value == "--lighting-baseline") { state.jagaeLightMode=JagaeLightMode::Directional;lightingBaseline=true; }
        if (value == "--area-light-benchmark") { areaBenchmark=true;frameLimit=1; }
        if (value == "--nacre-continuous-benchmark") { continuousBenchmark=true; frameLimit=1; }
        if (value == "--nacre-optics-benchmark") { opticsBenchmark=true; frameLimit=1; }
        if (value == "--nacre-thickness-benchmark") { thicknessBenchmark=true; frameLimit=1; }
        if (value == "--model-square") state.model = ModelSelection::Square;
        if (value == "--model-cylinder") state.model = ModelSelection::Cylinder;
        if (value == "--base-color-self-test") { baseColorSelfTest = true; state.scene = SceneSelection::Jagae; frameLimit = 1; }
        if (value == "--light-render-test") { state.scene = SceneSelection::Jagae; lightRenderTest = true; }
        if (value == "--smoke-test") frameLimit = 90;
        if (value == "--capture-frame") { captureFrame = true; frameLimit = 1; }
        if (value == "--post-self-test") { postSelfTest = true; frameLimit = 1; }
        if (value == "--nacre-self-test") { nacreSelfTest = true; state.scene = SceneSelection::Jagae; frameLimit = 1; }
        if (value == "--reference-post") postSettings = PostSettings::inspectionProfile();
    }
    if (lightRenderTest) state.scene = SceneSelection::Jagae;
    if (nacreMixTest >= 0.0f) { state.scene = SceneSelection::Jagae; state.jagaeBaseColor = JagaeBaseColor::White;
        std::cout << "Diagnostic Nacre structural mix=" << nacreMixTest << '\n'; }
    if (state.scene == SceneSelection::Jagae) state.camera.resetInspection();
    if (state.scene == SceneSelection::Jagae)
        glfwSetWindowTitle(window, "Korean Traditional Material - Modify_Jagae");
    glfwSetWindowUserPointer(window, &state);
    glfwSetFramebufferSizeCallback(window, framebufferSizeCallback);
    glfwSetCursorPosCallback(window, cursorPositionCallback);
    glfwSetScrollCallback(window, scrollCallback);
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.WindowBorderSize = 1.0f;
    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.082f, 0.094f, 1.0f);
    style.Colors[ImGuiCol_TitleBg] = ImVec4(0.055f, 0.060f, 0.070f, 1.0f);
    style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.075f, 0.085f, 0.10f, 1.0f);
    style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.36f, 0.54f, 0.72f);
    style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.16f, 0.48f, 0.72f, 0.82f);
    style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.17f, 0.52f, 0.78f, 1.0f);
    const bool imguiGlfwInitialized = ImGui_ImplGlfw_InitForOpenGL(window, true);
    const bool imguiOpenGLInitialized = imguiGlfwInitialized && ImGui_ImplOpenGL3_Init("#version 330 core");
    if (!imguiGlfwInitialized || !imguiOpenGLInitialized)
    {
        std::cerr << "Dear ImGui initialization failed.\n";
        // Only tear down backends that completed initialization, in reverse
        // order. This matters when the GLFW backend succeeds but OpenGL fails.
        if (imguiOpenGLInitialized) ImGui_ImplOpenGL3_Shutdown();
        if (imguiGlfwInitialized) ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window); glfwTerminate(); return 1;
    }

    glEnable(GL_DEPTH_TEST);
    // The display texture and ImGui already contain display code values.
    glDisable(GL_FRAMEBUFFER_SRGB);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    try
    {
        Program shader("shaders/gat.vert", "shaders/gat.frag");
        JagaeScene jagaeScene;
        if(!jagaeScene.setEnvironment(state.jagaeEnvironment,state.environmentError)) {
            state.jagaeEnvironment=jagaeScene.environmentPreset();
            std::cerr<<state.environmentError<<'\n';
        }
        if (thicknessBenchmark && !jagaeScene.thicknessBenchmark(std::cout))
            throw std::runtime_error("Cell thickness benchmark failed.");
        if (opticsBenchmark && !jagaeScene.thicknessBenchmark(std::cout,true))
            throw std::runtime_error("Coherent thickness/normal benchmark failed.");
        if (continuousBenchmark && !jagaeScene.thicknessBenchmark(std::cout,false,true))
            throw std::runtime_error("Continuous Nacre benchmark failed.");
        if (areaBenchmark && !jagaeScene.thicknessBenchmark(std::cout,false,false,true))
            throw std::runtime_error("Area light benchmark failed.");
        if (nacreSelfTest && !runNacreSelfTests(std::cout))
            throw std::runtime_error("Nacre self-test failed.");
        if (baseColorSelfTest && !jagaeScene.baseColorSelfTest(std::cout))
            throw std::runtime_error("Jagae base-color self-test failed.");
        SceneSwitcher sceneSwitcher;
        RenderTarget renderTarget;
        PostProcess postProcess;
        if (postSelfTest && !postProcess.selfTest(std::cout))
            throw std::runtime_error("Post-processing self-test failed.");
        Mesh sphere = makeInspectionSphere();
        Mesh square = makeInspectionSquare();
        Mesh cylinder = makeCylinder(0.43f, 1.10f);
        float previousTime = static_cast<float>(glfwGetTime());

        int renderedFrames = 0;
        while (!glfwWindowShouldClose(window))
        {
            const float now = static_cast<float>(glfwGetTime());
            state.deltaSeconds = std::min(now - previousTime, 0.1f);
            previousTime = now;
            int width = 0, height = 0;
            glfwGetFramebufferSize(window, &width, &height);
            // A minimized GLFW window reports a zero-sized framebuffer.  Do not
            // construct a zero-aspect projection or try to swap that framebuffer.
            if (width <= 0 || height <= 0)
            {
                // Avoid a full-speed render loop while the window is minimized.
                glfwWaitEvents();
                continue;
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            const SceneSelection previousScene = state.scene;
            const RenderViewport viewport = sceneSwitcher.draw(width, height, postProcess.displayTexture(),
                                                                state.scene, state.model, state.jagaeLight, state.jagaeBaseColor, state.clearCoatEnabled,
                                                                state.jagaeEnvironment, state.environmentError,
                                                                nacreMixTest >= 0.0f ? nacreMixTest : JagaeStructuralMix,&state.jagaeLightMode,
                                                                &state.jagaePatternEnabled);
            if(state.jagaeEnvironment!=jagaeScene.environmentPreset()
                && !jagaeScene.setEnvironment(state.jagaeEnvironment,state.environmentError))
                state.jagaeEnvironment=jagaeScene.environmentPreset();
            state.viewportHovered = viewport.hovered;
            processInput(window, state);
            if (viewport.resetView || previousScene != state.scene)
            {
                state.camera = Camera{};
                if (state.scene == SceneSelection::Jagae) state.camera.resetInspection();
                state.firstMouse = true;
            }
            ImGui::Render();

            renderTarget.resize(viewport.width, viewport.height);
            renderTarget.bind();
            const glm::mat4 projection = glm::perspective(glm::radians(state.camera.zoom),
                static_cast<float>(viewport.width) / static_cast<float>(viewport.height), 0.01f, 100.0f);
            const glm::mat4 viewProjection = projection * state.camera.view();
            // A neutral scene-view gray keeps the near-black Gat weave legible
            // without washing out Jagae's spectral highlights.
            // Background is an inspection display swatch. Decode it into the
            // linear Jagae scene so default sRGB output preserves its appearance.
            const auto decode=[](float x) { return x<=.04045f?x/12.92f:std::pow((x+.055f)/1.055f,2.4f); };
            if (state.scene == SceneSelection::Jagae)
                glClearColor(decode(.16f), decode(.17f), decode(.19f), 1.0f);
            else glClearColor(.16f,.17f,.19f,1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            if (state.scene == SceneSelection::Gat)
            {
                shader.use();
                const Mesh* selectedMesh = &sphere;
                if (state.model == ModelSelection::Square) selectedMesh = &square;
                if (state.model == ModelSelection::Cylinder) selectedMesh = &cylinder;
                drawInspectionMaterial(shader, *selectedMesh, viewProjection, state.camera);
            }
            else
            {
                if (lightRenderTest) glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                const float lightGain=lightingBaseline?1.0f:state.jagaeLightMode==JagaeLightMode::Area?.672f:.4f;
                jagaeScene.draw(viewProjection, state.camera.position, now, state.model, state.jagaeLight, state.jagaeBaseColor, thicknessBaseline, nacreMixTest, state.clearCoatEnabled, detailBaseline, opticsBaseline, continuousBaseline,waveBaseline,state.jagaeLightMode,lightGain,smoothBaseline,state.jagaePatternEnabled);
                if (lightRenderTest)
                {
                    if (!jagaeScene.lightUniformsMatch(state.jagaeLight,state.jagaeLightMode,lightGain))
                        throw std::runtime_error("Jagae baseline light uniform readback failed.");
                    if (!jagaeScene.environmentSelfTest(std::cout))
                        throw std::runtime_error("Jagae indirect environment self-test failed.");
                    state.jagaeLight.setAzimuthDegrees(80.0f);
                    state.jagaeLight.setElevationDegrees(-20.0f);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    jagaeScene.draw(viewProjection, state.camera.position, now, state.model, state.jagaeLight, state.jagaeBaseColor, thicknessBaseline, nacreMixTest, state.clearCoatEnabled, detailBaseline, opticsBaseline, continuousBaseline,waveBaseline,state.jagaeLightMode,lightGain,smoothBaseline,state.jagaePatternEnabled);
                    if (!jagaeScene.lightUniformsMatch(state.jagaeLight,state.jagaeLightMode,lightGain))
                        throw std::runtime_error("Jagae adjusted light uniform readback failed.");
                    const size_t pixelValues = static_cast<size_t>(viewport.width) * static_cast<size_t>(viewport.height) * 4u;
                    std::vector<float> adjustedPixels(pixelValues);
                    glReadPixels(0, 0, viewport.width, viewport.height, GL_RGBA, GL_FLOAT, adjustedPixels.data());
                    state.jagaeLight.setAzimuthDegrees(230.0f);
                    state.jagaeLight.setElevationDegrees(35.0f);
                    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                    jagaeScene.draw(viewProjection, state.camera.position, now, state.model, state.jagaeLight, state.jagaeBaseColor, thicknessBaseline, nacreMixTest, state.clearCoatEnabled, detailBaseline, opticsBaseline, continuousBaseline,waveBaseline,state.jagaeLightMode,lightGain,smoothBaseline,state.jagaePatternEnabled);
                    if (!jagaeScene.lightUniformsMatch(state.jagaeLight,state.jagaeLightMode,lightGain))
                        throw std::runtime_error("Jagae alternate-angle uniform readback failed.");
                    std::vector<float> alternatePixels(pixelValues);
                    glReadPixels(0, 0, viewport.width, viewport.height, GL_RGBA, GL_FLOAT, alternatePixels.data());
                    double pixelDifference = 0;
                    float peakRadiance = 0;
                    for (size_t i = 0; i < pixelValues; ++i)
                    {
                        if (!std::isfinite(adjustedPixels[i]) || !std::isfinite(alternatePixels[i]) ||
                            adjustedPixels[i]<0 || alternatePixels[i]<0)
                            throw std::runtime_error("Raw Jagae HDR contains invalid/negative radiance.");
                        pixelDifference += std::abs(adjustedPixels[i] - alternatePixels[i]);
                        if (i%4!=3) peakRadiance=std::max(peakRadiance,std::max(adjustedPixels[i],alternatePixels[i]));
                    }
                    const double minimumDifference = static_cast<double>(viewport.width) * viewport.height
                        *(state.jagaeLightMode==JagaeLightMode::Area?.0001:.01*lightGain);
                    if (pixelDifference <= minimumDifference)
                        throw std::runtime_error("Jagae light-angle framebuffer comparison showed no material pixel change.");
                    std::cout << "Jagae light render test passed (fixed-radiance angle uniform readback; framebuffer delta "
                              << pixelDifference << ", raw linear HDR peak " << peakRadiance << ").\n";
                }
            }

            postProcess.render(renderTarget.colorTexture(),viewport.width,viewport.height,
                state.scene==SceneSelection::Jagae?OutputMode::LinearScene:OutputMode::LegacyDisplay,postSettings);
            if (captureFrame)
            {
                postProcess.bindDisplayForRead();
                const std::string filename = nacreMixLabel.empty() ? "inspection.bmp"
                    : "nacre-mix-" + nacreMixLabel + (state.model == ModelSelection::Square ? "-square.bmp" : "-sphere.bmp");
                captureFramebuffer(viewport.width,viewport.height,filename);
            }
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDisable(GL_FRAMEBUFFER_SRGB);
            if (postSelfTest && glIsEnabled(GL_FRAMEBUFFER_SRGB))
                throw std::runtime_error("UI output must not apply hardware sRGB conversion.");
            glViewport(0, 0, width, height);
            glClearColor(0.035f, 0.040f, 0.050f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
            glfwPollEvents();
            if (lightRenderTest) glfwSetWindowShouldClose(window, GLFW_TRUE);
            if (frameLimit > 0 && ++renderedFrames >= frameLimit)
                glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        if (imguiOpenGLInitialized) ImGui_ImplOpenGL3_Shutdown();
        if (imguiGlfwInitialized) ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window); glfwTerminate(); return 1;
    }
    if (imguiOpenGLInitialized) ImGui_ImplOpenGL3_Shutdown();
    if (imguiGlfwInitialized) ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
