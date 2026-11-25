#version 330 core
in vec2 uv;
out vec4 FragColor;
uniform sampler2D sourceTex;
uniform sampler2D lowTex;
uniform int stage; // 0 prefilter, 1 horizontal/down, 2 vertical, 3 up, 4 display, 5 legacy
uniform int toneMode;
uniform float thresholdLinear, scatter, bloomClamp, bloomIntensity;
uniform float vignetteIntensity, vignetteSmoothness;

// Port of URP17/Core Color.hlsl NeutralCurve/NeutralTonemap, not ACES.
vec3 neutralCurve(vec3 x)
{
    return ((x*(0.2*x+0.24*0.29)+0.272*0.02)/(x*(0.2*x+0.29)+0.272*0.3))-0.02/0.3;
}
vec3 neutral(vec3 x)
{
    vec3 whiteScale = vec3(1.0)/neutralCurve(vec3(5.3));
    return neutralCurve(max(x,vec3(0.0))*whiteScale)*whiteScale;
}
vec3 encodeSRGB(vec3 x)
{
    x = clamp(x,0.0,1.0);
    return mix(1.055*pow(x,vec3(1.0/2.4))-0.055,12.9232102*x,lessThanEqual(x,vec3(0.0031308)));
}
vec3 sampleOffset(vec2 offset) { return texture(sourceTex,uv+offset).rgb; }
void main()
{
    vec3 c=texture(sourceTex,uv).rgb;
    vec2 texel=1.0/vec2(textureSize(sourceTex,0));
    if(stage==0) // Unity low-quality bilinear prefilter (HQ is false in reference).
    {
        c=min(c,vec3(bloomClamp));
        float brightness=max(c.r,max(c.g,c.b)), knee=thresholdLinear*0.5;
        float softness=clamp(brightness-thresholdLinear+knee,0.0,2.0*knee);
        softness=softness*softness/(4.0*knee+1e-4);
        c*=max(brightness-thresholdLinear,softness)/max(brightness,1e-4);
        c=max(c,vec3(0.0));
    }
    else if(stage==1)
    {
        const float weights[9]=float[9](0.01621622,0.05405405,0.12162162,0.19459459,
            0.22702703,0.19459459,0.12162162,0.05405405,0.01621622);
        c=vec3(0.0);
        for(int i=0;i<9;++i)
        {
            // Unity passes doubled texel size to ClampUVForBilinear here.
            vec2 p=clamp(uv+vec2(float(i-4)*texel.x*2.0,0.0),texel,vec2(1.0)-texel);
            c+=texture(sourceTex,p).rgb*weights[i];
        }
    }
    else if(stage==2)
    {
        c=sampleOffset(vec2(0,-texel.y*3.23076923))*0.07027027
          +sampleOffset(vec2(0,-texel.y*1.38461538))*0.31621622
          +c*0.22702703
          +sampleOffset(vec2(0,texel.y*1.38461538))*0.31621622
          +sampleOffset(vec2(0,texel.y*3.23076923))*0.07027027;
    }
    else if(stage==3) c=mix(c,texture(lowTex,uv).rgb,scatter);
    else if(stage==4)
    {
        if(bloomIntensity>0.0) c+=texture(lowTex,uv).rgb*bloomIntensity;
        // URP Uber applies vignette before color grading/tonemapping.
        vec2 dist=abs(uv-vec2(0.5))*vignetteIntensity*3.0;
        c*=pow(clamp(1.0-dot(dist,dist),0.0,1.0),vignetteSmoothness*5.0);
        if(toneMode==1) c=neutral(c);
        c=encodeSRGB(c);
    }
    // stage5 deliberately copies historical Gat display code values unchanged.
    FragColor=vec4(c,1.0);
}
