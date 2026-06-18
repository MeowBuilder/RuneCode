#include "stdafx.h"
#include "SlashCue.h"
#include "Scene.h"
#include "VFXManager.h"
#include "Camera.h"
#include "Dx12App.h"
#include "AudioManager.h"
#include "HitStopSystem.h"
#include "WhiteFlashOverlay.h"

using namespace DirectX;

SlashCue::~SlashCue()
{
    End();
}

const char* SlashCue::ChargeAuraEffectName(ElementType e)
{
    switch (e)
    {
    case ElementType::Fire:  return "Boss_SwordCharge_Fire";
    case ElementType::Water: return "Boss_SwordCharge_Water";
    case ElementType::Wind:  return "Boss_SwordCharge_Wind";
    case ElementType::Earth: return "Boss_SwordCharge_Earth";
    default:                 return "Boss_SwordCharge_Fire";
    }
}

const char* SlashCue::ImpactEffectName(ElementType e)
{
    switch (e)
    {
    case ElementType::Fire:  return "Boss_SigilImpact_Fire";
    case ElementType::Water: return "Boss_SigilImpact_Water";
    case ElementType::Wind:  return "Boss_SigilImpact_Wind";
    case ElementType::Earth: return "Boss_SigilImpact_Earth";
    default:                 return "Boss_SigilImpact_Fire";
    }
}

XMFLOAT3 SlashCue::ImpactFlashColor(ElementType e)
{
    switch (e)
    {
    case ElementType::Fire:  return { 1.0f, 0.55f, 0.18f };  // 주황
    case ElementType::Water: return { 0.40f, 0.85f, 1.0f };
    case ElementType::Wind:  return { 0.65f, 1.0f,  0.80f };
    case ElementType::Earth: return { 1.0f,  0.85f, 0.50f };
    default:                 return { 1.0f, 1.0f,  1.0f };
    }
}

void SlashCue::Begin(Scene* pScene, const SlashVFXDesc& desc,
                     const XMFLOAT3& swordTipPos, const XMFLOAT3& swordDir)
{
    if (m_bActive) End();   // 이전 cue 가 살아있으면 정리

    m_pScene    = pScene;
    m_desc      = desc;
    m_bActive   = true;
    m_bReleased = false;

    // L1 — 검 끝에 차징 오라 부착
    if (m_desc.chargeDuration > 0.0f && pScene)
    {
        if (VFXManager* pVFX = pScene->GetVFXManager())
        {
            const char* effectName = ChargeAuraEffectName(m_desc.element);
            m_nChargeVFXSlot = pVFX->Spawn(effectName, swordTipPos, swordDir, 0u, false);
        }
    }

    // 차징 SFX hook (파일 없으면 silent)
    if (auto* pApp = Dx12App::GetInstance())
    {
        if (auto* pAudio = pApp->GetAudio())
            pAudio->PlaySFX(L"boss_slash_charge", 0.7f);
    }
}

void SlashCue::Tick(float /*rawDt*/,
                    const XMFLOAT3& swordTipPos, const XMFLOAT3& swordDir)
{
    if (!m_bActive || m_nChargeVFXSlot < 0 || !m_pScene) return;
    if (VFXManager* pVFX = m_pScene->GetVFXManager())
        pVFX->Track(m_nChargeVFXSlot, swordTipPos, swordDir);
}

void SlashCue::Release()
{
    if (!m_bActive || m_bReleased) return;
    m_bReleased = true;

    // L2 (a) — 풀스크린 화이트 플래시
    if (auto* pApp = Dx12App::GetInstance())
    {
        if (auto* pFlash = pApp->GetWhiteFlash())
        {
            if (m_desc.whiteFlashAlpha > 0.0f && m_desc.whiteFlashFade > 0.0f)
                pFlash->RequestFlash(m_desc.whiteFlashAlpha, m_desc.whiteFlashFade,
                                     { 1.0f, 1.0f, 1.0f });
        }
    }

    // L2 (b) — Hit-Stop (찰나의 정지)
    if (m_desc.hitStopSeconds > 0.0f)
        HitStopSystem::Get().Request(m_desc.hitStopSeconds);

    // L2 (c) — 카메라 펄스
    if (m_pScene)
    {
        if (CCamera* pCam = m_pScene->GetCamera())
        {
            pCam->RequestSlashPulse(m_desc.cameraShakeIntensity,
                                    m_desc.cameraShakeDuration,
                                    m_desc.cameraZoomBoost,
                                    0.30f /*zoomHoldSec*/);
        }
    }

    // L2 (d) — Whoosh SFX (silent if no file)
    if (auto* pApp = Dx12App::GetInstance())
    {
        if (auto* pAudio = pApp->GetAudio())
            pAudio->PlaySFX(L"boss_slash_whoosh", 0.9f);
    }

    // 차징 오라는 발도와 동시에 정지 — 검기 본체가 시작되므로
    if (m_nChargeVFXSlot >= 0 && m_pScene)
    {
        if (auto* pVFX = m_pScene->GetVFXManager())
            pVFX->Stop(m_nChargeVFXSlot);
        m_nChargeVFXSlot = -1;
    }
}

void SlashCue::Impact(const XMFLOAT3& impactPos,
                       const XMFLOAT3& groundPos,
                       const XMFLOAT3& dir)
{
    if (!m_bActive) return;

    // L5 (a) — Ring 충격파 + Cone spark burst (Boss_SigilImpact_{원소})
    // L4 (잔류) — Boss_GroundResidue_{원소} (불티/얼음/먼지/돌조각 2~3s 떠다님)
    if (m_pScene)
    {
        if (auto* pVFX = m_pScene->GetVFXManager())
        {
            const char* impactFx = ImpactEffectName(m_desc.element);
            pVFX->Spawn(impactFx, groundPos, dir, 0u, false);

            // 잔류 데칼 — desc.useGroundResidue 가 true 일 때만. Light/Projectile 등
            // 가벼운 톤은 잔재 안 남김 (Day 6 +: 검기 발사 후 작은 잔재 누적 방지).
            if (m_desc.useGroundResidue)
            {
                std::string residueName = "Boss_GroundResidue_";
                switch (m_desc.element)
                {
                case ElementType::Fire:  residueName += "Fire";  break;
                case ElementType::Water: residueName += "Water"; break;
                case ElementType::Wind:  residueName += "Wind";  break;
                case ElementType::Earth: residueName += "Earth"; break;
                default:                 residueName += "Fire";  break;
                }
                pVFX->Spawn(residueName, groundPos, dir, 0u, false);
            }
        }
    }

    // L5 (b) — micro hit-stop (Release 의 큰 stop 위에 살짝 더)
    if (m_desc.impactHitStopSeconds > 0.0f)
        HitStopSystem::Get().Request(m_desc.impactHitStopSeconds);

    // L5 (c) — 짧고 강한 추가 셰이크 (Release 셰이크에 누적)
    if (m_pScene)
    {
        if (CCamera* pCam = m_pScene->GetCamera())
        {
            if (m_desc.impactShakeIntensity > 0.0f && m_desc.impactShakeDuration > 0.0f)
                pCam->StartShake(m_desc.impactShakeIntensity, m_desc.impactShakeDuration);
        }
    }

    // L5 (d) — 작은 원소색 펄스 (Release 의 흰 플래시 위에 살짝 색 입힘)
    if (auto* pApp = Dx12App::GetInstance())
    {
        if (auto* pFlash = pApp->GetWhiteFlash())
        {
            if (m_desc.impactFlashAlpha > 0.0f && m_desc.impactFlashFade > 0.0f)
                pFlash->RequestFlash(m_desc.impactFlashAlpha,
                                     m_desc.impactFlashFade,
                                     ImpactFlashColor(m_desc.element));
        }

        if (auto* pAudio = pApp->GetAudio())
            pAudio->PlaySFX(L"boss_slash_impact", 1.0f);
    }
}

void SlashCue::End()
{
    if (m_nChargeVFXSlot >= 0 && m_pScene)
    {
        if (auto* pVFX = m_pScene->GetVFXManager())
            pVFX->Stop(m_nChargeVFXSlot);
    }
    m_nChargeVFXSlot = -1;
    m_pScene    = nullptr;
    m_bActive   = false;
    m_bReleased = false;
}
