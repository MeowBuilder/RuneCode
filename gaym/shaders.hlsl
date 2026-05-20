// Material Struct
struct MATERIAL
{
    float4 m_cAmbient;
    float4 m_cDiffuse;
    float4 m_cSpecular; // a = power
    float4 m_cEmissive;
};

// Per-Object Constant Buffer
cbuffer cbGameObject : register(b0)
{
    matrix World;
    uint MaterialIndex;
    uint bIsSkinned;
    uint bHasTexture;
    uint bIsLava;
    uint bIsWater;
    uint bHasEmissiveTexture;
    float g_HitFlash;
    uint bIsRocky;
    uint bIsGrass;   // 절차적 풀(grass) 셰이딩 플래그 — VS sway + PS vertex 그라데이션
    uint bIsPortal;  // 차원문 셰이딩 — 시안/마젠타 듀얼톤 와류 + 블랙홀 + 림 글로우
    uint bIsDecal;   // 지면 데칼 — 텍스처 알파 마스킹, 조명 우회
    uint _gpad2;
    float4 g_StatusColor;     // element outline color (RGB) + unused (A)
    float  g_StatusIntensity; // 0 = no status outline, 1 = full element tint
    float  _spad0; float _spad1; float _spad2;
    MATERIAL gMaterial;
    matrix gBoneTransforms[128];
};

// Torch light struct
#define MAX_TORCH_LIGHTS 8

struct TorchLight
{
    float3 Position; float Range;
    float3 Color;    float Intensity;
};

// Gerstner Wave Parameters
struct WaveParams
{
    float wavelength;
    float amplitude;
    float steepness;
    float speed;
    float2 direction;
    float fadeSpeed;
    float pad;
};

// Per-Pass Constant Buffer
cbuffer cbPass : register(b1)
{
    matrix ViewProj;
    matrix LightViewProj; // Shadow Map용 Light View-Projection
    float4 g_LightColor; // Directional Light Color
    float3 g_LightDirection; float pad0; // Directional Light Direction
    float4 g_PointLightColor; // Point Light Color
    float3 g_PointLightPosition; float pad1; // Point Light Position
    float g_PointLightRange; float pad2; float pad3; float pad4; // Point Light Range and padding
    float4 g_AmbientLight; // Ambient Light Color
    float3 g_CameraPosition; float pad_cam; // Camera World Position for specular

    // SpotLight
    float4 g_SpotLightColor;
    float3 g_SpotLightPosition; float g_SpotLightRange;
    float3 g_SpotLightDirection; float g_SpotLightInnerCone;
    float g_SpotLightOuterCone; float pad5; float pad6; float pad7;

    // Time for animations
    float g_Time; float g_TimePad1; float g_TimePad2; float g_TimePad3;

    // Torch lights array
    TorchLight g_TorchLights[MAX_TORCH_LIGHTS];
    int g_nActiveTorchLights; int g_TorchPad1; int g_TorchPad2; int g_TorchPad3;

    // Gerstner Waves (5 waves for ocean simulation)
    WaveParams g_Waves[5];

    // Stage theme: 0=Fire, 1=Water, 2=Earth, 3=Grass — drives caustics/fog
    // g_ToonEnabled: 0=original Phong, 1=Genshin-style cel shading (F7 toggle)
    int g_StageTheme; int g_ToonEnabled; int _themePad2; int _themePad3;
};

Texture2D gAlbedoMap    : register(t0);
Texture2D gShadowMap    : register(t1);
Texture2D gNormalMap    : register(t2);  // Water normal map 1
Texture2D gHeightMap    : register(t3);  // Water height map 1
Texture2D gEmissiveMap  : register(t4);  // Emissive map
Texture2D gAOMap        : register(t5);  // Ambient Occlusion map (stylized water)
Texture2D gRoughnessMap : register(t6);  // Roughness map (stylized water)
Texture2D gNormalMap2   : register(t7);  // Water normal map 2 (Water_6)
Texture2D gHeightMap2   : register(t8);  // Water height map 2 (Water_6)
Texture2D gFoamOpacity  : register(t9);  // Foam opacity map (foam4)
Texture2D gFoamDiffuse  : register(t10); // Foam diffuse map (foam4)
SamplerState gSampler : register(s0);
SamplerComparisonState gShadowSampler : register(s1);

struct VS_INPUT
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    int4 boneIndices : BONEINDICES;
    float4 boneWeights : BONEWEIGHTS;
};

struct PS_INPUT
{
    float4 position : SV_POSITION;
    float3 worldNormal : NORMAL;
    float3 worldPosition : POSITION; // Added for point light calculation
    float2 uv : TEXCOORD;
    float4 posLightSpace : TEXCOORD1; // Shadow Map용 Light 공간 위치
    float crestFactor : TEXCOORD2; // Wave crest factor for foam (0~1)
    float waterDisp : TEXCOORD3;  // Wave vertical displacement from baseline (depth proxy)
};

// ========================================================================
// Gerstner Wave Functions
// Based on: https://developer.nvidia.com/gpugems/GPUGems/gpugems_ch01.html
// ========================================================================

// Calculate single Gerstner wave displacement
// Returns: float4(offsetX, offsetY, offsetZ, crestFactor)
float4 GerstnerWave(float3 worldPos, WaveParams wave, float time)
{
    float2 d = normalize(wave.direction);
    float f = 2.0 / wave.wavelength;  // frequency
    float phi = wave.speed * f * time;
    float theta = f * dot(d, worldPos.xz) + phi;

    // Fade amplitude over time (breathing effect)
    float fade = cos(wave.fadeSpeed * time) * 0.5 + 0.5;
    float amp = wave.amplitude * fade;

    // Gerstner wave displacement
    float3 offset;
    offset.x = (wave.steepness / wave.wavelength) * d.x * cos(theta);
    offset.z = (wave.steepness / wave.wavelength) * d.y * cos(theta);
    offset.y = amp * sin(theta);

    // Crest factor: sin value normalized to 0~1 (peaks at wave crests)
    float crest = sin(theta) * 0.5 + 0.5;
    crest = crest * saturate(wave.steepness) * fade;

    return float4(offset, crest);
}

// Calculate Gerstner wave normal
float3 GerstnerNormal(float3 worldPos, WaveParams wave, float time)
{
    float2 d = normalize(wave.direction);
    float f = 2.0 / wave.wavelength;
    float phi = wave.speed * f * time;
    float theta = f * dot(d, worldPos.xz) + phi;

    float fade = cos(wave.fadeSpeed * time) * 0.5 + 0.5;
    float amp = wave.amplitude * fade;

    float WA = f * amp;
    float C = cos(theta);
    float S = sin(theta);

    float3 normal;
    normal.x = -d.x * WA * C;
    normal.z = -d.y * WA * C;
    normal.y = 1.0 - (wave.steepness / wave.wavelength) * WA * S;

    return normal;
}

// Apply all 5 Gerstner waves and return combined offset + crest
void ApplyGerstnerWaves(inout float3 worldPos, inout float3 normal, out float crestFactor)
{
    float3 totalOffset = float3(0, 0, 0);
    float3 totalNormal = float3(0, 0, 0);
    float totalCrest = 0.0;
    float totalSteepness = 0.0;

    // Sum all 5 waves
    [unroll]
    for (int i = 0; i < 5; i++)
    {
        float4 wave = GerstnerWave(worldPos, g_Waves[i], g_Time);
        totalOffset += wave.xyz;
        totalCrest += wave.w;
        totalNormal += GerstnerNormal(worldPos + totalOffset, g_Waves[i], g_Time);
        totalSteepness += g_Waves[i].steepness;
    }

    // 위로 상승하는 변위는 소프트 캡으로 점근 제한 (플레이어 발밑 넘어오지 않게)
    // 작은 파도는 거의 그대로 (0.5 → 0.45 수준), 큰 피크만 자연스럽게 수렴
    const float MAX_UP_DISP = 2.5f;   // 베이스라인 기준 최대 상승
    if (totalOffset.y > 0.0f)
    {
        totalOffset.y = MAX_UP_DISP * (1.0f - exp(-totalOffset.y / MAX_UP_DISP));
    }

    // Apply displacement to world position
    worldPos += totalOffset;

    // Normalize combined normal
    normal = normalize(float3(-totalNormal.x, 1.0 - totalNormal.y, -totalNormal.z));

    // Normalize crest factor
    crestFactor = saturate(totalCrest / max(0.01, totalSteepness));
}

// ========================================================================

// ========================================================================
// Water-stage environmental effects
// ========================================================================

// Domain-warped pseudo-Voronoi caustics. 4 layers with sine/cosine domain warp
// per octave to break up axial regularity. No texture sample needed.
// p: world XZ (units = meters), t: seconds. Output: ~[0,1] intensity, sharp ridges.
float WaterCaustics(float2 p, float t)
{
    p *= 0.35;

    float c = 1.0;
    [unroll]
    for (int n = 0; n < 4; ++n)
    {
        float i = float(n);
        float2 q = p + float2(i * 0.13 + t * 0.11, i * 0.08 - t * 0.13);
        // Domain warp breaks the regular grid alignment of frac().
        q.x += (0.55 / (i + 1.0)) * sin(p.y * (i + 3.5) + t * 0.40);
        q.y += (0.55 / (i + 1.0)) * cos(p.x * (i + 2.5) - t * 0.30);
        // Distance to nearest cell center (post-warp = irregular).
        c = min(c, length(0.5 - frac(q)));
    }
    // Sharpen into thin bright ridges.
    return pow(saturate(c * 1.7), 5.0);
}

// Cheap hash + smoothed value noise (used by LavaCracks).
float _hash12(float2 p)
{
    p = frac(p * float2(123.34f, 456.21f));
    p += dot(p, p + 45.32f);
    return frac(p.x * p.y);
}
float _vnoise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0f - 2.0f * f);
    float a = _hash12(i);
    float b = _hash12(i + float2(1, 0));
    float c = _hash12(i + float2(0, 1));
    float d = _hash12(i + float2(1, 1));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// (1) Per-cell soft tile background mask. Heavily distorted circular blob per
//     cell — covers most of the cell but with irregular organic outline that
//     doesn't read as a square grid. Neighboring blobs gently overlap.
float LavaTileMask(float2 worldP)
{
    const float CELL_SIZE = 3.8f;

    float2 q  = worldP / CELL_SIZE;
    float2 ic = floor(q);
    float2 fc = frac(q) - 0.5f;

    float h1 = _hash12(ic);
    float h2 = _hash12(ic + float2(7.3f, 13.1f));

    // Per-cell rotation
    float ang = h1 * 6.2831853f;
    float ca = cos(ang), sa = sin(ang);
    float2 fr = float2(ca * fc.x - sa * fc.y, sa * fc.x + ca * fc.y);

    // Distance-based circular blob, heavily distorted by angular noise so the
    // outline is organic — no rectangular feeling.
    float r = length(fr);
    float theta = atan2(fr.y, fr.x);
    float distort = 0.55f + 0.40f * sin(theta * (3.0f + h2 * 2.0f) + h1 * 6.28f)
                          + 0.20f * sin(theta * (5.0f + h1 * 1.5f) - h2 * 4.7f);
    r *= distort;

    // Soft fade — wider so neighboring blobs overlap slightly.
    float mask = 1.0f - smoothstep(0.25f, 0.55f, r);

    return saturate(mask) * (0.75f + 0.30f * h2);
}

// (2) Procedural crack lines — domain-warped ridge noise FBM. 메안더링 곡선,
//     얇은 라인, 가지 치는 형태. 모든 위치에서 작동.
float LavaCrackLines(float2 worldP, float t)
{
    float2 p = worldP * 0.20f;
    float2 w = float2(
        _vnoise(p * 0.6f + float2(0.0f, t * 0.010f)),
        _vnoise(p * 0.8f + float2(t * 0.012f, 0.0f)));
    p += (w - 0.5f) * 2.2f;

    float n1 = _vnoise(p);
    float r1 = 1.0f - smoothstep(0.0f, 0.060f, abs(n1 * 2.0f - 1.0f));

    float n2 = _vnoise(p * 2.8f + 17.3f);
    float r2 = 1.0f - smoothstep(0.0f, 0.045f, abs(n2 * 2.0f - 1.0f));

    float n3 = _vnoise(p * 6.2f + 31.7f);
    float r3 = 1.0f - smoothstep(0.0f, 0.030f, abs(n3 * 2.0f - 1.0f));

    return max(max(r1, r2 * 0.80f), r3 * 0.55f);
}

// ========================================================================

PS_INPUT VS(VS_INPUT input)
{
    PS_INPUT output;

    float3 posL = input.position;
    float3 normalL = input.normal;

    if (bIsSkinned)
    {
        posL = float3(0.0f, 0.0f, 0.0f);
        normalL = float3(0.0f, 0.0f, 0.0f);
        
        for(int i = 0; i < 4; ++i)
        {
            int idx = input.boneIndices[i];
            float weight = input.boneWeights[i];
            
            if (weight > 0.0f)
            {
                posL += weight * mul(float4(input.position, 1.0f), gBoneTransforms[idx]).xyz;
                normalL += weight * mul(input.normal, (float3x3)gBoneTransforms[idx]);
            }
        }
    }

    // Initialize crest factor (default 0 for non-water)
    float crestFactor = 0.0;
    float waterDisp = 0.0;  // Wave displacement from baseline (for depth effect in PS)

    // Transform to world space first (needed for Gerstner waves)
    float4 worldPos = mul(float4(posL, 1.0f), World);

    // Water vertex displacement: Gerstner waves + Heightmap (Medium 글 방식)
    if (bIsWater)
    {
        float originalY = worldPos.y;  // Capture baseline Y before any displacement

        // === 1. Gerstner Waves (큰 파도, 2개만 사용) ===
        float3 waveNormal = normalL;
        ApplyGerstnerWaves(worldPos.xyz, waveNormal, crestFactor);

        // Heightmap displacement 제거 — Gerstner만으로 버텍스 변위
        // (heightmap은 PS normal map 레이어로 표면 디테일 처리)
        waterDisp = worldPos.y - originalY;

        normalL = waveNormal;
    }

    // === Grass sway: 갈대 끝 큰 폭으로 흔들림 (방향성 바람 + gust) ===
    // bIsGrass=1: world space 위치 + UV.y(0=뿌리, 1=끝)에 큐빅 가중 → 뿌리 고정, 끝 큰 호 그리며 휨
    if (bIsGrass)
    {
        // 베이스 잔물결: 3옥타브 합성 + per-blade 위상 차 → 인접 갈대도 다르게 흔들림
        float swayPhase = g_Time * 2.0f + worldPos.x * 1.10f + worldPos.z * 1.40f;
        float baseSway = sin(swayPhase) * 0.70f
                       + sin(swayPhase * 1.7f + 0.7f) * 0.30f
                       + sin(swayPhase * 3.1f + 1.3f) * 0.15f;

        // 1차 gust: 빠르게 변하는 바람 강도 (0~1)
        float gust1Phase = g_Time * 0.55f + worldPos.x * 0.06f + worldPos.z * 0.04f;
        float gust1 = sin(gust1Phase) * 0.5f + 0.5f;
        // 2차 weather: 매우 느린 modulation — 잔잔한 시간 vs 휘몰아치는 시간을 구분
        float gust2Phase = g_Time * 0.18f + worldPos.x * 0.02f;
        float gust2 = sin(gust2Phase) * 0.5f + 0.5f;
        // 합성 gust: weather 약하면 잔잔, 강하면 격렬 (squared로 sharp peak)
        float gust = gust1 * (0.30f + 0.70f * gust2);
        float gustPeak = gust * gust;                  // 더 sharp한 peak

        // 방향성 wind push — 항상 +X(+살짝 +Z) 방향으로 미는 일정한 바이어스. 양의 값.
        // gustPeak에 따라 push 강도가 변함 → 평소는 살짝, 휘몰아칠 땐 크게 쓸림
        float windPush = 0.25f + gustPeak * 1.10f;     // 평소 0.25, 폭풍 시 1.35

        // 잔물결 진폭은 gust 따라 같이 커짐 (격할 땐 진동도 격해짐)
        float oscAmp = 0.30f + 0.50f * gust;

        float swayAmount = baseSway * oscAmp + windPush;
        float heightF = input.uv.y * input.uv.y;       // 제곱: mid-row와 함께 곡선 휨
        worldPos.x += swayAmount * heightF;
        worldPos.z += swayAmount * 0.4f * heightF;
    }

    // Transform the position from world space to clip space
    output.position = mul(worldPos, ViewProj);

    // Transform the normal from object space to world space
    // mul(n, World3x3) == inverse-transpose for diagonal scale matrices (correct for mirrored objects too)
    output.worldNormal = mul(normalL, (float3x3)World);

    // Pass world position for point light calculation
    output.worldPosition = worldPos.xyz;

    // Pass crest factor for foam calculation in pixel shader
    output.crestFactor = crestFactor;
    output.waterDisp = waterDisp;

    output.uv = input.uv;

    // Calculate position in light space for shadow mapping
    output.posLightSpace = mul(worldPos, LightViewProj);

    return output;
}

// ========================================================================
// Outline Pass (Inverted Hull) — Genshin-style outline.
// Renders extruded back faces in solid color; main pass overdraws the
// interior, leaving only the silhouette ring visible.
//
// When g_ToonEnabled == 0, vertices are pushed outside NDC so the entire
// pass collapses to nothing (no need to skip from C++).
// ========================================================================

struct VS_OUTLINE_OUTPUT
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD;
};

VS_OUTLINE_OUTPUT VS_Outline(VS_INPUT input)
{
    VS_OUTLINE_OUTPUT output;

    // Outline only on skinned characters/enemies (the first approach that
    // visibly worked). World geometry has unpredictable winding so applying
    // inverted hull there gives the "all-black floor" failure mode.
    if (g_ToonEnabled == 0 || !bIsSkinned)
    {
        output.position = float4(2.0f, 2.0f, 2.0f, 1.0f);
        output.uv = float2(0.0f, 0.0f);
        return output;
    }
    output.uv = input.uv;

    float3 posL    = float3(0.0f, 0.0f, 0.0f);
    float3 normalL = float3(0.0f, 0.0f, 0.0f);

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        int   idx = input.boneIndices[i];
        float w   = input.boneWeights[i];

        if (w > 0.0f)
        {
            posL    += w * mul(float4(input.position, 1.0f), gBoneTransforms[idx]).xyz;
            normalL += w * mul(input.normal, (float3x3)gBoneTransforms[idx]);
        }
    }

    float4 worldPos    = mul(float4(posL, 1.0f), World);
    float3 worldNormal = normalize(mul(normalL, (float3x3)World));

    // Canonical inverted hull: push outward in world space along the normal.
    // Scale by clip-w to keep the on-screen thickness roughly constant.
    float4 clipFirst = mul(worldPos, ViewProj);
    float thickness  = 0.0040f * clipFirst.w;
    worldPos.xyz += worldNormal * thickness;

    output.position = mul(worldPos, ViewProj);
    return output;
}

float4 PS_Outline(VS_OUTLINE_OUTPUT input) : SV_TARGET
{
    // Genshin-style tinted outline: take the local base color (material *
    // albedo if textured), darken hard, and pull slightly toward cool-black
    // so it still reads as an "ink" line — but parts now keep their hue
    // (red coat → maroon line, skin → dark warm line, etc.).
    float3 baseColor = gMaterial.m_cDiffuse.rgb;
    if (bHasTexture != 0)
    {
        float3 alb = gAlbedoMap.Sample(gSampler, input.uv).rgb;
        baseColor *= alb;
    }

    // Slight saturation bump before darkening so washed-out albedos still
    // show their hue in the line.
    float lum = dot(baseColor, float3(0.299f, 0.587f, 0.114f));
    baseColor = lerp(float3(lum, lum, lum), baseColor, 1.20f);

    float3 tint = baseColor * 0.50f;
    tint = lerp(tint, float3(0.03f, 0.03f, 0.06f), 0.15f);
    tint = max(tint, 0.0f);

    // Status effect: lerp outline toward element color when intensity > 0
    if (g_StatusIntensity > 0.0f)
    {
        float pulse = 0.7f + 0.3f * sin(g_Time * 4.0f);
        tint = lerp(tint, g_StatusColor.rgb * pulse, g_StatusIntensity);
    }

    return float4(tint, 1.0f);
}

// Shadow Pass Vertex Shader (depth only)
float4 VS_Shadow(VS_INPUT input) : SV_POSITION
{
    float3 posL = input.position;

    if (bIsSkinned)
    {
        posL = float3(0.0f, 0.0f, 0.0f);

        for (int i = 0; i < 4; ++i)
        {
            int idx = input.boneIndices[i];
            float weight = input.boneWeights[i];

            if (weight > 0.0f)
            {
                posL += weight * mul(float4(input.position, 1.0f), gBoneTransforms[idx]).xyz;
            }
        }
    }

    float4 worldPos = mul(float4(posL, 1.0f), World);
    return mul(worldPos, LightViewProj);
}

// PCF 3x3 Shadow Calculation
float CalculateShadow(float4 posLightSpace)
{
    // Perspective divide
    float3 projCoords = posLightSpace.xyz / posLightSpace.w;

    // Transform to [0, 1] range for texture sampling
    projCoords.x = projCoords.x * 0.5f + 0.5f;
    projCoords.y = projCoords.y * -0.5f + 0.5f;  // Y is flipped in DirectX

    // Check if outside shadow map bounds
    if (projCoords.x < 0.0f || projCoords.x > 1.0f ||
        projCoords.y < 0.0f || projCoords.y > 1.0f ||
        projCoords.z < 0.0f || projCoords.z > 1.0f)
    {
        return 1.0f;  // No shadow outside bounds
    }

    float currentDepth = projCoords.z;
    float shadow = 0.0f;

    // Depth bias: 경사 기반으로 acne 방지 (음수 스케일/큰 맵에서 더 큰 값 필요)
    float bias = 0.002f;

    // PCF 3x3 sampling
    float texelSize = 1.0f / 2048.0f;  // Shadow map size
    [unroll]
    for (int x = -1; x <= 1; ++x)
    {
        [unroll]
        for (int y = -1; y <= 1; ++y)
        {
            float2 offset = float2(x, y) * texelSize;
            shadow += gShadowMap.SampleCmpLevelZero(gShadowSampler, projCoords.xy + offset, currentDepth - bias);
        }
    }
    shadow /= 9.0f;

    return shadow;
}

float4 PS(PS_INPUT input) : SV_TARGET
{
    // ========================================================================
    // Portal (차원문) — 라이팅 무시. 시안/마젠타 듀얼톤 와류 + 블랙홀 + 림 글로우
    // ========================================================================
    //   - RingMesh UV: u = 각도(0~1), v = 반경(0=외곽, 1=중심)
    //   - gMaterial.m_cEmissive = 코어색(마젠타), m_cDiffuse = 림색(시안)
    if (bIsPortal)
    {
        // RingMesh UV: u = 각도(0~1), v = 반경(0=외곽, 1=중심)
        // fbm 노이즈 + r 기반 회전 속도 차등(안쪽이 빠르게 빨려들어가는 깊이감)
        float r = saturate(1.0f - input.uv.y);
        float theta = input.uv.x * 6.2831853f;

        // 깊이감의 핵심: 안쪽으로 갈수록 빠르게 회전 (whirlpool / accretion disk)
        float radialBoost = 1.0f / max(r + 0.18f, 0.18f);
        float twistedTheta = theta + g_Time * 0.30f * radialBoost;

        float t = g_Time * 0.12f;
        float2 polar = float2(cos(twistedTheta), sin(twistedTheta)) * (0.6f + r * 1.4f);

        // 3 옥타브 fbm — 부드러운 유기적 패턴
        float n1 = _vnoise(polar * 2.5f + float2( t,        -t * 0.6f));
        float n2 = _vnoise(polar * 5.0f + float2(-t * 0.7f,  t * 0.9f));
        float n3 = _vnoise(polar * 9.5f + float2( t * 1.3f,  t * 0.4f));
        float swirl = saturate(n1 * 0.55f + n2 * 0.30f + n3 * 0.15f);

        float3 coreCol = gMaterial.m_cEmissive.rgb;
        float3 rimCol  = gMaterial.m_cDiffuse.rgb;
        float3 portalCol = lerp(rimCol * 0.25f, coreCol, swirl);

        // 블랙홀 + 깊이감 음영 — 안쪽으로 갈수록 어두워지며 빨려드는 깊이 표현
        // smoothstep 두 단계: 0~0.18 진한 어둠, 0.18~0.45 점차 밝아짐 → 우물 같은 깊이
        float blackhole = smoothstep(0.0f, 0.18f, r) * smoothstep(0.0f, 0.45f, r);
        portalCol *= blackhole;

        // 외곽 림 — 부드러운 그라데이션 + 부드러운 inner highlight (마법진 위에 떠있는 듯한 광막)
        float rimGlow = smoothstep(0.62f, 1.0f, r);
        rimGlow *= rimGlow;
        portalCol += rimCol * rimGlow * 1.8f;

        // 부드러운 빛점 — fbm swirl 의 핫스팟
        float hotspot = smoothstep(0.65f, 0.95f, swirl) * (1.0f - r * 0.5f);
        portalCol += coreCol * hotspot * 1.2f;

        // ── 주기적 임팩트 펄스 — 4초 주기, ring 이 중심→외곽으로 sweep ────────────
        //   살짝의 임팩트: 너무 강하지 않게 페이드 아웃. impactR 가 r 위치에 도달했을 때 작은 발광 띠.
        const float IMPACT_PERIOD = 4.0f;
        float impactT = frac(g_Time / IMPACT_PERIOD);     // [0, 1)
        float impactR = impactT * 0.95f;                  // ring 가 0→0.95 로 sweep
        float impactWidth = 0.07f;
        float impactBand = exp(-pow((r - impactR) / impactWidth, 2.0f));
        float impactFade = 1.0f - impactT;                // 시간 지날수록 약해짐
        portalCol += coreCol * impactBand * impactFade * 1.4f;

        // 호흡 펄스
        float pulse = 0.90f + 0.10f * sin(g_Time * 1.7f);
        portalCol *= pulse;

        return float4(portalCol, gMaterial.m_cDiffuse.a);
    }

    // Normalize the world normal
    float3 normal = normalize(input.worldNormal);
    float3 vToCamera = normalize(g_CameraPosition - input.worldPosition); // Vector from fragment to camera

    // UV animation for lava or water
    float2 uv = input.uv;
    float3 waterNormal = normal;  // Default to geometry normal
    float waterFresnel = 0.0f;
    float waterAO = 1.0f;           // Default AO (no darkening)
    float waterSpecularPower = 0.0f; // 0 means use material default

    if (bIsLava)
    {
        // Very slow flowing lava effect
        float2 flow1 = float2(g_Time * 0.003f, g_Time * 0.002f);
        float2 flow2 = float2(-g_Time * 0.002f, g_Time * 0.0025f);
        uv = input.uv + flow1 + flow2 * 0.5f;
    }
    else if (bIsWater)
    {
        // === GPU Gems 스타일 물 셰이더 (Gerstner waves 기반) ===

        // Water_6 normal map 4레이어 (모두 gNormalMap2 = t7, 올바른 normal map)
        // 서로 다른 스케일과 방향으로 흘러 자연스러운 물결 표면 생성
        float2 normalUV1 = input.uv * 1.8f + float2( g_Time * 0.014f,  g_Time * 0.010f);
        float2 normalUV2 = input.uv * 3.5f + float2(-g_Time * 0.009f,  g_Time * 0.018f);
        float2 normalUV3 = input.uv * 6.0f + float2( g_Time * 0.020f, -g_Time * 0.012f);
        float2 normalUV4 = input.uv * 10.f + float2(-g_Time * 0.006f, -g_Time * 0.022f);

        float3 n1 = gNormalMap2.Sample(gSampler, normalUV1).rgb * 2.0f - 1.0f;
        float3 n2 = gNormalMap2.Sample(gSampler, normalUV2).rgb * 2.0f - 1.0f;
        float3 n3 = gNormalMap2.Sample(gSampler, normalUV3).rgb * 2.0f - 1.0f;
        float3 n4 = gNormalMap2.Sample(gSampler, normalUV4).rgb * 2.0f - 1.0f;

        // 큰 파도 우선, 디테일은 낮은 가중치
        float3 combinedNormal = normalize(n1 * 0.40f + n2 * 0.30f + n3 * 0.20f + n4 * 0.10f);

        float3 tangent   = float3(1, 0, 0);
        float3 bitangent = float3(0, 0, 1);
        waterNormal = normalize(
            normal +
            tangent    * combinedNormal.x * 0.22f +
            bitangent  * combinedNormal.z * 0.22f
        );

        // 간단한 Fresnel
        float NdotV = saturate(dot(waterNormal, vToCamera));
        waterFresnel = pow(1.0f - NdotV, 5.0f);

        uv = input.uv;
    }

    float4 albedoColor;
    if (bIsGrass)
    {
        // 절차적 갈대: 두 팔레트(신선 녹 ↔ 마른 보리황) 사이를 블레이드별로 보간
        float t = input.uv.y;
        // 블레이드별 hash — 두 종류의 주파수 합성으로 더 풍부한 변동
        float h1 = sin(input.worldPosition.x * 1.73f + input.worldPosition.z * 2.41f) * 0.5f + 0.5f;
        float h2 = sin(input.worldPosition.x * 0.61f - input.worldPosition.z * 0.83f) * 0.5f + 0.5f;
        float vSeed = saturate(h1 * 0.7f + h2 * 0.3f);
        // 신선 녹 팔레트
        float3 freshBase = float3(0.20f, 0.34f, 0.10f);
        float3 freshTip  = float3(0.65f, 0.86f, 0.36f);
        // 마른 보리황 팔레트 — 더 따뜻한 톤
        float3 dryBase   = float3(0.34f, 0.30f, 0.12f);
        float3 dryTip    = float3(0.92f, 0.78f, 0.32f);
        // 위치별 보간 (vSeed 0=신선, 1=마른) — 갈대밭에 자연스러운 색 변동
        float3 baseG = lerp(freshBase, dryBase, vSeed);
        float3 tipG  = lerp(freshTip,  dryTip,  vSeed);
        // 미세한 노이즈 — 한 블레이드 내에서도 색이 단조롭지 않게
        float n = sin(input.worldPosition.x * 5.3f + input.worldPosition.y * 4.1f) * 0.05f;
        float3 grassRGB = lerp(baseG, tipG, t) + n;
        albedoColor = float4(saturate(grassRGB), 1.0f);
    }
    else if (bHasTexture)
    {
        albedoColor = gAlbedoMap.Sample(gSampler, uv);

    }
    else
    {
        albedoColor = float4(1.0f, 1.0f, 1.0f, 1.0f); // White if no texture
    }
    // Combine with material diffuse (optional: multiply)
    float4 baseColor = albedoColor * gMaterial.m_cDiffuse;

    // 지면 데칼: 텍스처 알파로 마스킹, 조명 계산 우회
    if (bIsDecal)
        return float4(baseColor.rgb, albedoColor.a * gMaterial.m_cDiffuse.a);

    // Apply water AO to base color
    if (bIsWater)
        baseColor.rgb *= waterAO;

    // Use water normal for lighting if water, otherwise use geometry normal
    float3 shadingNormal = bIsWater ? waterNormal : normal;

    // Specular power: use roughness-based power for water, material default otherwise
    float specPower = (bIsWater && waterSpecularPower > 0.0f) ? waterSpecularPower : gMaterial.m_cSpecular.a;

    // --- Shadow Calculation ---
    float shadowFactor = CalculateShadow(input.posLightSpace);

    // --- Directional Light Calculation ---
    // F7 toggles between original Phong and Genshin-style cel shading.
    float NdotL = saturate(dot(shadingNormal, -g_LightDirection));
    float3 vHalfDirectional = normalize(vToCamera + (-g_LightDirection));
    float specRaw = pow(max(dot(vHalfDirectional, shadingNormal), 0.0f), specPower);

    // Surfaces that should bypass cel even when toon is on:
    //   - Deep lava plane (Y < -2): emissive, cel boundary makes it look split
    //   - Lava-tagged ground tiles when their stage env effects are running
    //     (warmth/cracks already stylize the look — adding cel cool tint clashes)
    bool isDeepLavaPlane = bIsLava && (input.worldPosition.y < -2.0f);

    float4 directionalTotal;
    if (g_ToonEnabled != 0 && !isDeepLavaPlane)
    {
        // ── Genshin cel path ──
        // Combine NdotL with shadow before quantizing — single hard boundary
        // handles self-shadow and cast-shadow uniformly.
        float lightTerm = NdotL * shadowFactor;
        // Tight cel band — both characters and world get a hard boundary.
        // Wider bands wash out so much that toon mode looks identical to Phong
        // on flat surfaces. Sharp boundary is the whole point.
        float celBandLo = bIsSkinned ? 0.46f : 0.44f;
        float celBandHi = bIsSkinned ? 0.54f : 0.52f;
        float celDiffuse = smoothstep(celBandLo, celBandHi, lightTerm);

        // Cool shadow tint — characters get full lilac, world gets a milder
        // version so it doesn't fight stage atmospheric tints, but still cool
        // enough to read as "cel shadow" not just "darker albedo".
        float3 shadowTint = bIsSkinned ? float3(0.62f, 0.68f, 0.95f)
                                       : float3(0.62f, 0.66f, 0.88f);
        // Skinned characters get a brighter shadow side so their albedo (often
        // dark cloth/skin tones) doesn't crush to near-black after the global
        // contrast pop applied later.
        float shadowMul   = bIsSkinned ? 0.60f : 0.45f;
        float3 litRGB    = baseColor.rgb * g_LightColor.rgb * 0.85f;
        float3 shadowRGB = baseColor.rgb * shadowTint * shadowMul;
        float3 dirDiffuseRGB = lerp(shadowRGB, litRGB, celDiffuse);

        // Hard specular: sharp cutoff masked by celDiffuse so it never blooms
        // in shadow.
        float specMask = smoothstep(0.40f, 0.55f, specRaw) * celDiffuse;
        float3 dirSpecRGB = specMask * g_LightColor.rgb * gMaterial.m_cSpecular.rgb;

        directionalTotal = float4(dirDiffuseRGB + dirSpecRGB, 0.0f);
    }
    else
    {
        // ── Original Phong path (also used for deep lava plane) ──
        float4 dDiffuse  = NdotL * g_LightColor * baseColor;
        float4 dSpecular = specRaw * g_LightColor * gMaterial.m_cSpecular;
        directionalTotal = (dDiffuse + dSpecular) * shadowFactor;
    }

    // --- Point Light Calculation ---
    float3 lightVec = g_PointLightPosition - input.worldPosition;
    float dist = length(lightVec);
    float3 pointLightDir = normalize(lightVec);

    float attenuation = saturate(1.0f - dist / g_PointLightRange);
    
    float pointDiffuseFactor = saturate(dot(shadingNormal, pointLightDir));
    float3 vHalfPoint = normalize(vToCamera + pointLightDir); // Half vector for point specular
    float pointSpecularFactor = pow(max(dot(vHalfPoint, shadingNormal), 0.0f), specPower);

    float4 pointDiffuse = pointDiffuseFactor * g_PointLightColor * baseColor * attenuation;
    float4 pointSpecular = pointSpecularFactor * g_PointLightColor * gMaterial.m_cSpecular * attenuation;
    float4 pointTotal = pointDiffuse + pointSpecular;
    
    // --- Ambient Light Calculation ---
    // In cel mode reduced — full ambient washes out the hard light/shadow boundary.
    float ambientScale = (g_ToonEnabled != 0) ? 0.30f : 1.00f;
    float4 ambient = g_AmbientLight * gMaterial.m_cAmbient * albedoColor * ambientScale;
    
    // Final color is the sum of all light components + emissive
    // Emission Map이 있으면 _EmissionColor * EmissionMap, 없으면 _EmissionColor 그대로
    float4 emissiveContrib;
    if (bHasEmissiveTexture)
        emissiveContrib = float4(gMaterial.m_cEmissive.rgb * gEmissiveMap.Sample(gSampler, uv).rgb, 0.0f);
    else
        emissiveContrib = float4(gMaterial.m_cEmissive.rgb, 0.0f);
    float4 finalColor = directionalTotal + pointTotal + ambient + emissiveContrib;

    // --- Spot Light Calculation ---
    float3 spotLightVec = g_SpotLightPosition - input.worldPosition;
    float spotDist = length(spotLightVec);
    float3 spotLightDir = normalize(spotLightVec);

    // Distance attenuation
    float spotAttenuation = saturate(1.0f - spotDist / g_SpotLightRange);

    // Cone attenuation
    float cosTheta = dot(-spotLightDir, normalize(g_SpotLightDirection));
    float coneAttenuation = saturate((cosTheta - g_SpotLightOuterCone) / (g_SpotLightInnerCone - g_SpotLightOuterCone));

    spotAttenuation *= coneAttenuation;

    if (spotAttenuation > 0.0f)
    {
        float spotDiffuseFactor = saturate(dot(shadingNormal, spotLightDir));
        float3 vHalfSpot = normalize(vToCamera + spotLightDir);
        float spotSpecularFactor = pow(max(dot(vHalfSpot, shadingNormal), 0.0f), specPower);

        float4 spotDiffuse = spotDiffuseFactor * g_SpotLightColor * baseColor * spotAttenuation;
        float4 spotSpecular = spotSpecularFactor * g_SpotLightColor * gMaterial.m_cSpecular * spotAttenuation;
        float4 spotTotal = spotDiffuse + spotSpecular;
        finalColor += spotTotal;
    }

    // --- Torch Lights Calculation (multiple point lights) ---
    [loop]
    for (int t = 0; t < g_nActiveTorchLights && t < MAX_TORCH_LIGHTS; ++t)
    {
        float3 torchVec = g_TorchLights[t].Position - input.worldPosition;
        float torchDist = length(torchVec);

        if (torchDist < g_TorchLights[t].Range)
        {
            float3 torchDir = torchVec / torchDist;

            // Smooth quadratic attenuation for torch light
            float normalizedDist = torchDist / g_TorchLights[t].Range;
            float torchAtten = saturate(1.0f - normalizedDist * normalizedDist) * g_TorchLights[t].Intensity;

            // Diffuse
            float torchDiffuseFactor = saturate(dot(shadingNormal, torchDir));

            // Specular
            float3 vHalfTorch = normalize(vToCamera + torchDir);
            float torchSpecularFactor = pow(max(dot(vHalfTorch, shadingNormal), 0.0f), specPower);

            float4 torchColor = float4(g_TorchLights[t].Color, 1.0f);
            float4 torchDiffuse = torchDiffuseFactor * torchColor * baseColor * torchAtten;
            float4 torchSpecular = torchSpecularFactor * torchColor * gMaterial.m_cSpecular * torchAtten;

            finalColor += torchDiffuse + torchSpecular;
        }
    }

    // --- Genshin-style Rim Light (characters only) ---
    // Restricted to skinned meshes — environment rim catches odd grazing
    // angles on horizontal floors (lava plane, water bottom) and looks weird.
    // Genshin itself only rim-lights characters; world geometry stays flat.
    if (g_ToonEnabled != 0 && bIsSkinned)
    {
        float rim = 1.0f - saturate(dot(shadingNormal, vToCamera));
        // Bias toward lit side so rim reads as light wrap, not pure fresnel.
        float rimWeight = NdotL * 0.65f + 0.35f;
        float rimMask = smoothstep(0.55f, 0.95f, rim) * rimWeight;
        finalColor.rgb += float3(1.00f, 0.95f, 0.85f) * rimMask * 0.55f;
    }

    if (bIsWater)
    {
        // ================================================================
        // === Phase 3: Depth-based Water Shader ===========================
        // ================================================================

        // --- Depth Factor: wave displacement → depth proxy ---
        // waveDisp range: roughly -14 (trough) ~ +14 (crest)
        // depthFactor: 0.0 = deep trough, 1.0 = high crest
        float waveDisp = input.waterDisp;
        float depthFactor = saturate((waveDisp + 14.0f) / 28.0f);

        // --- Depth Color Gradient (3-stop) ---
        // Trough: very dark deep navy
        float3 troughColor = float3(0.003f, 0.018f, 0.07f);
        // Mid wave: medium ocean blue
        float3 midColor    = float3(0.015f, 0.07f,  0.18f);
        // Crest base: 더 밝게 (파도 마루 강조)
        float3 crestColor  = float3(0.08f,  0.28f,  0.55f);

        // Smooth 3-stop blend using two lerps
        float3 waterColor = lerp(troughColor, midColor,   saturate(depthFactor * 2.0f));
        waterColor        = lerp(waterColor,  crestColor, saturate((depthFactor - 0.5f) * 2.0f));

        // --- Subsurface Scattering (파도 마루에서 청록 빛 더 강하게) ---
        float3 sssColor   = float3(0.05f, 0.55f, 0.65f);  // 더 선명한 청록
        float sssStrength = pow(saturate(input.crestFactor * 1.5f), 2.0f) * 0.55f;  // 더 강하게
        waterColor = lerp(waterColor, sssColor, sssStrength);

        // --- Directional light shading (subtle) ---
        float diff = saturate(dot(shadingNormal, -g_LightDirection));
        waterColor = waterColor * (1.0f + diff * 0.35f);

        // --- Specular highlight (sun glint on waves) ---
        float3 halfVec = normalize(vToCamera + (-g_LightDirection));
        float spec = pow(max(dot(shadingNormal, halfVec), 0.0f), 200.0f);  // 300→200 (범위 넓게)
        float3 specColor = float3(1.0f, 0.97f, 0.90f) * spec * 0.80f;     // 0.55→0.80

        // --- Wave Crest Foam (threshold 낮춰서 더 잘 터지게) ---
        float crestFoam = pow(input.crestFactor, 1.2f);           // 1.5→1.2 (더 낮은 crest에서도 활성)
        crestFoam = smoothstep(0.30f, 0.70f, crestFoam);          // 0.55/0.90 → 0.30/0.70
        float foamStrength = saturate(crestFoam * 1.0f);          // 0.75→1.0
        float3 foamColor   = float3(0.90f, 0.94f, 0.98f);

        // --- Fresnel (edge reflectivity) ---
        float3 fresnelColor = float3(0.06f, 0.18f, 0.30f);

        // --- Depth-based transparency ---
        // Crests (thin water) = more transparent; troughs (deep) = more opaque
        float waterAlpha = lerp(0.96f, 0.82f, saturate(waveDisp / 12.0f));

        // === Final Composite ===
        finalColor.rgb = waterColor;
        finalColor.rgb += specColor * shadowFactor;
        finalColor.rgb  = lerp(finalColor.rgb, foamColor, foamStrength);
        finalColor.rgb  = lerp(finalColor.rgb, fresnelColor, waterFresnel * 0.18f);

        // 일반 지오메트리와 동일한 atmospheric tint/fog를 물 표면에도 적용 → 시각적 연속성.
        if (g_StageTheme == 1)
        {
            float camDist_w = distance(g_CameraPosition, input.worldPosition);
            float fog_w = saturate((camDist_w - 16.0f) / 70.0f);
            float3 fogColor_w = float3(0.30f, 0.55f, 0.72f);
            finalColor.rgb = lerp(finalColor.rgb, fogColor_w, fog_w * 0.65f);
            float3 tinted_w = finalColor.rgb * float3(0.95f, 1.05f, 1.18f);
            finalColor.rgb = lerp(finalColor.rgb, tinted_w, 0.85f);
            finalColor.rgb *= 1.08f;
        }

        return float4(finalColor.rgb, waterAlpha);
    }

    // ── Stage-themed environment: Water (caustics + atmospheric tint) ──
    // bIsWater 분기는 위에서 return 했으므로 여기 도달하는 픽셀은 일반 지오메트리.
    if (g_StageTheme == 1)
    {
        // Caustics — 위를 향한 면(바닥/지형)에만, 햇빛이 닿는 영역에만.
        // peak 색을 1.0 위로 밀어서 bloom 패스가 살짝 글린트로 잡아내게.
        // 캐릭터(skinned)에는 적용 X — 머리·어깨 위에 패턴 투영 방지.
        float upFacing = saturate(normal.y);
        if (upFacing > 0.05f && !bIsSkinned)
        {
            float caust = WaterCaustics(input.worldPosition.xz, g_Time);
            // 살짝 흔들리는 강도 (큰 파도 위 햇빛 흔들림 모사)
            float shimmer = 0.85f + 0.30f * sin(g_Time * 0.9f + input.worldPosition.x * 0.07f);
            float3 causticColor = float3(0.65f, 1.05f, 1.45f);  // peak가 1.0 초과 → bloom 통과
            finalColor.rgb += causticColor * caust * upFacing * shadowFactor * 0.85f * shimmer;
        }

        // 거리 기반 청록 안개 — 시작은 빠르게, 두께는 가볍게.
        float camDist = distance(g_CameraPosition, input.worldPosition);
        float fog = saturate((camDist - 16.0f) / 70.0f);
        float3 fogColor = float3(0.30f, 0.55f, 0.72f);  // 더 밝은 청록 (어둡게 가라앉지 않게)
        finalColor.rgb = lerp(finalColor.rgb, fogColor, fog * 0.65f);

        // 청량 톤 시프트 + 전체 밝기 살짝 lift — "햇빛 잘 드는 얕은 물" 느낌.
        float3 tinted = finalColor.rgb * float3(0.95f, 1.05f, 1.18f);
        finalColor.rgb = lerp(finalColor.rgb, tinted, 0.85f);
        finalColor.rgb *= 1.08f;  // overall lift
    }

    // ── Stage-themed environment: Earth (cave dimming + dusty fog) ──
    // 균열 무늬 제거 — 바위/타일 텍스처가 이미 충분한 디테일을 가짐.
    if (g_StageTheme == 2 && !bIsSkinned)
    {
        float upFacing = saturate(normal.y);
        if (upFacing > 0.20f)
        {
            float slopeMul = smoothstep(0.20f, 0.65f, upFacing);
            // 동굴 음영만 — 위쪽 표면을 살짝 어둡게 깔아 차가운 느낌
            finalColor.rgb *= lerp(1.0f, 0.88f, slopeMul);
        }

        // 거리 기반 흙먼지 안개 — 갈색 톤, 옅게 깔림
        float camDistE = distance(g_CameraPosition, input.worldPosition);
        float fogE = saturate((camDistE - 18.0f) / 80.0f);
        float3 fogColorE = float3(0.45f, 0.40f, 0.35f);
        finalColor.rgb = lerp(finalColor.rgb, fogColorE, fogE * 0.55f);
    }

    // ── Stage-themed environment: Fire (lava-crack glow on upward surfaces) ──
    // 용암 평면(bIsLava)은 자체 흐름 셰이더가 있으니 제외. 거의 수평인 면에만.
    // 캐릭터(skinned)·진짜 거대 lava plane(Y < -2)만 제외.
    // bIsLava 단독 게이트 X — MapLoader가 mesh name에 "lava" 있으면 무조건 SetLava(true)하므로
    // 일반 floor 타일들도 다 bIsLava=true 됨. Y로 진짜 lava plane(Y=-3.5)만 가려냄.
    bool isLavaSubmergedPlane = bIsLava && (input.worldPosition.y < -2.0f);
    if (g_StageTheme == 0 && !isLavaSubmergedPlane && !bIsSkinned)
    {
        float upFacing = saturate(normal.y);
        if (upFacing > 0.25f)  // 기울어진 타일·낮은 경사도 포함
        {
            float slopeMul = smoothstep(0.25f, 0.65f, upFacing);

            // (a) 베이스 워밍 — 약한 따뜻한 톤 (모든 floor 픽셀에 깔림).
            finalColor.rgb += float3(0.055f, 0.022f, 0.004f) * slopeMul;

            // (b) 셀별 부드러운 타일 배경 — 유기적 블롭, 옅은 주황 톤.
            float tileMask = LavaTileMask(input.worldPosition.xz);
            finalColor.rgb += float3(0.15f, 0.060f, 0.012f) * tileMask * slopeMul;

            // (c) 균열 라인 — 모든 위치에서 그어짐, 타일 위에 살짝 더 진함.
            float cracks = LavaCrackLines(input.worldPosition.xz, g_Time);
            float3 crackColor = float3(0.95f, 0.40f, 0.08f);
            float crackBoost = 0.55f + 0.45f * tileMask;
            finalColor.rgb += crackColor * cracks * crackBoost * slopeMul * 0.40f;
        }
    }

    // --- Toon final-color grading (saturation + contrast pop) ---
    // Stage envrironment effects (caustics, lava cracks, fog) overwhelm the
    // cel boundary on flat lit surfaces. A global saturation+contrast bump on
    // toon mode makes the difference visible everywhere, not just on shadow
    // boundaries.
    if (g_ToonEnabled != 0)
    {
        float lum = dot(finalColor.rgb, float3(0.299f, 0.587f, 0.114f));
        // Saturation 1.35× — cartoon-y vivid colors
        finalColor.rgb = lerp(float3(lum, lum, lum), finalColor.rgb, 1.35f);
        // Contrast 1.10× around 0.42 pivot — pivot lifted so dark areas
        // (cel-shaded character body) don't crush to black.
        // max() (not saturate) preserves HDR > 1 so bloom still picks up
        // emissive/caustic peaks.
        finalColor.rgb = max((finalColor.rgb - 0.42f) * 1.10f + 0.42f, 0.0f);
    }

    // Hit Flash: rim-based white outline flash + additive bloom pop
    //   lerp(→white)은 흰색에서 클램프되므로 림에 추가 additive로 "초과 밝기" 부여
    //   → 피격/대쉬 플래시가 훨씬 뚜렷하게 보임
    if (g_HitFlash > 0.001f)
    {
        float3 viewDir = normalize(g_CameraPosition - input.worldPosition);
        float rim = 1.0 - saturate(dot(normalize(input.worldNormal), viewDir));
        float outline = saturate(pow(rim, 2.5) * 3.0);
        float flashMix = g_HitFlash * (outline * 0.7 + 0.5); // 내부 기본 0.3 → 0.5
        finalColor.rgb = lerp(finalColor.rgb, float3(1, 1, 1), saturate(flashMix));
        // Additive 림 부스트 — 흰색 위로 푹 튀어오르게 (HDR pop 느낌)
        finalColor.rgb += outline * g_HitFlash * 1.2f;
    }

    return float4(finalColor.rgb, gMaterial.m_cDiffuse.a);
}