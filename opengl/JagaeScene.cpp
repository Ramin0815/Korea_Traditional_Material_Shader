#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincodec.h>
#include <wrl/client.h>

#include "JagaeScene.h"
#include "EnvironmentLighting.h"
#include "PostProcess.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr GLenum TextureMaxAnisotropy = 0x84FE;     // GL_TEXTURE_MAX_ANISOTROPY_EXT
constexpr GLenum MaxTextureMaxAnisotropy = 0x84FF;  // GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT

struct JagaeMaterial
{
    // Optical indices/periods retain the imported material. The spatial local
    // mean below is an authored bounded optical design, not measured shell data.
    glm::vec4 baseColor{0.0f, 0.0f, 0.0f, 1.0f / 255.0f};
    glm::vec4 baseMapST{1.0f, 1.0f, 0.0f, 0.0f};
    glm::vec4 thicknessMapST{1.0f, 1.0f, 0.0f, 0.0f};
    float smoothness = 0.78f;
    float clearCoatMask = 0.25f;
    float clearCoatSmoothness = 0.9f;
    // Continuous local-mean optical design: center 375, bounded 150..600 nm.
    float plateletThicknessNm = 375.0f;
    float plateletIOR = 1.63f;
    float substrateIOR = 1.43f;
    int layerCount = 4;
    float interlayerThicknessNm = 30.0f;
    float iridescenceStrength = 0.85f;
    float thicknessVariationNm = 450.0f;
};

struct JagaeVertex
{
    glm::vec3 position;
    glm::vec3 normal;
    glm::vec2 uv;
};

std::string readTextFile(const char* path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error(std::string("Cannot open Jagae shader: ") + path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

GLuint compileShader(GLenum stage, const char* path)
{
    const std::string source = readTextFile(path);
    const char* pointer = source.c_str();
    const GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &pointer, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success == GL_TRUE) return shader;

    GLint length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
    std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
    glGetShaderInfoLog(shader, length, nullptr, log.data());
    std::cerr << "Jagae shader compilation failed (" << path << "):\n" << log << '\n';
    glDeleteShader(shader);
    throw std::runtime_error("Jagae shader compilation failed.");
}

class JagaeProgram
{
public:
    JagaeProgram(const char* vertexPath, const char* fragmentPath)
    {
        GLuint vertex = 0;
        GLuint fragment = 0;
        try
        {
            vertex = compileShader(GL_VERTEX_SHADER, vertexPath);
            fragment = compileShader(GL_FRAGMENT_SHADER, fragmentPath);
            handle_ = glCreateProgram();
            glAttachShader(handle_, vertex);
            glAttachShader(handle_, fragment);
            glLinkProgram(handle_);
            glDeleteShader(vertex); vertex = 0;
            glDeleteShader(fragment); fragment = 0;
            GLint success = GL_FALSE;
            glGetProgramiv(handle_, GL_LINK_STATUS, &success);
            if (success == GL_TRUE) return;
            GLint length = 0;
            glGetProgramiv(handle_, GL_INFO_LOG_LENGTH, &length);
            std::string log(static_cast<size_t>(std::max(length, 1)), '\0');
            glGetProgramInfoLog(handle_, length, nullptr, log.data());
            std::cerr << "Jagae program link failed:\n" << log << '\n';
            throw std::runtime_error("Jagae program link failed.");
        }
        catch (...)
        {
            if (vertex) glDeleteShader(vertex);
            if (fragment) glDeleteShader(fragment);
            if (handle_) glDeleteProgram(handle_);
            handle_ = 0;
            throw;
        }
    }
    ~JagaeProgram() { if (handle_) glDeleteProgram(handle_); }
    JagaeProgram(const JagaeProgram&) = delete;
    JagaeProgram& operator=(const JagaeProgram&) = delete;
    void use() const { glUseProgram(handle_); }
    GLuint handle() const { return handle_; }
    void set(const char* name, const glm::mat4& value) const { glUniformMatrix4fv(glGetUniformLocation(handle_, name), 1, GL_FALSE, glm::value_ptr(value)); }
    void set(const char* name, const glm::vec3& value) const { glUniform3fv(glGetUniformLocation(handle_, name), 1, glm::value_ptr(value)); }
    void set(const char* name, const glm::vec4& value) const { glUniform4fv(glGetUniformLocation(handle_, name), 1, glm::value_ptr(value)); }
    void set(const char* name, float value) const { glUniform1f(glGetUniformLocation(handle_, name), value); }
    void set(const char* name, int value) const { glUniform1i(glGetUniformLocation(handle_, name), value); }
private:
    GLuint handle_ = 0;
};

struct WicImage
{
    UINT width = 0;
    UINT height = 0;
    std::vector<unsigned char> rgba;
};

WicImage loadWicImage(const char* path)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool ownsComInitialization = SUCCEEDED(comResult);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE)
        throw std::runtime_error("Cannot initialize COM for Jagae texture loading.");

    try
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(result)) throw std::runtime_error("Cannot create WIC imaging factory.");

        ComPtr<IWICBitmapDecoder> decoder;
        const std::wstring widePath = std::filesystem::path(path).wstring();
        result = factory->CreateDecoderFromFilename(widePath.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
        if (FAILED(result)) throw std::runtime_error(std::string("Cannot open Jagae texture: ") + path);

        ComPtr<IWICBitmapFrameDecode> frame;
        result = decoder->GetFrame(0, frame.GetAddressOf());
        if (FAILED(result)) throw std::runtime_error(std::string("Cannot decode Jagae texture: ") + path);

        ComPtr<IWICFormatConverter> converter;
        result = factory->CreateFormatConverter(converter.GetAddressOf());
        if (FAILED(result)) throw std::runtime_error("Cannot create WIC pixel converter.");
        result = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom);
        if (FAILED(result)) throw std::runtime_error(std::string("Cannot convert Jagae texture: ") + path);

        WicImage image;
        result = converter->GetSize(&image.width, &image.height);
        if (FAILED(result) || image.width == 0 || image.height == 0)
            throw std::runtime_error(std::string("Invalid Jagae texture dimensions: ") + path);
        const UINT stride = image.width * 4u;
        image.rgba.resize(static_cast<size_t>(stride) * image.height);
        result = converter->CopyPixels(nullptr, stride, static_cast<UINT>(image.rgba.size()), image.rgba.data());
        if (FAILED(result)) throw std::runtime_error(std::string("Cannot read Jagae texture pixels: ") + path);

        converter.Reset();
        frame.Reset();
        decoder.Reset();
        factory.Reset();
        if (ownsComInitialization) CoUninitialize();
        return image;
    }
    catch (...)
    {
        if (ownsComInitialization) CoUninitialize();
        throw;
    }
}

class JagaeTexture
{
public:
    enum class Encoding { LinearRed, LinearAlpha, SrgbRgba };

    JagaeTexture(const char* path, Encoding encoding, bool trilinear, float requestedAnisotropy)
    {
        const WicImage image = loadWicImage(path);
        std::vector<unsigned char> pixels;
        const size_t channels = encoding != Encoding::SrgbRgba ? 1u : 4u;
        pixels.resize(static_cast<size_t>(image.width) * image.height * channels);

        // WIC rows begin at the top; OpenGL UVs and Unity's mesh UV convention begin at the bottom.
        for (UINT y = 0; y < image.height; ++y)
        {
            const UINT sourceY = image.height - 1u - y;
            for (UINT x = 0; x < image.width; ++x)
            {
                const size_t source = (static_cast<size_t>(sourceY) * image.width + x) * 4u;
                const size_t destination = (static_cast<size_t>(y) * image.width + x) * channels;
                if (encoding != Encoding::SrgbRgba)
                {
                    pixels[destination] = image.rgba[source+(encoding==Encoding::LinearAlpha?3:0)];
                }
                else
                {
                    std::copy_n(image.rgba.data() + source, 4, pixels.data() + destination);
                }
            }
        }

        glGenTextures(1, &handle_);
        glBindTexture(GL_TEXTURE_2D, handle_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        if (encoding != Encoding::SrgbRgba)
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, static_cast<GLsizei>(image.width),
                         static_cast<GLsizei>(image.height), 0, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        else
            glTexImage2D(GL_TEXTURE_2D, 0, GL_SRGB8_ALPHA8, static_cast<GLsizei>(image.width),
                         static_cast<GLsizei>(image.height), 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                        trilinear ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST);
        if (requestedAnisotropy > 1.0f && supportsAnisotropicFiltering())
        {
            GLfloat maximumAnisotropy = 1.0f;
            glGetFloatv(MaxTextureMaxAnisotropy, &maximumAnisotropy);
            glTexParameterf(GL_TEXTURE_2D, TextureMaxAnisotropy,
                            std::min(requestedAnisotropy, maximumAnisotropy));
        }
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    ~JagaeTexture() { if (handle_) glDeleteTextures(1, &handle_); }
    JagaeTexture(const JagaeTexture&) = delete;
    JagaeTexture& operator=(const JagaeTexture&) = delete;
    void bind(GLenum textureUnit) const
    {
        glActiveTexture(textureUnit);
        glBindTexture(GL_TEXTURE_2D, handle_);
    }

private:
    static bool supportsAnisotropicFiltering()
    {
        GLint extensionCount = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extensionCount);
        for (GLint index = 0; index < extensionCount; ++index)
        {
            const char* extension = reinterpret_cast<const char*>(
                glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(index)));
            if (extension && std::strcmp(extension, "GL_EXT_texture_filter_anisotropic") == 0)
                return true;
        }
        return false;
    }

    GLuint handle_ = 0;
};

class JagaeMesh
{
public:
    JagaeMesh(const std::vector<JagaeVertex>& vertices, const std::vector<unsigned int>& indices)
        : indexCount_(static_cast<GLsizei>(indices.size()))
    {
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        glGenBuffers(1, &ebo_);
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(JagaeVertex)), vertices.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indices.size() * sizeof(unsigned int)), indices.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(JagaeVertex), reinterpret_cast<void*>(offsetof(JagaeVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(JagaeVertex), reinterpret_cast<void*>(offsetof(JagaeVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(JagaeVertex), reinterpret_cast<void*>(offsetof(JagaeVertex, uv)));
        glBindVertexArray(0);
    }
    ~JagaeMesh()
    {
        if (ebo_) glDeleteBuffers(1, &ebo_);
        if (vbo_) glDeleteBuffers(1, &vbo_);
        if (vao_) glDeleteVertexArrays(1, &vao_);
    }
    JagaeMesh(const JagaeMesh&) = delete;
    JagaeMesh& operator=(const JagaeMesh&) = delete;
    void draw() const
    {
        glBindVertexArray(vao_);
        glDrawElements(GL_TRIANGLES, indexCount_, GL_UNSIGNED_INT, nullptr);
    }
private:
    GLuint vao_ = 0, vbo_ = 0, ebo_ = 0;
    GLsizei indexCount_ = 0;
};

JagaeMesh makeInspectionSphere(int slices = 160, int stacks = 96)
{
    std::vector<JagaeVertex> vertices;
    std::vector<unsigned int> indices;
    vertices.reserve(static_cast<size_t>(slices + 1) * static_cast<size_t>(stacks + 1));
    indices.reserve(static_cast<size_t>(slices) * static_cast<size_t>(stacks) * 6);
    for (int y = 0; y <= stacks; ++y)
    {
        const float v = static_cast<float>(y) / static_cast<float>(stacks);
        const float latitude = Pi * v;
        for (int x = 0; x <= slices; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(slices);
            const float longitude = 2.0f * Pi * u;
            const glm::vec3 normal(std::sin(latitude) * std::cos(longitude),
                                   std::cos(latitude),
                                   std::sin(latitude) * std::sin(longitude));
            // A smooth, slightly flattened inspection object makes the angular
            // spectral shift easy to read while preserving correct normals.
            const glm::vec3 position = normal * 0.55f;
            vertices.push_back({position, normal, glm::vec2(u, v)});
        }
    }
    for (int y = 0; y < stacks; ++y)
    {
        for (int x = 0; x < slices; ++x)
        {
            const unsigned int a = static_cast<unsigned int>(y * (slices + 1) + x);
            const unsigned int b = a + 1;
            const unsigned int c = a + static_cast<unsigned int>(slices + 1);
            const unsigned int d = c + 1;
            // Longitude tangent cross latitude tangent points away from the sphere.
            indices.insert(indices.end(), {a, b, c, b, d, c});
        }
    }
    return JagaeMesh(vertices, indices);
}

JagaeMesh makeInspectionSquare()
{
    std::vector<JagaeVertex> vertices;
    std::vector<unsigned int> indices;
    const float h = 0.48f;
    const auto addFace = [&](const glm::vec3& normal, const glm::vec3& a,
                             const glm::vec3& b, const glm::vec3& c, const glm::vec3& d)
    {
        const unsigned int start = static_cast<unsigned int>(vertices.size());
        vertices.push_back({a, normal, glm::vec2(0.0f, 0.0f)});
        vertices.push_back({b, normal, glm::vec2(1.0f, 0.0f)});
        vertices.push_back({c, normal, glm::vec2(1.0f, 1.0f)});
        vertices.push_back({d, normal, glm::vec2(0.0f, 1.0f)});
        indices.insert(indices.end(), {start, start + 1, start + 2, start, start + 2, start + 3});
    };
    addFace({0, 0, 1}, {-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h});
    addFace({0, 0, -1}, {h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h});
    addFace({1, 0, 0}, {h, -h, h}, {h, -h, -h}, {h, h, -h}, {h, h, h});
    addFace({-1, 0, 0}, {-h, -h, -h}, {-h, -h, h}, {-h, h, h}, {-h, h, -h});
    addFace({0, 1, 0}, {-h, h, h}, {h, h, h}, {h, h, -h}, {-h, h, -h});
    addFace({0, -1, 0}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}, {-h, -h, h});
    return JagaeMesh(vertices, indices);
}

JagaeMesh makeInspectionCylinder(int segments = 128)
{
    std::vector<JagaeVertex> vertices;
    std::vector<unsigned int> indices;
    constexpr float radius = 0.43f;
    constexpr float halfHeight = 0.55f;
    for (int i = 0; i <= segments; ++i)
    {
        const float u = static_cast<float>(i) / static_cast<float>(segments);
        const float angle = 2.0f * Pi * u;
        const glm::vec3 normal(std::cos(angle), 0.0f, std::sin(angle));
        vertices.push_back({normal * radius + glm::vec3(0, -halfHeight, 0), normal, {u, 0}});
        vertices.push_back({normal * radius + glm::vec3(0, halfHeight, 0), normal, {u, 1}});
    }
    for (int i = 0; i < segments; ++i)
    {
        const unsigned int a = static_cast<unsigned int>(i * 2);
        indices.insert(indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    const auto addCap = [&](float y, const glm::vec3& normal, bool top)
    {
        const unsigned int center = static_cast<unsigned int>(vertices.size());
        vertices.push_back({{0, y, 0}, normal, {0.5f, 0.5f}});
        const unsigned int ring = static_cast<unsigned int>(vertices.size());
        for (int i = 0; i <= segments; ++i)
        {
            const float angle = 2.0f * Pi * static_cast<float>(i) / static_cast<float>(segments);
            const float x = std::cos(angle), z = std::sin(angle);
            vertices.push_back({{x * radius, y, z * radius}, normal, {x * 0.5f + 0.5f, z * 0.5f + 0.5f}});
        }
        for (int i = 0; i < segments; ++i)
        {
            const unsigned int a = ring + static_cast<unsigned int>(i);
            if (top) indices.insert(indices.end(), {center, a + 1, a});
            else indices.insert(indices.end(), {center, a, a + 1});
        }
    };
    addCap(halfHeight, {0, 1, 0}, true);
    addCap(-halfHeight, {0, -1, 0}, false);
    return JagaeMesh(vertices, indices);
}
}

struct JagaeScene::Impl
{
    Impl() : shader("shaders/modify_jagae.vert", "shaders/modify_jagae.frag"),
             sphere(makeInspectionSphere()), square(makeInspectionSquare()),
             cylinder(makeInspectionCylinder()),
             baseMap("assets/Jagae_BaseMap.png", JagaeTexture::Encoding::SrgbRgba, true, 4.0f),
             thicknessMap("assets/Jagae_ThicknessMap.png", JagaeTexture::Encoding::LinearRed, false, 1.0f),
             patternMap("assets/Jagae_PatternMask.png",JagaeTexture::Encoding::LinearAlpha,true,4.0f) {
    }
    JagaeProgram shader;
    JagaeMesh sphere;
    JagaeMesh square;
    JagaeMesh cylinder;
    JagaeTexture baseMap;
    JagaeTexture thicknessMap;
    JagaeTexture patternMap;
    EnvironmentLighting blackStudio{EnvironmentPreset::Studio,BlackStudioFillRadiance};
    EnvironmentLighting whiteStudio{EnvironmentPreset::Studio,WhiteStudioFillRadiance};
    bool whiteStudioSelected=false;
    std::unique_ptr<EnvironmentLighting> unityEnvironment;
    EnvironmentPreset environmentPreset = EnvironmentPreset::Studio;
    const EnvironmentLighting& environment() const {
        return environmentPreset==EnvironmentPreset::Studio?(whiteStudioSelected?whiteStudio:blackStudio):*unityEnvironment;
    }
    JagaeMaterial material;
};

JagaeScene::JagaeScene() : impl_(std::make_unique<Impl>()) {}
JagaeScene::~JagaeScene() = default;

bool JagaeScene::setEnvironment(EnvironmentPreset preset, std::string& error)
{
    try {
        if(preset==EnvironmentPreset::UnityReference && !impl_->unityEnvironment)
            impl_->unityEnvironment=std::make_unique<EnvironmentLighting>(preset);
        impl_->environmentPreset=preset;
        error.clear();return true;
    } catch(const std::exception& e) {
        error=std::string("Unity reference unavailable; keeping current environment. ")+e.what();
        return false;
    }
}
EnvironmentPreset JagaeScene::environmentPreset() const { return impl_->environmentPreset; }

bool JagaeScene::environmentSelfTest(std::ostream& output) const
{
    bool ok = impl_->environmentPreset==EnvironmentPreset::Studio
        ?impl_->blackStudio.selfTest(output)&&impl_->whiteStudio.selfTest(output):impl_->environment().selfTest(output);
    // Exercise the real linked material shader with direct radiance disabled.
    // Mask=0 must remove coat-smoothness dependence; mask=.25 must add it.
    GLint drawFbo=0, readFbo=0, viewport[4]{}, program=0, vao=0;
    GLint activeTexture=0, textureBinding=0, cullMode=0;
    GLfloat clearColor[4]{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFbo);
    glGetIntegerv(GL_VIEWPORT,viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM,&program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&activeTexture);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&textureBinding);
    glGetIntegerv(GL_CULL_FACE_MODE,&cullMode);
    glGetFloatv(GL_COLOR_CLEAR_VALUE,clearColor);
    const GLboolean depth=glIsEnabled(GL_DEPTH_TEST), cull=glIsEnabled(GL_CULL_FACE);
    const GLboolean blend=glIsEnabled(GL_BLEND), scissor=glIsEnabled(GL_SCISSOR_TEST);
    GLuint fbo=0, target=0;
    glGenFramebuffers(1,&fbo);
    glGenTextures(1,&target);
    glBindTexture(GL_TEXTURE_2D,target);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,16,16,0,GL_RGBA,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    ok=ok&&glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    glViewport(0,0,16,16);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
    glEnable(GL_CULL_FACE); glCullFace(GL_BACK);
    glClearColor(0,0,0,0);
    const glm::vec3 camera(0,0,3);
    const glm::mat4 vp=glm::ortho(-0.5f,0.5f,-0.5f,0.5f,0.1f,10.0f)
        * glm::lookAt(camera,glm::vec3(0),glm::vec3(0,1,0));
    // draw establishes all texture and SH uniforms and restores texture state;
    // the test then rebinds them while invoking the same linked shader again.
    draw(vp,camera,0,ModelSelection::Square,DirectionalLight{});
    impl_->shader.use();
    GLint base0=0, thickness1=0, cube2=0, albedo3=0;
    glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D,&base0);
    glActiveTexture(GL_TEXTURE1); glGetIntegerv(GL_TEXTURE_BINDING_2D,&thickness1);
    glActiveTexture(GL_TEXTURE2); glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP,&cube2);
    glActiveTexture(GL_TEXTURE3); glGetIntegerv(GL_TEXTURE_BINDING_2D,&albedo3);
    impl_->baseMap.bind(GL_TEXTURE0); impl_->thicknessMap.bind(GL_TEXTURE1);
    impl_->environment().bind(GL_TEXTURE2);
    impl_->environment().bindAlbedo(GL_TEXTURE3);
    const GLboolean seamless=glIsEnabled(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    impl_->shader.set("uLightRadiance",glm::vec3(0));
    const auto render=[&](float mask,float smoothness)
    {
        impl_->shader.set("uClearCoatMask",mask);
        impl_->shader.set("uClearCoatSmoothness",smoothness);
        glClear(GL_COLOR_BUFFER_BIT);
        impl_->square.draw();
        std::array<glm::vec4,256> pixels{};
        glReadPixels(0,0,16,16,GL_RGBA,GL_FLOAT,pixels.data());
        return pixels;
    };
    const auto zero=render(0,0.9f), zeroRough=render(0,0.1f);
    const auto coat=render(0.25f,0.9f), coatRough=render(0.25f,0.1f);
    float maskDifference=0,roughnessDifference=0,zeroDifference=0;
    for(size_t i=0;i<zero.size();++i)
    {
        for(int c=0;c<4;++c) ok=ok&&std::isfinite(coat[i][c])&&std::isfinite(zero[i][c]);
        maskDifference+=glm::length(glm::vec3(coat[i]-zero[i]));
        roughnessDifference+=glm::length(glm::vec3(coat[i]-coatRough[i]));
        zeroDifference+=glm::length(glm::vec3(zero[i]-zeroRough[i]));
    }
    ok=ok&&maskDifference>0.001f&&roughnessDifference>0.001f&&zeroDifference<0.00001f;
    output<<"Indirect shader mask0/.25 delta="<<maskDifference
        <<" coat roughness delta="<<roughnessDifference
        <<" mask0 roughness delta="<<zeroDifference<<'\n';
    impl_->shader.set("uClearCoatMask",impl_->material.clearCoatMask);
    impl_->shader.set("uClearCoatSmoothness",impl_->material.clearCoatSmoothness);
    impl_->shader.set("uLightRadiance",glm::vec3(1));
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(base0));
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(thickness1));
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_CUBE_MAP,static_cast<GLuint>(cube2));
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(albedo3));
    if(!seamless) glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(drawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(readFbo));
    glDeleteFramebuffers(1,&fbo); glDeleteTextures(1,&target);
    glActiveTexture(static_cast<GLenum>(activeTexture));
    glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(textureBinding));
    glUseProgram(static_cast<GLuint>(program)); glBindVertexArray(static_cast<GLuint>(vao));
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    glClearColor(clearColor[0],clearColor[1],clearColor[2],clearColor[3]);
    glCullFace(static_cast<GLenum>(cullMode));
    if(depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    if(cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if(blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if(scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    return ok;
}

bool JagaeScene::lightUniformsMatch(const DirectionalLight& light, JagaeLightMode mode, float gain) const
{
    const GLint directionLocation = glGetUniformLocation(impl_->shader.handle(), "uLightDirection");
    const GLint radianceLocation = glGetUniformLocation(impl_->shader.handle(), "uLightRadiance");
    if (directionLocation < 0 || radianceLocation < 0) return false;
    glm::vec3 direction(0.0f), radiance(0.0f);
    glGetUniformfv(impl_->shader.handle(), directionLocation, &direction.x);
    glGetUniformfv(impl_->shader.handle(), radianceLocation, &radiance.x);
    return glm::length(direction - light.direction()) < 0.0001f &&
           glm::length(radiance - light.radiance()*gain) < 0.0001f
           && [&] {
               GLint linkedMode=0;glm::vec3 center(0);
               glGetUniformiv(impl_->shader.handle(),glGetUniformLocation(impl_->shader.handle(),"uLightMode"),&linkedMode);
               glGetUniformfv(impl_->shader.handle(),glGetUniformLocation(impl_->shader.handle(),"uAreaCenter"),&center.x);
               return linkedMode==static_cast<int>(mode)&&glm::length(center-JagaeRectangleLight(light).center)<0.0001f;
           }();
}

bool JagaeScene::baseColorSelfTest(std::ostream& output) const
{
    // Actual linked material shader, floating HDR and material diagnostics.
    constexpr int size=192; // Resolve intra-fragment bands as well as cell joins.
    GLint drawFbo=0,readFbo=0,viewport[4]{},program=0,vao=0,active=0,binding=0,depthBinding=0;
    GLfloat clear[4]{};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawFbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFbo);
    glGetIntegerv(GL_VIEWPORT,viewport); glGetFloatv(GL_COLOR_CLEAR_VALUE,clear);
    glGetIntegerv(GL_CURRENT_PROGRAM,&program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&active); glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    glGetIntegerv(GL_RENDERBUFFER_BINDING,&depthBinding);
    const GLboolean depth=glIsEnabled(GL_DEPTH_TEST),blend=glIsEnabled(GL_BLEND),scissor=glIsEnabled(GL_SCISSOR_TEST);
    GLuint fbo=0,target=0,depthTarget=0;
    glGenFramebuffers(1,&fbo); glGenTextures(1,&target); glGenRenderbuffers(1,&depthTarget);
    glBindTexture(GL_TEXTURE_2D,target);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,size,size,0,GL_RGBA,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glBindRenderbuffer(GL_RENDERBUFFER,depthTarget);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,size,size);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,target,0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depthTarget);
    bool ok=glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    glViewport(0,0,size,size);glClearColor(0,0,0,0);
    glEnable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);
    const glm::vec3 camera(0,0,3);
    const glm::mat4 projection=glm::ortho(-0.62f,0.62f,-0.62f,0.62f,0.1f,10.0f);
    DirectionalLight light; light.setAzimuthDegrees(0); light.setElevationDegrees(30);
    GLuint mutations[2]{};
    glGenTextures(2,mutations);
    const unsigned char alteredBase[4]={37,103,173,255},alteredThickness=25;
    for(int index=0;index<2;++index) {
        glBindTexture(GL_TEXTURE_2D,mutations[index]);
        glTexImage2D(GL_TEXTURE_2D,0,index?GL_R8:GL_RGBA8,1,1,0,index?GL_RED:GL_RGBA,
            GL_UNSIGNED_BYTE,index?&alteredThickness:alteredBase);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }
    const auto render=[&](JagaeBaseColor style,ModelSelection model,int diagnostic,const glm::vec3& eye,
                          bool pattern=false,bool mutation=false,JagaeLightMode mode=JagaeLightMode::Directional,
                          float gain=1.0f,bool coat=true,bool thicknessReference=false) {
        const glm::vec3 up=std::abs(eye.y)>2.9f?glm::vec3(0,0,1):glm::vec3(0,1,0);
        const glm::mat4 vp=projection*glm::lookAt(eye,glm::vec3(0),up);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        draw(vp,eye,0,model,light,style,thicknessReference,-1,coat,false,false,false,false,mode,gain,false,pattern);
        ok=ok&&lightUniformsMatch(light,mode,gain);
        GLfloat strength=0;glGetUniformfv(impl_->shader.handle(),glGetUniformLocation(impl_->shader.handle(),"uIridescenceStrength"),&strength);
        ok=ok&&strength==JagaeNacreStructuralMix;
        GLint savedActive=0,saved[5]{};
        glGetIntegerv(GL_ACTIVE_TEXTURE,&savedActive);
        for(int unit=0;unit<5;++unit) {
            glActiveTexture(GL_TEXTURE0+unit);
            glGetIntegerv(unit==2?GL_TEXTURE_BINDING_CUBE_MAP:GL_TEXTURE_BINDING_2D,&saved[unit]);
        }
        impl_->baseMap.bind(GL_TEXTURE0);impl_->thicknessMap.bind(GL_TEXTURE1);
        impl_->environment().bind(GL_TEXTURE2);impl_->environment().bindAlbedo(GL_TEXTURE3);
        impl_->patternMap.bind(GL_TEXTURE4);
        if(mutation) {
            glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,mutations[0]);
            glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,mutations[1]);
        }
        const GLboolean seamless=glIsEnabled(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
        impl_->shader.use();impl_->shader.set("uNacreTestMode",diagnostic);
        if(model==ModelSelection::Sphere)impl_->sphere.draw();
        else if(model==ModelSelection::Square)impl_->square.draw();else impl_->cylinder.draw();
        std::vector<glm::vec4> pixels(size*size);
        glReadPixels(0,0,size,size,GL_RGBA,GL_FLOAT,pixels.data());
        impl_->shader.set("uNacreTestMode",0);
        for(int unit=0;unit<5;++unit) {
            glActiveTexture(GL_TEXTURE0+unit);
            glBindTexture(unit==2?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_2D,static_cast<GLuint>(saved[unit]));
        }
        glActiveTexture(savedActive);
        if(!seamless)glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
        return pixels;
    };
    // Inspect actual linear alpha upload, not RGB white-border values.
    GLint patternFormat=0,patternWidth=0,patternHeight=0;
    impl_->patternMap.bind(GL_TEXTURE4);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&patternFormat);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_WIDTH,&patternWidth);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_HEIGHT,&patternHeight);
    std::vector<unsigned char> alpha(static_cast<size_t>(patternWidth)*patternHeight);
    glGetTexImage(GL_TEXTURE_2D,0,GL_RED,GL_UNSIGNED_BYTE,alpha.data());
    const auto sourceAlpha=[&](int x,int y){return alpha[(patternHeight-1-y)*patternWidth+x];};
    ok=ok&&patternFormat==GL_R8&&patternWidth==2048&&patternHeight==2048
        &&sourceAlpha(0,0)==0&&sourceAlpha(1024,1024)==0&&sourceAlpha(1024,256)==255;
    size_t outside=0,inside=0,filteredEdges=0;
    double outsideMutation=0,insideMutation=0,colorOpticsDifference=0,normalError=0;
    float roughMin=1,roughMax=0,thicknessMin=600,thicknessMax=150,bodyRatioError=0;
    const auto mask=render(JagaeBaseColor::White,ModelSelection::Square,34,camera,true);
    // Optional diagnostic data from real uploaded texture, GPU derivatives and
    // actual mesh UVs. No CPU hash approximation or viewport hue inference.
    if(const char* diagnosticCsv=std::getenv("KOR_NACRE_DIAGNOSTIC_CSV")) {
        const auto thickness=render(JagaeBaseColor::White,ModelSelection::Square,24,camera);
        const auto spectral=render(JagaeBaseColor::White,ModelSelection::Square,25,camera);
        std::ofstream csv(diagnosticCsv);
        csv<<"pixel_x,pixel_y,thickness_nm,previous_thickness_nm,linear_r,linear_g,linear_b\n";
        for(size_t i=0;i<thickness.size();++i)if(thickness[i].w>.5f) {
            const float priorMapped=500+(thickness[i].y-375)*(120.0f/450.0f);
            const float priorHeadroom=std::max(std::min(priorMapped-440,560-priorMapped),0.0f);
            const float prior=priorMapped+(2*thickness[i].z-1)*std::min(20.0f,priorHeadroom);
            csv<<i%size<<','<<i/size<<','<<thickness[i].x<<','<<prior<<','
               <<spectral[i].x<<','<<spectral[i].y<<','<<spectral[i].z<<'\n';
        }
        ok=ok&&csv.good();
        output<<"GPU thickness/spectrum chart CSV="<<diagnosticCsv
            <<"; actual square pixels, Pattern Off, fixed facet incidence 30deg; previous d reconstructed from unchanged GPU mean/growth and source map\n";
    }
    for(auto style:{JagaeBaseColor::Black,JagaeBaseColor::White})
        for(auto model:{ModelSelection::Sphere,ModelSelection::Square,ModelSelection::Cylinder})
            for(const auto& eye:{camera,glm::vec3(0,3,0),glm::vec3(0,-3,0)}) {
                const auto normals=render(style,model,21,eye);
                const auto geometry=render(style,model,31,eye);
                const auto rough=render(style,model,22,eye);
                const auto thickness=render(style,model,24,eye);
                for(size_t i=0;i<rough.size();++i)if(rough[i].w>.5f) {
                    normalError=std::max(normalError,double(glm::length(glm::vec3(normals[i]-geometry[i]))));
                    roughMin=std::min(roughMin,rough[i].x);roughMax=std::max(roughMax,rough[i].x);
                    thicknessMin=std::min(thicknessMin,thickness[i].x);thicknessMax=std::max(thicknessMax,thickness[i].x);
                    for(int c=0;c<4;++c)ok=ok&&std::isfinite(normals[i][c])&&std::isfinite(thickness[i][c]);
                    ok=ok&&std::abs(glm::length(glm::vec3(normals[i]))-1)<.00001f;
                }
            }
    for(bool pattern:{false,true})for(int diagnostic:{21,22,23,24,25,34}) {
        const auto black=render(JagaeBaseColor::Black,ModelSelection::Square,diagnostic,camera,pattern);
        const auto white=render(JagaeBaseColor::White,ModelSelection::Square,diagnostic,camera,pattern);
        for(size_t i=0;i<white.size();++i)colorOpticsDifference=std::max(colorOpticsDifference,double(glm::length(black[i]-white[i])));
    }
    const auto blackBody=render(JagaeBaseColor::Black,ModelSelection::Square,20,camera);
    const auto whiteBody=render(JagaeBaseColor::White,ModelSelection::Square,20,camera);
    for(size_t i=0;i<whiteBody.size();++i)if(whiteBody[i].w>.5f)
        bodyRatioError=std::max(bodyRatioError,glm::length(glm::vec3(whiteBody[i])/.82f-glm::vec3(blackBody[i])/.025f));
    for(auto style:{JagaeBaseColor::Black,JagaeBaseColor::White})
        for(auto mode:{JagaeLightMode::Directional,JagaeLightMode::Area})
            for(int diagnostic:{0,20,24,26,27}) {
                const auto original=render(style,ModelSelection::Square,diagnostic,camera,true,false,mode);
                const auto changed=render(style,ModelSelection::Square,diagnostic,camera,true,true,mode);
                for(size_t i=0;i<mask.size();++i)if(mask[i].w>.5f) {
                    const double difference=glm::length(original[i]-changed[i]);
                    if(mask[i].x==0) {
                        outsideMutation=std::max(outsideMutation,difference);
                        // Outside: opposite unlit display color, nominal diagnostic thickness.
                        ok=ok&&mask[i].y==0&&mask[i].z==impl_->material.plateletThicknessNm;
                        if(diagnostic==20||diagnostic==0) {
                            const float expectedBackground=style==JagaeBaseColor::Black
                                ?JagaePatternLightBackground:JagaePatternDarkBackground;
                            ok=ok&&glm::vec3(original[i])==glm::vec3(expectedBackground);
                        }
                        if(diagnostic==26||diagnostic==27)ok=ok&&glm::vec3(original[i])==glm::vec3(0);
                        if(diagnostic==0)++outside;
                    } else if(mask[i].x>.99f) {
                        insideMutation=std::max(insideMutation,difference);if(diagnostic==0)++inside;
                    } else if(diagnostic==0)++filteredEdges;
                }
            }
    // Outside spectra are disabled in both direct and indirect budgets:
    // arbitrary thickness offset changes have zero observable effect there.
    double outsideOptical=0,outsideBackgroundError=0;
    const auto reflections=render(JagaeBaseColor::White,ModelSelection::Square,35,camera,true);
    for(size_t i=0;i<mask.size();++i)if(mask[i].w>.5f&&mask[i].x==0)
        outsideBackgroundError=std::max(outsideBackgroundError,
            double(glm::length(glm::vec3(reflections[i])-glm::vec3(JagaePatternDarkBackground))));
    for(int diagnostic:{26,27}) {
        const auto original=render(JagaeBaseColor::White,ModelSelection::Square,diagnostic,camera,true);
        const auto disabledOffset=render(JagaeBaseColor::White,ModelSelection::Square,diagnostic,camera,true,false,
            JagaeLightMode::Directional,1,true,true);
        for(size_t i=0;i<mask.size();++i)if(mask[i].w>.5f&&mask[i].x==0)
            outsideOptical=std::max(outsideOptical,double(glm::length(original[i]-disabledOffset[i])));
    }
    // Actual output pipeline endpoint audit; coverage is blended before sRGB encoding.
    PostProcess endpointPost;
    const EnvironmentPreset savedEnvironment=impl_->environmentPreset;
    double coverageError=0,outsideUnlitError=0,patternRestore=0;
    const auto expectedEncodedByte=[](float linear) {
        const float encoded=linear<=0.0031308f?12.9232102f*linear:1.055f*std::pow(linear,1.0f/2.4f)-0.055f;
        return static_cast<unsigned char>(std::lround(encoded*255.0f));
    };
    const unsigned char expectedLightByte=expectedEncodedByte(JagaePatternLightBackground);
    const unsigned char expectedDarkByte=expectedEncodedByte(JagaePatternDarkBackground);
    size_t encodedOutside=0,edgeCoverage=0;
    std::string environmentError;
    for(auto preset:{EnvironmentPreset::Studio,EnvironmentPreset::UnityReference}) {
        if(!const_cast<JagaeScene*>(this)->setEnvironment(preset,environmentError)) {
            output<<"Optional Unity unlit-background audit skipped: "<<environmentError<<'\n';continue;
        }
        for(auto style:{JagaeBaseColor::Black,JagaeBaseColor::White})
            for(auto model:{ModelSelection::Sphere,ModelSelection::Square,ModelSelection::Cylinder})
                for(auto mode:{JagaeLightMode::Directional,JagaeLightMode::Area})
                    for(const auto& eye:{camera,glm::vec3(0,3,0)}) {
                        const glm::vec3 background(style==JagaeBaseColor::Black
                            ?JagaePatternLightBackground:JagaePatternDarkBackground);
                        const auto coverage=render(style,model,34,eye,true,false,mode);
                        const auto inlay=render(style,model,0,eye,false,false,mode);
                        const auto composed=render(style,model,0,eye,true,false,mode);
                        endpointPost.render(target,size,size,OutputMode::LinearScene,PostSettings{});
                        endpointPost.bindDisplayForRead();
                        std::vector<unsigned char> encoded(size*size*4);
                        glReadPixels(0,0,size,size,GL_RGBA,GL_UNSIGNED_BYTE,encoded.data());
                        glBindFramebuffer(GL_FRAMEBUFFER,fbo);glViewport(0,0,size,size);
                        const auto changed=render(style,model,0,eye,true,true,mode,1,false);
                        const auto restored=render(style,model,0,eye,false,false,mode);
                        for(size_t i=0;i<coverage.size();++i)if(coverage[i].w>.5f) {
                            const float alphaValue=coverage[i].x;
                            const glm::vec3 expected=glm::mix(background,glm::vec3(inlay[i]),alphaValue);
                            coverageError=std::max(coverageError,double(glm::length(glm::vec3(composed[i])-expected)));
                            patternRestore=std::max(patternRestore,double(glm::length(restored[i]-inlay[i])));
                            if(alphaValue>0&&alphaValue<1)++edgeCoverage;
                            if(alphaValue==0) {
                                outsideUnlitError=std::max(outsideUnlitError,double(glm::length(glm::vec3(composed[i])-background)));
                                outsideUnlitError=std::max(outsideUnlitError,double(glm::length(glm::vec3(changed[i])-background)));
                                const unsigned char expectedByte=style==JagaeBaseColor::Black
                                    ?expectedLightByte:expectedDarkByte;
                                for(int c=0;c<3;++c)ok=ok&&encoded[4*i+c]==expectedByte;
                                ++encodedOutside;
                            }
                        }
                    }
    }
    ok=ok&&const_cast<JagaeScene*>(this)->setEnvironment(savedEnvironment,environmentError)
        &&expectedLightByte==231&&expectedDarkByte==44
        &&outsideUnlitError==0&&coverageError<.000002&&patternRestore==0
        &&encodedOutside>1000&&edgeCoverage>1000;
    output<<"Unlit opposite background GPU: raw endpoint error="<<outsideUnlitError
        <<" encoded expected light/dark bytes="<<static_cast<int>(expectedLightByte)<<'/'<<static_cast<int>(expectedDarkByte)
        <<" outside samples="<<encodedOutside<<" fractional coverage samples="<<edgeCoverage
        <<" linear final-color coverage error="<<coverageError<<" Pattern Off restore="<<patternRestore
        <<"; both colors/meshes/cap views/light modes/Studio+available Unity, texture mutations and coat toggles\n";
    const auto directional=render(JagaeBaseColor::White,ModelSelection::Sphere,27,camera,false,false,JagaeLightMode::Directional,.4f);
    const auto oldArea=render(JagaeBaseColor::White,ModelSelection::Sphere,27,camera,false,false,JagaeLightMode::Area,.56f);
    const auto newArea=render(JagaeBaseColor::White,ModelSelection::Sphere,27,camera,false,false,JagaeLightMode::Area,.672f);
    const auto restored=render(JagaeBaseColor::White,ModelSelection::Sphere,27,camera,false,false,JagaeLightMode::Directional,.4f);
    double oldEnergy=0,newEnergy=0,restoreError=0;float gainError=0;
    for(size_t i=0;i<newArea.size();++i) {
        oldEnergy+=glm::dot(glm::vec3(oldArea[i]),glm::vec3(.2126,.7152,.0722));
        newEnergy+=glm::dot(glm::vec3(newArea[i]),glm::vec3(.2126,.7152,.0722));
        restoreError=std::max(restoreError,double(glm::length(directional[i]-restored[i])));
        gainError=std::max(gainError,glm::length(glm::vec3(newArea[i])-1.2f*glm::vec3(oldArea[i])));
    }
    double coatRestore=0;
    for(auto style:{JagaeBaseColor::Black,JagaeBaseColor::White})
        for(auto model:{ModelSelection::Sphere,ModelSelection::Square,ModelSelection::Cylinder})
            for(auto mode:{JagaeLightMode::Directional,JagaeLightMode::Area})for(bool pattern:{false,true}) {
                const auto on=render(style,model,0,camera,pattern,false,mode);
                const auto off=render(style,model,0,camera,pattern,false,mode,1,false);
                const auto back=render(style,model,0,camera,pattern,false,mode);
                double delta=0;
                for(size_t i=0;i<on.size();++i) {
                    delta+=glm::length(on[i]-off[i]);
                    coatRestore=std::max(coatRestore,double(glm::length(on[i]-back[i])));
                    for(int c=0;c<3;++c)ok=ok&&std::isfinite(on[i][c])&&on[i][c]>=0;
                }
                ok=ok&&delta>0;
            }
    ok=ok&&outside>1000&&inside>1000&&filteredEdges>0&&outsideMutation==0&&insideMutation>.01
        &&outsideOptical==0&&outsideBackgroundError==0&&colorOpticsDifference==0&&normalError==0&&roughMin==roughMax
        &&std::abs(roughMin-.22f)<.000001f&&thicknessMin>=150&&thicknessMax<=600
        &&bodyRatioError<.000001f&&gainError<.000001f&&oldEnergy>0
        &&std::abs(newEnergy/oldEnergy-1.2)<.000001&&restoreError==0&&coatRestore==0;
    output<<"Shared Black/White GPU: optical/normal/roughness/strength diagnostic difference="<<colorOpticsDifference
        <<" mesh normal difference="<<normalError<<" roughness="<<roughMin<<".."<<roughMax
        <<" thickness="<<thicknessMin<<".."<<thicknessMax<<" tint-only normalized body error="<<bodyRatioError<<'\n';
    output<<"Pattern linear-alpha GPU: format="<<patternFormat<<" dimensions="<<patternWidth<<'x'<<patternHeight
        <<" outside/inside/filtered-edge samples="<<outside<<'/'<<inside<<'/'<<filteredEdges
        <<" actual base+thickness texture mutation outside max="<<outsideMutation<<" inside max="<<insideMutation
        <<" outside optical-offset direct/indirect max="<<outsideOptical
        <<" White-selected dark-background display error="<<outsideBackgroundError<<'\n';
    output<<"Area .56->.672 GPU direct-only ratio="<<newEnergy/oldEnergy<<" max scaling error="<<gainError
        <<" Directional .4 restore="<<restoreError<<" coat restore="<<coatRestore<<'\n';
    glDeleteTextures(2,mutations);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(drawFbo));
    glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(readFbo));
    glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&target);glDeleteRenderbuffers(1,&depthTarget);
    glBindRenderbuffer(GL_RENDERBUFFER,static_cast<GLuint>(depthBinding));
    glActiveTexture(static_cast<GLenum>(active));glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(binding));
    glUseProgram(static_cast<GLuint>(program));glBindVertexArray(static_cast<GLuint>(vao));
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);glClearColor(clear[0],clear[1],clear[2],clear[3]);
    if(depth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    if(blend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    if(scissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    return ok&&glGetError()==GL_NO_ERROR;
}

bool JagaeScene::thicknessBenchmark(std::ostream& output, bool optics, bool continuous, bool area) const
{
    // Startup-only bounded benchmark: real meshes, legacy thickness texture,
    // selected environment, cream body, facets and full production BRDF/IBL.
    GLint oldDraw=0,oldRead=0,viewport[4]{},active=0,binding=0,renderbuffer=0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&oldDraw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&oldRead);
    glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
    glActiveTexture(GL_TEXTURE0);glGetIntegerv(GL_TEXTURE_BINDING_2D,&binding);
    glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);
    const GLboolean oldDepth=glIsEnabled(GL_DEPTH_TEST),oldBlend=glIsEnabled(GL_BLEND),oldScissor=glIsEnabled(GL_SCISSOR_TEST);
    GLuint fbo=0,color=0,depth=0,query=0;
    glGenFramebuffers(1,&fbo);glGenTextures(1,&color);glGenRenderbuffers(1,&depth);glGenQueries(1,&query);
    glBindTexture(GL_TEXTURE_2D,color);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA16F,900,650,0,GL_RGBA,GL_FLOAT,nullptr);
    glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(binding));
    glBindRenderbuffer(GL_RENDERBUFFER,depth);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,900,650);
    glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,color,0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,depth);
    bool ok=glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    glViewport(0,0,900,650);glEnable(GL_DEPTH_TEST);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);
    const glm::vec3 eye=glm::normalize(glm::vec3(3.02f-6.072351f,2.14f-.992f,-1.97f-2.207f))*2.25f;
    const glm::mat4 vp=glm::perspective(glm::radians(42.0f),900.0f/650.0f,.01f,100.0f)*glm::lookAt(eye,glm::vec3(0),glm::vec3(0,1,0));
    const DirectionalLight light;
    for(ModelSelection model:{ModelSelection::Sphere,ModelSelection::Square}) {
        std::vector<double> baseline,feature;
        const auto frame=[&](bool reference) {
            glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
            draw(vp,eye,0,model,light,JagaeBaseColor::Nacre,(optics||continuous||area)?false:reference,
                -1,true,false,optics&&reference,continuous&&reference,false,
                area&&!reference?JagaeLightMode::Area:JagaeLightMode::Directional,area?(reference?.4f:.672f):1.0f);
        };
        // Warm both branches before trials; alternate their order to reduce drift.
        for(int i=0;i<32;++i) frame(i%2==0);
        glFinish();
        for(int trial=0;trial<7;++trial) for(int pass=0;pass<2;++pass) {
            const bool reference=(trial+pass)%2==0;
            for(int i=0;i<8;++i) frame(reference);
            glFinish();glBeginQuery(GL_TIME_ELAPSED,query);
            for(int i=0;i<40;++i) frame(reference);
            glEndQuery(GL_TIME_ELAPSED);GLuint64 ns=0;glGetQueryObjectui64v(query,GL_QUERY_RESULT,&ns);
            const double ms=static_cast<double>(ns)/40e6;ok=ok&&std::isfinite(ms)&&ms>0;
            (reference?baseline:feature).push_back(ms);
        }
        std::sort(baseline.begin(),baseline.end());std::sort(feature.begin(),feature.end());
        output<<(area?"Directional .4 vs rectangle .672 GPU full production ":continuous?"Continuous vs mosaic GPU full production ":optics?"Coherent thickness GPU full production ":"Cell thickness GPU full production ")<<(model==ModelSelection::Sphere?"sphere":"square")
            <<" 900x650 RGBA16F: baseline median="<<baseline[3]<<" ms ["<<baseline.front()<<','<<baseline.back()
            <<"] feature median="<<feature[3]<<" ms ["<<feature.front()<<','<<feature.back()<<"] delta="
            <<100*(feature[3]/baseline[3]-1)<<"%; 7 alternating trials x40 frames, 32 initial/8 per-trial warmup, clear+draw/no post/swap\n";
    }
    output<<"Timing uses one production shader with a runtime reference flag; compilation/cache/scheduling noise can hide small arithmetic costs.\n";
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(oldDraw));glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(oldRead));
    glDeleteQueries(1,&query);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&color);glDeleteRenderbuffers(1,&depth);
    glBindRenderbuffer(GL_RENDERBUFFER,static_cast<GLuint>(renderbuffer));glActiveTexture(static_cast<GLenum>(active));
    glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
    if(oldDepth)glEnable(GL_DEPTH_TEST);else glDisable(GL_DEPTH_TEST);
    if(oldBlend)glEnable(GL_BLEND);else glDisable(GL_BLEND);
    if(oldScissor)glEnable(GL_SCISSOR_TEST);else glDisable(GL_SCISSOR_TEST);
    return ok&&glGetError()==GL_NO_ERROR;
}

void JagaeScene::draw(const glm::mat4& viewProjection, const glm::vec3& cameraWorld,
                      float seconds, ModelSelection model, const DirectionalLight& light,
                      JagaeBaseColor baseColor, bool thicknessBaseline, float nacreMixTest, bool clearCoatEnabled,
                      bool detailBaseline, bool opticsBaseline, bool continuousBaseline, bool waveBaseline,
                      JagaeLightMode lightMode, float lightGain, bool smoothBaseline, bool patternEnabled) const
{
    impl_->whiteStudioSelected=baseColor!=JagaeBaseColor::Black;
    GLint previousProgram = 0;
    GLint previousVertexArray = 0;
    GLint previousActiveTexture = 0;
    GLint previousTexture0 = 0;
    GLint previousTexture1 = 0;
    GLint previousCube2 = 0;
    GLint previousAlbedo3 = 0;
    GLint previousPattern4 = 0;
    const GLboolean previousSeamless = glIsEnabled(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture1);
    glActiveTexture(GL_TEXTURE2);
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP, &previousCube2);
    glActiveTexture(GL_TEXTURE3);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousAlbedo3);
    glActiveTexture(GL_TEXTURE4);glGetIntegerv(GL_TEXTURE_BINDING_2D,&previousPattern4);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

    impl_->shader.use();
    const glm::mat4 modelMatrix(1.0f);
    impl_->shader.set("uNacreTestMode", 0);
    impl_->shader.set("uNacreTestSourceThicknessNm", -1.0f);
    impl_->shader.set("uNacreThicknessBaseline", thicknessBaseline ? 1 : 0);
    impl_->shader.set("uNacreDetailBaseline", detailBaseline ? 1 : 0);
    impl_->shader.set("uNacreOpticsBaseline", opticsBaseline ? 1 : 0);
    impl_->shader.set("uNacreContinuousBaseline", continuousBaseline ? 1 : 0);
    impl_->shader.set("uNacreWaveBaseline", waveBaseline ? 1 : 0);
    impl_->shader.set("uNacreSmoothBaseline", smoothBaseline ? 1 : 0);
    impl_->shader.set("uPatternEnabled",patternEnabled?1:0);
    const float patternBackground=baseColor==JagaeBaseColor::Black
        ?JagaePatternLightBackground:JagaePatternDarkBackground;
    impl_->shader.set("uPatternBackground",glm::vec3(patternBackground));
    impl_->shader.set("uLightMode",static_cast<int>(lightMode));
    const JagaeRectangleLight rectangle(light);
    impl_->shader.set("uAreaCenter",rectangle.center);impl_->shader.set("uAreaNormal",rectangle.normal);
    impl_->shader.set("uAreaHalfU",rectangle.halfU);impl_->shader.set("uAreaHalfV",rectangle.halfV);
    glUniform3fv(glGetUniformLocation(impl_->shader.handle(),"uAreaPoints[0]"),9,glm::value_ptr(rectangle.points[0]));
    glUniform1fv(glGetUniformLocation(impl_->shader.handle(),"uAreaPatchAreas[0]"),9,rectangle.patchAreas.data());
    impl_->shader.set("uBaseColorMode", static_cast<int>(baseColor));
    impl_->shader.set("uModel", modelMatrix);
    impl_->shader.set("uViewProjection", viewProjection);
    impl_->shader.set("uCameraWorld", cameraWorld);
    impl_->shader.set("uLightDirection", light.direction());
    impl_->shader.set("uLightRadiance", light.radiance()*lightGain);
    impl_->shader.set("uTime", seconds);

    const JagaeMaterial& material = impl_->material;
    impl_->baseMap.bind(GL_TEXTURE0);
    impl_->thicknessMap.bind(GL_TEXTURE1);
    impl_->shader.set("uBaseMap", 0);
    impl_->shader.set("uThicknessMap", 1);
    impl_->patternMap.bind(GL_TEXTURE4);impl_->shader.set("uPatternMask",4);
    impl_->environment().bind(GL_TEXTURE2);
    impl_->shader.set("uEnvironmentMap", 2);
    impl_->environment().bindAlbedo(GL_TEXTURE3);
    impl_->shader.set("uClearCoatAlbedo",3);
    impl_->shader.set("uEnvironmentMaxMip", impl_->environment().maxMip());
    const auto& sh = impl_->environment().diffuseSH();
    glUniform3fv(glGetUniformLocation(impl_->shader.handle(), "uDiffuseSH[0]"), 9,
                 glm::value_ptr(sh[0]));
    impl_->shader.set("uBaseColor", material.baseColor);
    impl_->shader.set("uBaseMapST", material.baseMapST);
    impl_->shader.set("uThicknessMapST", material.thicknessMapST);
    impl_->shader.set("uPlateletThicknessNm", material.plateletThicknessNm);
    impl_->shader.set("uPlateletIOR", material.plateletIOR);
    impl_->shader.set("uOrganicIOR", material.substrateIOR);
    impl_->shader.set("uLayerCount", material.layerCount);
    impl_->shader.set("uOrganicThicknessNm", material.interlayerThicknessNm);
    // Explicit bright-body appearance presets; same convex, energy-bounded BSDF.
    const float strength = nacreMixTest>=0.0f?nacreMixTest:JagaeNacreStructuralMix;
    impl_->shader.set("uIridescenceStrength", strength);
    impl_->shader.set("uThicknessVariationNm", material.thicknessVariationNm);
    impl_->shader.set("uSmoothness", material.smoothness);
    impl_->shader.set("uClearCoatMask", clearCoatEnabled ? material.clearCoatMask : 0.0f);
    impl_->shader.set("uClearCoatSmoothness", material.clearCoatSmoothness);
    switch (model)
    {
    case ModelSelection::Square: impl_->square.draw(); break;
    case ModelSelection::Cylinder: impl_->cylinder.draw(); break;
    case ModelSelection::Sphere: impl_->sphere.draw(); break;
    }

    glBindVertexArray(static_cast<GLuint>(previousVertexArray));
    glUseProgram(static_cast<GLuint>(previousProgram));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture0));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousTexture1));
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_CUBE_MAP, static_cast<GLuint>(previousCube2));
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(previousAlbedo3));
    glActiveTexture(GL_TEXTURE4);glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(previousPattern4));
    if (!previousSeamless) glDisable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
    glActiveTexture(static_cast<GLenum>(previousActiveTexture));
}
