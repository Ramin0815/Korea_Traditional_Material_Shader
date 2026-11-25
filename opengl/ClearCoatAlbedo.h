#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

// Directional hemispherical albedo of the production single-scattering GGX.
// VNDF integration avoids losing narrow lobes and is bounded sample-by-sample.
namespace ClearCoatAlbedo {
constexpr int Width=128, Height=32;
constexpr double Pi=3.14159265358979323846;
constexpr double Guard=.005; // Absorptive numerical margin, not multiscatter compensation.
inline double fresnel(double c,double eta=1.5) {
    c=std::clamp(c,0.,1.);double ct=std::sqrt(std::max(0.,1-(1-c*c)/(eta*eta)));
    double rs=(c-eta*ct)/std::max(c+eta*ct,1e-12),rp=(eta*c-ct)/std::max(eta*c+ct,1e-12);
    return .5*(rs*rs+rp*rp);
}
inline double g1(double c,double alpha) {
    if(c<=0)return 0;return 2*c/(c+std::sqrt(alpha*alpha+(1-alpha*alpha)*c*c));
}
inline double radical(unsigned bits) {
    bits=(bits<<16u)|(bits>>16u);bits=((bits&0x55555555u)<<1u)|((bits&0xAAAAAAAAu)>>1u);
    bits=((bits&0x33333333u)<<2u)|((bits&0xCCCCCCCCu)>>2u);
    bits=((bits&0x0F0F0F0Fu)<<4u)|((bits&0xF0F0F0F0u)>>4u);
    bits=((bits&0x00FF00FFu)<<8u)|((bits&0xFF00FF00u)>>8u);return bits*2.3283064365386963e-10;
}
inline glm::dvec3 visibleNormal(glm::dvec3 v,double alpha,double u,double w) {
    glm::dvec3 vh=glm::normalize(glm::dvec3(alpha*v.x,alpha*v.y,v.z));
    double len=vh.x*vh.x+vh.y*vh.y;
    glm::dvec3 t1=len>0?glm::dvec3(-vh.y,vh.x,0)/std::sqrt(len):glm::dvec3(1,0,0);
    glm::dvec3 t2=glm::cross(vh,t1);
    double r=std::sqrt(u),phi=2*Pi*w,x=r*std::cos(phi),y=r*std::sin(phi),s=.5*(1+vh.z);
    y=(1-s)*std::sqrt(std::max(0.,1-x*x))+s*y;
    glm::dvec3 nh=x*t1+y*t2+std::sqrt(std::max(0.,1-x*x-y*y))*vh;
    return glm::normalize(glm::dvec3(alpha*nh.x,alpha*nh.y,std::max(0.,nh.z)));
}
inline glm::dvec2 integrate(double mu,double alpha,int count=4096) {
    mu=std::max(mu,1e-6);glm::dvec3 v(std::sqrt(1-mu*mu),0,mu);glm::dvec2 sum(0);
    for(int i=0;i<count;++i){auto h=visibleNormal(v,alpha,(i+.5)/count,radical(static_cast<unsigned>(i)));
        double vh=glm::dot(v,h);auto l=2*vh*h-v;if(l.z<=0)continue;
        double weight=g1(l.z,alpha);sum+=glm::dvec2(fresnel(vh)*weight,weight);}
    return sum/static_cast<double>(count);
}
inline const std::vector<float>& table() {
    static const std::vector<float> values=[] {
        std::vector<float> result(Width*Height*2);
        for(int y=0;y<Height;++y)for(int x=0;x<Width;++x){double t=static_cast<double>(x)/(Width-1);
            auto e=integrate(t*t,std::exp2(-7.+7.*y/(Height-1)));
            result[(y*Width+x)*2]=static_cast<float>(e.x);result[(y*Width+x)*2+1]=static_cast<float>(e.y);}
        return result;
    }();return values;
}
inline glm::dvec2 lookup(double mu,double alpha) {
    const auto& data=table();double x=std::sqrt(std::clamp(mu,0.,1.))*(Width-1);
    double y=std::clamp((std::log2(std::max(alpha,1./128))+7)/7,0.,1.)*(Height-1);
    int ix=std::min(static_cast<int>(x),Width-2),iy=std::min(static_cast<int>(y),Height-2);
    auto at=[&](int a,int b){size_t k=(b*Width+a)*2;return glm::dvec2(data[k],data[k+1]);};
    return glm::mix(glm::mix(at(ix,iy),at(ix+1,iy),x-ix),glm::mix(at(ix,iy+1),at(ix+1,iy+1),x-ix),y-iy);
}
inline double budget(double mu,double alpha){return std::min(1.,lookup(mu,alpha).x+Guard);}
inline GLuint createTexture() {
    GLuint id=0;GLint previous=0;glGetIntegerv(GL_TEXTURE_BINDING_2D,&previous);glGenTextures(1,&id);glBindTexture(GL_TEXTURE_2D,id);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RG32F,Width,Height,0,GL_RG,GL_FLOAT,table().data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(previous));return id;
}
}
