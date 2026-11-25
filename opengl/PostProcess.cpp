#include "PostProcess.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sstream>

namespace
{
// Every state mutated by the fullscreen passes is restored, including read/draw
// FBOs independently. Resize and tests use this guard too.
struct State
{
    GLint draw,read,viewport[4],program,vao,active,texture[2],sampler[2],renderbuffer;
    GLboolean depth,cull,blend,scissor,srgb,mask[4];
    State()
    {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw); glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
        glGetIntegerv(GL_VIEWPORT,viewport); glGetIntegerv(GL_CURRENT_PROGRAM,&program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao); glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);
        for(int i=0;i<2;++i) { glActiveTexture(GL_TEXTURE0+i); glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture[i]); glGetIntegeri_v(GL_SAMPLER_BINDING,i,&sampler[i]); }
        depth=glIsEnabled(GL_DEPTH_TEST); cull=glIsEnabled(GL_CULL_FACE); blend=glIsEnabled(GL_BLEND);
        scissor=glIsEnabled(GL_SCISSOR_TEST); srgb=glIsEnabled(GL_FRAMEBUFFER_SRGB); glGetBooleanv(GL_COLOR_WRITEMASK,mask);
    }
    ~State()
    {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,draw); glBindFramebuffer(GL_READ_FRAMEBUFFER,read);
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]); glUseProgram(program); glBindVertexArray(vao);
        for(int i=0;i<2;++i) { glActiveTexture(GL_TEXTURE0+i); glBindTexture(GL_TEXTURE_2D,texture[i]); glBindSampler(i,sampler[i]); }
        glActiveTexture(active); glBindRenderbuffer(GL_RENDERBUFFER,renderbuffer);
        restore(GL_DEPTH_TEST,depth); restore(GL_CULL_FACE,cull); restore(GL_BLEND,blend);
        restore(GL_SCISSOR_TEST,scissor); restore(GL_FRAMEBUFFER_SRGB,srgb); glColorMask(mask[0],mask[1],mask[2],mask[3]);
    }
    static void restore(GLenum cap, GLboolean on) { if(on) glEnable(cap); else glDisable(cap); }
};
GLuint compile(GLenum type,const char* path)
{
    std::ifstream file(path); if(!file) throw std::runtime_error(std::string("Cannot open ")+path);
    std::string source{std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
    const char* p=source.c_str(); GLuint shader=glCreateShader(type); glShaderSource(shader,1,&p,nullptr); glCompileShader(shader);
    GLint ok=0; glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) { char log[4096]; glGetShaderInfoLog(shader,sizeof(log),nullptr,log); glDeleteShader(shader); throw std::runtime_error(log); }
    return shader;
}
float srgb(float x) { return x<=0.0031308f?12.9232102f*x:1.055f*std::pow(x,1.0f/2.4f)-0.055f; }
float curve(float x) { return (x*(.2f*x+.24f*.29f)+.272f*.02f)/(x*(.2f*x+.29f)+.272f*.3f)-.02f/.3f; }
float neutral(float x) { const float scale=1.0f/curve(5.3f); return curve(x*scale)*scale; }
}

PostSettings PostSettings::inspectionProfile()
{
    PostSettings s; s.tone=ToneMode::Neutral; s.bloomIntensity=1; s.threshold=1;
    s.vignetteIntensity=.25f; s.vignetteSmoothness=.4f; return s;
}
PostProcess::PostProcess()
{
    State state;
    GLuint v=0,f=0;
    try {
        v=compile(GL_VERTEX_SHADER,"shaders/post.vert"); f=compile(GL_FRAGMENT_SHADER,"shaders/post.frag");
        program_=glCreateProgram(); glAttachShader(program_,v); glAttachShader(program_,f); glLinkProgram(program_);
        GLint ok; glGetProgramiv(program_,GL_LINK_STATUS,&ok);
        if(!ok) { char log[4096]; glGetProgramInfoLog(program_,sizeof(log),nullptr,log); throw std::runtime_error(log); }
        glGenVertexArrays(1,&vao_); resize(display_,1,1,GL_RGBA8);
    } catch(...) { if(v)glDeleteShader(v); if(f)glDeleteShader(f); if(program_)glDeleteProgram(program_); if(vao_)glDeleteVertexArrays(1,&vao_); destroy(display_); throw; }
    glDeleteShader(v); glDeleteShader(f);
}
PostProcess::~PostProcess()
{
    destroy(display_); for(auto& t:down_)destroy(t); for(auto& t:up_)destroy(t);
    if(program_)glDeleteProgram(program_); if(vao_)glDeleteVertexArrays(1,&vao_);
}
void PostProcess::destroy(Target& t)
{
    if(t.texture)glDeleteTextures(1,&t.texture); if(t.framebuffer)glDeleteFramebuffers(1,&t.framebuffer); t={};
}
void PostProcess::resize(Target& t,int width,int height,GLenum format)
{
    if(t.width==width&&t.height==height)return;
    if(!t.texture)glGenTextures(1,&t.texture); if(!t.framebuffer)glGenFramebuffers(1,&t.framebuffer);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,t.texture);
    glTexImage2D(GL_TEXTURE_2D,0,format,width,height,0,GL_RGBA,GL_FLOAT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER,t.framebuffer); glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t.texture,0);
    if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Post framebuffer incomplete");
    t.width=width; t.height=height;
}
void PostProcess::pass(Target& target,GLuint source,GLuint low,int stage,const PostSettings& s)
{
    glBindFramebuffer(GL_FRAMEBUFFER,target.framebuffer); glViewport(0,0,target.width,target.height);
    glUseProgram(program_); glBindVertexArray(vao_);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,source); glBindSampler(0,0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,low); glBindSampler(1,0);
    const auto integer=[&](const char* n,int v){glUniform1i(glGetUniformLocation(program_,n),v);};
    const auto scalar=[&](const char* n,float v){glUniform1f(glGetUniformLocation(program_,n),v);};
    integer("sourceTex",0); integer("lowTex",1); integer("stage",stage); integer("toneMode",static_cast<int>(s.tone));
    scalar("thresholdLinear",s.threshold<=1.0f?(s.threshold<=.04045f?s.threshold/12.92f:std::pow((s.threshold+.055f)/1.055f,2.4f)):std::pow(s.threshold,2.2f));
    scalar("scatter",.05f+.9f*s.scatter); scalar("bloomClamp",s.bloomClamp); scalar("bloomIntensity",s.bloomIntensity);
    scalar("vignetteIntensity",s.vignetteIntensity); scalar("vignetteSmoothness",s.vignetteSmoothness);
    glDrawArrays(GL_TRIANGLES,0,3);
}
void PostProcess::render(GLuint scene,int width,int height,OutputMode mode,const PostSettings& settings)
{
    if(width<=0||height<=0)return;
    State state;
    glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST); glDisable(GL_FRAMEBUFFER_SRGB);
    glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE); resize(display_,width,height,GL_RGBA8);
    GLuint bloom=0;
    if(mode==OutputMode::LinearScene&&settings.bloomIntensity>0)
    {
        int w=std::max(1,width>>1),h=std::max(1,height>>1);
        int count=std::clamp(static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(w,h)))))-1,1,std::clamp(settings.maxIterations,2,8));
        for(size_t i=static_cast<size_t>(count);i<down_.size();++i)destroy(down_[i]);
        for(size_t i=static_cast<size_t>(count);i<up_.size();++i)destroy(up_[i]);
        down_.resize(count); up_.resize(count);
        for(int i=0;i<count;++i) { resize(down_[i],w,h,GL_RGBA16F); resize(up_[i],w,h,GL_RGBA16F); w=std::max(1,w>>1); h=std::max(1,h>>1); }
        pass(down_[0],scene,0,0,settings);
        for(int i=1;i<count;++i) { pass(up_[i],down_[i-1].texture,0,1,settings); pass(down_[i],up_[i].texture,0,2,settings); }
        bloom=down_.back().texture;
        for(int i=count-2;i>=0;--i) { pass(up_[i],down_[i].texture,bloom,3,settings); bloom=up_[i].texture; }
    }
    pass(display_,scene,bloom,mode==OutputMode::LegacyDisplay?5:4,settings);
}
void PostProcess::bindDisplayForRead() const { glBindFramebuffer(GL_READ_FRAMEBUFFER,display_.framebuffer); }

bool PostProcess::selfTest(std::ostream& output)
{
    State state;
    Target raw; resize(raw,64,64,GL_RGBA16F);
    struct Cleanup { Target& t; ~Cleanup(){PostProcess::destroy(t);} } cleanup{raw};
    std::vector<float> data(64*64*4,0.0f);
    const float ramp[]={0,.001f,.0031308f,.018f,.18f,.5f,1,2,4,16};
    for(int y=0;y<64;++y)for(int x=0;x<64;++x) { size_t k=(y*64+x)*4; data[k]=data[k+1]=data[k+2]=ramp[x%10]; data[k+3]=1; }
    const auto upload=[&](){glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,raw.texture);glTexSubImage2D(GL_TEXTURE_2D,0,0,0,64,64,GL_RGBA,GL_FLOAT,data.data());};
    upload(); GLint format=0; glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&format);
    std::vector<float> rawRead(data.size());glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_FLOAT,rawRead.data());
    if(format!=GL_RGBA16F||rawRead[9*4]<=1)return false;
    for(float v:rawRead)if(!std::isfinite(v)||v<0)return false;
    const auto read=[&](){std::vector<unsigned char> bytes(data.size());bindDisplayForRead();glReadPixels(0,0,64,64,GL_RGBA,GL_UNSIGNED_BYTE,bytes.data());return bytes;};
    PostSettings none;
    glEnable(GL_FRAMEBUFFER_SRGB);glEnable(GL_BLEND);glEnable(GL_SCISSOR_TEST);
    glViewport(3,5,17,23);glColorMask(GL_FALSE,GL_TRUE,GL_FALSE,GL_TRUE);
    render(raw.texture,64,64,OutputMode::LinearScene,none);const auto noTone=read();
    GLint restoredViewport[4];GLboolean restoredMask[4];glGetIntegerv(GL_VIEWPORT,restoredViewport);glGetBooleanv(GL_COLOR_WRITEMASK,restoredMask);
    if(!glIsEnabled(GL_FRAMEBUFFER_SRGB)||!glIsEnabled(GL_BLEND)||!glIsEnabled(GL_SCISSOR_TEST)||
        restoredViewport[0]!=3||restoredViewport[1]!=5||restoredViewport[2]!=17||restoredViewport[3]!=23||
        restoredMask[0]!=GL_FALSE||restoredMask[2]!=GL_FALSE)return false;
    glDisable(GL_FRAMEBUFFER_SRGB);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
    for(int x=0;x<10;++x)if(std::abs(int(noTone[x*4])-int(std::round(srgb(std::min(ramp[x],1.0f))*255)))>1)return false;
    PostSettings tone; tone.tone=ToneMode::Neutral;
    render(raw.texture,64,64,OutputMode::LinearScene,tone);const auto toned=read();
    for(int x=0;x<10;++x)if(std::abs(int(toned[x*4])-int(std::round(srgb(std::clamp(neutral(ramp[x]),0.0f,1.0f))*255)))>1)return false;
    if(toned==noTone)return false;
    // Independent reference: Unity6000.0.39f1/D3D11 executed the unmodified
    // installed Core17.0.3 Color.hlsl, not the duplicated CPU oracle above.
    std::ifstream reference("assets/UnityPostRamp.csv");
    if(!reference)return false;
    std::string line;std::getline(reference,line);
    int referenceCount=0,maxReferenceDelta=0;
    while(std::getline(reference,line))
    {
        std::replace(line.begin(),line.end(),',',' ');std::istringstream row(line);
        int mode;float input,encoded;if(!(row>>mode>>input>>encoded)||mode<0||mode>1)return false;
        int x=referenceCount%10;if(std::abs(input-ramp[x])>1e-6f)return false;
        int delta=std::abs(int((mode==0?noTone:toned)[x*4])-int(std::round(encoded*255)));
        maxReferenceDelta=std::max(maxReferenceDelta,delta);if(delta>1)return false;
        ++referenceCount;
    }
    if(referenceCount!=20)return false;
    render(raw.texture,64,64,OutputMode::LegacyDisplay,PostSettings::inspectionProfile());const auto legacy=read();
    if(std::abs(int(legacy[4*4])-46)>1)return false;
    GLint displayFormat=0;glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,display_.texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D,0,GL_TEXTURE_INTERNAL_FORMAT,&displayFormat);
    if(displayFormat!=GL_RGBA8)return false; // Never a hardware-decoded sRGB texture.
    render(raw.texture,64,64,OutputMode::LinearScene,none);
    // Emulate ImGui's white-tint texture sampling into a linear attachment:
    // encoded values must survive unchanged, without a second transfer.
    glDisable(GL_DEPTH_TEST);glDisable(GL_CULL_FACE);glDisable(GL_BLEND);glDisable(GL_SCISSOR_TEST);glDisable(GL_FRAMEBUFFER_SRGB);
    pass(raw,display_.texture,0,5,none);glBindFramebuffer(GL_READ_FRAMEBUFFER,raw.framebuffer);
    glReadPixels(0,0,64,64,GL_RGBA,GL_FLOAT,rawRead.data());
    if(std::abs(rawRead[4*4]-float(noTone[4*4])/255.0f)>.001f)return false;
    std::fill(data.begin(),data.end(),0.0f);for(size_t k=3;k<data.size();k+=4)data[k]=1;
    for(int y=28;y<36;++y)for(int x=28;x<36;++x){size_t k=(y*64+x)*4;data[k]=data[k+1]=data[k+2]=16;}
    upload();render(raw.texture,64,64,OutputMode::LinearScene,none);const auto disabled=read();
    PostSettings bloom=none;bloom.bloomIntensity=1;render(raw.texture,64,64,OutputMode::LinearScene,bloom);const auto spread=read();
    const size_t outside=(32*64+24)*4;
    if(disabled[outside]!=0||spread[outside]<=disabled[outside])return false;
    render(raw.texture,64,64,OutputMode::LinearScene,none);if(read()!=disabled)return false;
    // Exercise shrinking chain, odd-sized targets, 1x1 and minimized no-op.
    render(raw.texture,17,9,OutputMode::LinearScene,bloom);render(raw.texture,1,1,OutputMode::LinearScene,bloom);render(raw.texture,0,0,OutputMode::LinearScene,bloom);
    output<<"Post tests passed: RGBA16F HDR >1, finite/nonnegative, sRGB ramps, Unity D3D11 Neutral/None reference max delta "
          <<maxReferenceDelta<<" LSB, mode distinction, legacy Gat, encoded UI sampling, bloom spread/disabled invariant, resize/minimize.\n";
    return glGetError()==GL_NO_ERROR;
}
