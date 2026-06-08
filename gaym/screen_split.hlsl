// ─── Fullscreen Screen-Split (화면 베기 후 두 조각 분리 이동) ───────────────
// 입력: g_capture (방금 캡쳐된 backbuffer 텍스처).
// 베기 라인 -45° 기준 양쪽 절반이 normal 방향(베기 수직)으로 분리되어 슬라이드.
// 가운데 검은 슬릿이 점점 벌어짐. progress 0→1 동안 분리 magnitude ↑.

cbuffer cbSplit : register(b0)
{
    float4 g_params;  // x=progress (0~1), y=peakOffsetUV, z=angleRad, w=slitWidthUV
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

    float2 normal = float2(sin(angle), -cos(angle));
    float2 centered = input.uv - float2(0.5, 0.5);
    float  dist     = dot(centered, normal);

    float ease   = 1.0f - pow(1.0f - progress, 3.0f);
    float offset = peakOffset * ease;

    // 슬릿 영역 (가운데 검은 갭) — 진행도에 따라 두꺼워짐.
    float slitDynamic = slitWidth + offset * 0.5f;
    if (abs(dist) < slitDynamic)
        return float4(0.02f, 0.0f, 0.01f, 1.0f);

    // ★ 진짜 "두 조각 분리" — 위쪽 절반(+normal)은 자기 콘텐츠를 +normal 방향으로
    //   슬라이드 시킨 것처럼. 픽셀 P 가 자기 자리에서 보일 색 = (P - normal*offset).
    //   sampleUV 의 dist 가 부호 반대편이거나 화면 밖이면 빈 공간 (검정).
    float side = (dist >= 0.0f) ? 1.0f : -1.0f;
    float2 sampleUV = input.uv - normal * (offset * side);

    // sampleUV 도 베기 라인 같은 쪽에 있어야 자기 콘텐츠. 반대편이면 빈 공간.
    float2 sampleCentered = sampleUV - float2(0.5, 0.5);
    float  sampleDist     = dot(sampleCentered, normal);
    if (sampleDist * side < 0.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);   // 빈 영역 — 검정

    // 화면 밖 sample 도 검정.
    if (sampleUV.x < 0.0f || sampleUV.x > 1.0f
     || sampleUV.y < 0.0f || sampleUV.y > 1.0f)
        return float4(0.0f, 0.0f, 0.0f, 1.0f);

    return g_capture.Sample(g_samp, sampleUV);
}
