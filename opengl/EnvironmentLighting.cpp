#include "EnvironmentLighting.h"
#include "ClearCoatAlbedo.h"
#include <algorithm>
#include <cmath>
#include <ostream>
#include <fstream>
#include <stdexcept>
#include <cstdint>

namespace
{
constexpr float Pi = 3.14159265358979323846f;
constexpr int CubeSize = 64, MipCount = 7, Samples = 128;
struct CapturedSky
{
    int size=0;
    std::vector<glm::vec3> pixels;
    CapturedSky()
    {
        std::ifstream file("assets/UnityDefaultSky.f32cube",std::ios::binary);
        std::uint32_t magic=0;
        file.read(reinterpret_cast<char*>(&magic),4);
        file.read(reinterpret_cast<char*>(&size),4);
        if(!file || magic!=0x31424355 || size!=128)
            throw std::runtime_error("Missing/invalid Unity HDR sky capture: assets/UnityDefaultSky.f32cube");
        pixels.resize(static_cast<size_t>(6)*size*size);
        for(auto& p:pixels) for(int c=0;c<3;++c)
        {
            file.read(reinterpret_cast<char*>(&p[c]),4);
            if(!file || !std::isfinite(p[c]) || p[c]<0)
                throw std::runtime_error("Invalid Unity HDR sky radiance");
        }
    }
    glm::vec3 sample(const glm::vec3& d) const
    {
        const glm::vec3 a=glm::abs(d);
        int face; float u,v,major;
        if(a.x>=a.y && a.x>=a.z)
        { major=a.x; face=d.x>0?0:1; u=d.x>0?-d.z:d.z; v=-d.y; }
        else if(a.y>=a.z)
        { major=a.y; face=d.y>0?2:3; u=d.x; v=d.y>0?d.z:-d.z; }
        else
        { major=a.z; face=d.z>0?4:5; u=d.z>0?d.x:-d.x; v=-d.y; }
        const float x=(u/major+1)*.5f*size-.5f, y=(v/major+1)*.5f*size-.5f;
        const int x0=static_cast<int>(std::floor(x)), y0=static_cast<int>(std::floor(y));
        const auto fetch=[&](int px,int py) {return pixels[(static_cast<size_t>(face)*size+
            std::clamp(py,0,size-1))*size+std::clamp(px,0,size-1)];};
        return glm::mix(glm::mix(fetch(x0,y0),fetch(x0+1,y0),x-x0),
            glm::mix(fetch(x0,y0+1),fetch(x0+1,y0+1),x-x0),y-y0);
    }
};
glm::vec3 studioRadiance(const glm::vec3& direction,float fill)
{
    // Infinite-distance angular softboxes, fixed in world space, without a sun.
    const auto panel=[&](glm::vec3 center,float width,float height,float peak) {
        center=glm::normalize(center);
        const float forward=glm::dot(direction,center);
        if(forward<=0) return 0.0f;
        const glm::vec3 right=glm::normalize(glm::cross(glm::vec3(0,1,0),center));
        const glm::vec3 up=glm::cross(center,right);
        const float x=std::atan2(glm::dot(direction,right),forward)/width;
        const float y=std::atan2(glm::dot(direction,up),forward)/height;
        return peak*std::exp(-(x*x*x*x+y*y*y*y));
    };
    return glm::vec3(fill + panel(glm::vec3(-.35f,.65f,-.68f),.48f,.36f,.9f)
        + panel(glm::vec3(.72f,.28f,.64f),.24f,.58f,.45f));
}

glm::vec3 environmentRadiance(EnvironmentPreset preset,const glm::vec3& direction,float fill)
{
    if(preset==EnvironmentPreset::Studio) return studioRadiance(direction,fill);
    static const CapturedSky sky;
    return sky.sample(direction);
}

std::array<float, 9> basis(const glm::vec3& n)
{
    return {0.282094792f, 0.488602512f*n.y, 0.488602512f*n.z,
        0.488602512f*n.x, 1.092548431f*n.x*n.y, 1.092548431f*n.y*n.z,
        0.315391565f*(3*n.z*n.z-1), 1.092548431f*n.x*n.z,
        0.546274215f*(n.x*n.x-n.y*n.y)};
}

glm::vec3 cubeDirection(int face, float u, float v)
{
    switch (face)
    {
    case 0: return glm::normalize(glm::vec3(1,-v,-u));
    case 1: return glm::normalize(glm::vec3(-1,-v,u));
    case 2: return glm::normalize(glm::vec3(u,1,v));
    case 3: return glm::normalize(glm::vec3(u,-1,-v));
    case 4: return glm::normalize(glm::vec3(u,-v,1));
    default:return glm::normalize(glm::vec3(-u,-v,-1));
    }
}

float radicalInverse(unsigned int bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<float>(bits) * 2.3283064365386963e-10f;
}

glm::vec3 prefilter(EnvironmentPreset preset,const glm::vec3& n, float perceptualRoughness,float fill)
{
    if (perceptualRoughness < 0.001f) return environmentRadiance(preset,n,fill);
    const float alpha = perceptualRoughness * perceptualRoughness;
    const glm::vec3 up = std::abs(n.z) < 0.999f ? glm::vec3(0,0,1) : glm::vec3(1,0,0);
    const glm::vec3 tangent = glm::normalize(glm::cross(up, n));
    const glm::vec3 bitangent = glm::cross(n, tangent);
    glm::vec3 total(0);
    float weight = 0;
    for (unsigned int i = 0; i < Samples; ++i)
    {
        const float phi = 2*Pi * static_cast<float>(i)/Samples;
        const float xi = radicalInverse(i);
        const float cosTheta = std::sqrt((1-xi)/(1+(alpha*alpha-1)*xi));
        const float sinTheta = std::sqrt(std::max(0.0f,1-cosTheta*cosTheta));
        const glm::vec3 h = tangent*(std::cos(phi)*sinTheta) +
            bitangent*(std::sin(phi)*sinTheta) + n*cosTheta;
        const glm::vec3 l = glm::normalize(2*glm::dot(n,h)*h-n);
        const float ndotl = std::max(glm::dot(n,l),0.0f);
        total += environmentRadiance(preset,l,fill)*ndotl;
        weight += ndotl;
    }
    return total/std::max(weight,0.00001f);
}

glm::vec3 evaluateSHRaw(const std::array<glm::vec3,9>& sh, const glm::vec3& n)
{
    const auto b = basis(n);
    glm::vec3 result(0);
    for (int i=0;i<9;++i) result += sh[i]*b[i];
    return result;
}
}

EnvironmentLighting::EnvironmentLighting(EnvironmentPreset preset,float studioFill) : preset_(preset),studioFill_(studioFill)
{
    if(!std::isfinite(studioFill_)||studioFill_<0) throw std::invalid_argument("Invalid Studio fill radiance");
    // Validate a lazy reference before allocating any GL resource.
    environmentRadiance(preset_,glm::vec3(0,1,0),studioFill_);
    albedo_=ClearCoatAlbedo::createTexture();
    // Fibonacci quadrature projects radiance, then cosine convolution / PI gives
    // irradiance in the SampleSH convention used by URP's diffuse BRDF.
    constexpr int Count=4096;
    for (int i=0;i<Count;++i)
    {
        const float y=1-2*(static_cast<float>(i)+0.5f)/Count;
        const float phi=2.39996323f*static_cast<float>(i);
        const float r=std::sqrt(std::max(0.0f,1-y*y));
        const glm::vec3 n(r*std::cos(phi),y,r*std::sin(phi));
        const auto b=basis(n);
        const glm::vec3 radiance=environmentRadiance(preset_,n,studioFill_);
        for(int k=0;k<9;++k) sh_[k] += radiance*(b[k]*4*Pi/Count);
    }
    for(int k=0;k<9;++k) sh_[k] *= k==0 ? 1.0f : k<4 ? 2.0f/3.0f : 0.25f;

    GLint previous=0;
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP,&previous);
    glGenTextures(1,&cube_);
    glBindTexture(GL_TEXTURE_CUBE_MAP,cube_);
    for(int mip=0;mip<MipCount;++mip)
    {
        const int size=CubeSize>>mip;
        // Invert URP perceptualRoughness*(1.7-.7*perceptualRoughness)*maxMip.
        const float t=static_cast<float>(mip)/(MipCount-1);
        const float pr=(1.7f-std::sqrt(2.89f-2.8f*t))/1.4f;
        double sum=0;
        for(int face=0;face<6;++face)
        {
            std::vector<float> pixels(static_cast<size_t>(size)*size*3);
            for(int y=0;y<size;++y) for(int x=0;x<size;++x)
            {
                const glm::vec3 n=cubeDirection(face,2*(x+0.5f)/size-1,2*(y+0.5f)/size-1);
                const glm::vec3 color=prefilter(preset_,n,pr,studioFill_);
                const size_t offset=(static_cast<size_t>(y)*size+x)*3;
                for(int c=0;c<3;++c) { pixels[offset+c]=color[c]; sum+=color[c]; }
            }
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,mip,GL_RGB16F,
                size,size,0,GL_RGB,GL_FLOAT,pixels.data());
        }
        mipMeans_[mip]=sum/(6*size*size*3);
    }
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_WRAP_R,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAX_LEVEL,MipCount-1);
    glBindTexture(GL_TEXTURE_CUBE_MAP,static_cast<GLuint>(previous));
}
EnvironmentLighting::~EnvironmentLighting() { if(cube_) glDeleteTextures(1,&cube_); if(albedo_) glDeleteTextures(1,&albedo_); }
void EnvironmentLighting::bind(GLenum unit) const
{ glActiveTexture(unit); glBindTexture(GL_TEXTURE_CUBE_MAP,cube_); }
void EnvironmentLighting::bindAlbedo(GLenum unit) const
{ glActiveTexture(unit); glBindTexture(GL_TEXTURE_2D,albedo_); }

bool EnvironmentLighting::selfTest(std::ostream& output) const
{
    bool ok=true;
    const glm::vec3 sourceSun(-.243612927f,.791856796f,-.56001389f);
    const glm::vec3 sunRadiance=environmentRadiance(preset_,sourceSun,studioFill_);
    const glm::vec3 oppositeRadiance=environmentRadiance(preset_,-sourceSun,studioFill_);
    // Capture orientation must put the procedural sun at the source world ray.
    if(preset_==EnvironmentPreset::UnityReference)
        ok=ok&&glm::length(sunRadiance)>10 && glm::length(sunRadiance)>glm::length(oppositeRadiance)*10;
    for(const auto& c:sh_) for(int k=0;k<3;++k) ok=ok&&std::isfinite(c[k]);
    const glm::vec3 up=evaluateSHRaw(sh_,glm::vec3(0,1,0));
    const glm::vec3 down=evaluateSHRaw(sh_,glm::vec3(0,-1,0));
    ok=ok&&glm::length(up-down)>0.01f;
    GLint previous=0;
    glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP,&previous);
    glBindTexture(GL_TEXTURE_CUBE_MAP,cube_);
    for(int mip=0;mip<MipCount;++mip) for(int face=0;face<6;++face)
    {
        GLint width=0,format=0;
        glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,mip,GL_TEXTURE_WIDTH,&width);
        glGetTexLevelParameteriv(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,mip,GL_TEXTURE_INTERNAL_FORMAT,&format);
        ok=ok&&width==(CubeSize>>mip)&&format==GL_RGB16F;
        std::vector<float> pixels(static_cast<size_t>(width)*width*3);
        if(width>0) glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,mip,GL_RGB,GL_FLOAT,pixels.data());
        for(float value:pixels) ok=ok&&std::isfinite(value)&&value>=0;
        if(preset_==EnvironmentPreset::Studio) for(size_t i=0;i<pixels.size();i+=3)
            ok=ok&&pixels[i]==pixels[i+1]&&pixels[i]==pixels[i+2]&&pixels[i]<=studioFill_+1.35f;
    }
    glBindTexture(GL_TEXTURE_CUBE_MAP,static_cast<GLuint>(previous));
    ok=ok&&std::abs(mipMeans_[0]-mipMeans_[6])>0.0001;
    if(preset_==EnvironmentPreset::Studio) {
        float rawMinimum=1e9f,rawMaximum=0,sourceMinimum=1e9f,sourceMaximum=0;
        double cosineError=0,whiteNormalizationError=0;
        std::array<glm::vec3,9> whiteSH{};
        constexpr int count=16384;
        const auto sampleDirection=[](int i) {
            const float y=1-2*(static_cast<float>(i)+.5f)/count;
            const float phi=2.39996323f*static_cast<float>(i),r=std::sqrt(std::max(0.0f,1-y*y));
            return glm::vec3(r*std::cos(phi),y,r*std::sin(phi));
        };
        for(int i=0;i<count;++i) {
            const glm::vec3 d=sampleDirection(i),source=studioRadiance(d,studioFill_);
            const auto b=basis(d);
            for(int k=0;k<9;++k) whiteSH[k]+=glm::vec3(b[k]*4*Pi/count);
            const glm::vec3 raw=evaluateSHRaw(sh_,d);
            rawMinimum=std::min(rawMinimum,raw.x);rawMaximum=std::max(rawMaximum,raw.x);
            sourceMinimum=std::min(sourceMinimum,source.x);sourceMaximum=std::max(sourceMaximum,source.x);
            ok=ok&&std::isfinite(source.x)&&source.x>=studioFill_&&source.x<=studioFill_+1.35f
                &&source.x==source.y&&source.x==source.z&&raw.x==raw.y&&raw.x==raw.z;
        }
        for(int k=0;k<9;++k) whiteSH[k]*=k==0?1.0f:k<4?2.0f/3.0f:.25f;
        for(glm::vec3 n:{glm::vec3(1,0,0),glm::vec3(0,1,0),glm::vec3(0,0,1),glm::normalize(glm::vec3(-.35f,.65f,-.68f)),glm::vec3(0,-1,0)}) {
            double integral=0;
            for(int i=0;i<count;++i) { const auto d=sampleDirection(i); integral+=studioRadiance(d,studioFill_).x*std::max(glm::dot(n,d),0.0f)*4.0/count; }
            cosineError=std::max(cosineError,std::abs(integral-evaluateSHRaw(sh_,n).x));
            whiteNormalizationError=std::max(whiteNormalizationError,std::abs(1.0-double(evaluateSHRaw(whiteSH,n).x)));
        }
        const float key=studioRadiance(glm::normalize(glm::vec3(-.35f,.65f,-.68f)),studioFill_).x;
        const float opposite=studioRadiance(-glm::normalize(glm::vec3(-.35f,.65f,-.68f)),studioFill_).x;
        ok=ok&&rawMinimum>=0&&rawMaximum>rawMinimum+.01f&&key>opposite+.5f
            &&cosineError<.025&&whiteNormalizationError<.001;
        output<<"Studio neutral fixed angular panels: source="<<sourceMinimum<<".."<<sourceMaximum
            <<" raw SH="<<rawMinimum<<".."<<rawMaximum<<" cosine reference max error="<<cosineError
            <<" white irradiance/PI normalization error="<<whiteNormalizationError<<'\n';
    }
    output<<(preset_==EnvironmentPreset::Studio?"Studio procedural":"Unity reference captured")<<" HDR/SH9/GGX 64->1 mip validation "<<(ok?"passed":"FAILED")
        <<"; diffuse up="<<up.x<<","<<up.y<<","<<up.z
        <<" down="<<down.x<<","<<down.y<<","<<down.z<<'\n';
    return ok;
}
