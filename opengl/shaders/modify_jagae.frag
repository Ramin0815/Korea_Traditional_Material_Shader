#version 330 core
out vec4 FragColor;

in JAGAE_VERTEX {
    vec3 worldPosition;
    vec3 worldNormal;
    vec2 uv;
} jagae;

uniform vec3 uCameraWorld;
uniform vec3 uLightDirection;
uniform vec3 uLightRadiance;
uniform int uLightMode;
uniform vec3 uAreaCenter, uAreaNormal, uAreaHalfU, uAreaHalfV;
uniform vec3 uAreaPoints[9];
uniform float uAreaPatchAreas[9];
uniform float uTime;
uniform float uPlateletThicknessNm;
uniform float uPlateletIOR;
uniform float uOrganicIOR;
uniform int uLayerCount;
uniform float uOrganicThicknessNm;
uniform float uIridescenceStrength;
uniform float uThicknessVariationNm;
uniform float uSmoothness;
uniform float uClearCoatMask;
uniform float uClearCoatSmoothness;
uniform sampler2D uBaseMap;
uniform sampler2D uThicknessMap;
uniform sampler2D uPatternMask;
uniform int uPatternEnabled;
uniform samplerCube uEnvironmentMap;
uniform sampler2D uClearCoatAlbedo;
uniform float uEnvironmentMaxMip;
uniform vec3 uDiffuseSH[9];
uniform vec4 uBaseColor;
uniform vec4 uBaseMapST;
uniform vec4 uThicknessMapST;
uniform vec3 uPatternBackground;
// Appearance presets: 0=legacy black, 1=neutral white, 2=original cream cells.
uniform int uBaseColorMode;
// Zero in normal rendering. Tests inspect the same production spectral/BRDF paths.
uniform int uNacreTestMode;
uniform vec3 uNacreTestAlbedo;
uniform int uNacreTestResolution;
uniform int uNacreTestLobe;
// Diagnostic reference only; zero in production. Does not disable cell shading.
uniform int uNacreThicknessBaseline;
// Startup comparison only: reproduce the former constant-within-cell facets.
uniform int uNacreDetailBaseline;
// Restore preceding growth-only rendering for this revision's comparison.
uniform int uNacreOpticsBaseline;
// Reproduce the preceding mosaic revision; zero in normal rendering.
uniform int uNacreContinuousBaseline;
uniform int uNacreWaveBaseline;
uniform int uNacreSmoothBaseline;
// Only consumed by diagnostics 24/25; production always samples the legacy map.
uniform float uNacreTestSourceThicknessNm;

const float PI = 3.14159265359;
const float NACRE_CELLS_U = 48.0;
const float NACRE_CELLS_V = 36.0;
const int SPECTRAL_SAMPLE_COUNT = 33;
const float WAVELENGTH_START_NM = 380.0;
const float WAVELENGTH_STEP_NM = 10.0;

// Original deterministic math, authored here. No image or photograph input.
vec2 cellHash(vec2 cell)
{
    // The shared U count closes the sphere/cylinder longitude seam.
    cell.x = mod(cell.x, NACRE_CELLS_U);
    vec3 p = fract(vec3(cell.x, cell.y, cell.x) * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return fract((p.xx + p.yz) * p.zy);
}

vec4 nacreCell(vec2 uv, out vec2 fragmentPoint)
{
    vec2 point = uv * vec2(NACRE_CELLS_U, NACRE_CELLS_V);
    vec2 grid = floor(point), local = fract(point);
    float nearest = 10.0, second = 10.0;
    vec2 seed = vec2(0.0);
    for (int y=-1; y<=1; ++y) for (int x=-1; x<=1; ++x)
    {
        vec2 offset = vec2(x,y);
        vec2 random = cellHash(grid + offset);
        vec2 delta = offset + 0.18 + 0.64 * random - local;
        float distanceSquared = dot(delta,delta);
        if (distanceSquared < nearest) {
            second = nearest; nearest = distanceSquared; seed = random;
            fragmentPoint = -delta;
        } else second = min(second,distanceSquared);
    }
    // Anti-alias cell seams; all color variation stays cream/neutral reflectance.
    float gap = sqrt(second) - sqrt(nearest);
    float filterWidth = max(fwidth(gap), 0.001);
    float seam = 1.0 - smoothstep(0.012-filterWidth,0.012+filterWidth,gap);
    float polish = 0.5 + 0.5*sin(18.0*point.y + 6.0*seed.x);
    return vec4(seed, seam, polish);
}

// A scalar growth field shared by optical thickness and surface finish. Heights
// are in artist-authored cell units, not measured platelet geometry. Its analytic
// gradient gives continuous relief inside each fragment; no RGB hue is stored.
vec4 nacreGrowth(vec2 p, vec2 seed)
{
    float angle = (seed.y - 0.5) * 1.2;
    vec2 along = vec2(cos(angle), sin(angle));
    vec2 across = vec2(-along.y, along.x);
    float u = dot(p, along), v = dot(p, across);
    float curve = 2.0 * PI * (0.45 * u + seed.y);
    float phase = 2.0 * PI * (0.85 * v + 0.18 * sin(curve) + seed.x);
    vec2 gradient = 2.0 * PI * (0.85 * across
        + 0.18 * cos(curve) * 2.0 * PI * 0.45 * along);
    // Suppress unresolved bands and their slopes using the pixel footprint.
    // Seed-boundary derivative spikes also suppress detail at fragment joins.
    float footprint = length(vec2(dFdx(phase), dFdy(phase)));
    float filtered = exp(-0.5 * footprint * footprint);
    float fineFiltered = exp(-4.5 * footprint * footprint);
    float grain = filtered * sin(phase);
    vec2 slope = -(0.004 * filtered * cos(phase)
        + 0.0005 * 3.0 * fineFiltered * cos(3.0 * phase)) * gradient;
    return vec4(slope, grain, filtered);
}

// Smooth growth across the whole UV chart, independent of visible fragments.
// Integer frequencies close the U/V wrap; slopes retain authored cell-unit
// scaling only as an amplitude convention, with no cell search or seed.
vec4 nacrePeriodicGrowth(vec2 uv)
{
    float curve=2.0*PI*4.0*uv.x;
    float phase=2.0*PI*(18.0*uv.y+0.2*sin(curve));
    vec2 gradient=2.0*PI*vec2(0.2*cos(curve)*2.0*PI*4.0,18.0)
        /vec2(NACRE_CELLS_U,NACRE_CELLS_V);
    float footprint=length(vec2(dFdx(phase),dFdy(phase)));
    float filtered=exp(-0.5*footprint*footprint);
    float fineFiltered=exp(-4.5*footprint*footprint);
    vec2 slope=-(0.004*filtered*cos(phase)
        +0.0005*3.0*fineFiltered*cos(3.0*phase))*gradient;
    return vec4(slope,filtered*sin(phase),filtered);
}

// Spatially coherent mean platelet thickness, independent of image RGB and of
// the four layer periods. Integer UV frequencies repeat at both chart borders.
float nacrePeriodicMean(vec2 uv)
{
    float warp=2.0*PI*3.0*uv.x;
    float a=2.0*PI*(2.0*uv.y+0.25*sin(warp));
    float b=2.0*PI*(5.0*uv.x+uv.y);
    float fa=length(vec2(dFdx(a),dFdy(a)));
    float fb=length(vec2(dFdx(b),dFdy(b)));
    return 0.65*sin(a)*exp(-0.5*fa*fa)+0.35*sin(b)*exp(-0.5*fb*fb);
}

float noiseCorner(vec2 p, vec2 period)
{
    p=mod(p,period);
    vec3 h=fract(vec3(p.x,p.y,p.x)*vec3(.1031,.1030,.0973));
    h+=dot(h,h.yzx+33.33);
    return 2.0*fract((h.x+h.y)*h.z)-1.0;
}

// Value plus analytic gradient of quintic-interpolated, tiled random lattice.
// The chart repeats only at its seam; there are no sinusoidal stripe bands.
vec3 smoothField(vec2 uv, vec2 period)
{
    vec2 p=uv*period,g=floor(p),t=fract(p);
    vec2 f=t*t*t*(t*(t*6.0-15.0)+10.0);
    vec2 df=30.0*t*t*(t-1.0)*(t-1.0);
    float a=noiseCorner(g,period),b=noiseCorner(g+vec2(1,0),period);
    float c=noiseCorner(g+vec2(0,1),period),d=noiseCorner(g+vec2(1),period);
    float value=mix(mix(a,b,f.x),mix(c,d,f.x),f.y);
    return vec3(value,df*vec2(mix(b-a,d-c,f.y),mix(c-a,d-b,f.x))*period);
}

bool periodicReference()
{
    return uNacreWaveBaseline!=0 || uNacreContinuousBaseline!=0 || uNacreOpticsBaseline!=0 || uNacreDetailBaseline!=0;
}

vec4 nacreContinuousGrowth(vec2 uv)
{
    if(periodicReference()) return nacrePeriodicGrowth(uv);
    vec3 field=smoothField(uv,vec2(17,13));
    float footprint=length(vec2(length(dFdx(uv)*vec2(17,13)),length(dFdy(uv)*vec2(17,13))));
    float filtered=exp(-3.0*footprint*footprint);
    vec2 slope=-0.012*filtered*field.yz/vec2(NACRE_CELLS_U,NACRE_CELLS_V);
    return vec4(slope,filtered*field.x,filtered);
}

float nacreMeanThickness(vec2 uv)
{
    if(periodicReference()) return nacrePeriodicMean(uv);
    return .65*smoothField(uv,vec2(4,3)).x+.35*smoothField(uv+vec2(.17,.31),vec2(8,6)).x;
}


vec3 facetNormal(vec3 normal, vec2 seed, vec2 uv, vec2 growthSlope)
{
    // Derivative UV tangent, with a safe fallback at collapsed UV sphere poles.
    vec3 px=dFdx(jagae.worldPosition), py=dFdy(jagae.worldPosition);
    vec2 ux=dFdx(uv), uy=dFdy(uv);
    float determinant=ux.x*uy.y-ux.y*uy.x;
    vec3 tangent=px*uy.y-py*ux.y;
    if(determinant<0.0) tangent=-tangent;
    tangent-=normal*dot(normal,tangent);
    if(abs(determinant)<1e-8 || dot(tangent,tangent)<1e-12) {
        vec3 axis=abs(normal.z)<0.9?vec3(0,0,1):vec3(0,1,0);
        tangent=cross(axis,normal);
    }
    tangent=normalize(tangent);
    vec3 bitangent=normalize(cross(normal,tangent));
    // Orient V with the UV derivative, including mirrored charts. Keep the old
    // cell tilt exactly for the comparison; only the new height gradient uses V.
    vec3 uvBitangent=(py*ux.x-px*uy.x)*(determinant<0.0?-1.0:1.0);
    float vSign=dot(uvBitangent,bitangent)<0.0?-1.0:1.0;
    vec2 tilt=(seed*2.0-1.0)*0.052
        + growthSlope*vec2(1.0,vSign);
    // Normalized, finite, always on the geometric normal's front hemisphere.
    return normalize(normal+tangent*tilt.x+bitangent*tilt.y);
}

float cellThickness(float legacyNm, float driver)
{
    // Preserve the original smooth field. Taper cell offsets near the validated
    // 440..560 nm endpoints rather than flattening their sum with a final clamp.
    float headroom = max(min(legacyNm - 440.0, 560.0 - legacyNm), 0.0);
    return legacyNm + driver * min(20.0, headroom);
}

float continuousMeanThickness(float mappedNm, float driver)
{
    // A spatial local mean, shared by all four A/B periods. Keep the legacy
    // scalar map influential while spreading the coherent field over a bounded
    // 150..600 nm design interval. The tanh avoids clipped plateau regions.
    float halfRange = 0.5 * max(uThicknessVariationNm, 0.0);
    float mapDriver = (mappedNm-uPlateletThicknessNm)/max(halfRange,1e-5);
    return max(0.0,uPlateletThicknessNm+halfRange*tanh(2.0*(driver+mapDriver)));
}

vec3 cie1931(float wavelengthNm)
{
    float xs0 = wavelengthNm < 442.0 ? 0.0624 : 0.0374;
    float xs1 = wavelengthNm < 599.8 ? 0.0264 : 0.0323;
    float xs2 = wavelengthNm < 501.1 ? 0.0490 : 0.0382;
    float x = 0.362 * exp(-0.5 * pow((wavelengthNm - 442.0) * xs0, 2.0))
            + 1.056 * exp(-0.5 * pow((wavelengthNm - 599.8) * xs1, 2.0))
            - 0.065 * exp(-0.5 * pow((wavelengthNm - 501.1) * xs2, 2.0));
    float ys0 = wavelengthNm < 568.8 ? 0.0213 : 0.0247;
    float ys1 = wavelengthNm < 530.9 ? 0.0613 : 0.0322;
    float y = 0.821 * exp(-0.5 * pow((wavelengthNm - 568.8) * ys0, 2.0))
            + 0.286 * exp(-0.5 * pow((wavelengthNm - 530.9) * ys1, 2.0));
    float zs0 = wavelengthNm < 437.0 ? 0.0845 : 0.0278;
    float zs1 = wavelengthNm < 459.0 ? 0.0385 : 0.0725;
    float z = 1.217 * exp(-0.5 * pow((wavelengthNm - 437.0) * zs0, 2.0))
            + 0.681 * exp(-0.5 * pow((wavelengthNm - 459.0) * zs1, 2.0));
    return vec3(x, y, z);
}

vec3 xyzToLinearSrgb(vec3 xyz)
{
    return vec3(3.2406 * xyz.x - 1.5372 * xyz.y - 0.4986 * xyz.z,
               -0.9689 * xyz.x + 1.8758 * xyz.y + 0.0415 * xyz.z,
                0.0557 * xyz.x - 0.2040 * xyz.y + 1.0570 * xyz.z);
}

vec2 complexMultiply(vec2 a, vec2 b)
{
    return vec2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

vec2 complexDivide(vec2 a, vec2 b)
{
    float d = max(dot(b, b), 1e-8);
    return vec2(a.x * b.x + a.y * b.y, a.y * b.x - a.x * b.y) / d;
}

vec2 composeLayer(float interfaceReflection, vec2 reflectionBelow, float phase)
{
    vec2 propagated = complexMultiply(reflectionBelow, vec2(cos(phase), sin(phase)));
    return complexDivide(vec2(interfaceReflection, 0.0) + propagated,
                         vec2(1.0, 0.0) + interfaceReflection * propagated);
}

float safeRatio(float numerator, float denominator)
{
    // Fresnel denominators are positive for the clamped physical IOR range.
    // Keep this identical to Modify_Jagae.shader's SafePositiveDivide.
    return numerator / max(denominator, 1e-5);
}

float multilayerReflectance(float wavelengthNm, float plateletThicknessNm,
                            float organicThicknessNm, float cosTheta0,
                            float plateletIOR, float organicIOR, int layerCount, float incidentIOR)
{
    float sinTheta0Squared = clamp(1.0 - cosTheta0 * cosTheta0, 0.0, 1.0) * incidentIOR * incidentIOR;
    float cosThetaA = sqrt(clamp(1.0 - sinTheta0Squared / (plateletIOR * plateletIOR), 0.0, 1.0));
    float cosThetaB = sqrt(clamp(1.0 - sinTheta0Squared / (organicIOR * organicIOR), 0.0, 1.0));

    float rsAirA = safeRatio(incidentIOR * cosTheta0 - plateletIOR * cosThetaA,
                             incidentIOR * cosTheta0 + plateletIOR * cosThetaA);
    float rpAirA = safeRatio(plateletIOR * cosTheta0 - incidentIOR * cosThetaA,
                             plateletIOR * cosTheta0 + incidentIOR * cosThetaA);
    float rsAB = safeRatio(plateletIOR * cosThetaA - organicIOR * cosThetaB,
                           plateletIOR * cosThetaA + organicIOR * cosThetaB);
    float rpAB = safeRatio(organicIOR * cosThetaA - plateletIOR * cosThetaB,
                           organicIOR * cosThetaA + plateletIOR * cosThetaB);
    float phaseA = 4.0 * PI * plateletIOR * plateletThicknessNm * cosThetaA / wavelengthNm;
    float phaseB = 4.0 * PI * organicIOR * organicThicknessNm * cosThetaB / wavelengthNm;
    vec2 effectiveS = vec2(0.0);
    vec2 effectiveP = vec2(0.0);

    // Bottom-up coherent composition of up to eight platelet/organic periods.
    for (int period = 7; period >= 0; --period)
    {
        if (period < layerCount)
        {
            effectiveS = composeLayer(rsAB, effectiveS, phaseB);
            effectiveP = composeLayer(rpAB, effectiveP, phaseB);
            float rsAboveA = period > 0 ? -rsAB : rsAirA;
            float rpAboveA = period > 0 ? -rpAB : rpAirA;
            effectiveS = composeLayer(rsAboveA, effectiveS, phaseA);
            effectiveP = composeLayer(rpAboveA, effectiveP, phaseA);
        }
    }
    return clamp(0.5 * (dot(effectiveS, effectiveS) + dot(effectiveP, effectiveP)), 0.0, 1.0);
}

vec3 spectralInMedium(float thicknessNm, float cosTheta0, float incidentIOR)
{
    vec3 responseXYZ = vec3(0.0);
    vec3 whiteXYZ = vec3(0.0);
    for (int sampleIndex = 0; sampleIndex < SPECTRAL_SAMPLE_COUNT; ++sampleIndex)
    {
        float wavelengthNm = WAVELENGTH_START_NM + WAVELENGTH_STEP_NM * float(sampleIndex);
        vec3 cmf = cie1931(wavelengthNm);
        responseXYZ += multilayerReflectance(wavelengthNm, thicknessNm,
                                             max(uOrganicThicknessNm, 0.0), cosTheta0,
                                             max(uPlateletIOR, 1.0), max(uOrganicIOR, 1.0),
                                             clamp(uLayerCount, 1, 8), incidentIOR) * cmf;
        whiteXYZ += cmf;
    }
    vec3 rgb = xyzToLinearSrgb(responseXYZ) / max(xyzToLinearSrgb(whiteXYZ), vec3(1e-4));
    rgb = max(rgb, vec3(0.0));
    rgb /= max(1.0, max(rgb.r, max(rgb.g, rgb.b)));
    return clamp(rgb, 0.0, 1.0);
}

vec3 spectralMultilayer(float thicknessNm, float cosTheta0)
{
    return spectralInMedium(thicknessNm, cosTheta0, 1.0);
}

float dielectricFresnel(float cosine)
{
    float ct = sqrt(max(0.0, 1.0 - (1.0-cosine*cosine)/2.25));
    float rs = (cosine-1.5*ct)/max(cosine+1.5*ct, 1e-8);
    float rp = (1.5*cosine-ct)/max(1.5*cosine+ct, 1e-8);
    return 0.5*(rs*rs+rp*rp);
}

float smithG1(float cosine, float alpha)
{
    return 2.0*cosine/max(cosine+sqrt(alpha*alpha+(1.0-alpha*alpha)*cosine*cosine),1e-8);
}

float ggxTerm(float nv, float nl, float nh, float alpha)
{
    if(nv<=0.0 || nl<=0.0 || nh<=0.0) return 0.0;
    // Stable denominator at the sharp-lobe floor; normalized NDF D includes PI.
    float d = (1.0-nh*nh) + alpha*alpha*nh*nh;
    float distribution = alpha*alpha/(PI*d*d);
    return distribution*smithG1(nv,alpha)*smithG1(nl,alpha)/(4.0*nv*nl);
}

vec2 directionalAlbedo(float cosine, float alpha)
{
    vec2 grid = vec2(sqrt(clamp(cosine,0.0,1.0)), clamp((log2(alpha)+7.0)/7.0,0.0,1.0));
    return texture(uClearCoatAlbedo, (grid*vec2(127,31)+0.5)/vec2(128,32)).rg;
}

float dielectricBudget(float cosine, float alpha)
{
    // .005 absorbs LUT quadrature/interpolation error, validated by self-tests.
    return min(1.0, directionalAlbedo(cosine,alpha).r+0.005);
}

vec3 coveredSpectrum(float thicknessNm, float externalCosine)
{
    float internalCosine = sqrt(max(0.0,1.0-(1.0-externalCosine*externalCosine)/2.25));
    // Optical boundary changes to coat/nacre; this is an effective external
    // GGX lobe, NOT full refracted-direction/Jacobian layered transport.
    return spectralInMedium(thicknessNm, internalCosine, 1.5);
}

vec3 materialBrdfLobe(float thicknessNm, vec3 albedo, float nv, float nl, float nh, float lh,
                  float alpha, float coatAlpha, float strength, float mask, int lobe)
{
    if(nv<=0.0 || nl<=0.0) return vec3(0);
    float baseTerm=ggxTerm(nv,nl,nh,alpha);
    vec3 bareSpectrum=spectralMultilayer(thicknessNm, lh);
    float bodyTv=1.0-dielectricBudget(nv,alpha),bodyTl=1.0-dielectricBudget(nl,alpha);
    float coatTv=1.0-dielectricBudget(nv,coatAlpha),coatTl=1.0-dielectricBudget(nl,coatAlpha);
    // The non-iridescent body substrate IOR is 1.5, index matched to the coat.
    vec3 baseSpecular=((1.0-mask)*mix(vec3(dielectricFresnel(lh)),bareSpectrum,strength)
        +mask*strength*coveredSpectrum(thicknessNm,lh)*coatTv*coatTl)*baseTerm;
    vec3 coatSpecular=vec3(mask*dielectricFresnel(lh)*ggxTerm(nv,nl,nh,coatAlpha));
    vec3 diffuse=albedo*(1.0-strength)/PI*((1.0-mask)*bodyTv*bodyTl+mask*coatTv*coatTl);
    // Integrate each actual production lobe with its own sharp-GGX PDF.
    if(lobe>=0) {
        if(lobe==0) return baseSpecular;
        if(lobe==1) return coatSpecular;
        return diffuse;
    }
    return baseSpecular+coatSpecular+diffuse;
}

vec3 materialBrdf(float thicknessNm, vec3 albedo, float nv, float nl, float nh, float lh,
                  float alpha, float coatAlpha, float strength, float mask)
{
    return materialBrdfLobe(thicknessNm,albedo,nv,nl,nh,lh,alpha,coatAlpha,strength,mask,
        uNacreTestMode==6?uNacreTestLobe:-1);
}

bool seesRectangle(vec3 position, vec3 direction)
{
    float denominator=dot(direction,uAreaNormal);
    if(denominator>=-1e-6) return false;
    float distance=dot(uAreaCenter-position,uAreaNormal)/denominator;
    if(distance<=1e-5) return false;
    vec3 local=position+distance*direction-uAreaCenter;
    return abs(dot(local,uAreaHalfU))<=dot(uAreaHalfU,uAreaHalfU)
        &&abs(dot(local,uAreaHalfV))<=dot(uAreaHalfV,uAreaHalfV);
}

vec3 rectangleDirect(vec3 position, vec3 n, vec3 v, float thicknessNm, vec3 albedo,
    float alpha, float coatAlpha, float strength, float mask)
{
    float nv=max(dot(n,v),0.0);
    if(nv<=0.0) return vec3(0);
    vec3 axis=abs(n.z)<.9?vec3(0,0,1):vec3(0,1,0);
    vec3 tangent=normalize(cross(axis,n)),bitangent=cross(n,tangent);
    vec3 result=vec3(0);
    // Glossy integration samples the lobes, not nine point-like emitters. This
    // captures the whole finite rectangle even with the sharp alpha=.01 coat.
    const float radical[8]=float[8](0.0,.5,.25,.75,.125,.625,.375,.875);
    for(int lobe=0;lobe<2;++lobe)for(int i=0;i<8;++i) {
        float a=lobe==0?alpha:coatAlpha;
        float xi=(float(i)+.5)/8.0,phi=2.0*PI*radical[i];
        float cosine=sqrt((1.0-xi)/(1.0+(a*a-1.0)*xi));
        float sine=sqrt(max(0.0,1.0-cosine*cosine));
        vec3 h=tangent*(sine*cos(phi))+bitangent*(sine*sin(phi))+n*cosine;
        float vh=dot(v,h);
        if(vh<=1e-6) continue;
        vec3 l=2.0*vh*h-v;
        float nl=max(dot(n,l),0.0);
        if(nl<=0.0 || !seesRectangle(position,l)) continue;
        float d=(1.0-cosine*cosine)+a*a*cosine*cosine;
        float pdf=a*a/(PI*d*d)*cosine/(4.0*vh);
        vec3 response=lobe==0?materialBrdfLobe(thicknessNm,albedo,nv,nl,cosine,vh,
            alpha,coatAlpha,strength,mask,0):vec3(mask*dielectricFresnel(vh)*ggxTerm(nv,nl,cosine,a));
        result+=response*nl/max(pdf,1e-12)/8.0;
    }
    // Diffuse is smooth enough for 3x3 Gauss quadrature of cosEmitter*dA/r².
    // We never normalize these weights per fragment, so finite-distance falloff
    // is retained. Base/coat glossy terms are not counted in this integral.
    float bodyTv=1.0-dielectricBudget(nv,alpha),coatTv=1.0-dielectricBudget(nv,coatAlpha);
    for(int i=0;i<9;++i) {
        vec3 delta=uAreaPoints[i]-position;
        float distanceSquared=dot(delta,delta);
        if(distanceSquared<1e-8) continue;
        vec3 l=delta*inversesqrt(distanceSquared);
        float nl=max(dot(n,l),0.0),emitterCosine=max(dot(uAreaNormal,-l),0.0);
        float weight=emitterCosine*uAreaPatchAreas[i]/distanceSquared;
        vec3 diffuse=albedo*(1.0-strength)/PI*((1.0-mask)*bodyTv*(1.0-dielectricBudget(nl,alpha))
            +mask*coatTv*(1.0-dielectricBudget(nl,coatAlpha)));
        result+=diffuse*nl*weight;
    }
    return result*uLightRadiance;
}

float cosineMeanTransmission(float alpha)
{
    float result=0.0;
    for(int i=0;i<32;++i) result += 1.0-dielectricBudget(sqrt((float(i)+0.5)/32.0),alpha);
    return result/32.0;
}

vec3 sampleDiffuseSH(vec3 n)
{
    // Real SH9, cosine convolved and divided by PI on the CPU, like SampleSH.
    return max(vec3(0.0),
        uDiffuseSH[0]*0.282094792 + uDiffuseSH[1]*(0.488602512*n.y)
        + uDiffuseSH[2]*(0.488602512*n.z) + uDiffuseSH[3]*(0.488602512*n.x)
        + uDiffuseSH[4]*(1.092548431*n.x*n.y) + uDiffuseSH[5]*(1.092548431*n.y*n.z)
        + uDiffuseSH[6]*(0.315391565*(3.0*n.z*n.z-1.0))
        + uDiffuseSH[7]*(1.092548431*n.x*n.z)
        + uDiffuseSH[8]*(0.546274215*(n.x*n.x-n.y*n.y)));
}

vec3 glossyEnvironment(vec3 reflection, float perceptualRoughness)
{
    float p = clamp(perceptualRoughness, 0.0, 1.0);
    // Unity Core PerceptualRoughnessToMipmapLevel. CPU integration inverts this.
    float mip = p * (1.7 - 0.7*p) * uEnvironmentMaxMip;
    return textureLod(uEnvironmentMap, reflection, mip).rgb;
}

void main()
{
    bool opticalTest = (uNacreTestMode > 0 && uNacreTestMode <= 8) || uNacreTestMode==33;
    vec3 geometricNormal = opticalTest ? vec3(0,0,1) : normalize(jagae.worldNormal);
    vec3 n = geometricNormal;
    vec4 cell = vec4(0.5);
    bool historicalCells=uNacreContinuousBaseline!=0 || uNacreOpticsBaseline!=0 || uNacreDetailBaseline!=0;
    bool historicalSurface=historicalCells || uNacreWaveBaseline!=0 || uNacreSmoothBaseline!=0;
    vec4 growth = vec4(0.0);
    float meanThickness=0.0;
    if(!opticalTest) {
        if(historicalCells) {
            vec2 fragmentPoint=vec2(0.0);
            cell=nacreCell(jagae.uv,fragmentPoint);
            if(uNacreDetailBaseline==0) growth=nacreGrowth(fragmentPoint,cell.xy);
        } else growth=nacreContinuousGrowth(jagae.uv);
        meanThickness=nacreMeanThickness(jagae.uv);
        if(historicalSurface) n=facetNormal(n,cell.xy,jagae.uv,growth.xy);
    }
    vec3 v = normalize(uCameraWorld - (opticalTest ? vec3(0) : jagae.worldPosition));
    vec3 l = normalize(uLightDirection);
    float testPdf=1.0;
    if(uNacreTestMode==6) {
        vec2 xi=gl_FragCoord.xy/vec2(uNacreTestResolution,2*uNacreTestResolution);
        float alpha=max(pow(1.0-(uNacreTestLobe==1?uClearCoatSmoothness:uSmoothness),2.0),0.0078125);
        float phi=2.0*PI*xi.y;
        if(uNacreTestLobe==2) {
            float cosine=sqrt(1.0-xi.x),sine=sqrt(xi.x);
            l=vec3(sine*cos(phi),sine*sin(phi),cosine);testPdf=cosine/PI;
        } else {
            float cosine=sqrt((1.0-xi.x)/(1.0+(alpha*alpha-1.0)*xi.x));
            float sine=sqrt(max(0.0,1.0-cosine*cosine));
            vec3 sampleH=vec3(sine*cos(phi),sine*sin(phi),cosine);
            float vh=dot(v,sampleH);l=2.0*vh*sampleH-v;
            float d=(1.0-cosine*cosine)+alpha*alpha*cosine*cosine;
            testPdf=alpha*alpha/(PI*d*d)*cosine/max(4.0*vh,1e-12);
            if(vh<=0.0 || l.z<=0.0){FragColor=vec4(0,0,0,1);return;}
        }
    }
    vec3 lightViewSum = v + l;
    float lightViewSumLengthSquared = dot(lightViewSum, lightViewSum);
    bool hasValidHalfVector = lightViewSumLengthSquared > 1e-6;
    // Unity's SafeNormalize returns zero rather than NaN for a zero-length vector.
    vec3 h = lightViewSum * inversesqrt(max(lightViewSumLengthSquared, 1e-30));
    float nDotV = clamp(dot(n, v), 0.0, 1.0);
    float nDotL = clamp(dot(n, l), 0.0, 1.0);
    float nDotH = clamp(dot(n, h), 0.0, 1.0);
    float lDotH = clamp(dot(l, h), 0.0, 1.0);

    vec2 baseUV = jagae.uv * uBaseMapST.xy + uBaseMapST.zw;
    vec2 thicknessUV = jagae.uv * uThicknessMapST.xy + uThicknessMapST.zw;
    vec3 albedo;
    if(opticalTest) albedo=uNacreTestAlbedo;
    else {
        vec3 tint=vec3(uBaseColorMode==0?.025:.82);
        vec3 mapped=texture(uBaseMap,baseUV).rgb;
        albedo=tint*mapped;
    }
    // Diagnostic35 isolates ordinary and structural reflections without body diffuse.
    if(uNacreTestMode==35) albedo=vec3(0);
    // Jagae_ThicknessMap is uploaded as GL_R8, so this sample stays linear just
    // like the Unity importer's sRGBTexture: 0 setting.
    float region=(!opticalTest&&uPatternEnabled!=0)?texture(uPatternMask,jagae.uv).r:1.0;
    float thicknessField = opticalTest ? 0.5 : texture(uThicknessMap, thicknessUV).r;
    float thicknessNm = max(0.0, uPlateletThicknessNm
                                 + (thicknessField - 0.5) * uThicknessVariationNm);
    if((uNacreTestMode==24 || uNacreTestMode==25) && uNacreTestSourceThicknessNm>=0.0)
        thicknessNm=uNacreTestSourceThicknessNm;
    float legacyThicknessNm=thicknessNm;
    float thicknessDriver = 2.0*cell.x-1.0;
    if(uNacreOpticsBaseline==0 && uNacreDetailBaseline==0) thicknessDriver=meanThickness;
    if(uNacreDetailBaseline==0) thicknessDriver=0.7*thicknessDriver+0.3*growth.z;
    if(!opticalTest && uNacreThicknessBaseline==0)
        thicknessNm=historicalCells || uNacreWaveBaseline!=0
            ?cellThickness(thicknessNm,thicknessDriver)
            :continuousMeanThickness(thicknessNm,thicknessDriver);
    float shownThickness=region==0.0?uPlateletThicknessNm:thicknessNm;
    if(uNacreTestMode==34) {FragColor=vec4(region,uIridescenceStrength*region,shownThickness,1);return;}
    if(uNacreTestMode==24) { FragColor=vec4(shownThickness,region==0.0?uPlateletThicknessNm:legacyThicknessNm,
        region==0.0?0.5:0.5+0.5*thicknessDriver,1);return; }
    if(uNacreTestMode==28) { FragColor=vec4(cell.x,growth.z,growth.w,1);return; }
    if(uNacreTestMode==30) { FragColor=vec4(meanThickness,growth.z,0,1);return; }
    if(uNacreTestMode==31) { FragColor=vec4(geometricNormal,1);return; }
    if(uNacreTestMode==29) {
        vec2 wrappedPoint=vec2(0);
        vec4 wrappedCell=vec4(0.5);
        vec4 wrappedGrowth;
        if(historicalCells) {
            wrappedCell=nacreCell(jagae.uv+vec2(1,0),wrappedPoint);
            wrappedGrowth=nacreGrowth(wrappedPoint,wrappedCell.xy);
        } else wrappedGrowth=nacreContinuousGrowth(jagae.uv+vec2(1,0));
        FragColor=vec4(length(cell.xy-wrappedCell.xy),
            length(growth.xy-wrappedGrowth.xy),
            max(abs(growth.z-wrappedGrowth.z),abs(meanThickness-nacreMeanThickness(jagae.uv+vec2(1,0)))),1);return;
    }
    // Fixed source thickness AND incidence isolate neighboring cell spectra from
    // camera position, facets, roughness, body color, coat and illumination.
    if(uNacreTestMode==25) { FragColor=vec4(spectralMultilayer(thicknessNm,0.866025404),1);return; }

    float iridescenceStrength = clamp(uIridescenceStrength, 0.0, 1.0);
    float clearCoatMask = clamp(uClearCoatMask, 0.0, 1.0);
    if(uNacreTestMode==1) { FragColor=vec4(spectralMultilayer(thicknessNm, lDotH),1);return; }
    if(uNacreTestMode==3) { FragColor=vec4(coveredSpectrum(thicknessNm,lDotH),1);return; }
    float basePerceptualRoughness = clamp(1.0 - uSmoothness, 0.0, 1.0);
    if(!opticalTest && historicalSurface)
        basePerceptualRoughness=clamp(basePerceptualRoughness+(historicalCells?(cell.y-0.5)*0.10+0.02*cell.z:0.0)
            +0.012*growth.z,0.16,0.28);
    float baseRoughness = max(basePerceptualRoughness * basePerceptualRoughness, 0.0078125);
    float coatPerceptualRoughness = clamp(1.0 - uClearCoatSmoothness, 0.0, 1.0);
    float coatRoughness = max(coatPerceptualRoughness * coatPerceptualRoughness, 0.0078125);
    vec3 unlitBackground=uPatternBackground;
    if(uNacreTestMode==20) { FragColor=vec4(region==0.0?unlitBackground:region==1.0?albedo:mix(unlitBackground,albedo,region),1);return; }
    if(uNacreTestMode==21) { FragColor=vec4(n,dot(n,geometricNormal));return; }
    if(uNacreTestMode==22) { FragColor=vec4(vec3(basePerceptualRoughness),1);return; }
    if(uNacreTestMode==23) { FragColor=vec4(spectralMultilayer(thicknessNm,lDotH)*iridescenceStrength*region,1);return; }

    bool useArea=(uLightMode==1&&!opticalTest) || uNacreTestMode==33;
    vec3 directBrdf=hasValidHalfVector&&!useArea ? materialBrdf(thicknessNm,clamp(albedo,0.0,1.0),
        nDotV,nDotL,nDotH,lDotH,baseRoughness,coatRoughness,iridescenceStrength,clearCoatMask) : vec3(0);
    if (uNacreTestMode == 2) { FragColor = vec4(directBrdf, 1); return; }
    if(uNacreTestMode==6) { FragColor=vec4(directBrdf*nDotL/testPdf,1);return; }
    if(uNacreTestMode==7) { FragColor=vec4(directionalAlbedo(nDotV,coatRoughness),dielectricBudget(nDotV,coatRoughness),1);return; }
    if(uNacreTestMode==4) { FragColor=vec4(vec3(dielectricFresnel(lDotH)*ggxTerm(nDotV,nDotL,nDotH,coatRoughness)),1);return; }
    vec3 direct=directBrdf*nDotL*uLightRadiance;
    if(useArea)
        direct=rectangleDirect(opticalTest?vec3(0):jagae.worldPosition,n,v,thicknessNm,
            clamp(albedo,0.0,1.0),baseRoughness,coatRoughness,iridescenceStrength,clearCoatMask);
    // Production geometry/material diagnostics, unlike flat optical modes 1..8.
    if(uNacreTestMode==27 || uNacreTestMode==33) { FragColor=vec4(direct*region,1);return; }

    // Bounded split-sum approximation: mirror-angle spectrum is multiplied by
    // unit-Fresnel directional GGX energy, never treated as F0 or Schlick again.
    // Incoming transmission uses mirror V for specular and cosine mean for SH.
    // This preserves the same BSDF budgets but not the exact angular integral.
    vec3 reflection = reflect(-v, n);
    vec2 baseEnergy=directionalAlbedo(nDotV,baseRoughness);
    vec2 coatEnergy=directionalAlbedo(nDotV,coatRoughness);
    float bodyT=1.0-dielectricBudget(nDotV,baseRoughness);
    float coatT=1.0-dielectricBudget(nDotV,coatRoughness);
    vec3 baseEnvironment = glossyEnvironment(reflection, basePerceptualRoughness);
    vec3 coatEnvironment = glossyEnvironment(reflection, coatPerceptualRoughness);
    vec3 diffuseEnvironment=sampleDiffuseSH(n)*clamp(albedo,0.0,1.0)*(1.0-iridescenceStrength);
    vec3 indirectBare=baseEnvironment*mix(vec3(baseEnergy.r),
        spectralMultilayer(thicknessNm,nDotV)*baseEnergy.g,iridescenceStrength)
        +diffuseEnvironment*bodyT*cosineMeanTransmission(baseRoughness);
    vec3 indirectCovered=coatEnvironment*coatEnergy.r
        +baseEnvironment*coveredSpectrum(thicknessNm,nDotV)*baseEnergy.g*iridescenceStrength*coatT*coatT
        +diffuseEnvironment*coatT*cosineMeanTransmission(coatRoughness);
    vec3 indirect=mix(indirectBare,indirectCovered,clearCoatMask);
    if(uNacreTestMode==5 || uNacreTestMode==26) { FragColor=vec4(indirect*region,1);return; }
    // Deliberate display convention, not a physical material outside the inlay:
    // convex coverage blend in linear RGB, then ordinary display encoding.
    vec3 color = region==0.0?unlitBackground:region==1.0?direct+indirect:mix(unlitBackground,direct+indirect,region);
    // Scene radiance stays linear HDR. Only sanitize invalid/negative channels
    // and overflow beyond finite half-float range, never clip highlights to 1.
    for (int channel=0; channel<3; ++channel)
        color[channel] = (isnan(color[channel]) || isinf(color[channel])) ? 0.0
            : clamp(color[channel], 0.0, 65504.0);
    FragColor = vec4(color, 1.0);
}
