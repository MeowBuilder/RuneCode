// leaf_particle.hlsl
// 잎새/꽃잎 빌보드 — fluid_particles.hlsl 와 거의 같지만 텍스처 sample + 입자 개별 회전.
// 사용처: LightEmitterSystem 의 sprite 모드 (Wind_CherryPetals/GreenLeaves/AutumnLeaves).

cbuffer cbLeafPass : register(b0)
{
    matrix gViewProj;
    float3 gCameraRight; float gPadR;
    float3 gCameraUp;    float gPadU;
    float  gTime;        float gRotSpeed;
    float  gSwayAmp;     float gSwayFreq;
};

// Root constant — 매 draw 마다 다른 baseInstance offset 전달
cbuffer cbRootConst : register(b1)
{
    int  gBaseInstance;
    int  _rpad0;
    int  _rpad1;
    int  _rpad2;
};

struct LeafParticleData
{
    float3 position;
    float  size;
    float4 color;
    float  yawAng;
    float  pitchAng;
    float  pad0;
    float  pad1;
};
StructuredBuffer<LeafParticleData> gParticles : register(t0);
Texture2D                          gLeafTex   : register(t1);
SamplerState                       gLeafSamp  : register(s0);

struct LeafVSOut
{
    float4 pos   : SV_POSITION;
    float4 color : COLOR0;
    float2 uv    : TEXCOORD0;
};

// TRIANGLE_STRIP 순서: 쿼드당 4개 정점
static const float2 kOffsets[4] = {
    { -0.5f, -0.5f },
    {  0.5f, -0.5f },
    { -0.5f,  0.5f },
    {  0.5f,  0.5f }
};
static const float2 kUVs[4] = {
    { 0.0f, 1.0f },
    { 1.0f, 1.0f },
    { 0.0f, 0.0f },
    { 1.0f, 0.0f }
};

LeafVSOut VS_Leaf(uint vertId : SV_VertexID, uint instId : SV_InstanceID)
{
    LeafParticleData p = gParticles[instId + (uint)gBaseInstance];

    float2 offset = kOffsets[vertId];
    float2 uv     = kUVs[vertId];

    // 잎새 plane 회전 — CPU 에서 누적된 yaw + pitch 사용 (입자별 다양성, 자연스러운 변화).
    float csYaw = cos(p.yawAng);
    float snYaw = sin(p.yawAng);
    float csP   = cos(p.pitchAng);
    float snP   = sin(p.pitchAng);

    float3 localRight   = float3(csYaw, 0.0f, snYaw);
    float3 localForward = float3(-snYaw * csP, snP, csYaw * csP);

    float3 worldPos = p.position
        + localRight   * (offset.x * p.size)
        + localForward * (offset.y * p.size);

    LeafVSOut output;
    output.pos   = mul(float4(worldPos, 1.0f), gViewProj);
    output.color = p.color;
    output.uv    = uv;
    return output;
}

float4 PS_Leaf(LeafVSOut input) : SV_TARGET
{
    // 텍스처 sample (RGBA + alpha mask)
    float4 tex = gLeafTex.Sample(gLeafSamp, input.uv);

    // 텍스처 알파 < 0.05 면 clip (잎 외곽 깔끔)
    clip(tex.a - 0.05f);

    // 입자 색상으로 tint (텍스처 색 × 입자 색)
    //   원소 톤 적용: 텍스처가 흰색에 가까우면 tint 가 그대로 보임.
    float3 col = tex.rgb * input.color.rgb;
    float  a   = tex.a * input.color.a;

    return float4(col, a);
}
