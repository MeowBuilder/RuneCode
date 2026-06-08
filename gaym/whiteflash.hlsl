// ─── Fullscreen White-Flash Overlay ─────────────────────────────────────────
// Bloom 직후, 텍스트 렌더 직전에 호출되는 풀스크린 알파 블렌딩 패스.
// 검기 임팩트의 "찰나" 화이트 플래시 (Sekiro/Genshin 컨인 스타일).

cbuffer cbFlash : register(b0)
{
    float4 g_flashColor;  // xyz=RGB, w=alpha (0~1)
};

struct VSOut
{
    float4 pos : SV_POSITION;
};

// 풀스크린 삼각형 (3 vertex, no IA). vertexId 0/1/2 → 화면 전체 덮음.
VSOut VS_Fullscreen(uint vertexId : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);  // 0,0 / 2,0 / 0,2
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float4 PS_Flash(VSOut input) : SV_TARGET
{
    return float4(g_flashColor.rgb, g_flashColor.a);
}
