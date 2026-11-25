#version 330 core
out vec4 FragColor;

in VS_OUT {
    vec3 localPosition;
    vec3 localNormal;
    vec3 worldPosition;
    vec3 worldNormal;
} fs;

uniform vec3 uCameraWorld;
uniform mat4 uModel;
uniform vec3 uAxisMillimeters;
uniform vec4 uBoundsY; // min Y, max Y
uniform vec4 uGat;     // inner diameter (m), density, thread width (m), rim height (m)
uniform float uRole;   // 0 = procedural shell, 1 = real rounded rim mesh

const float PI2 = 6.28318530718;
const float CoreRadiusMM = 0.001;

float periodicBoxIntegral(float x, float halfWidth)
{
    float width = 2.0 * halfWidth;
    return floor(x) * width + clamp(fract(x) - (0.5 - halfWidth), 0.0, width);
}

float stripeCoverage(float coordinate, float halfWidth, float footprint)
{
    halfWidth = clamp(halfWidth, 0.0, 0.5);
    footprint = max(abs(footprint), 1e-5);
    float center = fract(coordinate);
    float lo = center - 0.5 * footprint;
    float hi = center + 0.5 * footprint;
    return clamp((periodicBoxIntegral(hi, halfWidth) - periodicBoxIntegral(lo, halfWidth)) / footprint, 0.0, 1.0);
}

float signedThreadCoordinate(float coordinate) { return fract(coordinate + 0.5) - 0.5; }
float roundedProfile(float d) { return sqrt(max(0.0, 1.0 - d * d)); }

vec3 fakeThreadNormal(vec3 baseNormal, vec3 crossThreadDirection, float distanceFromCenter)
{
    float profile = max(roundedProfile(distanceFromCenter), 0.2);
    float slope = clamp(distanceFromCenter / profile, -2.5, 2.5);
    return normalize(normalize(baseNormal) + normalize(crossThreadDirection) * slope);
}

vec3 shadeBlackYarn(vec3 normal, vec3 tangent, float contact)
{
    vec3 n = normalize(normal);
    vec3 t = normalize(tangent);
    vec3 viewDirection = normalize(uCameraWorld - fs.worldPosition);
    vec3 lightDirection = normalize(vec3(0.35, 0.85, 0.40));
    vec3 halfDirection = normalize(lightDirection + viewDirection);
    float diffuse = 0.004 + 0.012 * max(dot(n, lightDirection), 0.0);
    float longitudinal = 1.0 - abs(dot(t, halfDirection));
    float highlight = 0.16 * pow(max(dot(n, halfDirection), 0.0), 36.0) * max(longitudinal, 0.0);
    return vec3((diffuse + highlight) * (1.0 - 0.58 * clamp(contact, 0.0, 1.0)));
}

void main()
{
    vec3 meshNormal = normalize(fs.worldNormal);
    vec2 planarMM = fs.localPosition.xz * uAxisMillimeters.xz;
    vec2 dxMM = dFdx(planarMM);
    vec2 dyMM = dFdy(planarMM);
    float radiusMM = length(planarMM);
    float radiusSquared = dot(planarMM, planarMM);
    float safeRadius = max(radiusMM, 1e-5);
    float density = clamp(floor(abs(uGat.y) * 0.5 + 0.5) * 2.0, 2.0, 2048.0);
    float scale = density / PI2;
    // Match Gat.shader: the public thread-thickness range is 0.001–0.25 mm.
    float widthMM = clamp(uGat.z * 1000.0, 0.001, 0.25);

    if (uRole > 0.5)
    {
        vec2 p = fs.localPosition.xz;
        vec3 tangent = dot(p, p) > 1e-8 ? normalize(vec3(-p.y, 0.0, p.x)) : vec3(0.0, 0.0, 1.0);
        FragColor = vec4(shadeBlackYarn(meshNormal, mat3(uModel) * tangent, 0.0), 1.0);
        return;
    }

    float innerRadiusMM = max(uGat.x * 1000.0, 0.0) * 0.5;
    float safeInner = max(innerRadiusMM, CoreRadiusMM);
    float ringCoordinate = scale * (log(max(radiusMM, CoreRadiusMM) / safeInner)
                         + (min(radiusMM, CoreRadiusMM) - min(innerRadiusMM, CoreRadiusMM)) / CoreRadiusMM);
    float radialDerivative = scale / max(radiusMM, CoreRadiusMM);
    vec2 radialGradient = planarMM / safeRadius;
    float ringFootprint = (abs(dot(radialGradient, dxMM)) + abs(dot(radialGradient, dyMM))) * radialDerivative;
    if (radiusSquared <= 1e-8) ringFootprint = (length(dxMM) + length(dyMM)) * radialDerivative;

    float angle = radiusSquared > 1e-8 ? atan(planarMM.y, planarMM.x) : 0.0;
    float spokeCoordinate = angle * scale;
    float spokeHalfWidth = min(asin(clamp(0.5 * widthMM / safeRadius, 0.0, 1.0)) * scale, 0.5);
    vec2 angularGradient = vec2(-planarMM.y, planarMM.x) / max(radiusSquared, 1e-8);
    float spokeFootprint = (abs(dot(angularGradient, dxMM)) + abs(dot(angularGradient, dyMM))) * scale;
    if (radiusSquared <= 1e-8) spokeFootprint = density;

    float ring = stripeCoverage(ringCoordinate + 0.5, 0.5 * widthMM * radialDerivative, ringFootprint);
    float spoke = stripeCoverage(spokeCoordinate + 0.5, spokeHalfWidth, spokeFootprint);
    float capCoverage = ring + spoke - ring * spoke;

    float pitchMM = max(PI2 * safeRadius / density, 1e-5);
    float yMM = fs.localPosition.y * uAxisMillimeters.y;
    float yFromBottomMM = yMM - uBoundsY.x * uAxisMillimeters.y;
    float yToTopMM = uBoundsY.y * uAxisMillimeters.y - yMM;
    float horizontalCoordinate = yFromBottomMM / pitchMM;
    float vertical = stripeCoverage(spokeCoordinate + 0.5, spokeHalfWidth, spokeFootprint);
    float horizontal = stripeCoverage(horizontalCoordinate + 0.5, min(0.5 * widthMM / pitchMM, 0.5), abs(dFdx(horizontalCoordinate)) + abs(dFdy(horizontalCoordinate)));
    float sideCoverage = vertical + horizontal - vertical * horizontal;
    float rimMask = max(step(yFromBottomMM, uGat.w * 1000.0), step(yToTopMM, uGat.w * 1000.0));
    sideCoverage = max(sideCoverage, rimMask);

    bool cap = abs(normalize(fs.localNormal).y) >= 0.5;
    if (cap && radiusMM < innerRadiusMM) discard;
    float coverage = cap ? capCoverage : sideCoverage;
    if (coverage < 0.0001) discard;

    vec3 radial = radiusSquared > 1e-8 ? normalize(vec3(planarMM.x, 0.0, planarMM.y)) : vec3(1.0, 0.0, 0.0);
    vec3 angular = vec3(-radial.z, 0.0, radial.x);
    vec3 baseNormal = meshNormal;
    mat3 directionToWorld = mat3(uModel);
    float halfWidth = 0.5 * widthMM;
    vec3 colour = vec3(0.0);

    if (cap)
    {
        float ringDistance = (signedThreadCoordinate(ringCoordinate) / max(radialDerivative, 1e-5)) / max(halfWidth, 1e-5);
        float spokeDistance = (safeRadius * sin(signedThreadCoordinate(spokeCoordinate) / max(scale, 1e-5))) / max(halfWidth, 1e-5);
        float ringOnTop = mod(abs(floor(ringCoordinate + 0.5) + floor(spokeCoordinate + 0.5)), 2.0) < 0.5 ? 1.0 : 0.0;
        float crossing = ring * spoke;
        float ringVisible = ring * (1.0 - spoke) + crossing * ringOnTop;
        float spokeVisible = spoke * (1.0 - ring) + crossing * (1.0 - ringOnTop);
        vec3 ringN = fakeThreadNormal(baseNormal, directionToWorld * radial, ringDistance);
        vec3 spokeN = fakeThreadNormal(baseNormal, directionToWorld * angular, spokeDistance);
        colour = shadeBlackYarn(ringN, directionToWorld * angular, (1.0 - ringOnTop) * (1.0 - abs(spokeDistance))) * ringVisible
               + shadeBlackYarn(spokeN, directionToWorld * radial, ringOnTop * (1.0 - abs(ringDistance))) * spokeVisible;
    }
    else if (rimMask > 0.5)
    {
        colour = vec3(0.0); // opaque contact underlay behind the CPU-generated rim.
    }
    else
    {
        float verticalDistance = (safeRadius * sin(signedThreadCoordinate(spokeCoordinate) / max(scale, 1e-5))) / max(halfWidth, 1e-5);
        float horizontalDistance = (signedThreadCoordinate(horizontalCoordinate) * pitchMM) / max(halfWidth, 1e-5);
        float verticalOnTop = mod(abs(floor(spokeCoordinate + 0.5) + floor(horizontalCoordinate + 0.5)), 2.0) < 0.5 ? 1.0 : 0.0;
        float crossing = vertical * horizontal;
        float verticalVisible = vertical * (1.0 - horizontal) + crossing * verticalOnTop;
        float horizontalVisible = horizontal * (1.0 - vertical) + crossing * (1.0 - verticalOnTop);
        vec3 verticalN = fakeThreadNormal(baseNormal, directionToWorld * angular, verticalDistance);
        vec3 horizontalN = fakeThreadNormal(baseNormal, directionToWorld * vec3(0.0, 1.0, 0.0), horizontalDistance);
        colour = shadeBlackYarn(verticalN, directionToWorld * vec3(0.0, 1.0, 0.0), (1.0 - verticalOnTop) * (1.0 - abs(horizontalDistance))) * verticalVisible
               + shadeBlackYarn(horizontalN, directionToWorld * angular, verticalOnTop * (1.0 - abs(verticalDistance))) * horizontalVisible;
    }
    FragColor = vec4(colour, 1.0);
}
