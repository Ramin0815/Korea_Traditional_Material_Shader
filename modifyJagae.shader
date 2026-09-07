Shader "Custom/ModifyJagaeSpectral"
{
    Properties
    {
        [Header(Base Properties)]
        _BaseColor("Base Color", Color) = (1, 1, 1, 1)
        [MainTexture] _BaseMap("Base Map", 2D) = "white" {}
        _Smoothness("Smoothness", Range(0.0, 1.0)) = 0.9

        [Header(Multilayer Spectral Iridescence)]
        _IridescenceThickness("Platelet Thickness (nm)", Range(0.0, 1000.0)) = 500.0
        _IridescenceIOR("Platelet IOR", Range(1.0, 3.0)) = 1.63
        _SubstrateIOR("Organic IOR", Range(1.0, 3.0)) = 1.43
        [IntRange] _LayerCount("Platelet Layers", Range(1, 8)) = 6
        _InterlayerThickness("Organic Layer Thickness (nm)", Range(0.0, 200.0)) = 30.0
        _IridescenceStrength("Iridescence Strength", Range(0.0, 1.0)) = 0.8
        _ThicknessMap("Thickness Map", 2D) = "gray" {}
        _ThicknessVariation("Thickness Variation (nm)", Range(0.0, 500.0)) = 250.0
    }

    SubShader
    {
        Tags
        {
            "RenderPipeline" = "UniversalPipeline"
            "RenderType" = "Opaque"
            "Queue" = "Geometry"
        }

        ZWrite On
        Cull Back

        Pass
        {
            Name "UniversalForward"
            Tags { "LightMode" = "UniversalForward" }

            HLSLPROGRAM
            #pragma target 3.5
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_instancing
            #pragma multi_compile_fog
            #pragma multi_compile _ _MAIN_LIGHT_SHADOWS _MAIN_LIGHT_SHADOWS_CASCADE _MAIN_LIGHT_SHADOWS_SCREEN
            #pragma multi_compile _ _ADDITIONAL_LIGHTS_VERTEX _ADDITIONAL_LIGHTS
            #pragma multi_compile _ _ADDITIONAL_LIGHT_SHADOWS
            #pragma multi_compile _ _SHADOWS_SOFT
            #pragma multi_compile _ _MIXED_LIGHTING_SUBTRACTIVE
            #pragma multi_compile_fragment _ _SCREEN_SPACE_OCCLUSION

            // UniversalFragmentPBR selects the explicit specular workflow from this keyword.
            #define _SPECULAR_SETUP 1

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Lighting.hlsl"

            static const float JAGAE_PI = 3.14159265359;
            static const int JAGAE_SPECTRAL_SAMPLE_COUNT = 17;
            static const float JAGAE_WAVELENGTH_START_NM = 380.0;
            static const float JAGAE_WAVELENGTH_STEP_NM = 20.0;

            struct Attributes
            {
                float4 positionOS : POSITION;
                float3 normalOS : NORMAL;
                float2 uv : TEXCOORD0;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float3 positionWS : TEXCOORD0;
                float3 normalWS : TEXCOORD1;
                float3 viewDirWS : TEXCOORD2;
                float2 baseUV : TEXCOORD3;
                float2 thicknessUV : TEXCOORD4;
                UNITY_VERTEX_INPUT_INSTANCE_ID
                UNITY_VERTEX_OUTPUT_STEREO
            };

            TEXTURE2D(_BaseMap);
            SAMPLER(sampler_BaseMap);
            TEXTURE2D(_ThicknessMap);
            SAMPLER(sampler_ThicknessMap);

            CBUFFER_START(UnityPerMaterial)
                half4 _BaseColor;
                float4 _BaseMap_ST;
                float4 _ThicknessMap_ST;
                half _Smoothness;
                float _IridescenceThickness;
                float _IridescenceIOR;
                float _SubstrateIOR;
                float _LayerCount;
                float _InterlayerThickness;
                float _IridescenceStrength;
                float _ThicknessVariation;
            CBUFFER_END

            // Wyman, Sloan and Shirley (2013), Eq. 3: compact analytic fits to
            // the CIE 1931 2-degree color matching functions. Wavelength is nm.
            float3 CIE1931ColorMatchingFunctions(float wavelengthNm)
            {
                float xScale0 = wavelengthNm < 442.0 ? 0.0624 : 0.0374;
                float xScale1 = wavelengthNm < 599.8 ? 0.0264 : 0.0323;
                float xScale2 = wavelengthNm < 501.1 ? 0.0490 : 0.0382;
                float x = 0.362 * exp(-0.5 * pow((wavelengthNm - 442.0) * xScale0, 2.0))
                        + 1.056 * exp(-0.5 * pow((wavelengthNm - 599.8) * xScale1, 2.0))
                        - 0.065 * exp(-0.5 * pow((wavelengthNm - 501.1) * xScale2, 2.0));

                float yScale0 = wavelengthNm < 568.8 ? 0.0213 : 0.0247;
                float yScale1 = wavelengthNm < 530.9 ? 0.0613 : 0.0322;
                float y = 0.821 * exp(-0.5 * pow((wavelengthNm - 568.8) * yScale0, 2.0))
                        + 0.286 * exp(-0.5 * pow((wavelengthNm - 530.9) * yScale1, 2.0));

                float zScale0 = wavelengthNm < 437.0 ? 0.0845 : 0.0278;
                float zScale1 = wavelengthNm < 459.0 ? 0.0385 : 0.0725;
                float z = 1.217 * exp(-0.5 * pow((wavelengthNm - 437.0) * zScale0, 2.0))
                        + 0.681 * exp(-0.5 * pow((wavelengthNm - 459.0) * zScale1, 2.0));
                return float3(x, y, z);
            }

            float3 XYZToLinearSRGB(float3 xyz)
            {
                return float3(
                    3.2406 * xyz.x - 1.5372 * xyz.y - 0.4986 * xyz.z,
                   -0.9689 * xyz.x + 1.8758 * xyz.y + 0.0415 * xyz.z,
                    0.0557 * xyz.x - 0.2040 * xyz.y + 1.0570 * xyz.z);
            }

            float SafePositiveDivide(float numerator, float denominator)
            {
                return numerator / max(denominator, 1e-5);
            }

            float2 ComplexMultiply(float2 left, float2 right)
            {
                return float2(left.x * right.x - left.y * right.y,
                              left.x * right.y + left.y * right.x);
            }

            float2 ComplexDivide(float2 numerator, float2 denominator)
            {
                float denominatorMagnitudeSquared = max(dot(denominator, denominator), 1e-8);
                return float2(numerator.x * denominator.x + numerator.y * denominator.y,
                              numerator.y * denominator.x - numerator.x * denominator.y)
                       / denominatorMagnitudeSquared;
            }

            float2 ComposeLayerReflection(float interfaceReflection, float2 belowReflection,
                                          float phase)
            {
                float2 roundTripPhase = float2(cos(phase), sin(phase));
                float2 propagatedBelow = ComplexMultiply(belowReflection, roundTripPhase);
                float2 numerator = float2(interfaceReflection, 0.0) + propagatedBelow;
                float2 denominator = float2(1.0, 0.0) + interfaceReflection * propagatedBelow;
                return ComplexDivide(numerator, denominator);
            }

            // Coherent unpolarized reflectance of air -> [platelet A, organic B] repeated
            // -> semi-infinite organic B. Thicknesses and wavelengths are measured in nm.
            float MultilayerReflectance(float wavelengthNm, float plateletThicknessNm,
                                        float organicThicknessNm, float cosTheta0,
                                        float plateletIOR, float organicIOR, int layerCount)
            {
                const float airIOR = 1.0;
                float sinTheta0Squared = saturate(1.0 - cosTheta0 * cosTheta0);
                // The Snell invariant is evaluated from air for every medium.
                float cosThetaA = sqrt(saturate(1.0 - sinTheta0Squared / (plateletIOR * plateletIOR)));
                float cosThetaB = sqrt(saturate(1.0 - sinTheta0Squared / (organicIOR * organicIOR)));

                float rsAirA = SafePositiveDivide(airIOR * cosTheta0 - plateletIOR * cosThetaA,
                                                   airIOR * cosTheta0 + plateletIOR * cosThetaA);
                float rpAirA = SafePositiveDivide(plateletIOR * cosTheta0 - airIOR * cosThetaA,
                                                   plateletIOR * cosTheta0 + airIOR * cosThetaA);
                float rsAB = SafePositiveDivide(plateletIOR * cosThetaA - organicIOR * cosThetaB,
                                                plateletIOR * cosThetaA + organicIOR * cosThetaB);
                float rpAB = SafePositiveDivide(organicIOR * cosThetaA - plateletIOR * cosThetaB,
                                                organicIOR * cosThetaA + plateletIOR * cosThetaB);
                float rsBA = -rsAB;
                float rpBA = -rpAB;

                float phaseA = 4.0 * JAGAE_PI * plateletIOR * plateletThicknessNm * cosThetaA / wavelengthNm;
                float phaseB = 4.0 * JAGAE_PI * organicIOR * organicThicknessNm * cosThetaB / wavelengthNm;
                float2 effectiveS = float2(0.0, 0.0);
                float2 effectiveP = float2(0.0, 0.0);

                // Begin at the semi-infinite B substrate. The fixed upper bound avoids
                // dynamic reverse-loop compiler issues while preserving bottom-up ordering.
                [unroll]
                for (int period = 7; period >= 0; --period)
                {
                    if (period < layerCount)
                    {
                        // Through B, seen from A, then through A, seen from B or air.
                        effectiveS = ComposeLayerReflection(rsAB, effectiveS, phaseB);
                        effectiveP = ComposeLayerReflection(rpAB, effectiveP, phaseB);

                        float rsAboveA = period > 0 ? rsBA : rsAirA;
                        float rpAboveA = period > 0 ? rpBA : rpAirA;
                        effectiveS = ComposeLayerReflection(rsAboveA, effectiveS, phaseA);
                        effectiveP = ComposeLayerReflection(rpAboveA, effectiveP, phaseA);
                    }
                }

                float reflectanceS = dot(effectiveS, effectiveS);
                float reflectanceP = dot(effectiveP, effectiveP);
                return saturate(0.5 * (reflectanceS + reflectanceP));
            }

            // A finite 17-sample visible-spectrum approximation (380 through 700 nm).
            float3 SpectralMultilayerToLinearSRGB(float plateletThicknessNm, float organicThicknessNm,
                                                  float cosTheta0, float plateletIOR, float organicIOR,
                                                  int layerCount)
            {
                float3 responseXYZ = 0.0;
                float3 equalEnergyWhiteXYZ = 0.0;

                [unroll]
                for (int sampleIndex = 0; sampleIndex < JAGAE_SPECTRAL_SAMPLE_COUNT; ++sampleIndex)
                {
                    float wavelengthNm = JAGAE_WAVELENGTH_START_NM + JAGAE_WAVELENGTH_STEP_NM * sampleIndex;
                    float3 cmf = CIE1931ColorMatchingFunctions(wavelengthNm);
                    responseXYZ += MultilayerReflectance(wavelengthNm, plateletThicknessNm,
                                                         organicThicknessNm, cosTheta0, plateletIOR,
                                                         organicIOR, layerCount) * cmf;
                    equalEnergyWhiteXYZ += cmf;
                }

                // Convert both integrations with the same standard matrix. Dividing in RGB
                // makes a wavelength-independent reflectance neutral under equal-energy light.
                float3 responseRGB = XYZToLinearSRGB(responseXYZ);
                float3 equalEnergyWhiteRGB = XYZToLinearSRGB(equalEnergyWhiteXYZ);
                float3 rgb = responseRGB / max(equalEnergyWhiteRGB, float3(1e-4, 1e-4, 1e-4));

                // Remove negative out-of-gamut components, then scale peaks rather than
                // independently clipping channels, preserving the remaining hue ratio.
                rgb = max(rgb, 0.0);
                rgb /= max(1.0, max(rgb.r, max(rgb.g, rgb.b)));
                return saturate(rgb);
            }

            Varyings vert(Attributes input)
            {
                Varyings output;
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_TRANSFER_INSTANCE_ID(input, output);
                UNITY_INITIALIZE_VERTEX_OUTPUT_STEREO(output);

                output.positionHCS = TransformObjectToHClip(input.positionOS.xyz);
                output.positionWS = TransformObjectToWorld(input.positionOS.xyz);
                output.normalWS = TransformObjectToWorldNormal(input.normalOS);
                output.viewDirWS = GetWorldSpaceViewDir(output.positionWS);
                output.baseUV = TRANSFORM_TEX(input.uv, _BaseMap);
                output.thicknessUV = TRANSFORM_TEX(input.uv, _ThicknessMap);
                return output;
            }

            half4 frag(Varyings input) : SV_Target
            {
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_SETUP_STEREO_EYE_INDEX_POST_VERTEX(input);

                float3 normalWS = normalize(input.normalWS);
                float3 viewDirectionWS = SafeNormalize(input.viewDirWS);
                float4 baseSample = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, input.baseUV);
                float thicknessMap = SAMPLE_TEXTURE2D(_ThicknessMap, sampler_ThicknessMap, input.thicknessUV).r;
                float thicknessNm = max(0.0, _IridescenceThickness
                                            + (thicknessMap - 0.5) * _ThicknessVariation);
                float cosTheta0 = saturate(dot(normalWS, viewDirectionWS));
                int layerCount = clamp((int)round(_LayerCount), 1, 8);
                float3 spectralSpecular = SpectralMultilayerToLinearSRGB(
                    thicknessNm, max(0.0, _InterlayerThickness), cosTheta0,
                    max(_IridescenceIOR, 1.0), max(_SubstrateIOR, 1.0), layerCount);

                SurfaceData surfaceData = (SurfaceData)0;
                surfaceData.albedo = baseSample.rgb * _BaseColor.rgb;
                surfaceData.specular = lerp(half3(0.04, 0.04, 0.04), (half3)spectralSpecular,
                                            saturate(_IridescenceStrength));
                surfaceData.metallic = 0.0;
                surfaceData.smoothness = _Smoothness;
                surfaceData.normalTS = half3(0.0, 0.0, 1.0);
                surfaceData.emission = half3(0.0, 0.0, 0.0);
                surfaceData.occlusion = 1.0;
                surfaceData.alpha = 1.0;
                surfaceData.clearCoatMask = 0.0;
                surfaceData.clearCoatSmoothness = 1.0;

                InputData inputData = (InputData)0;
                inputData.positionWS = input.positionWS;
                inputData.positionCS = input.positionHCS;
                inputData.normalWS = normalWS;
                inputData.viewDirectionWS = viewDirectionWS;
                inputData.shadowCoord = TransformWorldToShadowCoord(input.positionWS);
                inputData.fogCoord = ComputeFogFactor(input.positionHCS.z);
                inputData.vertexLighting = VertexLighting(input.positionWS, normalWS);
                inputData.bakedGI = SampleSH(normalWS);
                inputData.normalizedScreenSpaceUV = GetNormalizedScreenSpaceUV(input.positionHCS);
                inputData.shadowMask = half4(1.0, 1.0, 1.0, 1.0);
                inputData.tangentToWorld = half3x3(1.0, 0.0, 0.0,
                                                   0.0, 1.0, 0.0,
                                                   0.0, 0.0, 1.0);

                // The spectral result is used once as the PBR specular input; it is not
                // multiplied over the completed lighting result a second time.
                half4 color = UniversalFragmentPBR(inputData, surfaceData);
                color.rgb = MixFog(color.rgb, inputData.fogCoord);
                return color;
            }
            ENDHLSL
        }

        Pass
        {
            Name "ShadowCaster"
            Tags { "LightMode" = "ShadowCaster" }

            ZWrite On
            ZTest LEqual
            ColorMask 0

            HLSLPROGRAM
            #pragma target 3.5
            #pragma vertex ShadowPassVertex
            #pragma fragment ShadowPassFragment
            #pragma multi_compile_vertex _ _CASTING_PUNCTUAL_LIGHT_SHADOW
            #pragma multi_compile_instancing

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"
            // Shadows.hlsl uses LerpWhiteTo from the core material helpers.
            #include "Packages/com.unity.render-pipelines.core/ShaderLibrary/CommonMaterial.hlsl"
            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Shadows.hlsl"

            float3 _LightDirection;
            float3 _LightPosition;

            struct ShadowAttributes
            {
                float4 positionOS : POSITION;
                float3 normalOS : NORMAL;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            struct ShadowVaryings
            {
                float4 positionCS : SV_POSITION;
                UNITY_VERTEX_INPUT_INSTANCE_ID
            };

            ShadowVaryings ShadowPassVertex(ShadowAttributes input)
            {
                ShadowVaryings output;
                UNITY_SETUP_INSTANCE_ID(input);
                UNITY_TRANSFER_INSTANCE_ID(input, output);

                float3 positionWS = TransformObjectToWorld(input.positionOS.xyz);
                float3 normalWS = TransformObjectToWorldNormal(input.normalOS);
                #if _CASTING_PUNCTUAL_LIGHT_SHADOW
                    float3 lightDirectionWS = normalize(_LightPosition - positionWS);
                #else
                    float3 lightDirectionWS = _LightDirection;
                #endif
                output.positionCS = TransformWorldToHClip(
                    ApplyShadowBias(positionWS, normalWS, lightDirectionWS));
                output.positionCS = ApplyShadowClamping(output.positionCS);
                return output;
            }

            half4 ShadowPassFragment(ShadowVaryings input) : SV_TARGET
            {
                UNITY_SETUP_INSTANCE_ID(input);
                return 0;
            }
            ENDHLSL
        }
    }
}
