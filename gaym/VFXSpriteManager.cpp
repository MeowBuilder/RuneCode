#include "stdafx.h"
#include "VFXSpriteManager.h"
#include <algorithm>

using namespace DirectX;

VFXSpriteManager& VFXSpriteManager::Get()
{
    static VFXSpriteManager inst;
    return inst;
}

void VFXSpriteManager::RegisterTex(const std::string& id, D3D12_GPU_DESCRIPTOR_HANDLE srv, UINT w, UINT h)
{
    m_texMap[id] = { srv, w, h };
}

int VFXSpriteManager::Spawn(const std::string& texId, const XMFLOAT3& worldPos,
                             float screenSize, float lifetime, XMFLOAT4 color,
                             float rotateSpeed, VFXSpriteAnim anim, float initialRotation)
{
    int slot = -1;
    for (int i = 0; i < MAX_ENTRIES; ++i)
    {
        if (!m_pool[i].active) { slot = i; break; }
    }
    if (slot < 0) return -1;

    Entry& e    = m_pool[slot];
    e.texId     = texId;
    e.worldPos  = worldPos;
    e.screenSize= screenSize;
    e.lifeMax   = lifetime;
    e.lifeRemain= lifetime;
    e.color     = color;
    e.rotAngle  = initialRotation;
    e.rotSpeed  = rotateSpeed;
    e.anim      = anim;
    e.active    = true;
    return slot;
}

void VFXSpriteManager::SetPosition(int slot, const XMFLOAT3& pos)
{
    if (slot < 0 || slot >= MAX_ENTRIES || !m_pool[slot].active) return;
    m_pool[slot].worldPos = pos;
}

void VFXSpriteManager::Stop(int slot)
{
    if (slot < 0 || slot >= MAX_ENTRIES) return;
    m_pool[slot].active = false;
}

void VFXSpriteManager::Update(float dt)
{
    for (auto& e : m_pool)
    {
        if (!e.active) continue;
        e.lifeRemain -= dt;
        e.rotAngle   += e.rotSpeed * dt;
        if (e.lifeRemain <= 0.f) e.active = false;
    }
}

void VFXSpriteManager::Render(SpriteBatch* pBatch, const XMFLOAT4X4& viewProj, int screenW, int screenH)
{
    if (!pBatch) return;
    XMMATRIX mVP = XMLoadFloat4x4(&viewProj);

    for (const auto& e : m_pool)
    {
        if (!e.active) continue;

        auto it = m_texMap.find(e.texId);
        if (it == m_texMap.end()) continue;
        const TexInfo& tex = it->second;
        if (!tex.srv.ptr) continue;

        // 월드 → NDC
        XMVECTOR worldV = XMLoadFloat3(&e.worldPos);
        float    w      = XMVectorGetW(XMVector3Transform(worldV, mVP));
        if (w <= 0.01f) continue;
        XMVECTOR ndc = XMVector3TransformCoord(worldV, mVP);
        float sx = (XMVectorGetX(ndc) *  0.5f + 0.5f) * (float)screenW;
        float sy = (XMVectorGetY(ndc) * -0.5f + 0.5f) * (float)screenH;
        if (sx < -300.f || sx > (float)screenW + 300.f) continue;
        if (sy < -300.f || sy > (float)screenH + 300.f) continue;

        float t     = 1.f - (e.lifeRemain / e.lifeMax); // 0→1 경과
        float scale = e.screenSize / (float)(std::max)(tex.width, 1u);
        float alpha = e.color.w;

        if (e.anim == VFXSpriteAnim::SkullPop)
        {
            float sizeMult;
            if      (t < 0.2f) { float p = t / 0.2f;        sizeMult = 1.f + 0.35f * p; }
            else if (t < 0.5f) { float p = (t - 0.2f)/0.3f; sizeMult = 1.35f - 0.35f * p; }
            else               { float p = (t - 0.5f)/0.5f; sizeMult = 1.f; alpha *= (1.f - p); }
            scale *= sizeMult;
        }
        else // FadeOut: 마지막 30% 구간에서 페이드
        {
            constexpr float kFadeStart = 0.7f;
            if (t > kFadeStart)
                alpha *= 1.f - (t - kFadeStart) / (1.f - kFadeStart);
        }
        alpha = (std::max)(0.f, (std::min)(1.f, alpha));

        XMVECTORF32 col = { e.color.x, e.color.y, e.color.z, alpha };
        XMFLOAT2    origin((float)tex.width * 0.5f, (float)tex.height * 0.5f);

        pBatch->Draw(tex.srv, XMUINT2(tex.width, tex.height),
                     XMFLOAT2(sx, sy), nullptr,
                     col, e.rotAngle, origin, scale);
    }
}
