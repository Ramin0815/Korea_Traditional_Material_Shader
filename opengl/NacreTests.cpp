#include "NacreTests.h"
#include "ClearCoatAlbedo.h"
#include "JagaeLight.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {
constexpr double pi=3.14159265358979323846;
using Vec=glm::dvec3;
using C=std::complex<double>;
Vec cmf(double w) {
    const auto g=[](double x){return std::exp(-.5*x*x);};
    return Vec(.362*g((w-442)*(w<442?.0624:.0374))+1.056*g((w-599.8)*(w<599.8?.0264:.0323))-.065*g((w-501.1)*(w<501.1?.049:.0382)),
        .821*g((w-568.8)*(w<568.8?.0213:.0247))+.286*g((w-530.9)*(w<530.9?.0613:.0322)),
        1.217*g((w-437)*(w<437?.0845:.0278))+.681*g((w-459)*(w<459?.0385:.0725)));
}
Vec rgb(Vec x) {return Vec(3.2406*x.x-1.5372*x.y-.4986*x.z,-.9689*x.x+1.8758*x.y+.0415*x.z,.0557*x.x-.204*x.y+1.057*x.z);}
// Independent generic interface recursion: air, [A,B] periods, B substrate.
// No shader complex helpers or hard-coded AB sign recurrence are reused.
double stack(double w,double d,double cosine,int periods,double a=1.63,double b=1.43,double medium=1) {
    std::vector<double> n{medium}, thickness{0};
    for(int i=0;i<periods;++i){n.push_back(a);thickness.push_back(d);n.push_back(b);thickness.push_back(30);}
    n.push_back(b); thickness.push_back(0);
    std::vector<double> c(n.size());
    for(size_t i=0;i<n.size();++i)c[i]=std::sqrt(std::max(0.,1-(1-cosine*cosine)*medium*medium/(n[i]*n[i])));
    double power=0;
    for(int pol=0;pol<2;++pol){
        C r=0;
        for(int i=static_cast<int>(n.size())-2;i>=0;--i){
            const double top=pol==0?n[i]*c[i]:n[i+1]*c[i];
            const double bottom=pol==0?n[i+1]*c[i+1]:n[i]*c[i+1];
            const double interfaceR=(top-bottom)/std::max(top+bottom,1e-5);
            const C phase=std::polar(1.,4*pi*n[i+1]*thickness[i+1]*c[i+1]/w);
            r=(interfaceR+r*phase)/(1.+interfaceR*r*phase);
        }
        power+=std::norm(r)*.5;
    }
    return power;
}
Vec spectral(double d,double cosine,int periods,int step=1,double a=1.63,double b=1.43,double medium=1) {
    Vec response(0),white(0);
    for(int w=380;w<=700;w+=step){const Vec c=cmf(w);response+=c*stack(w,d,cosine,periods,a,b,medium);white+=c;}
    Vec value=glm::max(rgb(response)/rgb(white),Vec(0));
    return value/std::max(1.,std::max(value.x,std::max(value.y,value.z)));
}
double maximum(Vec v){return std::max(v.x,std::max(v.y,v.z));}
double term(double nh,double lh,double roughness){
    const double r2=std::max(roughness*roughness,.00006103515625);
    const double d=nh*nh*(r2-1)+1.00001;
    return r2/std::max(d*d*std::max(.1,lh*lh)*(roughness*4+2),1e-7);
}
Vec underCoat(Vec f){return glm::clamp(f*(f*.526868+.529324)-.0482256,Vec(0),Vec(1));}
Vec legacyBrdf(Vec v,Vec l,double albedo,double mask,double strength=0.85) {
    const Vec h=glm::normalize(v+l);
    const double nv=std::max(v.z,0.),nh=std::max(h.z,0.),lh=std::max(glm::dot(l,h),0.);
    const Vec vf=Vec(.04)*(1-strength)+spectral(500,nv,4,20)*strength;
    const Vec df=Vec(.04)*(1-strength)+spectral(500,lh,4,20)*strength;
    const double r=.22*.22,eta=1-mask+mask/1.5;
    const double adjusted=std::max(std::sqrt(2/((2/(r*r)-2)*eta*eta+2)),.0078125);
    const double trans=1-mask*(.04+.96*std::pow(1-nv,4));
    return (Vec(albedo*(1-maximum(vf)))+(df*(1-mask)+underCoat(df)*mask)*term(nh,lh,adjusted))*trans
        +Vec(.04*term(nh,lh,.01)*mask);
}
double normalizedTerm(double nv,double nl,double nh,double alpha) {
    if(nv<=0 || nl<=0)return 0;double d=(1-nh*nh)+alpha*alpha*nh*nh;
    return alpha*alpha/(pi*d*d)*ClearCoatAlbedo::g1(nv,alpha)*ClearCoatAlbedo::g1(nl,alpha)/(4*nv*nl);
}
Vec coveredSpectral(double d,double cosine,int step=10) {
    return spectral(d,std::sqrt(1-(1-cosine*cosine)/2.25),4,step,1.63,1.43,1.5);
}
Vec brdf(Vec v,Vec l,double albedo,double mask,double strength=.85,double alpha=.22*.22,double coatAlpha=.01) {
    Vec h=glm::normalize(v+l);double lh=glm::dot(l,h),base=normalizedTerm(v.z,l.z,h.z,alpha);
    double tv=1-ClearCoatAlbedo::budget(v.z,alpha),tl=1-ClearCoatAlbedo::budget(l.z,alpha);
    Vec bare=(Vec(ClearCoatAlbedo::fresnel(lh)*base)+Vec(albedo*tv*tl/pi))*(1-strength)+spectral(500,lh,4,10)*base*strength;
    double cv=1-ClearCoatAlbedo::budget(v.z,coatAlpha),cl=1-ClearCoatAlbedo::budget(l.z,coatAlpha);
    Vec covered=Vec(ClearCoatAlbedo::fresnel(lh)*normalizedTerm(v.z,l.z,h.z,coatAlpha))
        +(Vec(albedo/pi)*(1-strength)+coveredSpectral(500,lh)*base*strength)*cv*cl;
    return bare*(1-mask)+covered*mask;
}
GLuint compile(GLenum type,const std::string& source){
    GLuint id=glCreateShader(type);const char* s=source.c_str();glShaderSource(id,1,&s,nullptr);glCompileShader(id);
    GLint ok=0;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
    if(!ok){char log[4096]{};glGetShaderInfoLog(id,4096,nullptr,log);glDeleteShader(id);throw std::runtime_error(log);}return id;
}
struct Probe {
    GLuint program=0,vao=0,fbo=0,texture=0,albedoTexture=0,whiteCube=0;
    Probe(bool oldAngle=false,bool viewOnlyCoat=false,const char* path="shaders/modify_jagae.frag"){
        const std::string vert="#version 330 core\nout JAGAE_VERTEX {vec3 worldPosition;vec3 worldNormal;vec2 uv;} jagae;\nvoid main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2-1,0,1);jagae.worldPosition=vec3(0);jagae.worldNormal=vec3(0,0,1);jagae.uv=vec2(.5);}";
        std::ifstream file(path); if(!file)throw std::runtime_error("Cannot read nacre fragment");
        std::string frag{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
        if(oldAngle){const std::string corrected="spectralMultilayer(thicknessNm, lh)";
            size_t at=frag.find(corrected);if(at==std::string::npos)throw std::runtime_error("Cannot locate angle mutation point");
            frag.replace(at,corrected.size(),"spectralMultilayer(thicknessNm, nh)");}
        if(viewOnlyCoat){const std::string symmetric="coatTl=1.0-dielectricBudget(nl,coatAlpha)";size_t at=frag.find(symmetric);
            if(at==std::string::npos)throw std::runtime_error("Cannot locate coat mutation point");frag.replace(at,symmetric.size(),"coatTl=1.0");}
        const GLuint vs=compile(GL_VERTEX_SHADER,vert),fs=compile(GL_FRAGMENT_SHADER,frag);
        program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);glLinkProgram(program);glDeleteShader(vs);glDeleteShader(fs);
        GLint ok=0;glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)throw std::runtime_error("Nacre probe link failed");
        glGenVertexArrays(1,&vao);glBindVertexArray(vao);glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glActiveTexture(GL_TEXTURE0);
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,1,1,0,GL_RGBA,GL_FLOAT,nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Nacre probe framebuffer failed");
        glBindTexture(GL_TEXTURE_2D,0); // Avoid framebuffer texture feedback from unused material samples.
        glViewport(0,0,1,1);glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_FRAMEBUFFER_SRGB);
        glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        glUseProgram(program);set("uThicknessVariationNm",0);set("uPlateletIOR",1.63);set("uOrganicIOR",1.43);set("uOrganicThicknessNm",30);
        glUniform1i(glGetUniformLocation(program,"uBaseMap"),0);
        glUniform1i(glGetUniformLocation(program,"uThicknessMap"),1);
        glUniform1i(glGetUniformLocation(program,"uEnvironmentMap"),2);
        albedoTexture=ClearCoatAlbedo::createTexture();glActiveTexture(GL_TEXTURE3);glBindTexture(GL_TEXTURE_2D,albedoTexture);
        glUniform1i(glGetUniformLocation(program,"uClearCoatAlbedo"),3);
        glGenTextures(1,&whiteCube);glActiveTexture(GL_TEXTURE2);glBindTexture(GL_TEXTURE_CUBE_MAP,whiteCube);
        const GLfloat white[3]{1,1,1};for(int face=0;face<6;++face)glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X+face,0,GL_RGB32F,1,1,0,GL_RGB,GL_FLOAT,white);
        glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_CUBE_MAP,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glUniform3f(glGetUniformLocation(program,"uDiffuseSH[0]"),static_cast<float>(1/.282094792),static_cast<float>(1/.282094792),static_cast<float>(1/.282094792));
        glActiveTexture(GL_TEXTURE0);
        set("uIridescenceStrength",.85);set("uSmoothness",.78);set("uClearCoatSmoothness",.9);
        glUniform1i(glGetUniformLocation(program,"uLayerCount"),4);
    }
    ~Probe(){glDeleteProgram(program);glDeleteVertexArrays(1,&vao);glDeleteFramebuffers(1,&fbo);glDeleteTextures(1,&texture);glDeleteTextures(1,&albedoTexture);glDeleteTextures(1,&whiteCube);}
    void set(const char* name,double x){glUniform1f(glGetUniformLocation(program,name),static_cast<float>(x));}
    Vec read(Vec v,Vec l,int mode,double d=500,double albedo=0,double mask=0){
        if(mode==33) {
            DirectionalLight aim;
            aim.setAzimuthDegrees(static_cast<float>(std::atan2(l.x,l.z)*180/pi));
            aim.setElevationDegrees(static_cast<float>(std::asin(l.y)*180/pi));
            const JagaeRectangleLight rectangle(aim);
            glUniform3fv(glGetUniformLocation(program,"uAreaCenter"),1,&rectangle.center.x);
            glUniform3fv(glGetUniformLocation(program,"uAreaNormal"),1,&rectangle.normal.x);
            glUniform3fv(glGetUniformLocation(program,"uAreaHalfU"),1,&rectangle.halfU.x);
            glUniform3fv(glGetUniformLocation(program,"uAreaHalfV"),1,&rectangle.halfV.x);
            glUniform3fv(glGetUniformLocation(program,"uAreaPoints[0]"),9,&rectangle.points[0].x);
            glUniform1fv(glGetUniformLocation(program,"uAreaPatchAreas[0]"),9,rectangle.patchAreas.data());
            glUniform3f(glGetUniformLocation(program,"uLightRadiance"),1,1,1);
        }
        glUniform3f(glGetUniformLocation(program,"uCameraWorld"),static_cast<float>(v.x),static_cast<float>(v.y),static_cast<float>(v.z));
        glUniform3f(glGetUniformLocation(program,"uLightDirection"),static_cast<float>(l.x),static_cast<float>(l.y),static_cast<float>(l.z));
        glUniform3f(glGetUniformLocation(program,"uNacreTestAlbedo"),static_cast<float>(albedo),static_cast<float>(albedo),static_cast<float>(albedo));
        glUniform1i(glGetUniformLocation(program,"uNacreTestMode"),mode);set("uPlateletThicknessNm",d);set("uClearCoatMask",mask);
        const GLfloat poison[4]{-123,-123,-123,-123};glClearBufferfv(GL_COLOR,0,poison);
        glDrawArrays(GL_TRIANGLES,0,3);std::array<float,4> pixel{};glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,pixel.data());
        if(glGetError()!=GL_NO_ERROR || pixel[3]!=1)throw std::runtime_error("Nacre GPU probe did not write a valid pixel");
        for(int c=0;c<3;++c)if(!std::isfinite(pixel[c]) || pixel[c]<0)throw std::runtime_error("Nacre GPU probe invalid channel");
        return Vec(pixel[0],pixel[1],pixel[2]);
    }
    Vec furnaceGpu(Vec v,double albedo,double mask,int resolution,double strength=.85) {
        Vec total(0);set("uIridescenceStrength",strength);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,resolution,2*resolution,0,GL_RGBA,GL_FLOAT,nullptr);glBindTexture(GL_TEXTURE_2D,0);
        glViewport(0,0,resolution,2*resolution);
        glUniform1i(glGetUniformLocation(program,"uNacreTestResolution"),resolution);
        glUniform3f(glGetUniformLocation(program,"uCameraWorld"),static_cast<float>(v.x),static_cast<float>(v.y),static_cast<float>(v.z));
        glUniform3f(glGetUniformLocation(program,"uNacreTestAlbedo"),static_cast<float>(albedo),static_cast<float>(albedo),static_cast<float>(albedo));
        glUniform1i(glGetUniformLocation(program,"uNacreTestMode"),6);set("uPlateletThicknessNm",500);set("uClearCoatMask",mask);
        std::vector<glm::vec4> pixels(static_cast<size_t>(resolution)*resolution*2);
        for(int lobe=0;lobe<3;++lobe){if(lobe==1&&mask==0)continue;if(lobe==2&&(albedo==0||strength==1))continue;
            glUniform1i(glGetUniformLocation(program,"uNacreTestLobe"),lobe);glDrawArrays(GL_TRIANGLES,0,3);
            glReadPixels(0,0,resolution,2*resolution,GL_RGBA,GL_FLOAT,pixels.data());Vec sum(0);
            for(auto p:pixels){for(int c=0;c<3;++c)if(!std::isfinite(p[c])||p[c]<0)throw std::runtime_error("GPU furnace invalid integrand");sum+=Vec(p);}
            total+=sum/static_cast<double>(pixels.size());}
        glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,1,1,0,GL_RGBA,GL_FLOAT,nullptr);glBindTexture(GL_TEXTURE_2D,0);glViewport(0,0,1,1);
        if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("GPU furnace GL failure");return total;
    }
    double benchmark() {
        glBindFramebuffer(GL_FRAMEBUFFER,fbo);glUseProgram(program);glBindVertexArray(vao);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,900,650,0,GL_RGBA,GL_FLOAT,nullptr);glBindTexture(GL_TEXTURE_2D,0);glViewport(0,0,900,650);
        glUniform1i(glGetUniformLocation(program,"uNacreTestMode"),8);
        glUniform3f(glGetUniformLocation(program,"uCameraWorld"),.342020143f,0,.939692621f);
        glUniform3f(glGetUniformLocation(program,"uLightDirection"),-.5f,0,.866025404f);
        glUniform3f(glGetUniformLocation(program,"uLightRadiance"),1,1,1);
        glUniform3f(glGetUniformLocation(program,"uNacreTestAlbedo"),0,0,0);
        set("uPlateletThicknessNm",500);set("uClearCoatMask",.25);set("uIridescenceStrength",.85);set("uSmoothness",.78);set("uClearCoatSmoothness",.9);
        for(int i=0;i<8;++i)glDrawArrays(GL_TRIANGLES,0,3);glFinish();
        GLuint query=0;glGenQueries(1,&query);glBeginQuery(GL_TIME_ELAPSED,query);
        for(int i=0;i<60;++i)glDrawArrays(GL_TRIANGLES,0,3);
        glEndQuery(GL_TIME_ELAPSED);GLuint64 elapsed=0;glGetQueryObjectui64v(query,GL_QUERY_RESULT,&elapsed);glDeleteQueries(1,&query);
        glBindTexture(GL_TEXTURE_2D,texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,1,1,0,GL_RGBA,GL_FLOAT,nullptr);glBindTexture(GL_TEXTURE_2D,0);glViewport(0,0,1,1);
        return static_cast<double>(elapsed)/60e6;
    }
};
Vec direction(double degrees){double t=degrees*pi/180;return Vec(std::sin(t),0,std::cos(t));}
// GGX half-vector quadrature avoids missing the narrow normal-incidence lobe.
// Integrate each base/coat lobe independently with its own D(h) importance PDF.
Vec furnace(Vec v,double albedo,double mask,int resolution,double strength=.85,double alpha=.22*.22,double coatAlpha=.01){
    const double bv=1-ClearCoatAlbedo::budget(v.z,alpha),cv=1-ClearCoatAlbedo::budget(v.z,coatAlpha);
    double diffuse=0;
    for(int i=0;i<resolution;++i){double nl=std::sqrt((i+.5)/resolution);
        diffuse+=(1-mask)*bv*(1-ClearCoatAlbedo::budget(nl,alpha))+mask*cv*(1-ClearCoatAlbedo::budget(nl,coatAlpha));}
    Vec result(albedo*(1-strength)*diffuse/resolution);
    // Precompute angular spectral factors to save stack work per sample.
    // Reported convergence compares angular quadrature, not this fixed LUT.
    static const auto lut=[] {std::array<std::array<Vec,2049>,2> values{};
        for(size_t i=0;i<2049;++i){double c=static_cast<double>(i)/2048;values[0][i]=spectral(500,c,4,10);values[1][i]=coveredSpectral(500,c);}return values;}();
    for(int lobe=0;lobe<2;++lobe){
        if(lobe==1 && mask==0)continue;
        double a=lobe==0?alpha:coatAlpha;Vec sum(0);
        for(int i=0;i<resolution;++i){double u=(i+.5)/resolution;
            double cosH=std::sqrt((1-u)/(1+(a*a-1)*u)),sinH=std::sqrt(1-cosH*cosH);
            for(int j=0;j<resolution*2;++j){double phi=2*pi*(j+.5)/(resolution*2);
                Vec h(sinH*std::cos(phi),sinH*std::sin(phi),cosH);double vh=glm::dot(v,h);if(vh<=0)continue;
                Vec l=2*vh*h-v;if(l.z<=0)continue;
                const double d=(1-cosH*cosH)+a*a*cosH*cosH;
                const double pdf=a*a/(pi*d*d)*cosH/(4*vh);
                Vec factor(ClearCoatAlbedo::fresnel(vh)*mask);
                if(lobe==0){double index=std::clamp(vh,0.,1.)*2048;size_t k=std::min(static_cast<size_t>(index),size_t(2047));
                    Vec sf=glm::mix(lut[0][k],lut[0][k+1],index-k),sc=glm::mix(lut[1][k],lut[1][k+1],index-k);
                    factor=(Vec(ClearCoatAlbedo::fresnel(vh)*(1-strength))+sf*strength)*(1-mask)
                        +sc*strength*mask*cv*(1-ClearCoatAlbedo::budget(l.z,coatAlpha));}
                sum+=factor*(normalizedTerm(v.z,l.z,cosH,a)*l.z/pdf);
            }
        }
        result+=sum/static_cast<double>(resolution*resolution*2);
    }
    return result;
}
struct RestoreState {
    GLint draw=0,read=0,program=0,vao=0,activeTexture=0,viewport[4]{};
    std::array<GLint,4> textures{},cubes{};
    const std::array<GLenum,5> states{GL_DEPTH_TEST,GL_CULL_FACE,GL_BLEND,GL_SCISSOR_TEST,GL_FRAMEBUFFER_SRGB};
    std::array<GLboolean,5> enabled{};
    GLboolean colorMask[4]{};
    RestoreState(){
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);glGetIntegerv(GL_CURRENT_PROGRAM,&program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_ACTIVE_TEXTURE,&activeTexture);
        for(int i=0;i<4;++i){glActiveTexture(GL_TEXTURE0+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&textures[i]);glGetIntegerv(GL_TEXTURE_BINDING_CUBE_MAP,&cubes[i]);}
        glActiveTexture(static_cast<GLenum>(activeTexture));
        for(size_t i=0;i<states.size();++i)enabled[i]=glIsEnabled(states[i]);
        glGetBooleanv(GL_COLOR_WRITEMASK,colorMask);
    }
    ~RestoreState(){
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(draw));glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(read));glUseProgram(static_cast<GLuint>(program));glBindVertexArray(static_cast<GLuint>(vao));
        for(int i=0;i<4;++i){glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(textures[i]));glBindTexture(GL_TEXTURE_CUBE_MAP,static_cast<GLuint>(cubes[i]));}
        glActiveTexture(static_cast<GLenum>(activeTexture));glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        for(size_t i=0;i<states.size();++i)if(enabled[i])glEnable(states[i]);else glDisable(states[i]);
        glColorMask(colorMask[0],colorMask[1],colorMask[2],colorMask[3]);
    }
};
}

bool runNacreSelfTests(std::ostream& output){
    const RestoreState restoreState;
    bool ok=true;double maxError=0,oldAngleError=0;
    // Explicit 33-sample speed/accuracy compromise: the former 65-sample
    // .0002 budget is not satisfied by 10nm sampling at grazing incidence.
    // Independent 1nm scans over 150..600nm / 0..80deg give <.000635;
    // .001 is the accepted bare RGB budget, not a physical-transport tolerance.
    constexpr double bareSpectralRgbBudget=.001;
    {
        Probe probe;
        std::vector<double> thicknessSamples;
        for(int thickness=150;thickness<=600;++thickness) thicknessSamples.push_back(thickness);
        for(double fractional:{150.25,199.75,280.25,374.75,449.5,540.25,599.75}) thicknessSamples.push_back(fractional);
        for(double thickness:thicknessSamples)for(double angle:{0.,5.,10.,15.,20.,25.,30.,35.,40.,45.,50.,55.,60.,65.,70.,75.,80.}){
            Vec v=direction(angle),l=direction(-angle);Vec gpu=probe.read(v,l,1,thickness),reference=spectral(thickness,std::cos(angle*pi/180),4);
            double error=maximum(glm::abs(gpu-reference));maxError=std::max(maxError,error);
            ok=ok&&error<=bareSpectralRgbBudget&&std::isfinite(maximum(gpu))&&glm::all(glm::greaterThanEqual(gpu,Vec(0)));
            if(angle==60)oldAngleError=std::max(oldAngleError,maximum(glm::abs(gpu-spectral(thickness,1,4,10))));
        }
        ok=ok&&oldAngleError>.02;
        output<<std::setprecision(9)<<"Nacre production GPU vs independent 1 nm CPU: 33 spectral samples / 4 periods, 150..600 nm at 1 nm thickness steps +7 fractional samples, 0..80 deg at 5deg steps max RGB error="<<maxError<<" accepted bare RGB budget="<<bareSpectralRgbBudget<<" (was .0002 with 65 samples); old N.H separation="<<oldAngleError<<'\n';
        double eightError=0;glUniform1i(glGetUniformLocation(probe.program,"uLayerCount"),8);
        for(int d=150;d<=600;d+=10)for(double a:{0.,30.,60.})eightError=std::max(eightError,maximum(glm::abs(probe.read(direction(a),direction(-a),1,d)-spectral(d,std::cos(a*pi/180),8))));
        output<<"Eight-period envelope diagnostic max RGB error="<<eightError<<" (outside validated 4-period tolerance)\n";
        glUniform1i(glGetUniformLocation(probe.program,"uLayerCount"),4);
        // Identical indices eliminate internal interfaces, leaving air/substrate Fresnel.
        probe.set("uPlateletIOR",1.43);probe.set("uOrganicIOR",1.43);
        double matchedError=0;
        for(double a:{0.,30.,60.}){double c=std::cos(a*pi/180),ct=std::sqrt(1-(1-c*c)/(1.43*1.43));
            double rs=(c-1.43*ct)/(c+1.43*ct),rp=(1.43*c-ct)/(1.43*c+ct);double expected=.5*(rs*rs+rp*rp);
            matchedError=std::max(matchedError,maximum(glm::abs(probe.read(direction(a),direction(-a),1)-Vec(expected))));}
        probe.set("uPlateletIOR",1);probe.set("uOrganicIOR",1);Vec zero=probe.read(direction(60),direction(-60),1);ok=ok&&matchedError<1e-5&&maximum(glm::abs(zero))<1e-7;
        output<<"Matched-index Fresnel max error="<<matchedError<<" all-air max="<<maximum(zero)<<'\n';
        probe.set("uPlateletIOR",1.63);probe.set("uOrganicIOR",1.43);
        double coveredError=0;
        for(int d=150;d<=600;d+=10)for(double a:{0.,5.,10.,15.,20.,25.,30.,35.,40.,45.,50.,55.,60.,65.,70.,75.,80.})coveredError=std::max(coveredError,
            maximum(glm::abs(probe.read(direction(a),direction(-a),3,d)-coveredSpectral(d,std::cos(a*pi/180),1))));
        ok=ok&&coveredError<.0002;output<<"Coat/nacre boundary (n0=1.5, Snell incidence), 150..600 nm GPU vs independent 1 nm max RGB error="<<coveredError<<'\n';
        double brdfRelativeError=0,reciprocity=0,endpointError=0,whiteIblMax=0;
        int reciprocityCases=0;
        for(double smooth:{.1,.78,1.})for(double coatSmooth:{.1,.9,1.}){
            probe.set("uSmoothness",smooth);probe.set("uClearCoatSmoothness",coatSmooth);
            double alpha=std::max((1-smooth)*(1-smooth),1./128),coatAlpha=std::max((1-coatSmooth)*(1-coatSmooth),1./128);
            for(double mask:{0.,.25,1.})for(double albedo:{0.,.5,1.})for(double strength:{0.,.35,.50,.55,.60,.85,1.}){
                probe.set("uIridescenceStrength",strength);
                for(double angle:{0.,30.,60.,85.}){
                    Vec v=direction(angle),l=direction(-angle);
                    Vec expected=brdf(v,l,albedo,mask,strength,alpha,coatAlpha),actual=probe.read(v,l,2,500,albedo,mask);
                    brdfRelativeError=std::max(brdfRelativeError,maximum(glm::abs(actual-expected))/std::max(1.,maximum(expected)));
                    Vec x=probe.read(direction(20),direction(angle),2,500,albedo,mask),y=probe.read(direction(angle),direction(20),2,500,albedo,mask);
                    reciprocity=std::max(reciprocity,maximum(glm::abs(x-y))/std::max(1e-8,maximum(x)));++reciprocityCases;
                    Vec ibl=probe.read(v,l,5,500,albedo,mask);whiteIblMax=std::max(whiteIblMax,maximum(ibl));
                }
            }
            Vec bare=probe.read(direction(20),direction(65),2,500,.5,0),covered=probe.read(direction(20),direction(65),2,500,.5,1);
            Vec partial=probe.read(direction(20),direction(65),2,500,.5,.25);
            endpointError=std::max(endpointError,maximum(glm::abs(partial-(bare*.75+covered*.25))));
        }
        ok=ok&&brdfRelativeError<.005&&reciprocity<2e-5&&endpointError<2e-5&&whiteIblMax<=1.00001;
        output<<"Production normalized BRDF CPU/GPU relative error="<<brdfRelativeError<<" reciprocal swap relative="<<reciprocity
            <<" cases="<<reciprocityCases<<" mask convex endpoint error="<<endpointError<<" white split-sum IBL maximum="<<whiteIblMax<<'\n';
        probe.set("uSmoothness",.78);probe.set("uClearCoatSmoothness",.9);
        double furnaceMax=0,convergence=0,gpuCpuError=0;int furnaceCases=0;
        for(double mask:{0.,.25,1.})for(double albedo:{0.,.5,1.})for(double strength:{0.,.35,.50,.55,.60,.85,1.})for(double angle:{0.,60.}){
            Vec coarse=probe.furnaceGpu(direction(angle),albedo,mask,64,strength),fine=probe.furnaceGpu(direction(angle),albedo,mask,128,strength);
            Vec reference=furnace(direction(angle),albedo,mask,256,strength);
            convergence=std::max(convergence,maximum(glm::abs(coarse-fine)));gpuCpuError=std::max(gpuCpuError,maximum(glm::abs(fine-reference)));
            furnaceMax=std::max(furnaceMax,maximum(fine));++furnaceCases;
            ok=ok&&maximum(fine)<=1.002;
            if(albedo==0&&strength==.85)output<<"Production GPU furnace mask="<<mask<<" view="<<angle<<" RGB="<<fine.x<<','<<fine.y<<','<<fine.z<<'\n';
        }
        double lutUnder=0,lutAbsolute=0,lutGpuLookupError=0;
        // New Nacre appearance preset's local facet-frame roughness envelope.
        // These do not claim energy conservation across geometric grazing edges
        // of a tilted shading-normal approximation.
        for(double strength:{.35,.50,.55,.60})for(double rough:{.16,.22,.28})for(double angle:{0.,60.,85.}) {
            probe.set("uSmoothness",1-rough);probe.set("uClearCoatSmoothness",.9);
            Vec coarse=probe.furnaceGpu(direction(angle),1,.25,128,strength);
            Vec fine=probe.furnaceGpu(direction(angle),1,.25,256,strength);
            Vec reference=furnace(direction(angle),1,.25,256,strength,rough*rough,.01);
            convergence=std::max(convergence,maximum(glm::abs(coarse-fine)));
            gpuCpuError=std::max(gpuCpuError,maximum(glm::abs(fine-reference)));
            furnaceMax=std::max(furnaceMax,maximum(fine));++furnaceCases;
            ok=ok&&maximum(fine)<=1.002;
        }
        probe.set("uSmoothness",.78);
        for(double alpha:{1./128,.01,.0484,.17,.53,1.})for(double mu:{.001,.037,.1,.347,.793,1.}){
            probe.set("uClearCoatSmoothness",1-std::sqrt(alpha));
            Vec v(std::sqrt(1-mu*mu),0,mu);
            Vec fine=probe.furnaceGpu(v,0,1,512,0);
            auto lut=ClearCoatAlbedo::lookup(mu,alpha);
            lutUnder=std::max(lutUnder,fine.x-lut.x);lutAbsolute=std::max(lutAbsolute,std::abs(fine.x-lut.x));
            ok=ok&&fine.x<=ClearCoatAlbedo::budget(mu,alpha)+.001&&fine.x<=1.001;
            Vec hdr=probe.read(v,direction(20),7);lutGpuLookupError=std::max(lutGpuLookupError,std::abs(hdr.x-lut.x));
        }
        for(double smooth:{.1,.5,1.})for(double angle:{30.,75.,85.}){
            probe.set("uSmoothness",smooth);probe.set("uClearCoatSmoothness",smooth);
            Vec coarse=probe.furnaceGpu(direction(angle),1,1,128,.85),fine=probe.furnaceGpu(direction(angle),1,1,256,.85);
            furnaceMax=std::max(furnaceMax,maximum(fine));convergence=std::max(convergence,maximum(glm::abs(fine-coarse)));
            ok=ok&&maximum(fine)<=1.002;++furnaceCases;
        }
        // Hardware bilinear interpolation can quantize interpolation weights.
        ok=ok&&convergence<.006&&gpuCpuError<.006&&lutUnder<ClearCoatAlbedo::Guard&&lutGpuLookupError<5e-5;
        output<<"Physical hemispherical integral (no post-hoc pi division): GPU cases="<<furnaceCases<<" maximum="<<furnaceMax
            <<" quadrature convergence="<<convergence<<" independent CPU discrepancy="<<gpuCpuError<<'\n';
        output<<"Directional-albedo LUT independent production-GPU NDF audit: 36 points x 524288 samples, under="<<lutUnder
            <<" absolute="<<lutAbsolute<<" CPU/GPU lookup="<<lutGpuLookupError<<" absorption guard="<<ClearCoatAlbedo::Guard<<'\n';
    }
    {
        Probe mutant(true);double error=0;
        for(double a:{30.,60.}){Vec v=direction(a),l=direction(-a),expected=brdf(v,l,0,0);
            error=std::max(error,maximum(glm::abs(mutant.read(v,l,2)-expected))/std::max(1.,maximum(expected)));}
        ok=ok&&error>.005;output<<"In-memory old-N.H production BRDF mutation rejected: relative error="<<error<<" >.005\n";
    }
    {
        Probe mutant(false,true);Vec v=direction(20),l=direction(65);
        Vec a=mutant.read(v,l,2,500,.5,1),b=mutant.read(l,v,2,500,.5,1);
        double error=maximum(glm::abs(a-b))/std::max(1e-12,maximum(a));ok=ok&&error>1e-3;
        double legacy=maximum(glm::abs(legacyBrdf(v,l,0,.25)-legacyBrdf(l,v,0,.25)))/maximum(legacyBrdf(v,l,0,.25));
        output<<"In-memory one-sided coat mutation rejected: swap relative="<<error<<"; former URP mask=.25 black-base violation="<<legacy<<'\n';
    }
    {
        Probe production;double current=production.benchmark();
        std::ifstream baseline("../../../tools/fixtures/LegacyUrpClearCoat.frag");
        if(baseline){baseline.close();Probe legacy(false,false,"../../../tools/fixtures/LegacyUrpClearCoat.frag");double previous=legacy.benchmark();
            output<<"Steady GPU timestamp (900x650 flat sample, 8 warmup/60 draws, offscreen/no swap, white IBL): former URP="<<previous
                <<" ms physical="<<current<<" ms; excludes startup LUT/SH and UI\n";}
        else output<<"Steady GPU timestamp 900x650 physical="<<current<<" ms (legacy fixture unavailable)\n";
    }
    {
        // Independent dense emitter-area quadrature in double precision,
        // compared with the GPU's lobe-direction importance estimator.
        Probe area;area.set("uIridescenceStrength",.55);
        for(float azimuth:{0.f,203.f,359.f})for(float elevation:{-89.f,0.f,89.f}) {
            DirectionalLight aim;aim.setAzimuthDegrees(azimuth);aim.setElevationDegrees(elevation);
            const JagaeRectangleLight rectangle(aim);
            float totalArea=0;for(float patch:rectangle.patchAreas) totalArea+=patch;
            ok=ok&&std::abs(glm::length(rectangle.center)-2.5f)<1e-5f
                &&std::abs(glm::length(rectangle.halfU)-1.25f)<1e-5f
                &&std::abs(glm::length(rectangle.halfV)-1.0f)<1e-5f
                &&std::abs(glm::dot(rectangle.halfU,rectangle.normal))<1e-5f
                &&std::abs(glm::dot(rectangle.halfV,rectangle.normal))<1e-5f
                &&std::abs(glm::dot(rectangle.halfU,rectangle.halfV))<1e-5f&&std::abs(totalArea-5.0f)<1e-5f;
        }
        double maxAbsolute=0,maxRelative=0,maxConvergence=0;
        for(int auditCase=0;auditCase<4;++auditCase) {
            const double angle=auditCase==3?26.:20.*auditCase;
            const double sourceAngle=auditCase==3?0.:-angle;
            Vec v=direction(angle),l=direction(sourceAngle);
            DirectionalLight aim;aim.setAzimuthDegrees(static_cast<float>(std::atan2(l.x,l.z)*180/pi));
            aim.setElevationDegrees(0);
            const JagaeRectangleLight rectangle(aim);
            const auto integral=[&](int resolution) {
                Vec total(0);
                for(int y=0;y<resolution;++y)for(int x=0;x<resolution;++x) {
                    Vec delta=Vec(rectangle.center)+Vec(rectangle.halfU)*(2*(x+.5)/resolution-1)
                        +Vec(rectangle.halfV)*(2*(y+.5)/resolution-1);
                    const double r2=glm::dot(delta,delta);const Vec incoming=delta/std::sqrt(r2);
                    if(incoming.z<=0) continue;
                    const double weight=std::max(0.,glm::dot(Vec(rectangle.normal),-incoming))*5.0/(resolution*resolution*r2);
                    total+=brdf(v,incoming,.79,.25,.55)*incoming.z*weight;
                }
                return total;
            };
            const int resolution=auditCase==3?256:128;
            const Vec reference=integral(resolution),coarse=integral(resolution/2),gpu=area.read(v,l,33,500,.79,.25);
            const double absolute=maximum(glm::abs(reference-gpu));
            const double relative=absolute/std::max(1e-8,maximum(reference));
            const double convergence=maximum(glm::abs(reference-coarse));
            maxAbsolute=std::max(maxAbsolute,absolute);maxRelative=std::max(maxRelative,relative);
            maxConvergence=std::max(maxConvergence,convergence);
            output<<"Rectangle "<<(auditCase==3?"near-edge":"center-reflection")<<" audit view="<<angle
                <<" source="<<sourceAngle<<" GPU="<<gpu.x<<','<<gpu.y<<','<<gpu.z
                <<" CPU"<<resolution<<'='<<reference.x<<','<<reference.y<<','<<reference.z<<" absolute="<<absolute
                <<" relative="<<relative<<" CPU"<<resolution/2<<'/'<<resolution<<" convergence="<<convergence<<'\n';
            ok=ok&&convergence<.0002;
            if(auditCase<3) ok=ok&&absolute<.005&&relative<.05;
            else ok=ok&&absolute<.015;
        }
        output<<"Rectangle 8-sample/lobe approximation: independent dense-area max absolute="<<maxAbsolute
            <<" relative="<<maxRelative<<" convergence="<<maxConvergence
            <<"; sampled interior tolerance abs .005 / relative .05; near-edge abs .015; CPU convergence .0002; not all normals or panel edges\n";
    }
    output<<"Nacre self-tests "<<(ok?"passed":"FAILED")<<". Reciprocal energy-budgeted effective external GGX lobes; finite tested envelope, not full refracted layered transport.\n";
    return ok;
}
