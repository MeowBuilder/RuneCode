// ─── Fullscreen Screen-Split (화면 베기 후 두 조각 분리 이동) ───────────────
// 입력: g_capture (방금 캡쳐된 backbuffer 텍스처).
// 베기 라인 -45° 기준 양쪽 절반이 normal 방향(베기 수직)으로 분리되어 슬라이드.
// 가운데 검은 슬릿이 점점 벌어짐. progress 0→1 동안 분리 magnitude ↑.

cbuffer cbSplit : register(b0)
{
    float4 g_params;   // x=progress (0~1), y=peakOffsetUV, z=angleRad, w=slitWidthUV
    float4 g_params2;  // x=aspect (W/H) — 화면 종횡비 보정용
}

Texture2D    g_capture : register(t0);
SamplerState g_samp    : register(s0);

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VS_Fullscreen(uint vid : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv  = uv;
    return o;
}

float4 PS_Split(VSOut input) : SV_TARGET
{
    float progress    = g_params.x;
    float peakOffset  = g_params.y;
    float angle       = g_params.z;
    float slitWidth   = g_params.w;
    float aspect      = g_params2.x;   // W/H (예: 16:9 → 1.778)

    // ★ Aspect 보정: UV (0~1, 0~1) 는 비정사각. UV 공간에서의 45° 는 화면 픽셀에선
    //   atan(H/W * tan(45°)) ≈ 29° (16:9) → 스프라이트 -45° 와 mismatch.
    //   centered.x 에 aspect 곱해 "정사각 픽셀 공간" 으로 변환 — 이 공간에서 각도/normal
    //   계산하면 SpriteBatch 가 그리는 -45° 와 정확히 일치.
    float2 normal = float2(sin(angle), -cos(angle));
    float2 centered = (input.uv - 0.5f) * float2(aspect, 1.0f);
    float  dist     = dot(centered, normal);

    float ease   = 1.0f - pow(1.0f - progress, 3.0f);
    float offset = peakOffset * ease;

    // 슬릿 (검정 갭) — 진행도 비례 두꺼워짐. dist 는 정사각공간이므로 slitWidth/offset 도
    //   "H 단위" 로 해석되어 화면에서 perceptually 일정.
    float slitDynamic = slitWidth + offset * 0.5f;
    if (abs(dist) < slitDynamic)
        return float4(0.02f, 0.0f, 0.01f, 1.0f);

    float side = (dist >= 0.0f) ? 1.0f : -1.0f;

    // 샘플 오프셋: 정사각공간에서 (offset * normal) 만큼 슬라이드 → UV 공간으론 X 축만
    //   aspect 로 나눠 환산해야 동일 픽셀 거리.
    float2 normalUV = float2(normal.x / aspect, normal.y);
    float2 sampleUV = input.uv - normalUV * (offset * side);

    // sample 의 side 체크도 정사각공간에서.
    float2 sampleCentered = (sampleUV - 0.5f) * float2(aspect, 1.0f);
    float  sampleDist     = dot(sampleCentered, normal);
    if (sampleDist * side < 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    if (sampleUV.x < 0.0f || sampleUV.x > 1.0f
     || sampleUV.y < 0.0f || sampleUV.y > 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    return g_capture.Sample(g_samp, sampleUV);
}
