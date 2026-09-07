Shader "URP/Unlit/Gat"
{
    Properties
    {
        _InnerDiameterMM ("Inner Diameter (mm)", Float) = 100.0
        _ObjectUnitsToMillimeters ("World Units To Millimeters", Float) = 1000.0
        [IntRange] _Density ("Weave Density", Range(2, 2048)) = 512
        _Thickness ("Thread Thickness (mm)", Range(0.001, 0.25)) = 0.05
        _RimHeightMM ("Rim Height (mm)", Range(0, 20)) = 2.0
        [HideInInspector] _GatLocalYBounds ("Gat Local Y Bounds", Vector) = (-1.0, 1.0, 0.0, 0.0)
    }

    SubShader
    {
        Tags
        {
            "RenderType"="Transparent"
            "Queue"="Transparent"
            "RenderPipeline"="UniversalPipeline"
        }

        // PASS 1: flat black cap and cylindrical-side thread pattern.
        Pass
        {
            Name "ForwardUnlit"
            Tags { "LightMode"="SRPDefaultUnlit" }

            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite On
            Cull Off

            HLSLPROGRAM
            #pragma target 3.5
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_fog

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            float _InnerDiameterMM;
            float _ObjectUnitsToMillimeters;
            float _Density;
            float _Thickness;
            float _RimHeightMM;
            float4 _GatLocalYBounds;

            struct Attributes
            {
                float4 positionOS : POSITION;
                float3 normalOS   : NORMAL;
            };

            struct Varyings
            {
                float4 positionHCS  : SV_POSITION;
                float3 positionOS   : TEXCOORD0;
                float3 normalOS     : TEXCOORD1;
                float  fogFactor    : TEXCOORD2;
            };

            static const float GAT_TWO_PI = 6.28318530718;
            static const float GAT_RADIAL_CORE_RADIUS_MM = 0.001;
            static const float GAT_CAP_NORMAL_Y_THRESHOLD = 0.5;

            // Antiderivative of a unit-period box centered at 0.5.
            float PeriodicBoxIntegral(float x, float halfWidth)
            {
                float width = 2.0 * halfWidth;
                return floor(x) * width
                     + clamp(frac(x) - (0.5 - halfWidth), 0.0, width);
            }

            // Analytic box-filtered coverage for a periodic stripe train.
            float PeriodicStripeCoverage(float centerCoord, float halfWidth, float footprint)
            {
                halfWidth = clamp(halfWidth, 0.0, 0.5);
                footprint = max(abs(footprint), 1e-5);

                // Integer shifts do not change the pattern. Reducing the coordinate avoids
                // subtracting large antiderivative values on distant rings.
                float center = frac(centerCoord);
                float lo = center - 0.5 * footprint;
                float hi = center + 0.5 * footprint;
                float coverage = (PeriodicBoxIntegral(hi, halfWidth)
                                - PeriodicBoxIntegral(lo, halfWidth)) / footprint;
                return saturate(coverage);
            }

            float EvenRadialCount(float requestedCount)
            {
                float count = clamp(abs(requestedCount), 2.0, 2048.0);
                return clamp(floor(count * 0.5 + 0.5) * 2.0, 2.0, 2048.0);
            }

            // Logarithmic outside a tiny fixed core, with a continuous linear
            // extension inside it. The fixed core prevents the mapping from becoming
            // singular at the pole without making ring positions depend on thickness.
            float RadialWeaveCoordinate(
                float radiusMM,
                float innerRadiusMM,
                float coordinateScale)
            {
                float safeRadiusMM = max(radiusMM, GAT_RADIAL_CORE_RADIUS_MM);
                float safeInnerRadiusMM = max(innerRadiusMM, GAT_RADIAL_CORE_RADIUS_MM);
                float linearExtension = (min(radiusMM, GAT_RADIAL_CORE_RADIUS_MM)
                                       - min(innerRadiusMM, GAT_RADIAL_CORE_RADIUS_MM))
                                      / GAT_RADIAL_CORE_RADIUS_MM;
                return coordinateScale
                     * (log(safeRadiusMM / safeInnerRadiusMM)
                      + linearExtension);
            }

            Varyings vert(Attributes IN)
            {
                Varyings OUT;
                OUT.positionHCS = TransformObjectToHClip(IN.positionOS.xyz);
                OUT.positionOS = IN.positionOS.xyz;
                OUT.normalOS = IN.normalOS;
                OUT.fogFactor = ComputeFogFactor(OUT.positionHCS.z);
                return OUT;
            }

            half4 frag(Varyings IN) : SV_Target
            {
                float millimetersPerWorldUnit = max(abs(_ObjectUnitsToMillimeters), 1e-4);
                float4x4 objectToWorld = GetObjectToWorldMatrix();
                float xMillimetersPerObjectUnit = length(float3(
                    objectToWorld._m00, objectToWorld._m10, objectToWorld._m20))
                    * millimetersPerWorldUnit;
                float yMillimetersPerObjectUnit = length(float3(
                    objectToWorld._m01, objectToWorld._m11, objectToWorld._m21))
                    * millimetersPerWorldUnit;
                float zMillimetersPerObjectUnit = length(float3(
                    objectToWorld._m02, objectToWorld._m12, objectToWorld._m22))
                    * millimetersPerWorldUnit;
                float2 planarMM = IN.positionOS.xz
                                * float2(xMillimetersPerObjectUnit,
                                         zMillimetersPerObjectUnit);
                float2 dxMM = ddx(planarMM);
                float2 dyMM = ddy(planarMM);
                float yMM = IN.positionOS.y * yMillimetersPerObjectUnit;
                float radiusMM = length(planarMM);
                float radiusSquared = dot(planarMM, planarMM);
                float innerRadiusMM = 0.5 * max(_InnerDiameterMM, 0.0);
                float radialCount = EvenRadialCount(_Density);
                float fullWidthMM = clamp(_Thickness, 0.001, 0.25);
                float coordinateScale = radialCount / GAT_TWO_PI;

                float safeRadius = max(radiusMM, 1e-5);
                float2 radialGradient = planarMM / safeRadius;
                float ringCoord = RadialWeaveCoordinate(
                    radiusMM, innerRadiusMM, coordinateScale);
                float radialCoordinateDerivative = coordinateScale
                                                 / max(radiusMM, GAT_RADIAL_CORE_RADIUS_MM);
                float ringFootprint = (abs(dot(radialGradient, dxMM))
                                     + abs(dot(radialGradient, dyMM)))
                                    * radialCoordinateDerivative;
                if (radiusSquared <= 1e-8)
                    ringFootprint = (length(dxMM) + length(dyMM))
                                  * radialCoordinateDerivative;

                // Locally linearize the radial mapping so _Thickness remains a
                // physical full width in millimeters as logarithmic spacing expands.
                float ringHalfWidth = 0.5 * fullWidthMM
                                    * radialCoordinateDerivative;

                float angle = radiusSquared > 1e-8 ? atan2(planarMM.y, planarMM.x) : 0.0;
                float spokeCoord = angle * coordinateScale;

                // The angular half-width is derived from perpendicular distance to a
                // straight radial thread, so both thread families have the same width in mm.
                float spokeHalfAngle = asin(saturate((0.5 * fullWidthMM) / safeRadius));
                float spokeHalfWidth = min(spokeHalfAngle * coordinateScale, 0.5);

                // Differentiate atan analytically in XZ to remain continuous at +/-pi.
                float2 angularGradient = float2(-planarMM.y, planarMM.x)
                                       / max(radiusSquared, 1e-8);
                float spokeFootprint = (abs(dot(angularGradient, dxMM))
                                      + abs(dot(angularGradient, dyMM)))
                                     * coordinateScale;
                if (radiusSquared <= 1e-8)
                    spokeFootprint = radialCount;

                // +0.5 converts integer-centered line coordinates to the helper's
                // half-integer-centered box convention.
                float ring = PeriodicStripeCoverage(
                    ringCoord + 0.5, ringHalfWidth, ringFootprint);
                float spoke = PeriodicStripeCoverage(
                    spokeCoord + 0.5, spokeHalfWidth, spokeFootprint);
                float capAlpha = saturate(ring + spoke - ring * spoke);

                // A side is parameterized by angle and distance from the mesh's
                // bottom bound. Its vertical and horizontal thread pitches both
                // approximate the local circumference divided by N.
                float sidePitchMM = max(GAT_TWO_PI * safeRadius / radialCount, 1e-5);
                float sideVerticalCoord = spokeCoord;
                float sideVerticalHalfWidth = spokeHalfWidth;
                float sideVerticalFootprint = spokeFootprint;
                float localYMin = min(_GatLocalYBounds.x, _GatLocalYBounds.y);
                float localYMax = max(_GatLocalYBounds.x, _GatLocalYBounds.y);
                float yFromBottomMM = yMM - localYMin * yMillimetersPerObjectUnit;
                float yToTopMM = localYMax * yMillimetersPerObjectUnit - yMM;
                float sideHorizontalCoord = yFromBottomMM / sidePitchMM;
                float sideHorizontalHalfWidth = min(0.5 * fullWidthMM / sidePitchMM, 0.5);
                float sideHorizontalFootprint = abs(ddx(sideHorizontalCoord))
                                              + abs(ddy(sideHorizontalCoord));
                float sideVertical = PeriodicStripeCoverage(
                    sideVerticalCoord + 0.5,
                    sideVerticalHalfWidth,
                    sideVerticalFootprint);
                float sideHorizontal = PeriodicStripeCoverage(
                    sideHorizontalCoord + 0.5,
                    sideHorizontalHalfWidth,
                    sideHorizontalFootprint);
                float sideAlpha = saturate(sideVertical + sideHorizontal
                                          - sideVertical * sideHorizontal);

                float rimHeightMM = max(_RimHeightMM, 0.0);
                float totalHeightMM = max(yFromBottomMM + yToTopMM, 0.0);
                float rimMask = max(step(yFromBottomMM, rimHeightMM),
                                    step(yToTopMM, rimHeightMM));
                rimMask = max(rimMask, step(totalHeightMM, 2.0 * rimHeightMM));
                sideAlpha = max(sideAlpha, rimMask);

                float capMask = step(GAT_CAP_NORMAL_Y_THRESHOLD,
                                     abs(normalize(IN.normalOS).y));
                float alpha = lerp(sideAlpha, capAlpha, capMask);

                // Keep derivatives above the discard. The mesh itself supplies the outer bound.
                // The opening is only cut from upward/downward-facing cap triangles.
                clip(lerp(1.0, radiusMM - innerRadiusMM, capMask));
                clip(alpha - 0.0001);

                float3 finalColor = MixFog(float3(0.0, 0.0, 0.0), IN.fogFactor);
                return half4(finalColor, alpha);
            }

            ENDHLSL
        }

    }
}
