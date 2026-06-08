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
    // g_StormStrength: Earth 모래폭풍 강도 0~1 — CPU envelope (attack-release)
    // g_GustStrength : Grass 돌풍 강도 0~1 — CPU envelope (attack-release)
    int g_StageTheme; int g_ToonEnabled; float g_StormStrength; float g_GustStrength;
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

// Procedural sand-ripple pattern. 노이즈 기반 도메인 워프 + ridge noise.
// sin 워프는 평행 wave 곡선이 그대로 휘어 "물결" 패턴이 됨 → ridge noise로
// 끊기고 갈라지는 dune crest 형태를 만든다. 바람 방향 압축으로 약한 줄무늬
// 성향만 남김. Output ~[0,1] ridge intensity for shading.
float SandRipple(float2 worldP, float t)
{
    // 노이즈 도메인 워프 — 곡선이 각자 다른 방향으로 휘어 organic.
    float2 p = worldP * 0.18f;
    float2 warp = float2(
        _vnoise(p * 0.6f + 1.7f),
        _vnoise(p * 0.6f + 7.3f)) * 2.0f - 1.0f;
    p += warp * 1.4f;

    // 바람 방향(살짝 기운 +X). perp(직교) 압축으로 줄무늬 성향만 약하게.
    float2 windDir = normalize(float2(1.0f, 0.25f));
    float2 perpDir = float2(-windDir.y, windDir.x);
    float along = dot(p, windDir);
    float perp  = dot(p, perpDir);

    // 1차: 큰 dune ridge — perp 강압축으로 줄무늬 성향, 그러나 ridge noise는
    //      마루가 끊기고 갈라져 평행 wave가 아닌 crest 형태.
    float n1 = _vnoise(float2(along * 0.45f, perp * 1.4f));
    float ridge1 = 1.0f - abs(n1 * 2.0f - 1.0f);

    // 2차: 중간 ripple
    float n2 = _vnoise(float2(along * 1.1f + 13.0f, perp * 3.2f));
    float ridge2 = 1.0f - abs(n2 * 2.0f - 1.0f);

    // 3차: 자잘한 grain — 모든 방향 균등
    float n3 = _vnoise(p * 2.6f + 27.0f);

    float ripple = ridge1 * 0.55f + ridge2 * 0.30f + n3 * 0.15f;
    return pow(saturate(ripple), 1.6f);
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

    // === Grass sway: 끝 큰 폭으로 흔들림 (방향성 바람 + gust) ===
    // bIsGrass=1: world space 위치 + UV.y(0=뿌리, 1=끝)에 큐빅 가중 → 뿌리 고정, 끝 큰 호 그리며 휨
    // 주의: GrassClumpMesh 는 per-blade ID 없음 → worldPos 기반 spatial 함수만 사용 가능.
    //   intra-blade 변동 = 도형 찢어짐. 따라서 swayPhase 공간 주파수는 보수적으로,
    //   "군무 방지" 는 클럼프 스케일 (~5u) 에서 desync 되는 저주파 gust 로 처리.
    if (bIsGrass)
    {
        // 베이스 잔물결: 3옥타브 합성 + 공간 위상 (intra-blade 안 깨지는 수준).
        //   x/z 계수 1.6/1.9 — 원본 1.10/1.40 보다 살짝 높아 인접 블레이드 desync 강화.
        //   너무 높이면(>3) 블레이드 폭 안에서 sin 한 cycle 다 돌아 도형 망가짐.
        float swayPhase = g_Time * 2.3f + worldPos.x * 1.60f + worldPos.z * 1.90f;
        float baseSway = sin(swayPhase) * 0.90f
                       + sin(swayPhase * 1.7f + 0.7f) * 0.40f
                       + sin(swayPhase * 3.1f + 1.3f) * 0.20f;

        // 1차 gust: 클럼프 스케일 (~5u) 에서 desync. 인접 클럼프 다른 강도로 부풀기/잠잠.
        //   공간 주파수 0.55/0.45 — 0.6~0.7u 블레이드 폭에서는 거의 동일값 (안 깨짐).
        float gust1Phase = g_Time * 0.65f + worldPos.x * 0.55f + worldPos.z * 0.45f;
        float gust1 = sin(gust1Phase) * 0.5f + 0.5f;
        // 2차 weather: 느린 modulation.
        float gust2Phase = g_Time * 0.22f + worldPos.x * 0.18f + worldPos.z * 0.12f;
        float gust2 = sin(gust2Phase) * 0.5f + 0.5f;
        float gust = gust1 * (0.40f + 0.70f * gust2);
        float gustPeak = gust * gust;

        // 방향성 wind push — peak 진폭 완화 (1.95 → 1.20). "확 꺾이는" 인상 해소.
        //   돌풍(g_GustStrength) 시 windPush 강화 — 풀 전체가 한 방향으로 깊게 누움.
        float gustEvent = saturate(g_GustStrength);
        float windPush  = 0.45f + gustPeak * 1.20f + gustEvent * 1.60f;

        // 잔물결 진폭 — 돌풍 시 진동 폭 증가.
        float oscAmp = (0.45f + 0.75f * gust) * lerp(1.0f, 1.45f, gustEvent);

        float swayAmount = baseSway * oscAmp + windPush;
        // 곡선 smoothstep — uv.y^2 가속 곡선보다 중앙부 부드럽게 위로.
        float t = input.uv.y;
        float heightF = t * t * (3.0f - 2.0f * t);
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
    //   기본 0.0025 — 일반 적/플레이어 두께 유지.
    //   대형 보스(World scale > 5) 만 0.55× 적용 — DarkLord 외곽선 얇게.
    float worldScaleX = length(float3(World._11, World._12, World._13));
    float thicknessMul = (worldScaleX > 5.0f) ? 0.55f : 1.0f;
    float4 clipFirst = mul(worldPos, ViewProj);
    float thickness  = 0.0025f * clipFirst.w * thicknessMul;
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

    // 0.85f + floor: 어두운 텍스처에서 outline 이 진검정으로 떨어지는 인위적 느낌 제거.
    //   - 곱셈 계수 ↑ → 베이스 색 보존
    //   - 쿨톤 lerp 약화 (0.12→0.05) → 자체 색조 살아남
    //   - floor (0.22) → 베이스가 검정이어도 outline 은 회색에 멈춤
    float3 tint = baseColor * 0.85f;
    tint = lerp(tint, float3(0.22f, 0.22f, 0.26f), 0.05f);
    tint = max(tint, float3(0.22f, 0.22f, 0.24f));

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
        // ═══════════════════════════════════════════════════════════════════════
        // 포탈 셰이더 — alpha 직접 계산 + 핫스팟/임팩트 절제 + 중앙 hole + 외곽 fade
        //   메인 PSO 가 BlendEnable=FALSE 라 알파 곱셈으로 검정 처리해 "비어보임" 시뮬레이션
        // ═══════════════════════════════════════════════════════════════════════
        float r = saturate(1.0f - input.uv.y);
        float theta = input.uv.x * 6.2831853f;

        // 비대칭 왜곡 — disc 가 살짝 타원으로 흔들리게
        float warp = sin(theta * 3.0f + g_Time * 0.6f) * 0.05f
                   + sin(theta * 5.0f - g_Time * 0.4f) * 0.03f;
        float warpedR = saturate(r + warp);

        // ── body (vortex) 텍스처 — 폴라 매핑 (포탈 본체 결) + invert ──────────
        float radialBoost = 1.0f / max(warpedR + 0.18f, 0.18f);
        float twistedTheta = theta + g_Time * 0.22f * radialBoost;

        float t = g_Time * 0.10f;
        float2 polar = float2(cos(twistedTheta), sin(twistedTheta)) * (0.55f + warpedR * 1.25f);

        // disc 평면 직접 매핑 + 회전 (텍스처 결 그대로 회전)
        float angA = g_Time * 0.25f;
        float cA = cos(angA), sA = sin(angA);
        float2 discXY = float2(cos(theta), sin(theta)) * warpedR;
        float2 rotA = float2(cA * discXY.x - sA * discXY.y, sA * discXY.x + cA * discXY.y);
        float body = 1.0f - gAlbedoMap.Sample(gSampler, rotA * 0.5f + 0.5f).r;

        // 추가 디테일 — 큰 스케일로 tile 해서 외곽에도 결
        float angB = -g_Time * 0.18f;
        float cB = cos(angB), sB = sin(angB);
        float2 rotB = float2(cB * discXY.x - sB * discXY.y, sB * discXY.x + cB * discXY.y) * 1.8f;
        float detail = 1.0f - gAlbedoMap.Sample(gSampler, rotB * 0.5f + 0.5f + float2(0.13f, -0.21f)).r;

        // procedural noise 보조 (텍스처 결 흔들기)
        float n1 = _vnoise(polar * 2.2f + float2( t,        -t * 0.5f));
        float n2 = _vnoise(polar * 4.6f + float2(-t * 0.6f,  t * 0.8f));
        float procSwirl = n1 * 0.6f + n2 * 0.4f;

        // 패턴 합성 — body 더 dominant, procedural 비중 ↓ + 콘트라스트 강화로 뭉개짐 제거
        float swirl = saturate(body * 0.85f + detail * 0.20f + procSwirl * 0.12f);
        swirl = pow(swirl, 1.25f);  // 콘트라스트 ↑ — 진흙처럼 뭉개지지 않게

        // ── 색상 합성 (절제된 강도) ────────────────────────────────────────────
        float3 coreCol = gMaterial.m_cEmissive.rgb;
        float3 rimCol  = gMaterial.m_cDiffuse.rgb;

        // 중앙 hole + 외곽 페이드 — hole 더 좁게 → 내부가 먹물처럼 안 퍼짐
        float innerHole   = 1.0f - smoothstep(0.015f, 0.095f, warpedR);
        float singularity = 1.0f - smoothstep(0.0f,   0.045f, warpedR);  // 더 깊은 흡입점
        float outerFade   = 1.0f - smoothstep(0.93f,  1.00f,  warpedR);

        // 외곽 림 글로우 — 얇게 + procedural rimBreak 로 불규칙 (완벽한 원 깨기)
        float rimNoise = _vnoise(float2(theta * 2.0f, g_Time * 0.25f) + warpedR * 8.0f);
        float rimBreak = lerp(0.65f, 1.25f, rimNoise);
        float rimGlow = smoothstep(0.78f, 0.98f, warpedR);  // 0.72 → 0.78 (얇아짐)
        rimGlow *= rimGlow;
        rimGlow *= rimBreak;  // 불규칙 찢어짐

        // body 색 — 핫코어 → rim 색 lerp (어두운 코어 + 밝은 림)
        float3 col = lerp(coreCol * 0.25f, rimCol, swirl);
        col *= (1.0f - innerHole * 0.95f);  // 중앙 hole 검정 처리
        col = lerp(col, float3(0.0f, 0.0f, 0.0f), singularity);  // 정중앙 심연

        // 림 글로우 + 핫스팟 + 임팩트 — 모두 절제된 강도
        col += rimCol  * rimGlow * 0.70f;                         // ↓ 2.4 → 0.7
        float hotspot = smoothstep(0.62f, 0.95f, swirl) * (1.0f - warpedR * 0.4f);
        col += coreCol * hotspot * 0.45f;                         // ↓ 1.6 → 0.45

        const float IMPACT_PERIOD = 4.0f;
        float impactT = frac(g_Time / IMPACT_PERIOD);
        float impactR = impactT * 0.95f;
        float impactBand = exp(-pow((warpedR - impactR) / 0.07f, 2.0f));
        float impactFade = 1.0f - impactT;
        col += coreCol * impactBand * impactFade * 0.55f;         // ↓ 1.4 → 0.55

        // 호흡 펄스
        float pulse = 0.92f + 0.08f * sin(g_Time * 1.7f);
        col *= pulse;

        // ── 알파 계산 (메인 PSO 가 BlendEnable=FALSE 이므로 색에 곱해서 검정 시뮬레이션) ─
        float bodyAlpha = saturate(swirl * 0.65f + rimGlow * 0.45f);
        float alpha = bodyAlpha * outerFade;
        alpha *= (1.0f - innerHole * 0.85f);  // 중앙은 알파↓ → 색 검정 → 구멍 인상
        alpha = saturate(alpha + rimGlow * 0.4f);  // 림은 알파 약간 부스트해서 살짝 보임 보장

        // 메인 PSO 알파 블렌딩 없음 → 색에 알파 곱해 비어보이는 부분을 검정으로
        col *= alpha;

        return float4(col, 1.0f);
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
        // 절차적 풀: 순수 초록 팔레트 (R/B 낮춰 노란/라임끼 제거) + 뿌리 그늘 + tip 라이트림.
        float t = input.uv.y;
        float3 freshBase = float3(0.06f, 0.30f, 0.08f);  // 짙은 순초록 (뿌리)
        float3 freshTip  = float3(0.22f, 0.85f, 0.25f);  // 밝은 초록 (R/B 동률, 라임끼 X)
        // 블레이드별 미세 hue 변동 — ±5% (초록 채널만)
        float vSeed = sin(input.worldPosition.x * 1.73f + input.worldPosition.z * 2.41f) * 0.05f;
        float3 grassRGB = lerp(freshBase, freshTip, t);
        grassRGB.g += vSeed;

        // (a) 뿌리 그늘 — t<0.18 영역을 0.65× 더 어둡게 (자기 차폐 AO 느낌)
        float rootShadow = smoothstep(0.18f, 0.0f, t);
        grassRGB *= lerp(1.0f, 0.65f, rootShadow);

        // (b) Tip 라이트림 — 초록 림 (이전 노란빛 0.08, 0.10, 0.04 → 순초록 0.04, 0.12, 0.05).
        float tipRim = smoothstep(0.85f, 1.0f, t);
        grassRGB += float3(0.04f, 0.12f, 0.05f) * tipRim;

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

    // 채도 분리 폴라리티: 캐릭터는 vivid한 주인공, 환경은 약하게 desat된 무대.
    //   - 0.80× 곱: lightTerm=1 강제로 풀 lit 상태라 1.0 그대로면 '자체 발광' 느낌.
    //     원본 0.62 보다는 보존, 1.0 보다는 다운 — 중간점.
    //   - sat/contrast 분리는 toon 그레이딩에서 처리.
    if (bIsSkinned != 0)
        baseColor.rgb *= 0.80f;

    // 지면 데칼 / 공격 인디케이터: 조명 우회.
    //   bHasTexture: 기존 텍스처 데칼 (변화 X — emissive 안 더함, 알파는 텍스처 그대로).
    //   !bHasTexture: 인디케이터 — emissive 가산해서 발광색 노출 + V축 가장자리 soft fade.
    //                  ring/disc 메쉬는 V=0 외곽/V=1 내곽(또는 중심) 컨벤션.
    //                  box 메쉬는 V=0,1 양 long-edge → 부드러운 띠 형태.
    if (bIsDecal)
    {
        float3 decalRGB = baseColor.rgb;
        float  decalA   = albedoColor.a * gMaterial.m_cDiffuse.a;
        if (bHasTexture == 0)
        {
            // 검기 crescent (anchored 정적 호) — emissive.w >= 1.5 sentinel.
            //   diffuse.r = revealProgress (0→1 fadeIn), diffuse.g = dissolveProgress (0→1 fadeOut).
            //   조직적 결과를 만드는 레이어: UV displacement(geometric 깨기) + multi-octave noise + wisp tendrils.
            if (gMaterial.m_cEmissive.w >= 1.5f)
            {
                float u = input.uv.x;
                float v = input.uv.y;
                float revealProgress   = gMaterial.m_cDiffuse.r;
                float dissolveProgress = gMaterial.m_cDiffuse.g;
                float burstAmt         = saturate(gMaterial.m_cDiffuse.b);   // Hit-burst flash (0→1, fast decay)
                float t = g_Time;
                // 원소 id 추출 — ambient.w 에 정수 0~4 저장 (0=def,1=fire,2=water,3=earth,4=wind).
                int   elemId = (int)round(gMaterial.m_cAmbient.w);

                // 1) UV displacement — 호 모양 자체가 sin 으로 약간 흔들림. 완벽한 기하 형태 깨기.
                float uDisp = (sin(v * 8.0f + t * 2.0f) + sin(v * 19.0f - t * 4.0f) * 0.8f) * 0.04f;
                float vDisp = (sin(u * 14.0f + t * 3.5f) + sin(u * 7.0f - t * 5.0f) * 0.8f) * 0.03f;
                float uW = u + uDisp;
                float vW = saturate(v + vDisp);

                // 2) UV.u 좌우 그라데이션 (displaced UV 사용).
                float edgeT = abs(uW - 0.5f) * 2.0f;
                float3 colorMix = lerp(gMaterial.m_cEmissive.rgb, gMaterial.m_cAmbient.rgb, edgeT);
                // Burst 시 흰빛 코어 — colorMix 가 핵심부터 white 로 lerp (강한 컬러 보존 위해 0.55 까지만).
                colorMix = lerp(colorMix, float3(1.6f, 1.55f, 1.45f), burstAmt * 0.55f);

                // 3) Multi-octave panning noise — 4 sin 합성으로 sin 단조로움 해소.
                float oct1 = sin((v * 22.0f + t * 9.0f) * 3.14159f) * 0.50f + 0.50f;
                float oct2 = sin((v * 6.0f  - t * 4.0f + u * 2.0f) * 3.14159f) * 0.50f + 0.50f;
                float oct3 = sin((v * 35.0f + t * 13.0f - u * 4.0f) * 3.14159f) * 0.50f + 0.50f;
                float oct4 = sin((v * 11.0f + t * 6.0f + u * 8.0f) * 3.14159f) * 0.50f + 0.50f;
                float noiseMix = saturate(oct1 * 0.45f + oct2 * 0.30f + oct3 * 0.15f + oct4 * 0.20f);

                // 4) Reveal/dissolve threshold noise — 픽셀 별 임계값.
                float thr = sin((v * 18.0f + u * 5.0f) * 3.14159f) * 0.25f
                          + sin((v * 7.0f  - u * 9.0f) * 3.14159f) * 0.20f
                          + sin((v * 31.0f + u * 13.0f) * 3.14159f) * 0.12f
                          + 0.55f;
                thr = saturate(thr);
                float revealMask = smoothstep(thr - 0.08f, thr + 0.08f, revealProgress);
                float dissolveMask = 1.0f - smoothstep(thr - 0.08f, thr + 0.08f, dissolveProgress);
                float organicMask = revealMask * dissolveMask;
                // V축 directional sweep — 검이 지나간 방향으로 호가 그어짐. 단순 alpha pop X.
                //   revealProgress 0→1 동안 vW 0→1 픽셀이 차례로 visible. 0.12 폭 soft edge.
                float sweepMask = 1.0f - smoothstep(revealProgress, revealProgress + 0.12f, vW);
                organicMask *= sweepMask;
                // Burst 시 noise mask + sweep mask 무력화 — 임팩트 모먼트엔 호 전체가 fully visible.
                organicMask = lerp(organicMask, 1.0f, burstAmt);

                // 5) V 분포 — 대칭 bell + wobble (asymmetric brightness).
                float vProfile = sin(saturate(vW) * 3.14159f);
                vProfile *= 0.78f + sin(v * 4.0f + t * 2.5f) * 0.22f;

                // 6) Edge fade (displaced V) — 양 끝 ragged 하게 fade.
                float edgeFade = smoothstep(0.0f, 0.10f, vW) * smoothstep(1.0f, 0.88f, vW);

                // 7) Wisp tendrils — sin wave bend 한 high-freq 좁은 광선 (에너지 가닥 느낌).
                //    sin(u * 32 + wobble) → U 축 빠른 진동, wobble = V 따라 흐름.
                float wispWobble = sin(v * 6.0f + t * 2.0f) * 2.5f;
                float wisps = sin(u * 32.0f + wispWobble) * 0.5f + 0.5f;
                wisps = pow(saturate(wisps), 5.0f);                       // pow 로 좁은 선만 남김
                wisps *= sin(v * 9.0f + t * 5.0f) * 0.4f + 0.6f;          // V 따라 wisps 강도 변동

                // 8) Core streak — 중앙 검날. Burst 시 더 좁고 강하게 — 임팩트 모먼트의 "검날 광선".
                float coreSharp = lerp(5.0f, 8.5f, burstAmt);
                float coreStreak = exp(-pow((uW - 0.5f) * coreSharp, 2.0f));

                // 9) HDR 합성 — 본체/core/wisps. Burst boost: +2.5× intensity 가산.
                const float kHDR = 7.0f;
                float burstBoost = 1.0f + burstAmt * 2.5f;
                float intensity = (0.50f + 0.55f * vProfile) * (0.65f + 0.45f * noiseMix);
                decalRGB += colorMix * intensity * kHDR * organicMask * burstBoost;
                decalRGB += colorMix * coreStreak * kHDR * 0.65f * organicMask * burstBoost;
                decalRGB += colorMix * wisps * kHDR * 0.55f * organicMask;
                // Burst 의 별도 흰빛 코어 글로우 — coreStreak 위에 흰빛 1.4 ×HDR 가산. 한 frame 폭발감.
                decalRGB += float3(1.0f, 0.95f, 0.88f) * coreStreak * kHDR * 1.4f * burstAmt;

                // ── 원소별 stylization (Undead Lord 레퍼런스 톤) ──
                // Fire — hot yellow core overlay. 칼날 중심부에 노란 발광 추가.
                if (elemId == 1) {
                    float3 hotYellow = float3(1.4f, 1.05f, 0.30f);
                    decalRGB += hotYellow * coreStreak * kHDR * 0.85f * organicMask;
                }
                // Water — bright cyan-white highlight. 코어에 흰빛 cyan 광택.
                else if (elemId == 2) {
                    float3 cyanWhite = float3(0.70f, 1.20f, 1.50f);
                    decalRGB += cyanWhite * coreStreak * kHDR * 0.55f * organicMask;
                }
                // Earth — chunky stepped V profile. 매끄러운 호 → 부서진 돌결.
                else if (elemId == 3) {
                    float vChunk = floor(v * 16.0f) / 16.0f + 0.03f;
                    float chunkProfile = sin(saturate(vChunk) * 3.14159f);
                    float3 amberGold = float3(1.35f, 0.85f, 0.30f);
                    decalRGB += amberGold * chunkProfile * coreStreak * kHDR * 0.50f * organicMask;
                }
                // Wind — multi-strand 3 parallel + 투명도 ↑. 얇은 공기 칼날 3가닥.
                else if (elemId == 4) {
                    float strandSharp = 14.0f;
                    float strandA = exp(-pow((uW - 0.28f) * strandSharp, 2.0f));
                    float strandB = exp(-pow((uW - 0.50f) * strandSharp, 2.0f));
                    float strandC = exp(-pow((uW - 0.72f) * strandSharp, 2.0f));
                    float strands = strandA + strandB + strandC;
                    decalRGB += colorMix * strands * kHDR * 0.55f * organicMask;
                    // 본체는 좀 더 투명 — ethereal 느낌.
                    decalA *= 0.70f;
                }

                decalA   *= edgeFade * organicMask;
            }
            else
            {
                // 일반 indicator 메쉬 (FloorBox/Circle/Ring 등) — 자연스러운 shimmer + 톤다운.
                //   기존: emissive 가 raw 그대로 — 보스 시그니처(2.0+) blow-out, "스티커" 느낌.
                //   신규: 시간 기반 sin shimmer 로 살아있는 표면 + colorMix 강도 0.65 로 다운.
                float u = input.uv.x;
                float v = input.uv.y;
                float edgeT = abs(u - 0.5f) * 2.0f;
                float3 colorMix = lerp(gMaterial.m_cEmissive.rgb, gMaterial.m_cAmbient.rgb, edgeT);

                // 자연스러움 — 여러 주파수 sin 합성으로 표면 미세 변동 (눈에 띄지 않지만 살아있는 느낌).
                float t = g_Time;
                float shimmer1 = sin(t * 3.8f + v * 9.0f + u * 4.0f) * 0.10f + 0.90f;
                float shimmer2 = sin(t * 6.2f - u * 6.0f + v * 3.0f) * 0.07f + 0.93f;
                float natFactor = shimmer1 * shimmer2;

                // 강도 다운 — 형광 blow-out 회피. 보스 emissive 2.0+ 도 1.3 수준으로 자연화.
                decalRGB += colorMix * natFactor * 0.65f;

                // Edge fade + 알파 약간 다운 (0.85) — "스티커" 부피감 줄이고 투명도 ↑.
                float edgeFade = smoothstep(0.0f, 0.14f, v) * smoothstep(1.0f, 0.86f, v);
                decalA *= edgeFade * (0.78f + natFactor * 0.15f);
            }
        }
        else
        {
            // 텍스처드 데칼 경로 — crescent sentinel (>=1.5) 면 검기 텍스처 path, 아니면 center-out reveal.
            if (gMaterial.m_cEmissive.w >= 1.5f)
            {
                // ★ 검기 — 원소별 단일 호 텍스처 (Kenney slash_02/03/04 + Free Slash noise).
                //   ComboAttackBehavior::PickSlashTextureForElement 에서 선택. full-UV 로 호 silhouette 샘플.
                float u = input.uv.x;
                float v = input.uv.y;
                float revealProgress   = gMaterial.m_cDiffuse.r;
                float dissolveProgress = gMaterial.m_cDiffuse.g;
                float burstAmt         = saturate(gMaterial.m_cDiffuse.b);
                int   elemId = (int)round(gMaterial.m_cAmbient.w);
                float t = g_Time;

                // 1) 텍스처 샘플 — full UV. 살짝 UV 흔들기로 정형성 깨기 (특히 정적 crescent 의 인공적인 호 윤곽).
                float texUDisp = sin(v * 11.0f + t * 3.0f) * 0.015f;
                float texVDisp = sin(u * 8.0f  - t * 4.0f) * 0.012f;
                float2 sampleUV = saturate(input.uv + float2(texUDisp, texVDisp));
                float4 slashTex = gAlbedoMap.Sample(gSampler, sampleUV);
                // 텍스처는 흰 브러시 / 검은 배경 (Kenney) 또는 파란 노이즈 호 (free_slash_noise).
                // luminance × alpha 곱이 두 인코딩 방식 모두 안전.
                float slashLum  = max(max(slashTex.r, slashTex.g), slashTex.b);
                float slashMask = slashLum * slashTex.a;

                // 2) Procedural arc Bell — U 축 좌우 fade 만 담당 (silhouette 은 슬래시 텍스처가 담당).
                float coreStreak = exp(-pow((u - 0.5f) * 5.0f, 2.0f));
                float edgeT = abs(u - 0.5f) * 2.0f;
                float arcWindow = (1.0f - edgeT * edgeT);   // 0~1, 가운데 크고 양 끝 0 — soft window.
                arcWindow = saturate(arcWindow);

                // 3) Color gradient (core ↔ edge).
                float3 colorMix = lerp(gMaterial.m_cEmissive.rgb, gMaterial.m_cAmbient.rgb, edgeT);
                colorMix = lerp(colorMix, float3(1.6f, 1.55f, 1.45f), burstAmt * 0.55f);

                // 4) Reveal/dissolve + sweep masks (procedural — 텍스처와 독립).
                float thr = sin((v * 18.0f + u * 5.0f) * 3.14159f) * 0.25f
                          + sin((v * 7.0f  - u * 9.0f) * 3.14159f) * 0.20f
                          + 0.55f;
                thr = saturate(thr);
                float revealMask   = smoothstep(thr - 0.08f, thr + 0.08f, revealProgress);
                float dissolveMask = 1.0f - smoothstep(thr - 0.08f, thr + 0.08f, dissolveProgress);
                float organicMask  = revealMask * dissolveMask;
                float sweepMask    = 1.0f - smoothstep(revealProgress, revealProgress + 0.12f, v);
                organicMask *= sweepMask;
                organicMask = lerp(organicMask, 1.0f, burstAmt);

                // 5) Edge fade (V 축 양 끝 부드럽게).
                float edgeFade = smoothstep(0.0f, 0.05f, v) * smoothstep(1.0f, 0.95f, v);

                // 6) HDR 합성 — slashMask 가 메인 silhouette, arcWindow 가 부드러운 가장자리.
                const float kHDR = 7.0f;
                float burstBoost = 1.0f + burstAmt * 2.5f;
                float intensity = slashMask * arcWindow;

                decalRGB = float3(0.0f, 0.0f, 0.0f);
                decalRGB += colorMix * intensity * kHDR * organicMask * burstBoost;
                decalRGB += colorMix * slashMask * coreStreak * kHDR * 0.55f * organicMask * burstBoost;
                decalRGB += float3(1.0f, 0.95f, 0.88f) * slashMask * kHDR * 1.4f * burstAmt;

                // 7) 원소별 overlay.
                if (elemId == 1) {        // 불 — 노란 hot core
                    decalRGB += float3(1.4f, 1.05f, 0.30f) * slashMask * coreStreak * kHDR * 0.85f * organicMask;
                }
                else if (elemId == 2) {   // 물 — cyan-white 광택
                    decalRGB += float3(0.70f, 1.20f, 1.50f) * slashMask * coreStreak * kHDR * 0.55f * organicMask;
                }
                else if (elemId == 3) {   // 흙 — gold
                    decalRGB += float3(1.35f, 0.85f, 0.30f) * slashMask * arcWindow * kHDR * 0.40f * organicMask;
                }
                else if (elemId == 4) {   // 바람 — 투명도 ↑
                    decalA *= 0.70f;
                }

                // 8) 최종 alpha — slashMask 가 brush silhouette, arcWindow 로 끝단 soft fade.
                decalA = slashMask * arcWindow * edgeFade * organicMask;
            }
            else
            {
                // 중앙부터 바깥으로 펼쳐지는 reveal 효과 (기존 데칼)
                float normDist = length(input.uv - float2(0.5f, 0.5f)) * 2.0f;
                float revealMask = saturate((g_HitFlash - normDist) / 0.15f);
                decalA *= revealMask;
            }
        }
        return float4(decalRGB, decalA);
    }

    // Apply water AO to base color
    if (bIsWater)
        baseColor.rgb *= waterAO;

    // Use water normal for lighting if water, otherwise use geometry normal
    float3 shadingNormal = bIsWater ? waterNormal : normal;

    // Specular power: use roughness-based power for water, material default otherwise
    float specPower = (bIsWater && waterSpecularPower > 0.0f) ? waterSpecularPower : gMaterial.m_cSpecular.a;

    // --- Shadow Calculation ---
    float shadowFactor = CalculateShadow(input.posLightSpace);

    // 캐릭터(bIsSkinned)는 shadow map 영향 받지 않음 → self-shadow 제거.
    //   자기 부위/다른 캐릭터의 그림자가 캐릭터 표면에 떨어지지 않음.
    //   NdotL 셰이딩은 유지되어 빛 방향 음영은 자연스럽게 남음.
    //   환경(바닥/벽)은 그대로 캐릭터/지형 그림자 받음.
    if (bIsSkinned != 0)
        shadowFactor = 1.0f;

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

        // 캐릭터(bIsSkinned)는 NdotL 음영도 무시 → 항상 풀 라이트.
        //   "캐릭터 자체에는 그림자가 안 진다" 요건. shadow map + NdotL 모두 제거되어
        //   캐릭터 표면 전역이 lit 영역으로 분류됨. specular highlight 는 그대로 작동.
        if (bIsSkinned != 0)
            lightTerm = 1.0f;
        // Tight cel band — both characters and world get a hard boundary.
        // Wider bands wash out so much that toon mode looks identical to Phong
        // on flat surfaces. Sharp boundary is the whole point.
        // 캐릭터(bIsSkinned)는 cel boundary 를 NdotL=0.22 부근으로 내려 lit 영역을 넓힘
        //   → 환경광 약한 라바 위에서 캐릭터 측면/하단이 검정으로 빠지지 않게.
        //   여전히 hard boundary 라 셀 룩은 유지 (그라데이션 X).
        float celBandLo = bIsSkinned ? 0.18f : 0.44f;
        float celBandHi = bIsSkinned ? 0.28f : 0.52f;
        float celDiffuse = smoothstep(celBandLo, celBandHi, lightTerm);

        // Cool shadow tint — characters get full lilac, world gets a milder
        // version so it doesn't fight stage atmospheric tints, but still cool
        // enough to read as "cel shadow" not just "darker albedo".
        float3 shadowTint = bIsSkinned ? float3(0.62f, 0.68f, 0.95f)
                                       : float3(0.62f, 0.66f, 0.88f);
        // Skinned characters get a brighter shadow side so their albedo (often
        // dark cloth/skin tones) doesn't crush to near-black after the global
        // contrast pop applied later. 0.60→0.92: 라바 위처럼 환경광 약한 곳에서
        // 캐릭터의 그림자 면(NdotL<celBandLo) 도 거의 lit 수준 밝기로 — 검정 잠식 방지.
        float shadowMul   = bIsSkinned ? 0.92f : 0.45f;
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
        // 캐릭터는 NdotL 음영 제거 → 풀 라이트. shadowFactor 도 위에서 1로 강제됨.
        float effNdotL = (bIsSkinned != 0) ? 1.0f : NdotL;
        float4 dDiffuse  = effNdotL * g_LightColor * baseColor;
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
    // [v2] 0.30 → 0.55: DarkLord 같은 어두운 텍스처가 거의 검정으로 묻히는 문제 — ambient ↑.
    //   원본 텍스처 hue 가 살아나도록.
    float ambientScale = (g_ToonEnabled != 0) ? 0.55f : 1.00f;
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
        float3 troughColor = float3(0.005f, 0.025f, 0.09f);
        // Mid wave: 평균 픽셀 색 — 너무 어두우면 표면이 단조롭게 보임. 살짝 밝게.
        float3 midColor    = float3(0.022f, 0.10f,  0.24f);
        // Crest base: 파도 마루 강조
        float3 crestColor  = float3(0.10f,  0.32f,  0.60f);

        // Smooth 3-stop blend using two lerps
        float3 waterColor = lerp(troughColor, midColor,   saturate(depthFactor * 2.0f));
        waterColor        = lerp(waterColor,  crestColor, saturate((depthFactor - 0.5f) * 2.0f));

        // --- Subsurface Scattering (파도 마루 청록) ---
        float3 sssColor   = float3(0.08f, 0.40f, 0.52f);
        float sssStrength = pow(saturate(input.crestFactor * 1.5f), 2.0f) * 0.42f;
        waterColor = lerp(waterColor, sssColor, sssStrength);

        // --- Directional light shading (subtle) ---
        float diff = saturate(dot(shadingNormal, -g_LightDirection));
        waterColor = waterColor * (1.0f + diff * 0.32f);

        // --- Specular highlight (sun glint on waves) ---
        float3 halfVec = normalize(vToCamera + (-g_LightDirection));
        float spec = pow(max(dot(shadingNormal, halfVec), 0.0f), 220.0f);
        float3 specColor = float3(1.0f, 0.97f, 0.90f) * spec * 0.72f;

        // --- Wave Crest Foam ---
        float crestFoam = pow(input.crestFactor, 1.4f);
        crestFoam = smoothstep(0.38f, 0.78f, crestFoam);
        float foamStrength = saturate(crestFoam * 0.85f);
        float3 foamColor   = float3(0.90f, 0.94f, 0.98f);

        // --- Fresnel (edge reflectivity) ---
        float3 fresnelColor = float3(0.06f, 0.18f, 0.30f);

        // --- Depth-based transparency ---
        // Crests (thin water) = more transparent; troughs (deep) = more opaque
        float waterAlpha = lerp(0.96f, 0.82f, saturate(waveDisp / 12.0f));

        // === Final Composite ===
        finalColor.rgb = waterColor;
        finalColor.rgb += specColor * shadowFactor;

        // --- Surface sparkle: 넓은 표면의 단조로움 깨기 (멀리 바깥 물에서 살아남) ---
        //   grazing(시선이 표면과 평행할수록 1): 카메라 정면 위에서 본 평면은 sparkle 약함,
        //   원거리에서 표면을 거의 옆에서 본 픽셀은 sparkle 강함 → 광활한 바깥 물 면이
        //   햇빛 흩뿌리는 듯한 동적 패턴 획득.
        //   peak only (pow 3.0): 표면 도배 아니라 점점이 반짝이는 인상.
        float NdotV_w = saturate(dot(shadingNormal, vToCamera));
        float grazing = pow(1.0f - NdotV_w, 1.6f);
        float sparkle = WaterCaustics(input.worldPosition.xz * 0.45f, g_Time * 1.20f);
        sparkle = pow(saturate(sparkle), 3.0f);
        finalColor.rgb += float3(0.60f, 0.82f, 0.98f) * sparkle * grazing * 0.42f;

        finalColor.rgb  = lerp(finalColor.rgb, foamColor, foamStrength);
        finalColor.rgb  = lerp(finalColor.rgb, fresnelColor, waterFresnel * 0.18f);

        // 일반 지오메트리와 동일한 atmospheric tint/fog 적용 — 톤다운.
        //   - fog 시작 거리 ↑(10→18) + 강도 ↓(0.75→0.45): 가까운 픽셀 파랗게 덮이는 현상 회피.
        //   - tint 곱 약화(1.22→1.10) + 적용 비율 ↓(0.90→0.50): 청록 도배 완화.
        //   - 전체 1.06× 가산 제거: 물맵이 전반적으로 떠 보이는 원인.
        if (g_StageTheme == 1)
        {
            float camDist_w = distance(g_CameraPosition, input.worldPosition);
            float fog_w = saturate((camDist_w - 18.0f) / 55.0f);
            float3 fogColor_w = float3(0.22f, 0.42f, 0.58f);
            finalColor.rgb = lerp(finalColor.rgb, fogColor_w, fog_w * 0.45f);
            float3 tinted_w = finalColor.rgb * float3(0.96f, 1.04f, 1.10f);
            finalColor.rgb = lerp(finalColor.rgb, tinted_w, 0.50f);
        }

        return float4(finalColor.rgb, waterAlpha);
    }

    // ── Stage-themed environment: Water (caustics + atmospheric tint) ──
    // bIsWater 분기는 위에서 return 했으므로 여기 도달하는 픽셀은 일반 지오메트리.
    // 캐릭터(bIsSkinned)에는 환경 fog/tint 적용 안 함 → 카테고리 색이 물맵 청록에 묻히지 않음.
    if (g_StageTheme == 1 && !bIsSkinned)
    {
        // Caustics — 톤다운: HDR 피크(1.45)·shimmer 진폭·강도 모두 축소.
        //   peak > 1 유지(bloom 통과)는 살리되 네온 청록 폭격은 회피.
        float upFacing = saturate(normal.y);
        float caust = WaterCaustics(input.worldPosition.xz, g_Time);
        float shimmer = 0.90f + 0.18f * sin(g_Time * 0.9f + input.worldPosition.x * 0.07f);
        float3 causticColor = float3(0.50f, 0.80f, 1.05f);

        float caustWeight = lerp(0.20f, 0.65f, smoothstep(0.0f, 0.6f, upFacing));
        finalColor.rgb += causticColor * caust * caustWeight * shadowFactor * 0.45f * shimmer;

        // 거리 fog — 시작 거리 ↑(10→18) + 강도 ↓(0.75→0.45). 가까운 픽셀까지 파랗던 문제 해소.
        float camDist = distance(g_CameraPosition, input.worldPosition);
        float fog = saturate((camDist - 18.0f) / 55.0f);
        float3 fogColor = float3(0.22f, 0.42f, 0.58f);
        finalColor.rgb = lerp(finalColor.rgb, fogColor, fog * 0.45f);

        // 거리 desat — 최대 강도 0.35→0.22. 멀수록 자연스럽게 묻히되 색 다 빠지지 않게.
        float lum_w = dot(finalColor.rgb, float3(0.299f, 0.587f, 0.114f));
        float desat = saturate((camDist - 18.0f) / 50.0f) * 0.22f;
        finalColor.rgb = lerp(finalColor.rgb,
                              float3(lum_w * 0.90f, lum_w * 1.00f, lum_w * 1.06f),
                              desat);

        // 청량 톤 시프트 — 곱 약화 + 적용 비율 ↓, 전체 1.06× 가산 제거.
        float3 tinted = finalColor.rgb * float3(0.96f, 1.04f, 1.10f);
        finalColor.rgb = lerp(finalColor.rgb, tinted, 0.50f);
    }

    // ── Stage-themed environment: Earth (rocky-desert: sun-baked + sand ripple + heat haze) ──
    //   기본은 맑게 (옅은 dust + 약한 tone shift). g_StormStrength (0~1) 가 1 로 차면
    //   모래폭풍 — fog/dust 강해지고, 톤 amber-orange, vignette 살짝, ripple wobble 증폭.
    if (g_StageTheme == 2)
    {
        float upFacing = saturate(normal.y);
        float camDistE = distance(g_CameraPosition, input.worldPosition);
        float storm    = saturate(g_StormStrength);

        // ── Storm density: 시간축 gust pulse 만 (공간 노이즈는 격자 노출 → 사용 X) ──
        //   sharp attack-decay 두 옥타브 합성 → 짧은 강풍 + 긴 호흡 결합, "파도 도착" 펄스.
        //   spatial 변동은 아래 screen-space sand streak 으로 표현 (world geo 무관).
        float _phaseA = frac(g_Time * 0.55f);              // 1.8s 주기
        float _phaseB = frac(g_Time * 0.28f + 0.37f);      // 3.6s 주기 (느린 호흡)
        float _gustA  = exp(-_phaseA * 2.6f);
        float _gustB  = exp(-_phaseB * 1.4f);
        float densityMul = lerp(0.75f, 1.45f, saturate(_gustA * 0.55f + _gustB * 0.55f));

        // 지면 셰이딩(워밍·ripple·heat haze)은 환경 지오메트리에만.
        // 캐릭터에는 fog/tint만 입혀서 분위기에 녹게 한다.
        if (!bIsSkinned)
        {
            // (a) 위쪽 면에 강한 햇빛 워밍 — sun-baked sandstone.
            if (upFacing > 0.15f)
            {
                float slopeMul = smoothstep(0.15f, 0.70f, upFacing);
                finalColor.rgb += float3(0.080f, 0.045f, 0.008f) * slopeMul;

                // 절차적 모래 리플 — 골에 부드러운 음영 (wind-blown sand)
                // storm 시 wobble 증폭 (모래가 강하게 휘날리는 느낌).
                float ripple = SandRipple(input.worldPosition.xz, g_Time);
                float rippleStrength = lerp(0.55f, 0.95f, storm);
                finalColor.rgb *= lerp(1.0f, 0.82f, ripple * slopeMul * rippleStrength);
            }

            // (b) Heat haze — 멀리 픽셀 밝기를 sin파로 미세 진동 → 아지랑이.
            //     픽셀 단위 brightness wobble. 풀스크린 distortion은 아니지만
            //     멀리 표면이 출렁이는 신호로는 충분.
            float hazeRange = saturate((camDistE - 14.0f) / 30.0f);
            float hazeWobble = sin(input.worldPosition.y * 2.5f
                                 + g_Time * 3.4f
                                 + input.worldPosition.x * 0.30f) * 0.5f + 0.5f;
            float hazeAmp = lerp(0.10f, 0.22f, storm);
            finalColor.rgb *= 1.0f + (hazeWobble - 0.5f) * hazeAmp * hazeRange;
        }

        // (c) 거리 fog 2단 — 폭풍 시 실루엣만 보이도록 카메라 0u 부터 폭주.
        //     near 시작 0u → 가까운 캐릭터부터 잠식, range 8u → 매우 가파른 wall.
        //     peak 강도 1.15 (densityMul 거의 100% 통과) → 거의 풀 amber 잠김.
        float fogNear = saturate((camDistE - lerp(12.0f, 0.0f, storm)) / lerp(30.0f, 8.0f, storm));
        float3 fogColorNearBase  = float3(0.85f, 0.70f, 0.48f);
        float3 fogColorNearStorm = float3(0.85f, 0.45f, 0.14f);
        float3 fogColorNear      = lerp(fogColorNearBase, fogColorNearStorm, storm);
        float  fogNearMean       = lerp(0.18f, 1.15f, storm);
        float  fogNearStrength   = fogNearMean * lerp(1.0f, densityMul, storm);
        finalColor.rgb = lerp(finalColor.rgb, fogColorNear, saturate(fogNear * fogNearStrength));

        float fogFar = saturate((camDistE - lerp(38.0f, 12.0f, storm)) / lerp(45.0f, 18.0f, storm));
        float3 fogColorFar = lerp(float3(0.55f, 0.65f, 0.78f),
                                  float3(0.60f, 0.36f, 0.18f), storm);
        float  fogFarStrength = lerp(0.30f, 1.10f, storm) * lerp(1.0f, densityMul, storm * 0.6f);
        finalColor.rgb = lerp(finalColor.rgb, fogColorFar, saturate(fogFar * fogFarStrength));

        // (d) 따뜻한 톤 시프트 — sun-baked 분위기. storm 시 rusty orange (덜 핑크빛).
        float3 tintBase  = float3(1.08f, 1.03f, 0.95f);
        float3 tintStorm = float3(1.22f, 0.82f, 0.52f);
        float3 tintMul   = lerp(tintBase, tintStorm, storm);
        float3 tinted_e  = finalColor.rgb * tintMul;
        float  tintBlend = lerp(0.40f, 0.65f, storm);
        finalColor.rgb = lerp(finalColor.rgb, tinted_e, tintBlend);

        // (e) 폭풍 peak 시 모든 픽셀에 amber 흡수 — 실루엣 만 보이는 수준.
        //     near fog 만으로는 가까운 캐릭터 윤곽 살아남음 → 거리 무관 강제 amber lerp.
        //     0.45 흡수: 색 정보 절반 손실, 실루엣은 유지 (완전 흡수는 형체 사라짐).
        if (storm > 0.0f)
        {
            float3 stormAbsorb = float3(0.74f, 0.42f, 0.18f);
            finalColor.rgb = lerp(finalColor.rgb, stormAbsorb, storm * 0.45f);

            // (f) 미세 grain — per-pixel hash + fast wind scroll.
            //     모양 인지 안 될 만큼 작은 픽셀 클러스터에 ±작은 휘도 변동.
            //     "탁한 공기 + 흩날리는 모래" 느낌만 부여, 형태(원/네모) 0.
            //     ±3% 명도 흔들기라 격자 못 인지, scroll 로 wind 방향 흐름만 감지.
            float2 wind2D  = float2(-1.0f, -0.35f);
            float2 grainP  = (input.position.xy + wind2D * g_Time * 80.0f) * 0.5f;
            float  gh1     = _hash12(floor(grainP));
            float  gh2     = _hash12(floor(grainP * 0.42f) + 7.1f);
            float  grain   = (gh1 - 0.5f) * 0.55f + (gh2 - 0.5f) * 0.45f;
            finalColor.rgb *= 1.0f + grain * storm * 0.06f;

            // (g) gust pulse — 강풍 시 전체 휘도 펄스.
            float gustPunch = max(_gustA, _gustB);
            finalColor.rgb *= 1.0f + gustPunch * storm * 0.10f;
        }
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
            finalColor.rgb += float3(0.10f, 0.040f, 0.008f) * tileMask * slopeMul;

            // (c) 균열 라인 — 모든 위치에서 그어짐, 타일 위에 살짝 더 진함.
            //   누적 가산 + contrast 통과 시 bloom threshold(0.96) 초과로 흰빛 번짐 → 강도/색 모두 하향.
            //   crackColor red 0.75 (was 0.95) — 피크 자체를 1.0 미만 안전 영역으로.
            float cracks = LavaCrackLines(input.worldPosition.xz, g_Time);
            float3 crackColor = float3(0.75f, 0.32f, 0.06f);
            float crackBoost = 0.55f + 0.45f * tileMask;
            finalColor.rgb += crackColor * cracks * crackBoost * slopeMul * 0.18f;
        }
    }

    // ── Stage-themed environment: Grass (wind-swept sunny meadow) ──
    //   바람 컨셉 가시화 — 풀 sway 만으로는 부족 (다른 맵의 용암/물/모래처럼 환경 단서 필요).
    //   지면 wind streak: wind 방향으로 흘러가는 ridge noise 띠 — "보이지 않는 바람" 시각화.
    //   g_GustStrength: CPU envelope 0~1, 돌풍 시 streak 속도·강도·전체 톤 증폭.
    if (g_StageTheme == 3)
    {
        float upFacing = saturate(normal.y);
        float camDistG = distance(g_CameraPosition, input.worldPosition);
        float gust     = saturate(g_GustStrength);

        // (a) 베이스 워밍 — 햇살 비치는 풀밭 분위기. 위쪽 면에만 살짝 따뜻한 녹색 톤.
        if (!bIsSkinned && upFacing > 0.15f)
        {
            float slopeMul = smoothstep(0.15f, 0.65f, upFacing);
            finalColor.rgb += float3(0.035f, 0.070f, 0.022f) * slopeMul;
        }

        // (b) 지면 wind flow — domain warp 된 2D 노이즈, 곡선 결로 흐름.
        //   1D 노이즈는 평행 직선 띠 → "선" 으로 보임. 2D + domain warp 로 곡선/소용돌이 만들기.
        //   wind 방향으로 noise field scroll → 결이 wind 방향으로 흘러감.
        if (!bIsSkinned && !bIsGrass && upFacing > 0.30f)
        {
            float slopeMul = smoothstep(0.30f, 0.75f, upFacing);
            float speedMul = lerp(1.0f, 2.4f, gust);

            float2 wind = float2(0.85f, 0.30f);
            float2 q    = input.worldPosition.xz * 0.18f - wind * (g_Time * 0.42f * speedMul);

            // Domain warp — q 를 다른 노이즈로 흔들어 curling 패턴 만들기.
            //   warp 강도 0.85 (큼) → 결이 직선에서 곡선/소용돌이로 변형.
            float2 w;
            w.x = _vnoise(q * 1.4f + 1.7f) * 2.0f - 1.0f;
            w.y = _vnoise(q * 1.4f + 9.3f + g_Time * 0.12f) * 2.0f - 1.0f;
            float2 wq = q + w * 0.85f;

            // 옥타브별 회전 — axis bias 제거 (격자 회피).
            const float2x2 rot53g = float2x2(0.6f, 0.8f, -0.8f, 0.6f);
            float2 p1 = wq;
            float2 p2 = mul(rot53g, p1) * 2.1f + 7.7f;
            float n1  = _vnoise(p1);
            float n2  = _vnoise(p2);
            float flow = saturate(n1 * 0.62f + n2 * 0.45f);

            // 결: peak (햇살) + 골 (그늘) — 명도 굴곡으로 wind flow 인지.
            float flowHi = smoothstep(0.55f, 0.92f, flow);
            float flowLo = 1.0f - smoothstep(0.0f, 0.35f, flow);
            float3 flowBase = lerp(float3(0.030f, 0.058f, 0.024f),
                                   float3(0.018f, 0.070f, 0.044f), gust);
            float flowStrength = lerp(0.55f, 1.10f, gust);
            finalColor.rgb += flowBase * flowHi * slopeMul * flowStrength;
            finalColor.rgb *= 1.0f - flowLo * slopeMul * lerp(0.05f, 0.10f, gust);
        }

        // (c) 돌풍 화면 톤 — gust peak 시 옅은 cool 그린 wash + 명도 살짝 dim.
        //     "강풍이 부는 순간 풀밭이 한 톤 차가워지고 빛이 갇히는" 느낌.
        if (gust > 0.0f)
        {
            float3 gustTint = float3(0.78f, 0.92f, 0.82f);
            finalColor.rgb = lerp(finalColor.rgb, finalColor.rgb * gustTint, gust * 0.35f);
            finalColor.rgb *= 1.0f - gust * 0.04f;  // 살짝 dim
        }

        // (d) 거리 haze — 자연광 톤, 옅게. 멀어질수록 살짝 흐려지는 정도.
        //     돌풍 시 haze 살짝 더 진하게 (먼 시야 차단으로 임팩트).
        float fog = saturate((camDistG - lerp(22.0f, 16.0f, gust)) / 55.0f);
        float3 fogColor_g = lerp(float3(0.80f, 0.86f, 0.72f),
                                 float3(0.70f, 0.82f, 0.74f), gust);
        finalColor.rgb = lerp(finalColor.rgb, fogColor_g, fog * lerp(0.28f, 0.55f, gust));
    }

    // --- Toon final-color grading (saturation + contrast pop) ---
    // Stage envrironment effects (caustics, lava cracks, fog) overwhelm the
    // cel boundary on flat lit surfaces. A global saturation+contrast bump on
    // toon mode makes the difference visible everywhere, not just on shadow
    // boundaries.
    // 채도 분리 폴라리티 — 환경 desat 로 캐릭터 부상.
    //   0.65 는 불맵 라바 가산 발광이 hue 죽으면서 펠-화이트, 물맵은 청색 hue 죽어 다크 케이브화.
    //   contrast 1.10× 가 양극단 증폭 (밝은 곳 더 밝게, 어두운 곳 더 어둡게) 문제 가속.
    //   → sat 0.78 (14% 갭 — 인지 가능, hue 보존), contrast 환경도 1.03 으로 완화.
    if (g_ToonEnabled != 0)
    {
        float satBoost  = (bIsSkinned != 0) ? 0.92f : 0.78f;
        float contBoost = (bIsSkinned != 0) ? 1.05f : 1.03f;
        float lum = dot(finalColor.rgb, float3(0.299f, 0.587f, 0.114f));
        finalColor.rgb = lerp(float3(lum, lum, lum), finalColor.rgb, satBoost);
        // max() (not saturate) preserves HDR > 1 so bloom still picks up
        // emissive/caustic peaks.
        finalColor.rgb = max((finalColor.rgb - 0.42f) * contBoost + 0.42f, 0.0f);
    }

    // 캐릭터 최저 밝기 floor — 환경광/라이팅이 약한 스테이지(라바 위 등)에서
    //   캐릭터가 검정 실루엣으로 떨어지는 문제 차단. Bloom/Toon 모드 무관.
    //   0.55→0.38→0.22: lightTerm=1 강제로 이미 풀 lit 이라 floor 는 안전망 정도면 충분.
    //   너무 높이면 ambient/lit 위에 누적되어 발광·쨍한 인상.
    if (bIsSkinned != 0)
        finalColor.rgb = max(finalColor.rgb, baseColor.rgb * 0.22f);

    // 캐릭터 상시 림라이트 — sat 다운으로 칙칙해진 캐릭터 외곽 분리 보완.
    //   0.10 → 0.14: sat 0.92×로 떨어진 만큼 외곽 림에서 색 정보 보충.
    if (bIsSkinned != 0)
    {
        float3 rimView = normalize(g_CameraPosition - input.worldPosition);
        float rimT = 1.0f - saturate(dot(normalize(input.worldNormal), rimView));
        float rimMask = pow(rimT, 3.0f) * 0.14f;
        finalColor.rgb += baseColor.rgb * rimMask;
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