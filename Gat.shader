Shader "URP/Unlit/Gat"
{
    Properties
    {
        _BaseColor      ("Base Color", Color) = (0,0,0,1)
        _ThreadColor    ("Thread Color", Color) = (0.15,0.15,0.15,1)

        _Density        ("Thread Density", Float) = 80.0
        _Thickness      ("Thread Thickness", Range(0.1, 0.5)) = 0.01

        _NoiseScale     ("Noise Scale", Float) = 15.0
        _NoiseStrength  ("Noise Strength", Range(0.0, 0.1)) = 0.02

        _BumpStrength   ("Bump Strength", Range(0.0, 2.0)) = 0.5

        _FakeLightDir   ("Fake Light Direction", Vector) = (0.01, 0.0, 0.0, 0.0)

        _EdgeColor      ("Edge Color", Color) = (1,0,0,1)
    }

    SubShader
    {
        Tags
        {
            "RenderType"="Transparent"
            "Queue"="Transparent"
            "RenderPipeline"="UniversalPipeline"
        }
        // PASS 1 : 망사 패턴
        Pass
        {
            Name "ForwardUnlit"
            Tags { "LightMode"="SRPDefaultUnlit" }

            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite On
            Cull Off

            HLSLPROGRAM
            #pragma vertex vert
            #pragma fragment frag
            #pragma multi_compile_fog

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            float4 _BaseColor;
            float4 _ThreadColor;

            float  _Density;
            float  _Thickness;

            float  _NoiseScale;
            float  _NoiseStrength;

            float  _BumpStrength;

            float4 _FakeLightDir;

            float4 _EdgeColor;

            struct Attributes
            {
                float4 positionOS : POSITION;
                float3 normalOS   : NORMAL;
                float2 uv         : TEXCOORD0;
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float2 uv          : TEXCOORD0;
                float  fogFactor   : TEXCOORD1;

                float3 normalWS    : TEXCOORD2;
                float3 viewDirWS   : TEXCOORD3;
            };

            float hash21(float2 p)
            {
                p = frac(p * float2(123.34, 345.45));
                p += dot(p, p + 34.345);
                return frac(p.x * p.y);
            }

            float ThreadStripe(float coord, float density, float thickness, float noiseOffset)
            {
                float x = coord * density + noiseOffset;
                x = frac(x);

                float d = abs(x - 0.5);
                float edge = thickness;

                float stripeVal = 1.0 - smoothstep(thickness, thickness + edge, d);
                return stripeVal;
            }

            Varyings vert(Attributes IN)
            {
                Varyings OUT;

                float3 positionWS = TransformObjectToWorld(IN.positionOS.xyz);
                OUT.positionHCS = TransformWorldToHClip(positionWS);

                OUT.normalWS  = TransformObjectToWorldNormal(IN.normalOS);
                OUT.viewDirWS = GetWorldSpaceViewDir(positionWS);

                OUT.uv = IN.uv;
                OUT.fogFactor = ComputeFogFactor(OUT.positionHCS.z);

                return OUT;
            }

            half4 frag(Varyings IN) : SV_Target
            {
                float2 uv = IN.uv;

                // 엮인 패턴 구현
                float n = hash21(uv * _NoiseScale);
                float noiseOffset = (n - 0.5) * _NoiseStrength;

                float verticalThread   = ThreadStripe(uv.x, _Density, _Thickness,  noiseOffset);
                float horizontalThread = ThreadStripe(uv.y, _Density, _Thickness, -noiseOffset);

                float weave = saturate(max(verticalThread, horizontalThread));

                float3 baseCol   = _BaseColor.rgb;
                float3 threadCol = _ThreadColor.rgb;
                float3 albedo = lerp(baseCol, threadCol, weave);

                // Bump 구현
                float grad = weave;
                float dx = ddx(grad);
                float dy = ddy(grad);

                float3 N = normalize(float3(-dx * _BumpStrength,
                                            -dy * _BumpStrength,
                                             1.0));

                float3 L = normalize(_FakeLightDir.xyz); 
                float diff = saturate(dot(N, L)); 

                float lighting = 0.3 + 0.7 * diff; 

                float3 finalColor = albedo * lighting;

                finalColor = MixFog(finalColor, IN.fogFactor);

                float alpha = weave;
                return half4(finalColor, alpha);
            }

            ENDHLSL
        }
        // PASS 2 : Geometry Shader - Edge Detection
        Pass
        {
            Name "HardEdge_GS"
            Tags { "LightMode"="UniversalForward" }

            Blend SrcAlpha OneMinusSrcAlpha
            ZWrite Off
            ZTest LEqual
            Cull Off

            HLSLPROGRAM
            #pragma target 4.5
            #pragma vertex vert_edge
            #pragma geometry geom_edge
            #pragma fragment frag_edge

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            float4 _EdgeColor;

            struct AttributesEdge
            {
                float4 positionOS : POSITION;
                float3 normalOS   : NORMAL;
            };

            struct VaryingsEdge
            {
                float4 positionHCS : SV_POSITION;
                float3 posWS       : TEXCOORD0;
                float3 normalWS    : TEXCOORD1;
            };

            VaryingsEdge vert_edge(AttributesEdge IN)
            {
                VaryingsEdge OUT;

                float3 posWS = TransformObjectToWorld(IN.positionOS.xyz);
                OUT.positionHCS = TransformWorldToHClip(posWS);
                OUT.posWS       = posWS;
                OUT.normalWS    = TransformObjectToWorldNormal(IN.normalOS);

                return OUT;
            }

            // vertex normal 기반 edge 검출
            [maxvertexcount(6)]
            void geom_edge(triangle VaryingsEdge IN[3], inout LineStream<VaryingsEdge> lineStream)
            {
                float3 n0 = normalize(IN[0].normalWS);
                float3 n1 = normalize(IN[1].normalWS);
                float3 n2 = normalize(IN[2].normalWS);

                // dot = 1 → 동일 normal (smooth)
                float threshold = 0.951057;

                float d01 = dot(n0, n1);
                float d12 = dot(n1, n2);
                float d20 = dot(n2, n0);

                VaryingsEdge A, B;

                // v0 - v1
                if (d01 < threshold)
                {
                    A = IN[0];
                    B = IN[1];
                    A.positionHCS = TransformWorldToHClip(A.posWS);
                    B.positionHCS = TransformWorldToHClip(B.posWS);
                    lineStream.Append(A);
                    lineStream.Append(B);
                }

                // v1 - v2
                if (d12 < threshold)
                {
                    A = IN[1];
                    B = IN[2];
                    A.positionHCS = TransformWorldToHClip(A.posWS);
                    B.positionHCS = TransformWorldToHClip(B.posWS);
                    lineStream.Append(A);
                    lineStream.Append(B);
                }

                // v2 - v0
                if (d20 < threshold)
                {
                    A = IN[2];
                    B = IN[0];
                    A.positionHCS = TransformWorldToHClip(A.posWS);
                    B.positionHCS = TransformWorldToHClip(B.posWS);
                    lineStream.Append(A);
                    lineStream.Append(B);
                }
            }

            float4 frag_edge(VaryingsEdge IN) : SV_Target
            {
                return _EdgeColor;
            }

            ENDHLSL
        }
    }
}


