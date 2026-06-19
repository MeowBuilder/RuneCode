#include "stdafx.h"
#include "EnemySpawner.h"
#include "GameObject.h"
#include "TransformComponent.h"
#include "EnemyComponent.h"
#include "ColliderComponent.h"
#include "RenderComponent.h"
#include "AnimationComponent.h"
#include "CollisionLayer.h"
#include "MeleeAttackBehavior.h"
#include "RushAoEAttackBehavior.h"
#include "SpinDashAttackBehavior.h"
#include "FixatedChargeAttackBehavior.h"
#include "RushFrontAttackBehavior.h"
#include "RangedAttackBehavior.h"
#include "BreathAttackBehavior.h"
#include "FlyingBarrageAttackBehavior.h"
#include "FlyingStrafeAttackBehavior.h"
#include "DiveBombAttackBehavior.h"
#include "FlyingCircleAttackBehavior.h"
#include "FlyingSweepAttackBehavior.h"
#include "TailSweepAttackBehavior.h"
#include "SideSmashAttackBehavior.h"
#include "JumpSlamAttackBehavior.h"
#include "ComboAttackBehavior.h"
#include "DarkLordSigilSlash.h"
#include "DarkLordSigilField.h"
#include "DarkLordSwordRain.h"
#include "DarkLordSwordSeal.h"
#include "MegaBreathAttackBehavior.h"
#include "RockFallAttackBehavior.h"
#include "RockBarrageAttackBehavior.h"
#include "GroundRuptureAttackBehavior.h"
#include "SequentialCrossAttackBehavior.h"
#include "TornadoFieldAttackBehavior.h"
#include "GaleSlashAttackBehavior.h"
#include "ShockwaveRingAttackBehavior.h"
#include "BossPhaseConfig.h"
#include "BossPhaseController.h"
#include "Room.h"
#include "Scene.h"
#include "ProjectileManager.h"
#include "Shader.h"
#include "Mesh.h"
#include "MeshLoader.h"
#include <fstream>

// ─── DarkLord 검기 ComboHit 헬퍼 ─────────────────────────────────────────────
//   각 페이즈 Primary 람다가 rand() 로 variant 를 골라 사용. 한 페이즈에 여러 모션 ↑.
//   원소별 EffectDef 이름은 element switch 로 분기.
namespace
{
    using CHit = ComboAttackBehavior::ComboHit;
    using EShape = ComboAttackBehavior::SwordEnergyShape;

    const char* BossSigilName(ElementType e, bool heavy)
    {
        switch (e)
        {
        case ElementType::Fire:  return heavy ? "Boss_CrescentSigil_Fire_Heavy"  : "Boss_CrescentSigil_Fire";
        case ElementType::Water: return heavy ? "Boss_CrescentSigil_Water_Heavy" : "Boss_CrescentSigil_Water";
        case ElementType::Wind:  return heavy ? "Boss_CrescentSigil_Wind_Heavy"  : "Boss_CrescentSigil_Wind";
        case ElementType::Earth: return heavy ? "Boss_CrescentSigil_Earth_Heavy" : "Boss_CrescentSigil_Earth";
        default: return "Boss_CrescentSigil_Fire";
        }
    }

    // 공통 ComboHit 기본값. 호출자가 dmg/animation/eShape/strVFXOnHit 만 set.
    //   [튜닝] 전반적 크기 ↑ — 검기 시각 + cone 사거리 약 10% up. 피격 체감 개선용.
    CHit BaseHit()
    {
        CHit h;
        h.fWindupTime       = 0.55f;
        h.fHitTime          = 0.22f;
        h.fRecoveryTime     = 0.35f;
        h.fHitRange         = 15.0f;   // 11.5 → 15 (보스 스케일 14 비례)
        h.fConeAngle        = 115.0f;
        h.fVFXForwardOffset = 4.0f;    // 3 → 4
        h.fVFXYOffset       = 11.0f;   // 8 → 11 (스케일 ↑ → 검 위치 더 높이)
        h.fVFXScale         = 6.5f;    // 5 → 6.5
        h.strVFXImpact      = "";
        return h;
    }

    // ── 4가지 모션 variant — 페이즈가 원소 + heavy 만 결정해서 호출 ──────────

    // V1: 빠른 잽 (Slim, attack1)
    CHit MakeQuickJab(ElementType e)
    {
        CHit h = BaseHit();
        h.fDamage       = 70.0f;
        h.fWindupTime   = 0.40f;
        h.fHitTime      = 0.18f;
        h.fRecoveryTime = 0.28f;
        h.fHitRange     = 14.0f;   // 11 → 14
        h.fConeAngle    = 95.0f;
        h.strAnimation  = "attack1";
        h.strVFXOnHit   = BossSigilName(e, false);
        h.eShape        = EShape::Slim;
        return h;
    }

    // V2: 옆 베기 (Wide, attack2)
    CHit MakeSideCleave(ElementType e)
    {
        CHit h = BaseHit();
        h.fDamage       = 80.0f;
        h.fWindupTime   = 0.50f;
        h.fHitTime      = 0.20f;
        h.fHitRange     = 15.0f;   // 11.5 → 15
        h.fConeAngle    = 125.0f;
        h.strAnimation  = "attack2";
        h.strVFXOnHit   = BossSigilName(e, false);
        h.fVFXScale     = 7.5f;    // 6 → 7.5
        h.eShape        = EShape::Wide;
        return h;
    }

    // V3: 멀리 뻗는 베기 (Long, attack4)
    CHit MakeLongReach(ElementType e)
    {
        CHit h = BaseHit();
        h.fDamage       = 85.0f;
        h.fWindupTime   = 0.45f;
        h.fHitTime      = 0.20f;
        h.fHitRange     = 17.0f;   // 13 → 17 (가장 멀리)
        h.fConeAngle    = 85.0f;
        h.strAnimation  = "attack4";
        h.strVFXOnHit   = BossSigilName(e, false);
        h.fVFXForwardOffset = 4.5f;   // 3.5 → 4.5
        h.eShape        = EShape::Long;
        return h;
    }

    // V4: 묵직 일격 (Wide, Attack6, Heavy)
    CHit MakeHeavySlam(ElementType e)
    {
        CHit h = BaseHit();
        h.fDamage       = 100.0f;
        h.fWindupTime   = 0.75f;
        h.fHitTime      = 0.30f;
        h.fRecoveryTime = 0.50f;
        h.fHitRange     = 17.0f;   // 13 → 17
        h.fConeAngle    = 130.0f;
        h.strAnimation  = "Attack6";
        h.strVFXOnHit   = BossSigilName(e, true);   // Heavy
        h.fVFXScale     = 10.0f;   // 8 → 10
        h.eShape        = EShape::Wide;
        return h;
    }

    // V5: 회전 베기 (Cross/Double 다중 호 — 회전 모션)
    CHit MakeSpinCleave(ElementType e)
    {
        CHit h = BaseHit();
        h.fDamage       = 95.0f;
        h.fWindupTime   = 0.55f;
        h.fHitTime      = 0.28f;
        h.fHitRange     = 16.5f;   // 12.5 → 16.5
        h.fConeAngle    = 250.0f;
        h.strAnimation  = "attack9";
        h.strVFXOnHit   = BossSigilName(e, true);
        h.fVFXScale     = 9.0f;    // 7 → 9
        h.eShape        = EShape::Double;
        return h;
    }

    // V6: 채찍 호 — Light Presentation 전용. 검 끝 ribbon 이 길고 굵게 남아 채찍
    //     처럼 보이도록 fHitTime / Recovery 길게, fVFXScale 크게, eShape Long.
    //     emitter 본체는 Presentation 이 skip 처리 — ribbon 만 화면에 남음.
    CHit MakeWhipTrail(ElementType e)
    {
        CHit h = BaseHit();
        h.fDamage       = 65.0f;
        h.fWindupTime   = 0.40f;
        h.fHitTime      = 0.30f;     // 길게 → ribbon emission 길어짐
        h.fRecoveryTime = 0.55f;     // 길게 → ribbon 잔존 길어짐
        h.fHitRange     = 17.0f;     // 13 → 17
        h.fConeAngle    = 100.0f;
        h.strAnimation  = "attack2"; // 넓은 sweep 모션
        h.strVFXOnHit   = BossSigilName(e, false);   // ribbon 색 매핑용 (emitter spawn 안 됨)
        h.fVFXScale     = 11.0f;     // 8.5 → 11 (ribbon 두께 ↑↑)
        h.eShape        = EShape::Long;
        return h;
    }
}

EnemySpawner::EnemySpawner()
{
}

EnemySpawner::~EnemySpawner()
{
}

void EnemySpawner::Init(ID3D12Device* pDevice, ID3D12GraphicsCommandList* pCommandList, Scene* pScene, Shader* pShader)
{
    m_pDevice = pDevice;
    m_pCommandList = pCommandList;
    m_pScene = pScene;
    m_pShader = pShader;

    // Register default test enemy preset
    EnemySpawnData testEnemy;
    testEnemy.m_xmf3Scale = XMFLOAT3(1.0f, 2.0f, 1.0f);  // Human-like proportions
    // [카테고리: 근접] 큐브 적 → 강한 주황 (텍스처 없으므로 그대로 표현됨)
    testEnemy.m_xmf4Color = XMFLOAT4(1.0f, 0.55f, 0.20f, 1.0f);
    testEnemy.m_Stats.m_fMaxHP = 50.0f;
    testEnemy.m_Stats.m_fCurrentHP = 50.0f;
    testEnemy.m_Stats.m_fMoveSpeed = 4.0f;
    testEnemy.m_Stats.m_fAttackRange = 3.0f;
    testEnemy.m_Stats.m_fAttackCooldown = 1.5f;
    testEnemy.m_IndicatorConfig.m_eType = IndicatorType::Circle;
    testEnemy.m_IndicatorConfig.m_fHitRadius = 3.0f;
    testEnemy.m_fnCreateAttack = []() {
        return std::make_unique<MeleeAttackBehavior>(10.0f, 0.3f, 0.2f, 0.3f);
    };

    RegisterEnemyPreset("TestEnemy", testEnemy);

    // Shared animation config for Elemental enemies
    EnemyAnimationConfig elementalAnim;
    elementalAnim.m_strIdleClip    = "idle";
    elementalAnim.m_strChaseClip   = "Run_Forward";
    elementalAnim.m_strAttackClip  = "Combat_Unarmed_Attack";
    elementalAnim.m_strStaggerClip = "Combat_Stun";
    elementalAnim.m_strDeathClip   = "Death";

    // Register AirElemental preset (Melee - light blue air)
    EnemySpawnData airElemental;
    airElemental.m_strMeshPath      = "Assets/Enemies/Elementals/AirElemental_Bl/AirElemental_Bl.bin";
    airElemental.m_strAnimationPath = "Assets/Enemies/Elementals/AirElemental_Bl/AirElemental_Bl_Anim.bin";
    airElemental.m_strTexturePath   = "Assets/Enemies/Elementals/AirElemental_Bl/Textures/T_AirElemental_Body_Bl_D.png";
    airElemental.m_xmf3Scale = XMFLOAT3(5.5f, 5.5f, 5.5f);
    // [카테고리: 근접] MapLoader 와 동일 카테고리 색 통일
    airElemental.m_xmf4Color = XMFLOAT4(1.00f, 0.55f, 0.20f, 1.0f);

    airElemental.m_Stats.m_fMaxHP          = 80.0f;
    airElemental.m_Stats.m_fCurrentHP      = 80.0f;
    airElemental.m_Stats.m_fMoveSpeed      = 5.0f;
    airElemental.m_Stats.m_fAttackRange    = 4.0f;
    airElemental.m_Stats.m_fAttackCooldown = 2.0f;

    airElemental.m_AnimConfig = elementalAnim;

    airElemental.m_IndicatorConfig.m_eType      = IndicatorType::Circle;
    airElemental.m_IndicatorConfig.m_fHitRadius = 4.0f;
    airElemental.m_fnCreateAttack = []() {
        return std::make_unique<MeleeAttackBehavior>(15.0f, 0.4f, 0.2f, 0.4f);
    };

    RegisterEnemyPreset("AirElemental", airElemental);

    // Register RushAoEEnemy preset (Rush + 360 AoE - FireGolem_Rd)
    EnemySpawnData rushAoE;
    rushAoE.m_strMeshPath      = "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd.bin";
    rushAoE.m_strAnimationPath = "Assets/Enemies/Elementals/FireGolem_Rd/FireGolem_Rd_Anim.bin";
    rushAoE.m_strTexturePath   = "Assets/Enemies/Elementals/FireGolem_Rd/Textures/T_FireGolem_Rd_D.png";
    rushAoE.m_xmf3Scale = XMFLOAT3(5.5f, 5.5f, 5.5f);
    // [카테고리: 돌진] MapLoader 와 동일 카테고리 색 통일 (빨강)
    rushAoE.m_xmf4Color = XMFLOAT4(1.00f, 0.35f, 0.30f, 1.0f);

    rushAoE.m_Stats.m_fMaxHP          = 100.0f;
    rushAoE.m_Stats.m_fCurrentHP      = 100.0f;
    rushAoE.m_Stats.m_fMoveSpeed      = 5.0f;
    rushAoE.m_Stats.m_fAttackRange    = 20.0f;
    rushAoE.m_Stats.m_fAttackCooldown = 3.0f;

    rushAoE.m_AnimConfig = elementalAnim;

    // rushSpeed=15, rushDuration=1.2 → rushDistance=18
    rushAoE.m_IndicatorConfig.m_eType         = IndicatorType::RushCircle;
    rushAoE.m_IndicatorConfig.m_fRushDistance = 18.0f;
    rushAoE.m_IndicatorConfig.m_fHitRadius    = 5.0f;
    rushAoE.m_fnCreateAttack = []() {
        return std::make_unique<RushAoEAttackBehavior>(15.0f, 15.0f, 1.2f, 0.3f, 0.2f, 0.3f, 5.0f);
    };

    RegisterEnemyPreset("RushAoEEnemy", rushAoE);

    // Register RushFrontEnemy preset (Rush + cone - EarthElemental_Gn)
    EnemySpawnData rushFront;
    rushFront.m_strMeshPath      = "Assets/Enemies/Elementals/EarthElemental_Gn/EarthElemental_Gn.bin";
    rushFront.m_strAnimationPath = "Assets/Enemies/Elementals/EarthElemental_Gn/EarthElemental_Gn_Anim.bin";
    rushFront.m_strTexturePath   = "Assets/Enemies/Elementals/EarthElemental_Gn/Textures/T_EarthElemental_Gn_D.png";
    rushFront.m_xmf3Scale = XMFLOAT3(5.5f, 5.5f, 5.5f);
    // [카테고리: 돌진] MapLoader 와 동일 카테고리 색 통일 (빨강)
    rushFront.m_xmf4Color = XMFLOAT4(1.00f, 0.35f, 0.30f, 1.0f);

    rushFront.m_Stats.m_fMaxHP          = 80.0f;
    rushFront.m_Stats.m_fCurrentHP      = 80.0f;
    rushFront.m_Stats.m_fMoveSpeed      = 5.0f;
    rushFront.m_Stats.m_fAttackRange    = 18.0f;
    rushFront.m_Stats.m_fAttackCooldown = 2.5f;

    rushFront.m_AnimConfig = elementalAnim;

    // rushSpeed=18, rushDuration=1.0 → rushDistance=18
    rushFront.m_IndicatorConfig.m_eType         = IndicatorType::RushCone;
    rushFront.m_IndicatorConfig.m_fRushDistance = 18.0f;
    rushFront.m_IndicatorConfig.m_fHitRadius    = 4.0f;
    rushFront.m_IndicatorConfig.m_fConeAngle    = 90.0f;
    rushFront.m_fnCreateAttack = []() {
        return std::make_unique<RushFrontAttackBehavior>(20.0f, 18.0f, 1.0f, 0.2f, 0.2f, 0.3f, 4.0f, 90.0f);
    };

    RegisterEnemyPreset("RushFrontEnemy", rushFront);

    // Register RangedEnemy preset (Projectile - StormElemental_Bl)
    EnemySpawnData ranged;
    ranged.m_strMeshPath      = "Assets/Enemies/Elementals/StormElemental_Bl/StormElemental_Bl.bin";
    ranged.m_strAnimationPath = "Assets/Enemies/Elementals/StormElemental_Bl/StormElemental_Bl_Anim.bin";
    ranged.m_strTexturePath   = "Assets/Enemies/Elementals/StormElemental_Bl/Textures/T_StormElemental_Bl_D.png";
    ranged.m_xmf3Scale = XMFLOAT3(5.5f, 5.5f, 5.5f);
    // [카테고리: 원거리] MapLoader 와 동일 카테고리 색 통일 (청록)
    ranged.m_xmf4Color = XMFLOAT4(0.30f, 0.95f, 0.85f, 1.0f);

    ranged.m_Stats.m_fMaxHP          = 60.0f;
    ranged.m_Stats.m_fCurrentHP      = 60.0f;
    ranged.m_Stats.m_fMoveSpeed      = 3.0f;
    ranged.m_Stats.m_fAttackRange    = 30.0f;
    ranged.m_Stats.m_fAttackCooldown = 2.0f;

    ranged.m_AnimConfig = elementalAnim;

    ProjectileManager* pProjMgr = pScene->GetProjectileManager();
    ranged.m_fnCreateAttack = [pProjMgr]() {
        return std::make_unique<RangedAttackBehavior>(pProjMgr, 10.0f, 20.0f, 0.5f, 0.1f, 0.5f);
    };

    RegisterEnemyPreset("RangedEnemy", ranged);

    // Register Dragon Boss preset (Flying)
    EnemySpawnData dragon;
    dragon.m_strMeshPath = "Assets/Enemies/Dragon/Red.bin";
    dragon.m_strAnimationPath = "Assets/Enemies/Dragon/Red_Anim.bin";
    dragon.m_strTexturePath = "Assets/Enemies/Dragon/Textures/RedHP.png";
    dragon.m_xmf3Scale = XMFLOAT3(3.0f, 3.0f, 3.0f);  // 원본값 복원 (충돌/판정/브레스 범위 연동 때문에 보스는 스케일 유지)
    // [카테고리: 보스] 화염 컨셉 — 빨강 강조 (강도 완화: 기존 (1,0.3,0.1)은 텍스처와 곱해져 너무 어두워짐)
    dragon.m_xmf4Color = XMFLOAT4(1.0f, 0.50f, 0.30f, 1.0f);
    dragon.m_Stats.m_fMaxHP = 800.0f;           // HP 대폭 상향
    dragon.m_Stats.m_fCurrentHP = 800.0f;
    dragon.m_Stats.m_fMoveSpeed = 10.0f;        // 이동속도 상향
    dragon.m_Stats.m_fAttackRange = 40.0f;      // 공격 사거리 상향 (브레스 사거리)
    dragon.m_Stats.m_fAttackCooldown = 1.5f;    // 공격 쿨다운 감소
    dragon.m_Stats.m_fLongRangeThreshold = 35.0f;   // 원거리 기준
    dragon.m_Stats.m_fMidRangeThreshold = 18.0f;    // 중거리 기준

    // Flying mode disabled - boss intro handles the entrance
    dragon.m_bIsFlying = false;
    dragon.m_fFlyHeight = 0.0f;

    // Boss settings - immune to stagger, has special attack
    dragon.m_bIsBoss = true;
    dragon.m_fSpecialAttackCooldown = 4.0f;    // 특수 공격 쿨다운 감소
    dragon.m_fFlyingAttackCooldown = 8.0f;     // 비행 공격 쿨다운 감소
    dragon.m_nSpecialAttackChance = 45;        // 특수 공격 확률 증가
    dragon.m_nFlyingAttackChance = 50;         // 비행 공격 확률 증가

    // Ground combat animations
    dragon.m_AnimConfig.m_strIdleClip = "Idle01";
    dragon.m_AnimConfig.m_strChaseClip = "Walk";
    dragon.m_AnimConfig.m_strAttackClip = "Flame Attack";
    dragon.m_AnimConfig.m_strStaggerClip = "Get Hit";
    dragon.m_AnimConfig.m_strDeathClip = "Die";

    dragon.m_IndicatorConfig.m_eType = IndicatorType::Circle;
    dragon.m_IndicatorConfig.m_fHitRadius = 15.0f;

    // Normal attack: Breath attack (reduced projectile count for performance)
    dragon.m_fnCreateAttack = [pProjMgr]() {
        // scale 3.0 (뭉친 클러스터 크기 up)
        return std::make_unique<BreathAttackBehavior>(pProjMgr, 32.0f, 38.0f, 5, 50.0f, 0.4f, 0.8f, 0.3f, 1.0f, 3.0f);
    };

    // Special attack (fallback if no phase controller) — 투사체 수 감소로 렉 완화
    dragon.m_fnCreateSpecialAttack = [pProjMgr]() {
        // 12→7 per wave, 4→3 waves, damage 26→35 보상
        return std::make_unique<FlyingBarrageAttackBehavior>(
            pProjMgr, 35.0f, 18.0f, 7, 3, 0.4f, 16.0f, 0.8f, 0.8f);
    };

    // Boss Phase Configuration - 3 phases with varied attack patterns
    dragon.m_fnCreateBossPhaseConfig = [pProjMgr]() {
        auto pConfig = std::make_unique<BossPhaseConfig>();

        // ============ Phase 1 (100% - 70% HP): Ground Combat ============
        BossPhaseData phase1;
        phase1.m_fHealthThreshold = 1.0f;
        phase1.m_fSpeedMultiplier = 1.0f;
        phase1.m_fAttackSpeedMultiplier = 1.0f;
        phase1.m_nSpecialAttackChance = 35;
        phase1.m_bCanFly = false;

        // Primary: Breath attack (reduced projectiles for performance)
        phase1.m_fnPrimaryAttack = [pProjMgr]() {
            return std::make_unique<BreathAttackBehavior>(pProjMgr, 30.0f, 35.0f, 4, 45.0f, 0.4f, 0.8f, 0.3f);
        };

        // Special: 50/50 Jump Slam / Light Combo (Tail Sweep은 "Tail Attack" 애니 부재로 제외)
        phase1.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
            int choice = rand() % 2;
            if (choice == 0) {
                // Jump slam
                return std::make_unique<JumpSlamAttackBehavior>(45.0f, 10.0f, 0.45f, 7.0f, 0.25f, 0.4f, true);
            } else {
                // Light combo (3-hit)
                return std::unique_ptr<IAttackBehavior>(ComboAttackBehavior::CreateLightCombo());
            }
        };

        pConfig->AddPhase(phase1);

        // ============ Phase 2 (70% - 35% HP): Aerial Assault ============
        BossPhaseData phase2;
        phase2.m_fHealthThreshold = 0.7f;
        phase2.m_fSpeedMultiplier = 1.3f;     // 속도 증가
        phase2.m_fAttackSpeedMultiplier = 0.85f;
        phase2.m_nSpecialAttackChance = 45;
        phase2.m_nFlyingAttackChance = 55;    // 비행 공격 확률 증가
        phase2.m_bCanFly = true;

        // Primary: Faster breath (reduced projectiles for performance)
        phase2.m_fnPrimaryAttack = [pProjMgr]() {
            return std::make_unique<BreathAttackBehavior>(pProjMgr, 35.0f, 38.0f, 5, 50.0f, 0.35f, 0.75f, 0.25f);
        };

        // Special: Ground attacks (상향된 데미지)
        phase2.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
            int choice = rand() % 2;
            if (choice == 0) {
                return std::make_unique<JumpSlamAttackBehavior>(50.0f, 12.0f, 0.45f, 8.0f, 0.2f, 0.35f, true);
            } else {
                return std::unique_ptr<IAttackBehavior>(ComboAttackBehavior::CreateHeavyCombo());
            }
        };

        // Flying: Strafe or Circle attack (reduced projectiles for performance)
        phase2.m_fnFlyingAttack = [pProjMgr]() -> std::unique_ptr<IAttackBehavior> {
            int choice = rand() % 2;
            if (choice == 0) {
                // Strafe - 3→2 shots/burst, 데미지 24→30 보상
                return std::make_unique<FlyingStrafeAttackBehavior>(
                    pProjMgr, 30.0f, 22.0f, 18.0f, 22.0f, 0.15f, 2, 12.0f, 0.4f, 0.4f);
            } else {
                // Circle - 3→2 shots/burst, 데미지 22→28 보상
                return std::make_unique<FlyingCircleAttackBehavior>(
                    pProjMgr, 28.0f, 22.0f, 18.0f, 100.0f, 280.0f, 0.18f, 2, 12.0f, 0.4f, 0.4f);
            }
        };

        // Phase 2 transition: Mega Breath attack
        phase2.m_fnTransitionAttack = []() {
            return std::make_unique<MegaBreathAttackBehavior>(
                15.0f,  // 틱당 데미지
                0.2f,   // 틱 간격
                20.0f,  // 이동 속도
                3.0f,   // 벽 이동 시간
                5.5f,   // 준비 시간 — 입 집결 VFX 충분히 + 포지셔닝 여유
                6.5f,   // 브레스 지속
                1.2f,   // 회복 시간 — 카메라 복귀 블렌드 여유
                3.0f    // 엄폐물 크기
            );
        };
        phase2.m_bHasTransitionAttack = true;
        phase2.m_bInvincibleDuringTransition = true;
        phase2.m_fTransitionDuration = 0.0f;  // MegaBreath handles its own timing

        pConfig->AddPhase(phase2);

        // ============ Phase 3 (35% - 0% HP): Fury Mode ============
        BossPhaseData phase3;
        phase3.m_fHealthThreshold = 0.35f;
        phase3.m_fSpeedMultiplier = 1.6f;     // 더 빠르게
        phase3.m_fAttackSpeedMultiplier = 0.7f;
        phase3.m_nSpecialAttackChance = 55;
        phase3.m_nFlyingAttackChance = 65;    // 비행 공격 확률 대폭 증가
        phase3.m_bCanFly = true;

        // Primary: Rapid breath (reduced projectiles for performance)
        phase3.m_fnPrimaryAttack = [pProjMgr]() {
            return std::make_unique<BreathAttackBehavior>(pProjMgr, 42.0f, 42.0f, 6, 55.0f, 0.25f, 0.65f, 0.2f);
        };

        // Special: Fury combo or double jump slam (상향된 데미지)
        phase3.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
            int choice = rand() % 2;
            if (choice == 0) {
                return std::unique_ptr<IAttackBehavior>(ComboAttackBehavior::CreateFuryCombo());
            } else {
                return std::make_unique<JumpSlamAttackBehavior>(60.0f, 14.0f, 0.4f, 9.0f, 0.18f, 0.3f, true);
            }
        };

        // Flying: All aerial attacks available (reduced projectiles for performance)
        phase3.m_fnFlyingAttack = [pProjMgr]() -> std::unique_ptr<IAttackBehavior> {
            int choice = rand() % 4;
            switch (choice) {
            case 0:
                // Dive bomb - 4→3 shots/burst, 데미지 30→38 보상
                return std::make_unique<DiveBombAttackBehavior>(
                    pProjMgr, 38.0f, 32.0f, 36.0f, 40.0f, 7.0f, 3, 0.1f, 18.0f, 0.4f, 0.25f);
            case 1:
                // Sweep - 2 shots 유지 (가장 적음)
                return std::make_unique<FlyingSweepAttackBehavior>(
                    pProjMgr, 28.0f, 28.0f, 18.0f, 28.0f, 100.0f, 200.0f, 0.08f, 2, 10.0f, 0.35f, 0.35f);
            case 2:
                // Barrage - 10→6 per wave, 4→3 waves (렉 주범), 데미지 28→42 보상
                return std::make_unique<FlyingBarrageAttackBehavior>(
                    pProjMgr, 42.0f, 20.0f, 6, 3, 0.35f, 16.0f, 0.5f, 0.5f);
            default:
                // Fast strafe - 3→2 shots/burst, 데미지 26→33 보상
                return std::make_unique<FlyingStrafeAttackBehavior>(
                    pProjMgr, 33.0f, 26.0f, 22.0f, 25.0f, 0.12f, 2, 12.0f, 0.35f, 0.35f);
            }
        };

        // Phase 3 transition: Stronger Mega Breath attack
        phase3.m_fnTransitionAttack = []() {
            return std::make_unique<MegaBreathAttackBehavior>(
                25.0f,  // 틱당 데미지 (더 강함)
                0.15f,  // 틱 간격 (더 빠름)
                25.0f,  // 이동 속도
                2.5f,   // 벽 이동 시간
                4.8f,   // 준비 시간
                7.5f,   // 브레스 지속
                1.0f,   // 회복 시간 (카메라 복귀 블렌드)
                3.5f    // 엄폐물 크기
            );
        };
        phase3.m_bHasTransitionAttack = true;
        phase3.m_bInvincibleDuringTransition = true;
        phase3.m_fTransitionDuration = 0.0f;  // MegaBreath handles its own timing

        pConfig->AddPhase(phase3);

        return pConfig;
    };

    RegisterEnemyPreset("Dragon", dragon);

    // Register Kraken Boss preset (Water stage boss — 느릿 + 넓은 범위 + 4패턴)
    EnemySpawnData kraken;
    kraken.m_strMeshPath      = "Assets/Enemies/Kraken/KRAKEN.bin";
    kraken.m_strAnimationPath = "Assets/Enemies/Kraken/KRAKEN_Anim.bin";
    kraken.m_strTexturePath   = "Assets/Enemies/Kraken/Textures/Tex_KRAKEN_BODY_BaseColor.png";
    kraken.m_xmf3Scale = XMFLOAT3(3.0f, 3.0f, 3.0f);  // 원본값 복원
    // [카테고리: 보스] 심해 컨셉 — 보라 강화
    kraken.m_xmf4Color = XMFLOAT4(0.55f, 0.35f, 1.00f, 1.0f);
    kraken.m_fColliderXZMultiplier = 0.8f;   // 거대 몸체에 맞춰 XZ 피격 반경 확대

    kraken.m_Stats.m_fMaxHP              = 1000.0f;
    kraken.m_Stats.m_fCurrentHP          = 1000.0f;
    kraken.m_Stats.m_fMoveSpeed          = 5.0f;
    kraken.m_Stats.m_fAttackRange        = 30.0f;   // Breath 가 기본이라 사정거리 확보
    kraken.m_Stats.m_fAttackCooldown     = 1.6f;    // 빠른 견제 발사 텀
    kraken.m_Stats.m_fLongRangeThreshold = 40.0f;
    kraken.m_Stats.m_fMidRangeThreshold  = 18.0f;

    kraken.m_bIsBoss = true;
    kraken.m_fSpecialAttackCooldown = 4.5f;
    kraken.m_nSpecialAttackChance   = 70;     // 쿨 끝나면 70% 확률 특수기 (나머지는 Breath 지속)
    // 사각형 판정이 보스 중심(z=0)부터 전방으로 뻗어나가므로 offset 0 으로
    kraken.m_fAttackOriginForwardOffset = 0.0f;

    kraken.m_AnimConfig.m_strIdleClip    = "Idle";
    kraken.m_AnimConfig.m_strChaseClip   = "Walk";
    kraken.m_AnimConfig.m_strAttackClip  = "Attack_Forward_RM";   // 기본 = 잉크 발사 애니
    kraken.m_AnimConfig.m_strStaggerClip = "Hit";
    kraken.m_AnimConfig.m_strDeathClip   = "Death";

    // 전방 직사각형 인디케이터 — 촉수가 앞으로 휘두르는 과장된 범위
    kraken.m_IndicatorConfig.m_eType      = IndicatorType::ForwardBox;
    kraken.m_IndicatorConfig.m_fHitRadius = 14.0f;   // 반폭 (총 너비 28u)
    kraken.m_IndicatorConfig.m_fHitLength = 30.0f;   // 전방 30u — 과장된 촉수 휩쓸기 범위

    // ── 기본 공격 = 작은 잉크 투사체 다수 지속 발사 (견제기 역할) ────────────────
    //   · projectileCount 10 (많이)
    //   · projectileScale 1.0 (기본 작게) — 변주 시 0.55~1.9 배로 다양화
    //   · bVariedProjectiles=true 로 크기/속도/각도/데미지/발사 위치 모두 랜덤 변주
    kraken.m_fnCreateAttack = [pProjMgr]() {
        return std::make_unique<BreathAttackBehavior>(
            pProjMgr,
            7.0f,     // dmgPerHit (평균 — ±30% 변주)
            34.0f,    // projectileSpeed (평균 — 0.75~1.45 배)
            10,       // projectileCount (많이)
            55.0f,    // spreadAngle (넓게 뿌림)
            0.4f,     // windup (빠름)
            1.1f,     // breath duration
            0.2f,     // recovery
            0.6f,     // projectileRadius (평균)
            1.0f,     // projectileScale 기본 — 변주로 0.55~1.9 배
            ElementType::Water,
            "Attack_Forward_RM",
            true);    // ★ varied projectiles: 크기/속도/각/발사 위치 랜덤
    };

    // ── 특수기 팩토리: 4종 랜덤 (TailSweep / HeavyCombo / SideSmash / 360 탄막) ───
    kraken.m_fnCreateSpecialAttack = [pProjMgr]() -> std::unique_ptr<IAttackBehavior> {
        int roll = rand() % 100;
        if (roll < 35)
        {
            // 광역 휩쓸기 — 앞쪽 사각형 (14×30 확장)
            return std::make_unique<TailSweepAttackBehavior>(
                55.0f,   // dmg
                0.8f,    // windup
                0.5f,    // sweep duration
                0.7f,    // recovery
                14.0f,   // hitRange (미사용 — rect 모드)
                180.0f,  // sweepArc (미사용)
                false,
                "Sweep_Attack",
                14.0f,   // rectWidthHalf — 반폭 14 (총 28u)
                30.0f);  // rectLength — 전방 30u
        }
        else if (roll < 60)
        {
            // 3연타 필살 콤보 — 사각형 판정 (14×30)
            std::vector<ComboAttackBehavior::ComboHit> hits;
            ComboAttackBehavior::ComboHit h;
            h.strAnimation = "Sweep_Smash_Attack_3_HIt_Combo";
            h.fHitRange      = 14.0f;
            h.fConeAngle     = 160.0f;
            h.fRectWidthHalf = 14.0f;
            h.fRectLength    = 30.0f;
            h.bTrackTarget = true;
            h.fDamage = 30.0f; h.fWindupTime = 0.7f; h.fHitTime = 0.2f; h.fRecoveryTime = 0.6f;
            hits.push_back(h);
            h.bTrackTarget = false;
            h.fDamage = 55.0f; h.fWindupTime = 0.8f; h.fHitTime = 0.2f; h.fRecoveryTime = 0.5f;
            hits.push_back(h);
            h.fDamage = 85.0f; h.fWindupTime = 0.9f; h.fHitTime = 0.3f; h.fRecoveryTime = 0.6f;
            hits.push_back(h);
            return std::make_unique<ComboAttackBehavior>(hits);
        }
        else if (roll < 80)
        {
            // 측면 사이드스매시 — 플레이어가 있는 쪽으로 45° 틀며 내려찍기
            //   기존 3패턴이 전부 정면 rect 였어서 사이드스텝 한 방에 무력화되던 문제 해결.
            //   windup 1.2s 로 텔레그래프 충분히 주되, 회피 방향 예측을 강제한다.
            return std::make_unique<SideSmashAttackBehavior>(
                60.0f,   // damage
                45.0f,   // tilt angle
                12.0f,   // rect half-width (24u 총폭 — sweep 보다 약간 좁음)
                28.0f,   // rect length
                1.2f,    // windup
                0.3f,    // slam
                0.8f,    // recovery
                0.25f,   // camera shake intensity
                0.35f,   // camera shake duration
                0.0f);   // anim playback speed (기본)
        }
        else
        {
            // 360° 탄막 — 뒤에 숨은 플레이어 견제 + 혼란스러운 다양한 투사체
            //   spread 360°로 전 방향 스프레이, count 많음, 변주 활성
            //   지면 AoE 가 아니니 인디케이터 억제됨 (false 전달)
            auto pBehavior = std::make_unique<BreathAttackBehavior>(
                pProjMgr,
                10.0f,    // dmg/hit
                28.0f,    // speed (평균)
                16,       // count (많음)
                360.0f,   // spread — 전 방향
                0.9f,     // windup (좀 더 길게 — 몸을 웅크리는 느낌)
                1.6f,     // duration (계속 뿜음)
                0.4f,     // recovery
                0.8f,     // radius
                1.1f,     // scale 기본
                ElementType::Water,
                "Unreal Take",  // 포효 동시 분사 — 몸을 벌리며 뿜는 느낌
                true);    // varied
            return pBehavior;
        }
    };

    RegisterEnemyPreset("Kraken", kraken);

    // Register Golem Boss preset (Earth stage boss — 완전 고정형 거대 석상)
    //   이동/회전 전부 봉쇄. 방사형 광역 패턴으로만 전장 통제.
    //   한 방 한 방이 묵직·느림·강함. 애니 재생속도까지 낮춰 "무거운 석상" 체감 확보.
    EnemySpawnData golem;
    golem.m_strMeshPath      = "Assets/Enemies/Golem/Golem01_Generic_prefab.bin";
    golem.m_strAnimationPath = "Assets/Enemies/Golem/Golem01_Generic_prefab_Anim.bin";
    golem.m_strTexturePath   = "Assets/Enemies/Golem/Textures/chr_04_Golem_alb.png";
    golem.m_xmf3Scale = XMFLOAT3(14.0f, 14.0f, 14.0f);  // 원본값 복원 (17은 잔상/떨림 문제)
    // [카테고리: 보스] 대지 컨셉 — 골드 강화
    golem.m_xmf4Color = XMFLOAT4(1.00f, 0.75f, 0.30f, 1.0f);

    golem.m_Stats.m_fMaxHP              = 2500.0f;
    golem.m_Stats.m_fCurrentHP          = 2500.0f;
    golem.m_Stats.m_fMoveSpeed          = 0.0f;
    golem.m_Stats.m_fAttackRange        = 9999.0f;
    golem.m_Stats.m_fAttackCooldown     = 4.5f;
    golem.m_Stats.m_fLongRangeThreshold = 0.0f;
    golem.m_Stats.m_fMidRangeThreshold  = 0.0f;

    golem.m_bIsBoss        = true;
    golem.m_bStationary    = true;
    golem.m_fRotationSpeed = 0.0f;
    golem.m_fAnimationPlaybackSpeed = 0.7f;
    golem.m_fColliderXZMultiplier = 0.45f;

    //  Special 쿨타임 제거 (0). 매 공격마다 특수 롤 시도 → 특수가 훨씬 자주 나옴
    golem.m_fSpecialAttackCooldown  = 0.0f;
    golem.m_nSpecialAttackChance    = 75;   // 쿨 제거 대신 확률로만 제어 (75%)

    // 애니메이션 매핑 — 석상 컨셉: 걷기 X, 서 있는 idle 위주
    //   ★ attack01 = 실제 "주먹 내려찍기" 모션, attack02 = 실제 "팔 휘두르기" 모션
    //   (클립 이름은 번호로만 구분되고 실제 모션 의미는 이렇게 확인됨)
    golem.m_AnimConfig.m_strIdleClip    = "Golem_stand_ge";
    golem.m_AnimConfig.m_strChaseClip   = "Golem_battle_stand_ge";
    golem.m_AnimConfig.m_strAttackClip  = "Golem_battle_attack01_ge";   // fallback (내려찍기)
    golem.m_AnimConfig.m_strStaggerClip = "Golem_battle_harddamage_ge";
    golem.m_AnimConfig.m_strDeathClip   = "Golem_battle_die_ge";

    // Loop 설정 — attack freeze 방지 (behavior > anim 시 얼어붙음 회피)
    golem.m_AnimConfig.m_bLoopAttack  = true;
    golem.m_AnimConfig.m_bLoopStagger = true;

    // 바닥 인디케이터 — Circle 타입 (각 behavior 가 GetIndicatorRadius() 로 실제 반경 제공)
    //   preset 의 m_fHitRadius 는 fallback 기본값 (override 없을 때만 사용)
    golem.m_IndicatorConfig.m_eType      = IndicatorType::Circle;
    golem.m_IndicatorConfig.m_fHitRadius = 42.0f;

    // Primary: "주먹 내려찍기" — attack01 @ 0.7× = 8.1s
    //   windup 을 애니 slam 피크 (~45% = 3.6s) 에 맞춤 → 찍는 순간에 데미지/파편 동기화
    //   3.5 → 3.8: 사용자 체감상 데미지가 애니보다 빨라 windup 뒤로 살짝 이동
    golem.m_fnCreateAttack = []() {
        return std::make_unique<JumpSlamAttackBehavior>(
            160.0f, 0.0f, 0.25f, 70.0f,
            3.8f, 1.3f,                     // windup 3.5→3.8
            false,
            2.5f, 0.5f,
            "Golem_battle_attack01_ge"
        );
    };
    // Special: 6개 균등 (각 ~16.7%)
    //   0:점프 진동, 1:광역 내려찍기, 2:바위 발사, 3:바위 낙하, 4:십자 균열, 5:순차 십자 폭발
    //   앞으로 패턴 추가 시에도 균등 분배 유지 (rand() % N 으로 확장)
    golem.m_fnCreateSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
        // 직전에 쓴 패턴은 제외하고 재추첨 — 같은 패턴 연속 시전 방지
        static int s_lastIndex = -1;
        int r;
        do { r = rand() % 6; } while (r == s_lastIndex);
        s_lastIndex = r;
        if (r == 0)
        {
            // 점프 진동
            return std::make_unique<JumpSlamAttackBehavior>(
                140.0f, 6.5f, 1.8f, 85.0f,
                1.3f, 0.7f,
                false,
                3.6f, 0.7f,
                "Golem_jump_ge",
                0.5f
            );
        }
        else if (r == 1)
        {
            // 광역 내려찍기 (radius 120) — Primary 와 같은 slam 피크 타이밍
            return std::make_unique<JumpSlamAttackBehavior>(
                150.0f, 0.0f, 0.3f, 120.0f,
                3.6f, 1.8f,                 // windup 2.3→3.6 (피크 타이밍), recovery 2.0→1.8
                false,
                3.4f, 0.65f,
                "Golem_battle_attack01_ge"
            );
        }
        else if (r == 2)
        {
            // 바위 발사 — 애니 1회 재생 후 idle 자동 전환. 총 시간 자유롭게 설정 가능
            return std::make_unique<RockBarrageAttackBehavior>(
                16,      // 바위 개수
                90.0f,   // 데미지
                4.0f,    // 투사체 반경
                44.0f,   // 속도
                22.0f,   // 궤도 반경
                18.0f,   // 보스 위 높이
                2.6f,    // summon
                0.6f,    // charge
                0.16f,   // fire 간격
                3.5f,    // flight timeout
                2.2f,    // recovery — 바위 비행 + 보스 idle 잠시 대기
                0.0f, 0.0f,
                1.1f
            );
        }
        else if (r == 3)
        {
            // 바위 낙하 — 애니 1회 재생 후 idle 자동 전환
            return std::make_unique<RockFallAttackBehavior>(
                14,      // 바위 개수
                90.0f,   // 바위 당 데미지
                14.0f,   // 바위 당 AOE 반경
                25.0f,   // 최소 스폰 반경
                95.0f,   // 최대 스폰 반경
                2.0f,    // windup
                0.8f,    // drop
                2.0f,    // recovery
                2.8f, 0.5f
            );
        }
        else if (r == 4)
        {
            // 십자/X 바닥 균열 — 애니 1회 재생 후 idle 자동 전환
            auto shape = (rand() % 2 == 0)
                ? GroundRuptureAttackBehavior::RuptureShape::Cross
                : GroundRuptureAttackBehavior::RuptureShape::XDiag;
            return std::make_unique<GroundRuptureAttackBehavior>(
                shape,
                100.0f,  // damage
                100.0f,  // 균열 길이
                6.0f,    // 균열 반폭
                2.2f,    // windup
                0.6f,    // impact
                1.8f,    // recovery
                2.8f, 0.5f
            );
        }
        else
        {
            // 순차 십자 폭발 — 보스 중심에 꽉 찬 십자 3개가 0°/30°/60° 로 예약되어
            // 순서대로 터짐. 플레이어는 fill 차오르는 속도 + emissive 밝기로 순서를 읽고
            // 안전 웨지를 선점해야 한다.
            return std::make_unique<SequentialCrossAttackBehavior>(
                55.0f,   // damage per cross
                100.0f,  // 막대 반길이 (전체 200 — 원거리 이탈 불가)
                13.0f,   // 막대 반폭 (30° 웨지 d≈48까지 완전히 덮임)
                2.5f,    // windup
                0.65f,   // 폭발 간격 — 인접 웨지 이동이 "가능은 하지만 근거리에서만"
                0.35f,   // 폭발 flash
                1.4f,    // recovery
                2.4f, 0.45f
            );
        }
    };

    RegisterEnemyPreset("Golem", golem);

    // Register Demon Boss preset (Stage 4 — 빠른 돌진형 보스)
    //   컨셉: 돌진을 회피하고 후딜 사이에 딜을 넣어서 깨는 히트앤런 보스
    //   페이즈1 = 단발 돌진 (짧은/긴 두 변형), 페이즈2 = Rage 후 강화 돌진
    EnemySpawnData demon;
    demon.m_strMeshPath      = "Assets/Enemies/demon/Demon.bin";
    demon.m_strAnimationPath = "Assets/Enemies/demon/Demon_Anim.bin";
    demon.m_strTexturePath   = "Assets/Enemies/demon/Textures/_Albedo.png";
    demon.m_xmf3Scale = XMFLOAT3(8.0f, 8.0f, 8.0f);  // 보스급 위압감
    // [카테고리: 보스] 데몬 컨셉 — 짙은 빨강 (Dragon 보다 어둡고 차가운 톤)
    demon.m_xmf4Color = XMFLOAT4(1.00f, 0.30f, 0.25f, 1.0f);

    demon.m_Stats.m_fMaxHP              = 3500.0f;
    demon.m_Stats.m_fCurrentHP          = 3500.0f;
    // 어그로 대상이 회피에 집중해야 하는 압박감 — 이속 대폭 강화 + 공격 텀 대폭 단축
    demon.m_Stats.m_fMoveSpeed          = 20.0f;    // 14→20, 추격 끈질김
    // 사거리를 짧게 — 안에 들어오면 즉시 공격, 밖이면 계속 추격 (대기 프레임 최소화)
    //   사이즈 업으로 몸체 외곽이 늘어나서 6.0 정도가 "딱 붙는" 거리
    demon.m_Stats.m_fAttackRange        = 6.0f;
    demon.m_Stats.m_fAttackCooldown     = 0.4f;     // 0.6→0.4, 기본 공격 텀 짧음
    demon.m_Stats.m_fLongRangeThreshold = 32.0f;    // 더 멀리서도 압박
    demon.m_Stats.m_fMidRangeThreshold  = 15.0f;

    demon.m_bIsBoss = true;
    demon.m_fSpecialAttackCooldown = 1.2f;          // 1.8→1.2, 특수기 더 자주
    demon.m_nSpecialAttackChance   = 80;            // 75→80
    demon.m_fAnimationPlaybackSpeed = 1.25f;        // 1.15→1.25, 민첩한 인상 강화

    demon.m_AnimConfig.m_strIdleClip    = "Idle1";
    demon.m_AnimConfig.m_strChaseClip   = "Run";
    demon.m_AnimConfig.m_strAttackClip  = "attack3"; // 가장 짧은 1.33s — 빠른 잽
    demon.m_AnimConfig.m_strStaggerClip = "gethit2";
    demon.m_AnimConfig.m_strDeathClip   = "Death1";

    // ForwardBox preset — 모든 데몬 공격이 ForwardBox override 라 일관된 메시 셋업 필요
    //   (Circle preset 으로 두면 ring/disc 만 생성돼서 override 가 시각적으로 안 먹음)
    demon.m_IndicatorConfig.m_eType      = IndicatorType::ForwardBox;
    demon.m_IndicatorConfig.m_fHitRadius = 5.5f;   // 기본 corridor 절반 너비
    demon.m_IndicatorConfig.m_fHitLength = 20.0f;  // 기본 corridor 길이 (behavior 가 override)

    // 기본 공격 — 회전 돌진 (SpinDash). attack3 애니 + 긴 거리 + 넓은 AoE
    //   가까이 붙으면 즉시 발동 → 보스가 멈출 새 없이 돌진/회전으로 들이댐
    demon.m_fnCreateAttack = []() {
        return std::make_unique<SpinDashAttackBehavior>(
            18.0f /*tickDmg*/, 0.22f /*tickInterval*/,
            18.0f /*rushSpeed*/, 1.1f  /*rushDur ~20 unit*/,
            0.25f /*windup*/, 0.55f /*recovery=딜윈도우*/,
            7.0f  /*aoeRadius*/);
    };

    // 특수 공격 fallback (페이즈 컨트롤러 없을 때)
    demon.m_fnCreateSpecialAttack = []() {
        return std::make_unique<RushFrontAttackBehavior>(
            60.0f /*damage*/, 28.0f /*rushSpeed*/, 1.0f /*rushDur ~28 unit*/,
            0.25f /*windup*/, 0.15f /*hit*/, 1.0f /*recovery*/,
            8.0f /*hitRange*/, 80.0f /*coneDeg*/);
    };

    // ===== Boss Phase Configuration — 2 페이즈 (Rage 전환) =====
    demon.m_fnCreateBossPhaseConfig = []() {
        auto pConfig = std::make_unique<BossPhaseConfig>();

        // ----- Phase 1 (100% - 50% HP): Hit & Run -----
        BossPhaseData phase1;
        phase1.m_fHealthThreshold = 1.0f;
        phase1.m_fSpeedMultiplier = 1.0f;
        phase1.m_fAttackSpeedMultiplier = 1.0f;
        phase1.m_nSpecialAttackChance = 70;
        phase1.m_bCanFly = false;

        // Primary — SpinDash (회전 돌진). 매번 발동 = 거의 끊임없는 회전 압박
        phase1.m_fnPrimaryAttack = []() {
            return std::make_unique<SpinDashAttackBehavior>(
                18.0f, 0.22f, 18.0f, 1.1f, 0.25f, 0.55f, 7.0f);
        };

        // 돌진 2 변형:
        //   짧은 돌진 — 가깝게, 후딜 1.0s (회피 빡빡, 딜윈도우 보통)
        //   긴  돌진  — 멀리,   후딜 1.2s (회피 쉬움, 딜윈도우 큼)
        // 특수공격 풀: 짧은 돌진 / 긴 돌진 / 어글락 차지 (1/3 씩)
        //   FixatedCharge = 핵심 기믹. 어글자 추적 텔레그래프 → 장거리 돌진 → 기둥 박으면 그로기
        //   p1 풀: 짧은돌진(2) / 긴돌진(2) / 어글락(2) / 회오리장판(1) / 돌풍슬래시(1) / 충격파링(1)
        //   기존 돌진 3종은 등장 빈도 유지 위해 가중치 2배, 신규 풍속성 3종은 각 1배
        phase1.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
            int choice = rand() % 9;
            if (choice < 2) {
                // 짧은 돌진 — 거리/범위 모두 확장, 위협적
                return std::make_unique<RushFrontAttackBehavior>(
                    55.0f, 28.0f, 0.85f, 0.25f, 0.15f, 1.0f, 8.5f, 75.0f);
            } else if (choice < 4) {
                // 긴 돌진 — 방 가로지르는 거리
                return std::make_unique<RushFrontAttackBehavior>(
                    70.0f, 34.0f, 1.2f, 0.30f, 0.20f, 1.2f, 10.5f, 95.0f);
            } else if (choice < 6) {
                // 어글자 락온 차지 — 매우 멀리, 더 두꺼운 인디케이터
                return std::make_unique<FixatedChargeAttackBehavior>(
                    85.0f /*damage*/, 3.0f /*telegraph*/, 0.2f /*locked*/,
                    58.0f /*dashSpeed*/, 110.0f /*maxDist 매우 길게*/,
                    6.5f  /*playerHitR*/, 8.0f /*pillarHitR*/,
                    6.0f  /*indicatorHalfW 두껍게*/,
                    4.5f  /*groggyDur*/, 0.9f /*missRecovery*/);
            } else if (choice == 6) {
                // 회오리 장판 — 4개 토네이도, 지속 4초 area denial
                return std::make_unique<TornadoFieldAttackBehavior>(
                    4 /*count*/, 18.0f /*tickDmg*/, 0.45f /*tickInterval*/,
                    5.0f /*radius*/, 12.0f, 28.0f,
                    1.8f /*windup*/, 4.0f /*active*/, 1.0f /*recovery*/);
            } else if (choice == 7) {
                // 돌풍 슬래시 — 4방향 (랜덤 십자/X자), 즉발성 라인 데미지
                auto shape = (rand() % 2 == 0)
                    ? GaleSlashAttackBehavior::SlashShape::Cross
                    : GaleSlashAttackBehavior::SlashShape::XDiag;
                return std::make_unique<GaleSlashAttackBehavior>(
                    shape, 75.0f, 30.0f, 3.5f,
                    1.4f /*windup*/, 0.4f /*impact*/, 1.2f /*recovery*/);
            } else {
                // 충격파 링 — 보스 중심에서 외곽으로 wave 확장
                return std::make_unique<ShockwaveRingAttackBehavior>(
                    85.0f /*damage*/, 35.0f /*maxRadius*/, 4.0f /*thickness*/,
                    1.6f /*windup*/, 1.0f /*expand*/, 1.0f /*recovery*/);
            }
        };

        pConfig->AddPhase(phase1);

        // ----- Phase 2 (50% - 0% HP): Rage Mode -----
        BossPhaseData phase2;
        phase2.m_fHealthThreshold = 0.5f;
        phase2.m_fSpeedMultiplier = 1.55f;        // 1.35→1.55, 추격 속도 살벌하게
        phase2.m_fAttackSpeedMultiplier = 0.50f;  // 0.65→0.50, 쿨 더 짧아짐
        phase2.m_nSpecialAttackChance = 90;       // 85→90, 특수기 거의 항상
        phase2.m_bCanFly = false;

        // Primary 강화: 더 빠른 회전, 길게 끌고감, 후딜 짧음
        phase2.m_fnPrimaryAttack = []() {
            return std::make_unique<SpinDashAttackBehavior>(
                20.0f /*tickDmg*/, 0.18f /*tick 더 자주*/,
                22.0f /*rushSpeed*/, 1.25f /*rushDur ~27 unit*/,
                0.20f /*windup*/, 0.40f /*recovery 짧게*/,
                7.5f  /*aoeRadius*/);
        };

        // 더 빠른 돌진. 후딜도 같이 짧아져서 딜 윈도우 좁음 (빡빡한 패링)
        //   Rage 모드 특수 풀 (가중치): 짧은돌진(2)/긴돌진(2)/어글락(2)/
        //                              회오리장판 강화(1)/돌풍슬래시 강화(1)/충격파링 강화(1)/공중 슬램(1)
        //   공중 슬램은 시그니처 패턴 — 가끔 등장해서 광역 슬램으로 압박. 총 10 슬롯
        phase2.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
            int choice = rand() % 10;
            if (choice < 2) {
                // 빠른 짧은 돌진
                return std::make_unique<RushFrontAttackBehavior>(
                    60.0f, 36.0f, 0.7f, 0.20f, 0.15f, 0.70f, 8.5f, 75.0f);
            } else if (choice < 4) {
                // 빠른 긴 돌진
                return std::make_unique<RushFrontAttackBehavior>(
                    78.0f, 42.0f, 1.05f, 0.22f, 0.18f, 0.85f, 10.5f, 95.0f);
            } else if (choice < 6) {
                // Rage 모드의 차지 — 텔레그래프 짧고 더 빠름, 더 멀리
                return std::make_unique<FixatedChargeAttackBehavior>(
                    100.0f /*damage*/, 2.4f /*telegraph 짧게*/, 0.15f /*locked*/,
                    68.0f  /*dashSpeed*/, 120.0f /*maxDist 더 멀리*/,
                    6.5f   /*playerHitR*/, 8.0f /*pillarHitR*/,
                    6.0f   /*indicatorHalfW*/,
                    4.0f   /*groggyDur*/, 0.7f /*missRecovery*/);
            } else if (choice == 6) {
                // 회오리 장판 강화 — 6개, 더 큰 반경, 더 긴 지속
                return std::make_unique<TornadoFieldAttackBehavior>(
                    6, 22.0f, 0.4f, 6.0f, 12.0f, 32.0f,
                    1.5f, 5.0f, 0.8f);
            } else if (choice == 7) {
                // 돌풍 슬래시 강화 — windup 짧고 데미지 큼
                auto shape = (rand() % 2 == 0)
                    ? GaleSlashAttackBehavior::SlashShape::Cross
                    : GaleSlashAttackBehavior::SlashShape::XDiag;
                return std::make_unique<GaleSlashAttackBehavior>(
                    shape, 100.0f, 36.0f, 4.0f,
                    1.1f, 0.4f, 1.0f);
            } else if (choice == 8) {
                // 충격파 링 강화 — 더 큰 반경, 빠른 확장
                return std::make_unique<ShockwaveRingAttackBehavior>(
                    110.0f, 42.0f, 4.5f,
                    1.3f, 0.85f, 0.8f);
            } else {
                // 공중 슬램 (시그니처) — 점프 후 광역 강하. 데몬에 "Take Off/Land" 클립이
                //   없어서 jump 모션은 자체 Y 곡선만 의존. attack4 를 clipOverride 로 주면
                //   EnemyComponent 가 attack4 를 먼저 재생 → Execute 가 즉시 CrossFade 시도하나
                //   해당 클립 부재로 attack4 가 유지되어 점프 동안 공격 모션 노출.
                return std::make_unique<JumpSlamAttackBehavior>(
                    130.0f /*damage*/, 18.0f /*jumpHeight*/, 1.1f /*jumpDur*/,
                    16.0f /*slamRadius*/, 0.35f /*windup*/, 1.0f /*recovery*/,
                    true /*trackTarget*/, 3.0f /*shake*/, 0.5f /*shakeDur*/,
                    "attack4" /*clipOverride*/);
            }
        };

        // 페이즈 전환: Rage 포효 + 무적 (원본 입장 연출 유지)
        phase2.m_bHasTransitionAttack = false;
        phase2.m_bInvincibleDuringTransition = true;
        phase2.m_fTransitionDuration = 2.4f;          // Rage 클립 2.37s 매칭
        phase2.m_strTransitionAnimation = "Rage";

        pConfig->AddPhase(phase2);

        return pConfig;
    };

    RegisterEnemyPreset("Demon", demon);

    // Register Blue Dragon preset (Water boss Phase 1)
    EnemySpawnData blueDragon;
    blueDragon.m_strMeshPath      = "Assets/Enemies/Dragon_blue/Blue.bin";
    blueDragon.m_strAnimationPath = "Assets/Enemies/Dragon_blue/Blue_Anim.bin";
    blueDragon.m_strTexturePath   = "Assets/Enemies/Dragon_blue/Textures/BlueHP.png";
    blueDragon.m_xmf3Scale = XMFLOAT3(3.0f, 3.0f, 3.0f);  // 원본값 복원
    // [카테고리: 보스(중간보스)] 청룡 컨셉 — 청색 강화
    blueDragon.m_xmf4Color = XMFLOAT4(0.30f, 0.55f, 1.00f, 1.0f);
    blueDragon.m_fColliderXZMultiplier = 1.0f;   // 뚱뚱한 몸집에 맞게 피격 판정 확대 (기본 0.3 → 1.0)

    blueDragon.m_Stats.m_fMaxHP              = 80.0f;
    blueDragon.m_Stats.m_fCurrentHP          = 80.0f;
    blueDragon.m_Stats.m_fMoveSpeed          = 9.0f;
    blueDragon.m_Stats.m_fAttackRange        = 35.0f;
    blueDragon.m_Stats.m_fAttackCooldown     = 2.2f;  // 기본 공격 텀 넉넉하게
    blueDragon.m_Stats.m_fLongRangeThreshold = 30.0f;
    blueDragon.m_Stats.m_fMidRangeThreshold  = 15.0f;

    blueDragon.m_bIsFlying = false;
    blueDragon.m_fFlyHeight = 0.0f;
    blueDragon.m_bIsBoss = true;
    blueDragon.m_fSpecialAttackCooldown = 3.0f;   // 특수기 자주 나오게
    blueDragon.m_nSpecialAttackChance   = 60;     // 쿨다운 끝나면 60% 확률로 특수기
    blueDragon.m_nFlyingAttackChance    = 0;       // 페이즈 컨트롤러 없으면 작동 안 함

    blueDragon.m_AnimConfig.m_strIdleClip    = "Idle";
    blueDragon.m_AnimConfig.m_strChaseClip   = "Walk";
    blueDragon.m_AnimConfig.m_strAttackClip  = "Fireball Shoot";  // 브레스 전용 애니 (Basic Attack은 근접 물기)
    blueDragon.m_AnimConfig.m_strStaggerClip = "Get Hit";
    blueDragon.m_AnimConfig.m_strDeathClip   = "Die";
    blueDragon.m_AnimConfig.m_bLoopAttack    = true;  // 행동 지속 시간 동안 공격 포즈 유지

    blueDragon.m_IndicatorConfig.m_eType      = IndicatorType::Circle;
    blueDragon.m_IndicatorConfig.m_fHitRadius = 14.0f;

    // 기본 공격: 브레스. windup 넉넉히(0.4→1.0) 주고 clip override 명시해서 "Fireball Shoot" 확실히 재생.
    blueDragon.m_fnCreateAttack = [pProjMgr]() {
        return std::make_unique<BreathAttackBehavior>(
            pProjMgr, 32.0f, 38.0f, 5, 50.0f,
            1.0f /*windup*/, 1.2f /*breath*/, 0.5f /*recovery*/,
            1.0f, 3.0f, ElementType::Water,
            "Fireball Shoot" /*clipOverride*/);
    };

    // 특수 공격: 뚱뚱한 몸집에 맞는 묵직하고 느린 패턴들
    blueDragon.m_fnCreateSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
        int choice = rand() % 4;
        switch (choice) {
        case 0:
            // 꼬리 휩쓸기 - 크고 느린 호, 긴 선딜
            return std::make_unique<TailSweepAttackBehavior>(32.0f, 0.65f, 0.45f, 0.6f, 10.0f, 200.0f, true);
        case 1:
            // 점프 슬램 - 낮고 느린 점프, 무거운 착지. 도약 높이 줄이고 착지 후딜 길게
            return std::make_unique<JumpSlamAttackBehavior>(42.0f, 6.0f, 0.7f, 8.0f, 0.5f, 0.7f, true);
        case 2:
            // 3연타 - 묵직하고 간격 넓게 (LightCombo 파라미터 오버라이드 불가하므로 HeavyCombo 사용)
            return std::unique_ptr<IAttackBehavior>(ComboAttackBehavior::CreateHeavyCombo());
        default:
            // 느릿한 돌진 - 짧은 거리, 낮은 속도
            return std::make_unique<RushFrontAttackBehavior>(40.0f, 10.0f, 1.0f, 0.35f, 0.35f, 0.5f, 6.0f, 60.0f);
        }
    };

    RegisterEnemyPreset("BlueDragon", blueDragon);

    // ===== Final Boss: DarkLord (DarkKnight) — 최종 보스, 단순 근접 콤보 =====
    EnemySpawnData darkLord;
    // Mesh + Anim 는 skin3 짝 유지 (skin1 anim 부재 → 본 구조 mismatch 로 메쉬 깨짐).
    //   텍스처만 Skin1 (어두운 푸른 강철) 로 적용 — UV 는 skin3 mesh 의 것 그대로 사용.
    darkLord.m_strMeshPath      = "Assets/Enemies/DeathKnight/DarkKnight2_skin3.bin";
    darkLord.m_strAnimationPath = "Assets/Enemies/DeathKnight/DarkKnight2_skin3_Anim.bin";
    // Skin1 전체 풀세트 사용 — 어두운 푸른 강철 다크나이트 톤. Body/Armor/Helm/Sword 모두 Skin1.
    darkLord.m_strTexturePath   = "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Armor_Albedo.png";
    darkLord.m_vTextureOverrides = {
        { "Sword",  "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Sword_Albedo.png" },
        { "sword",  "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Sword_Albedo.png" },
        { "Weapon", "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Sword_Albedo.png" },
        { "Helm",   "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Helm_Albedo.png"  },
        { "helm",   "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Helm_Albedo.png"  },
        { "Hood",   "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Helm_Albedo.png"  },
        { "Body",   "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Body_Albedo.png"  },
        { "body",   "Assets/Enemies/DeathKnight/Textures/T_Skin1_DeathKnight_Body_Albedo.png"  },
        { "Head",   "Assets/Enemies/DeathKnight/Textures/T_DeathKnigh_2_Mat_DarkKnight2_Head_Albedo.png" },
        { "head",   "Assets/Enemies/DeathKnight/Textures/T_DeathKnigh_2_Mat_DarkKnight2_Head_Albedo.png" },
        { "Face",   "Assets/Enemies/DeathKnight/Textures/T_DeathKnigh_2_Mat_DarkKnight2_Head_Albedo.png" },
    };
    darkLord.m_xmf3Scale = XMFLOAT3(14.0f, 14.0f, 14.0f);   // 10 → 14 (최종 보스 위엄 ↑)
    // 자연 텍스처 — 다크 아레나의 cool 라이트 (1.0, 1.1, 1.5) 가 이미 차가운 톤 보장.
    //   별도 tint 불필요.
    darkLord.m_xmf4Color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

    // 테스트 편의 — 페이즈 진행을 빠르게 보기 위해 6000 → 1500. 추후 밸런스 잡힐 때 상향.
    //   카멘 톤(로스트아크 군단장) — 천천히 다가오고, 검 사거리 길고, 공격 사이 정지 위협적.
    //   moveSpeed 12→8 : 발걸음 묵직 (스킨 자체가 풀아머라 천천히 보여야 위엄).
    //   attackRange 7→8 : 거대 검이 멀리 닿는 느낌 (cone 은 단발 풀에서 처리).
    //   attackCooldown 1.0→1.4 : 공격 사이 1.4s 텔레그래프-정지 — 한 방 한 방이 위협적.
    darkLord.m_Stats.m_fMaxHP              = 1500.0f;
    darkLord.m_Stats.m_fCurrentHP          = 1500.0f;
    darkLord.m_Stats.m_fMoveSpeed          = 8.0f;
    darkLord.m_Stats.m_fAttackRange        = 11.0f;   // 8 → 11 (스케일 1.4× 비례)
    darkLord.m_Stats.m_fAttackCooldown     = 1.4f;
    darkLord.m_Stats.m_fLongRangeThreshold = 45.0f;   // 35 → 45
    darkLord.m_Stats.m_fMidRangeThreshold  = 24.0f;   // 18 → 24

    darkLord.m_bIsBoss = true;
    darkLord.m_fSpecialAttackCooldown = 6.0f;
    darkLord.m_nSpecialAttackChance   = 50;
    darkLord.m_fAnimationPlaybackSpeed = 1.0f;
    // 콜라이더 XZ 폭 — 스케일 14 ↑ 후 통로/모서리 끼임 방지.
    //   기본 0.3 * 14 = 4.2 → 통로 좁은 곳에서 코너에 낀다. 0.22 로 14 * 0.22 ≈ 3.08
    //   (= 스케일 10 시절의 풋프린트 3.0 와 동등) 유지하여 네비 안정성 확보.
    //   피격 판정용 콜라이더는 별도 시스템 — 이 콜라이더는 벽 통과 방지용.
    darkLord.m_fColliderXZMultiplier = 0.22f;

    // 애니메이션 클립 매핑 (Assets/Enemies/DeathKnight/DarkKnight2_skin3_Anim.bin)
    darkLord.m_AnimConfig.m_strIdleClip    = "fightidle";
    darkLord.m_AnimConfig.m_strChaseClip   = "run";
    darkLord.m_AnimConfig.m_strAttackClip  = "attack3";
    darkLord.m_AnimConfig.m_strStaggerClip = "gethit1";
    darkLord.m_AnimConfig.m_strDeathClip   = "death1";

    darkLord.m_IndicatorConfig.m_eType      = IndicatorType::ForwardBox;
    darkLord.m_IndicatorConfig.m_fHitRadius = 6.5f;   // 4.5 → 6.5 (스케일 비례)
    darkLord.m_IndicatorConfig.m_fHitLength = 13.0f;  // 9 → 13

    // 기본 공격 — 페이즈 컨트롤러가 처음 적용되기 전 fallback 용 단순 근접.
    darkLord.m_fnCreateAttack = []() {
        auto p = std::make_unique<MeleeAttackBehavior>(
            40.0f /*damage*/, 0.45f /*windup*/, 0.55f /*hit*/, 0.50f /*recovery*/);
        p->SetHitRange(11.0f);   // 8 → 11
        return p;
    };
    // Special fallback — phase 적용 전에만 사용
    darkLord.m_fnCreateSpecialAttack = []() {
        return std::make_unique<JumpSlamAttackBehavior>(
            70.0f, 16.0f, 0.9f, 13.0f, 0.40f, 0.8f, true, 2.0f, 0.35f, "Attack10");  // 12→16, 10→13
    };

    // ── DarkLord 5단계 페이즈 (땅 → 물 → 바람 → 불 → Final) ─────────────────
    //   [Day 3 재설계] 각 페이즈 Primary 는 모두 DarkLordSigilSlash (5-레이어 컷씬 검기).
    //   원소 페이즈 매핑: P0=Earth, P1=Water, P2=Wind, P3=Fire, P4=Fire/Ultimate.
    //   톤 다양화: P0/P3=Heavy(묵직 일격), P1/P2=Medium(빠른 잽), P4=Ultimate(각성).
    //   페이즈 전환 시 attack9(짧은 포효)로 1.6초 무적, 코너 색상 복귀 트리거.
    //
    //   기존 짤패 풀 헬퍼(kHitPool/kElementVfx/MakePrimaryAttack) 제거됨 — 각 페이즈
    //   Primary 람다가 DarkLordSigilSlash 직접 생성.
    darkLord.m_fnCreateBossPhaseConfig = []() {
        auto pConfig = std::make_unique<BossPhaseConfig>();

        // ── Phase 0 (100→80% HP) : 땅 / 묵직 2타 칼 + 십자 균열 ──────────────
        //   짤패: attack6 → attack9 묵직 2타, windup 길게, 사거리 넓게 (광역 칼바람 느낌).
        //   서명: GroundRupture 십자 균열 — 광장 전체 가로/세로 4가닥.
        {
            BossPhaseData p;
            p.m_fHealthThreshold = 1.0f;
            // 카멘 톤 : 가장 묵직한 시작 — 발걸음 가장 느림, 공격 간격 가장 김.
            p.m_fSpeedMultiplier = 0.80f;
            p.m_fAttackSpeedMultiplier = 1.50f;
            p.m_nSpecialAttackChance = 45;

            // [Day 3] P0 Earth — Massive 위주. Standard 비중 ↓.
            p.m_fnPrimaryAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                CHit hit;
                SlashPresentation style = SlashPresentation::Standard;
                SlashPowerLevel  lvl    = SlashPowerLevel::Signature;
                if (roll < 35)      { hit = MakeHeavySlam(ElementType::Earth);  style = SlashPresentation::Massive; }
                else if (roll < 55) { hit = MakeSpinCleave(ElementType::Earth); style = SlashPresentation::Massive; }
                else if (roll < 70) { hit = MakeLongReach(ElementType::Earth);  style = SlashPresentation::Projectile; lvl = SlashPowerLevel::Medium; }
                else if (roll < 85) { hit = MakeWhipTrail(ElementType::Earth);  style = SlashPresentation::Light;  lvl = SlashPowerLevel::Small; }
                else                { hit = MakeSideCleave(ElementType::Earth); style = SlashPresentation::Standard; lvl = SlashPowerLevel::Medium; }
                SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Earth, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            // Special: 십자 균열 — 광장 전체 가로/세로 4 가닥 솟구침
            // [Day 3] P0 Special — Earth Ultimate Massive (시그니처: 거대 지진 베기)
            // [Day 6] P0 Special — SigilField (지면 인장 지연 폭발) 35% 추가.
            p.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                // 35% : 검역 SigilField — 3개 인장 (플레이어 발치 1 + 보스 주변 2)
                if (roll < 35)
                    return std::make_unique<DarkLordSigilField>(
                        ElementType::Earth, 75.0f /*dmg*/, 9.0f /*radius*/,
                        1.30f /*delay*/, 3 /*count*/, 18.0f /*spread*/);  // R7→9, spread14→18
                CHit hit;
                SlashPresentation style = SlashPresentation::Massive;
                SlashPowerLevel  lvl   = SlashPowerLevel::Ultimate;
                if (roll < 70)      { hit = MakeHeavySlam(ElementType::Earth);  }
                else                { hit = MakeSpinCleave(ElementType::Earth); }
                SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Earth, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            pConfig->AddPhase(p);
        }

        // ── Phase 1 (80→60% HP) : 물 / 흐르는 3타 칼 + 풀맵 충격파 ──────────
        //   짤패: attack1 → attack2 → spin — 부드럽게 이어지는 3타, 마지막 spin 으로 휩쓰는 느낌.
        //   서명: ShockwaveRing — 보스 중심 거대 충격파 링.
        {
            BossPhaseData p;
            p.m_fHealthThreshold = 0.80f;
            // 카멘 톤 : 여전히 느림, 공격 간격 짧지 않음.
            p.m_fSpeedMultiplier = 0.90f;
            p.m_fAttackSpeedMultiplier = 1.30f;
            p.m_nSpecialAttackChance = 50;
            p.m_bInvincibleDuringTransition = true;
            p.m_fTransitionDuration = 1.6f;
            p.m_strTransitionAnimation = "attack9";

            // [Day 3] P1 Water — 흐름. Projectile/Light/Massive 골고루, Standard 최소.
            p.m_fnPrimaryAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                CHit hit;
                SlashPresentation style = SlashPresentation::Standard;
                SlashPowerLevel  lvl    = SlashPowerLevel::Medium;
                if (roll < 25)      { hit = MakeLongReach(ElementType::Water);  style = SlashPresentation::Projectile; }   // 물 흐름 = 발사형
                else if (roll < 50) { hit = MakeWhipTrail(ElementType::Water);  style = SlashPresentation::Light; lvl = SlashPowerLevel::Small; }
                else if (roll < 75) { hit = MakeHeavySlam(ElementType::Water);  style = SlashPresentation::Massive; lvl = SlashPowerLevel::Signature; }
                else if (roll < 90) { hit = MakeSpinCleave(ElementType::Water); style = SlashPresentation::Massive; lvl = SlashPowerLevel::Signature; }
                else                { hit = MakeSideCleave(ElementType::Water); style = SlashPresentation::Standard; }
                SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Water, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            // Special: 풀맵 충격파 링 (36 반경)
            // [Day 3] P1 Special — Water Ultimate Massive + 가끔 CrossSigil
            // [Day 6] P1 Special — 검기 폭격 Barrage (Projectile burst 5발) 30% 추가.
            p.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                CHit hit;
                SlashPresentation style = SlashPresentation::Massive;
                SlashPowerLevel  lvl   = SlashPowerLevel::Ultimate;
                ElementType element = ElementType::Water;
                if (roll < 30)
                {
                    // 검기 폭격 — Projectile 5연발 spread ±5°.
                    hit = MakeLongReach(ElementType::Water);
                    style = SlashPresentation::Projectile;
                    lvl   = SlashPowerLevel::Signature;
                    SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Water, lvl);
                    desc.ApplyPresentation(style);
                    desc.projectileBurstCount     = 5;
                    desc.projectileBurstInterval  = 0.14f;   // 0.12 → 0.14 (시간 간격 ↑)
                    desc.projectileBurstSpreadDeg = 12.0f;   // 6 → 12 (좌우 부채꼴 ↑)
                    return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
                }
                if (roll < 45)      { hit = MakeHeavySlam(ElementType::Fire); style = SlashPresentation::CrossSigil; element = ElementType::Fire; }  // 4원소 동시
                else if (roll < 75) { hit = MakeSpinCleave(ElementType::Water); }
                else                { hit = MakeHeavySlam(ElementType::Water); }
                SlashVFXDesc desc = SlashVFXDesc::Preset(element, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            pConfig->AddPhase(p);
        }

        // ── Phase 2 (60→40% HP) : 풀/바람 / 빠른 3타 칼 + X자 진공파 ────────
        //   짤패: attack1 → attack2 → attack4, windup 짧음·회복 짧음 — 칼바람 느낌.
        //   서명: GaleSlash XDiag — X자 진공파.
        {
            BossPhaseData p;
            p.m_fHealthThreshold = 0.60f;
            // 카멘 톤 : 중간 단계 — 베이스 속도, 공격 간격 살짝 짧음.
            p.m_fSpeedMultiplier = 1.00f;
            p.m_fAttackSpeedMultiplier = 1.10f;
            p.m_nSpecialAttackChance = 55;
            p.m_bInvincibleDuringTransition = true;
            p.m_fTransitionDuration = 1.6f;
            p.m_strTransitionAnimation = "attack9";

            // [Day 3] P2 Wind — 잽 + 발사형 위주. Light 채찍 비중 ↑.
            p.m_fnPrimaryAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                CHit hit;
                SlashPresentation style = SlashPresentation::Standard;
                SlashPowerLevel  lvl    = SlashPowerLevel::Medium;
                if (roll < 30)      { hit = MakeLongReach(ElementType::Wind);  style = SlashPresentation::Projectile; }
                else if (roll < 55) { hit = MakeWhipTrail(ElementType::Wind);  style = SlashPresentation::Light;   lvl = SlashPowerLevel::Small; }
                else if (roll < 80) { hit = MakeSpinCleave(ElementType::Wind); style = SlashPresentation::Massive; lvl = SlashPowerLevel::Signature; }
                else if (roll < 92) { hit = MakeHeavySlam(ElementType::Wind);  style = SlashPresentation::Massive; lvl = SlashPowerLevel::Signature; }
                else                { hit = MakeSideCleave(ElementType::Wind); style = SlashPresentation::Standard; }
                SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Wind, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            // Special: GaleSlash X자 진공파 — length 36, 4면 동시 발사
            // [Day 3] P2 Special — Wind Ultimate Projectile + CrossSigil
            // [Day 6] P2 Special — 십자검광 TwinCleave (±35° 동시 두 cone) 35% 추가.
            p.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                CHit hit;
                SlashPresentation style = SlashPresentation::Projectile;
                SlashPowerLevel  lvl   = SlashPowerLevel::Ultimate;
                ElementType element = ElementType::Wind;
                if (roll < 35)
                {
                    // 십자검광 — 정면 ±35° 두 cone 동시.
                    hit = MakeSideCleave(ElementType::Wind);
                    style = SlashPresentation::TwinCleave;
                    lvl   = SlashPowerLevel::Signature;
                    SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Wind, lvl);
                    desc.ApplyPresentation(style);
                    desc.twinSeparationDeg = 35.0f;
                    return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
                }
                if (roll < 60)      { hit = MakeHeavySlam(ElementType::Fire); style = SlashPresentation::CrossSigil; element = ElementType::Fire; }
                else if (roll < 85) { hit = MakeLongReach(ElementType::Wind); }
                else                { hit = MakeSpinCleave(ElementType::Wind); style = SlashPresentation::Massive; }
                SlashVFXDesc desc = SlashVFXDesc::Preset(element, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            pConfig->AddPhase(p);
        }

        // ── Phase 3 (40→20% HP) : 불 / 공격적 2타 칼 + 메테오 폭격 ──────────
        //   짤패: attack6 → attack9, windup 짧고 데미지 높은 burst 콤보 — 폭발적 칼질.
        //   서명: RockFall 메테오 — 하늘에서 8발 화염 낙하.
        {
            BossPhaseData p;
            p.m_fHealthThreshold = 0.40f;
            // 카멘 톤 : 불 페이즈 — 살짝 빨라짐, 그러나 공격 간격은 여전히 0.95× (베이스에 가깝게).
            p.m_fSpeedMultiplier = 1.05f;
            p.m_fAttackSpeedMultiplier = 0.95f;
            p.m_nSpecialAttackChance = 30;   // 60 → 30 (Day3: 검기 가시성 ↑, Day5 special 교체 후 재조정)
            p.m_bInvincibleDuringTransition = true;
            p.m_fTransitionDuration = 1.6f;
            p.m_strTransitionAnimation = "attack9";

            // [Day 3] P3 Fire — 폭발적 톤. Massive 비중 ↑, Light 채찍 양념.
            p.m_fnPrimaryAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                CHit hit;
                SlashPresentation style = SlashPresentation::Standard;
                SlashPowerLevel  lvl    = SlashPowerLevel::Signature;
                if (roll < 35)      { hit = MakeHeavySlam(ElementType::Fire);  style = SlashPresentation::Massive; }
                else if (roll < 60) { hit = MakeSpinCleave(ElementType::Fire); style = SlashPresentation::Massive; }
                else if (roll < 80) { hit = MakeLongReach(ElementType::Fire);  style = SlashPresentation::Projectile; lvl = SlashPowerLevel::Medium; }
                else if (roll < 92) { hit = MakeWhipTrail(ElementType::Fire);  style = SlashPresentation::Light; lvl = SlashPowerLevel::Small; }
                else                { hit = MakeSideCleave(ElementType::Fire); style = SlashPresentation::Standard; lvl = SlashPowerLevel::Medium; }
                SlashVFXDesc desc = SlashVFXDesc::Preset(ElementType::Fire, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            // Special: 메테오 폭격 — 8발, 20~55 반경 사이 무작위 착탄
            // [Day 3] P3 Special — Fire Ultimate Massive + CrossSigil + FinalJudgment
            // [Day 6] P3 Special — 검의 비 SwordRain (광장 7곳 동시 낙하 AoE) 35% 추가.
            // [Day 7] P3 Special — 검의 봉인 SwordSeal (시그니처 기믹) 15% 추가.
            p.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                if (roll < 15)
                {
                    // 검의 봉인 — 검 4자루 회전 (보스 일반 패턴 병행). 7초 후 소멸.
                    //   스케일 14 비례: orbitR 20→26, visScale 15→21.
                    return std::make_unique<DarkLordSwordSeal>(
                        ElementType::Fire, 45.0f /*dmg*/, 7.0f /*duration*/,
                        26.0f /*orbitR*/, 65.0f /*orbitSpeed*/,
                        4.0f /*hitR*/, 21.0f /*visScale*/, 4 /*count*/);
                }
                if (roll < 40)
                    return std::make_unique<DarkLordSwordRain>(
                        ElementType::Fire, 7, 60.0f, 8.5f, 9.0f, 42.0f, 1.7f, 1.5f);  // R6.5→8.5, spread32→42
                CHit hit;
                SlashPresentation style = SlashPresentation::Massive;
                SlashPowerLevel  lvl   = SlashPowerLevel::Ultimate;
                ElementType element = ElementType::Fire;
                if (roll < 58)      { hit = MakeHeavySlam(ElementType::Fire); style = SlashPresentation::CrossSigil; }
                else if (roll < 72) { hit = MakeHeavySlam(ElementType::Fire); style = SlashPresentation::FinalJudgment; }
                else if (roll < 89) { hit = MakeHeavySlam(ElementType::Fire); }
                else                { hit = MakeSpinCleave(ElementType::Fire); }
                SlashVFXDesc desc = SlashVFXDesc::Preset(element, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            pConfig->AddPhase(p);
        }

        // ── Phase 4 (20→0% HP) : Final / 순차 십자 + 메테오 + 충격파 랜덤 ────
        //   가장 화려한 단계. 다단/광역 패턴 무작위로 쏟아낸다.
        {
            BossPhaseData p;
            p.m_fHealthThreshold = 0.20f;
            // 카멘 톤 : Final 각성 — 베이스보다 빠르지만 여전히 위엄. 공격 간격 0.75× 로 제한 (난사 X).
            p.m_fSpeedMultiplier = 1.20f;
            p.m_fAttackSpeedMultiplier = 0.75f;
            p.m_nSpecialAttackChance = 35;   // 80 → 35 (Day3: 검기 가시성, Day5 special 교체 후 재조정)
            p.m_bInvincibleDuringTransition = true;
            p.m_fTransitionDuration = 2.0f;
            p.m_strTransitionAnimation = "attack9";

            // [Day 3] P4 Final — 잔치 톤. 4원소 순환 + Presentation 다양화.
            //   향후 (Day 5+) Cross Sigil / Final Judgment 로 교체.
            p.m_fnPrimaryAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;
                CHit hit;
                ElementType element = ElementType::Fire;
                SlashPresentation style = SlashPresentation::Standard;
                SlashPowerLevel lvl = SlashPowerLevel::Signature;
                if (roll < 25)
                {
                    // Fire Ultimate Massive — 가장 강한 일격
                    hit   = MakeHeavySlam(ElementType::Fire);
                    style = SlashPresentation::Massive;
                    lvl   = SlashPowerLevel::Ultimate;
                }
                else if (roll < 45)
                {
                    // Fire Spin — 회전 광역
                    hit   = MakeSpinCleave(ElementType::Fire);
                    style = SlashPresentation::Massive;
                }
                else
                {
                    // 4원소 순환 — Style 도 랜덤 (잔치)
                    static const ElementType kRotate[4] = {
                        ElementType::Earth, ElementType::Water,
                        ElementType::Wind,  ElementType::Fire };
                    element = kRotate[rand() % 4];
                    int styleRoll = rand() % 100;
                    if      (styleRoll < 35) { hit = MakeHeavySlam(element);  style = SlashPresentation::Massive; }
                    else if (styleRoll < 60) { hit = MakeLongReach(element);  style = SlashPresentation::Projectile; lvl = SlashPowerLevel::Medium; }
                    else if (styleRoll < 85) { hit = MakeWhipTrail(element);  style = SlashPresentation::Light; lvl = SlashPowerLevel::Small; }
                    else                     { hit = MakeSideCleave(element); style = SlashPresentation::Standard; }
                }
                SlashVFXDesc desc = SlashVFXDesc::Preset(element, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            // Special: 시그니처 풀맵 패턴 5종 중 랜덤
            // [Day 3] P4 Special — 시그니처 잔치 (CrossSigil + FinalJudgment 위주)
            // [Day 6] P4 Special — 신규 4종 잔치 합류 (SigilField/Barrage/TwinCleave/SwordRain).
            //   잔치 톤이라 신규 패턴 비중 ~40% 로 자주 등장.
            p.m_fnSpecialAttack = []() -> std::unique_ptr<IAttackBehavior> {
                int roll = rand() % 100;

                // 4원소 순환 헬퍼.
                static const ElementType kRotate[4] = {
                    ElementType::Earth, ElementType::Water,
                    ElementType::Wind,  ElementType::Fire };
                ElementType pickElem = kRotate[rand() % 4];

                if (roll < 10)
                {
                    // 검역 SigilField — 4방향 인장 (잔치 톤). R7.5→9.5, spread18→23.
                    return std::make_unique<DarkLordSigilField>(
                        pickElem, 80.0f, 9.5f, 1.20f, 4, 23.0f);
                }
                if (roll < 20)
                {
                    // 검의 비 SwordRain — 10발 광역. R7→9, spread38→48.
                    return std::make_unique<DarkLordSwordRain>(
                        pickElem, 10, 60.0f, 9.0f, 8.0f, 48.0f, 1.5f, 1.4f);
                }
                if (roll < 30)
                {
                    // 검기 폭격 Barrage — 6발 연속.
                    CHit hit = MakeLongReach(pickElem);
                    SlashVFXDesc desc = SlashVFXDesc::Preset(pickElem, SlashPowerLevel::Signature);
                    desc.ApplyPresentation(SlashPresentation::Projectile);
                    desc.projectileBurstCount     = 6;
                    desc.projectileBurstInterval  = 0.13f;   // 0.10 → 0.13
                    desc.projectileBurstSpreadDeg = 14.0f;   // 7 → 14 (잔치 톤 더 화려하게)
                    return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
                }
                if (roll < 40)
                {
                    // 십자검광 TwinCleave — Final 페이즈는 좀 더 좁은 분리각.
                    CHit hit = MakeSideCleave(pickElem);
                    SlashVFXDesc desc = SlashVFXDesc::Preset(pickElem, SlashPowerLevel::Signature);
                    desc.ApplyPresentation(SlashPresentation::TwinCleave);
                    desc.twinSeparationDeg = 30.0f;
                    return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
                }
                if (roll < 45)
                {
                    // 검의 봉인 SwordSeal — Final 시그니처. 6초 (P3 7초 보다 짧음).
                    //   스케일 14 비례: orbitR 20→26, visScale 15→21.
                    return std::make_unique<DarkLordSwordSeal>(
                        pickElem, 50.0f /*dmg*/, 6.0f /*duration*/,
                        26.0f /*orbitR*/, 80.0f /*orbitSpeed*/,
                        4.0f /*hitR*/, 21.0f /*visScale*/, 4 /*count*/);
                }

                // 기존: CrossSigil / FinalJudgment / 4원소 Massive.
                CHit hit;
                SlashPresentation style;
                SlashPowerLevel  lvl    = SlashPowerLevel::Ultimate;
                ElementType element = ElementType::Fire;
                if (roll < 65)
                {
                    hit   = MakeHeavySlam(ElementType::Fire);
                    style = SlashPresentation::CrossSigil;
                }
                else if (roll < 85)
                {
                    hit   = MakeHeavySlam(ElementType::Fire);
                    style = SlashPresentation::FinalJudgment;
                }
                else
                {
                    element = pickElem;
                    hit     = MakeHeavySlam(element);
                    style   = SlashPresentation::Massive;
                }
                SlashVFXDesc desc = SlashVFXDesc::Preset(element, lvl);
                desc.ApplyPresentation(style);
                return std::make_unique<DarkLordSigilSlash>(desc, std::vector<CHit>{ hit });
            };
            // [기존 5종 Special 잔재 — 코드 보존만, 더 이상 호출 안 됨. Day 5 cleanup 예정]
            /* OLD:
            p.m_fnSpecialAttack_OLD = []() -> std::unique_ptr<IAttackBehavior> {
                int choice = rand() % 5;
                switch (choice)
                {
                case 0:
                    // 순차 십자 폭발 (3개) — 가장 드라마틱
                    return std::make_unique<SequentialCrossAttackBehavior>(
                        80.0f, 38.0f, 4.0f,
                        2.4f, 0.55f, 0.40f,
                        1.5f, 3.2f, 0.55f,
                        "attack7");
                case 1:
                    // 메테오 폭격 (강화) — 10발
                    return std::make_unique<RockFallAttackBehavior>(
                        10, 95.0f, 9.5f, 20.0f, 60.0f,
                        1.8f, 0.85f, 1.6f, 3.6f, 0.6f, "Attack10");
                case 2:
                    // 풀맵 충격파 링 (강화)
                    return std::make_unique<ShockwaveRingAttackBehavior>(
                        125.0f, 40.0f, 5.0f, 1.6f, 1.0f, 1.2f, 3.0f, 0.6f);
                case 3:
                    // X자 진공파 (강화)
                    return std::make_unique<GaleSlashAttackBehavior>(
                        GaleSlashAttackBehavior::SlashShape::XDiag,
                        110.0f, 40.0f, 4.5f, 1.0f, 0.4f, 0.9f, 2.8f, 0.55f);
                default:
                    // 초대형 슬램 (Attack10) — 거의 풀맵
                    return std::make_unique<JumpSlamAttackBehavior>(
                        160.0f, 20.0f, 1.05f, 18.0f, 0.45f, 0.85f,
                        true, 5.0f, 0.7f, "Attack10");
                }
            };
            */
            pConfig->AddPhase(p);
        }

        return pConfig;
    };

    RegisterEnemyPreset("DarkLord", darkLord);

    // Create shared meshes for attack indicators
    // m_pRingMesh = 얇은 테두리 링 (공격 범위 윤곽) — 공격 내내 고정 표시
    m_pRingMesh = new RingMesh(pDevice, pCommandList, 1.0f, 0.96f, 48);   // 0.88 → 0.96 (더 얇게)
    m_pRingMesh->AddRef();
    // m_pDiscMesh = 꽉 찬 원판 (공격 타이밍에 맞춰 차오르는 fill)
    m_pDiscMesh = new RingMesh(pDevice, pCommandList, 1.0f, 0.0f, 48);
    m_pDiscMesh->AddRef();
    m_pLineMesh = new LineMesh(pDevice, pCommandList, 0.4f);
    m_pLineMesh->AddRef();

    m_pFanMesh = new FanMesh(pDevice, pCommandList, 90.0f, 24);
    m_pFanMesh->AddRef();

    // ForwardBox 전방 직사각형용 flat cube — 단위 크기, 실제 크기는 Transform 스케일로
    m_pBoxMesh = new CubeMesh(pDevice, pCommandList, 1.0f, 0.02f, 1.0f);
    m_pBoxMesh->AddRef();

    OutputDebugString(L"[EnemySpawner] Initialized with default presets\n");
}

void EnemySpawner::RegisterEnemyPreset(const std::string& name, const EnemySpawnData& data)
{
    m_mapPresets[name] = data;

    wchar_t buffer[128];
    swprintf_s(buffer, L"[EnemySpawner] Registered preset: %hs\n", name.c_str());
    OutputDebugString(buffer);
}

bool EnemySpawner::HasPreset(const std::string& name) const
{
    return m_mapPresets.find(name) != m_mapPresets.end();
}

GameObject* EnemySpawner::SpawnEnemy(CRoom* pRoom, const std::string& preset, const XMFLOAT3& position, GameObject* pTarget)
{
    auto it = m_mapPresets.find(preset);
    if (it == m_mapPresets.end())
    {
        wchar_t buffer[128];
        swprintf_s(buffer, L"[EnemySpawner] Preset not found: %hs, using TestEnemy\n", preset.c_str());
        OutputDebugString(buffer);

        // Fallback to test enemy
        return SpawnTestEnemy(pRoom, position, pTarget);
    }

    const EnemySpawnData& data = it->second;

    // Create enemy game object
    GameObject* pEnemy = nullptr;

    if (data.m_strMeshPath.empty())
    {
        // Use CubeMesh
        pEnemy = CreateCubeEnemy(pRoom, position, data.m_xmf3Scale, data.m_xmf4Color);
    }
    else
    {
        // Load mesh from file
        pEnemy = CreateMeshEnemy(pRoom, position, data);
    }

    if (pEnemy)
    {
        SetupEnemyComponents(pEnemy, data, pRoom, pTarget);

        // 파밍 사이클별 글로벌 HP 스케일 (오프라인 한정). 사이클 N → 1 + 0.5*N 배.
        // 데미지는 AttackBehavior 분산 보유라 추후 글로벌 곱하기 인터페이스 도입 시 적용.
        if (m_pScene)
        {
            int cycle = m_pScene->GetCycleCount();
            if (cycle > 0)
            {
                if (auto* pEC = pEnemy->GetComponent<EnemyComponent>())
                {
                    float mul = 1.0f + 0.5f * static_cast<float>(cycle);
                    auto& s = pEC->GetStats();
                    s.m_fMaxHP     *= mul;
                    s.m_fCurrentHP *= mul;
                }
            }
        }
    }

    return pEnemy;
}

GameObject* EnemySpawner::SpawnTestEnemy(CRoom* pRoom, const XMFLOAT3& position, GameObject* pTarget)
{
    return SpawnEnemy(pRoom, "TestEnemy", position, pTarget);
}

void EnemySpawner::SpawnRoomEnemies(CRoom* pRoom, const RoomSpawnConfig& config, GameObject* pTarget)
{
    if (!pRoom) return;

    for (const auto& spawn : config.m_vEnemySpawns)
    {
        SpawnEnemy(pRoom, spawn.first, spawn.second, pTarget);
    }

    wchar_t buffer[128];
    swprintf_s(buffer, L"[EnemySpawner] Spawned %zu enemies in room\n", config.m_vEnemySpawns.size());
    OutputDebugString(buffer);
}

GameObject* EnemySpawner::CreateCubeEnemy(CRoom* pRoom, const XMFLOAT3& position, const XMFLOAT3& scale, const XMFLOAT4& color)
{
    if (!m_pDevice || !m_pCommandList || !m_pScene) return nullptr;

    // Create game object via Scene (handles descriptor allocation)
    GameObject* pEnemy = m_pScene->CreateGameObject(m_pDevice, m_pCommandList);
    if (!pEnemy) return nullptr;

    // Set position and scale
    TransformComponent* pTransform = pEnemy->GetTransform();
    if (pTransform)
    {
        pTransform->SetPosition(position);
        pTransform->SetScale(scale);
    }

    // Create cube mesh (1x1x1 base, scaled by transform)
    CubeMesh* pCubeMesh = new CubeMesh(m_pDevice, m_pCommandList, 2.0f, 2.0f, 2.0f);
    pCubeMesh->AddRef();
    pEnemy->SetMesh(pCubeMesh);

    // Set material (red color)
    MATERIAL material;
    material.m_cAmbient = XMFLOAT4(color.x * 0.3f, color.y * 0.3f, color.z * 0.3f, 1.0f);
    material.m_cDiffuse = color;
    material.m_cSpecular = XMFLOAT4(0.5f, 0.5f, 0.5f, 32.0f);
    material.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    pEnemy->SetMaterial(material);

    // Add RenderComponent
    if (m_pShader)
    {
        auto* pRenderComp = pEnemy->AddComponent<RenderComponent>();
        pRenderComp->SetMesh(pCubeMesh);
        m_pShader->AddRenderComponent(pRenderComp);
    }

    // Add ColliderComponent (reduced size to avoid getting stuck on terrain)
    auto* pCollider = pEnemy->AddComponent<ColliderComponent>();
    pCollider->SetExtents(scale.x * 0.5f, scale.y, scale.z * 0.5f);  // Half extents, narrower XZ
    pCollider->SetCenter(0.0f, scale.y, 0.0f);  // Center at mid-height
    pCollider->SetLayer(CollisionLayer::Enemy);
    pCollider->SetCollisionMask(CollisionMask::Enemy);

    wchar_t buffer[128];
    swprintf_s(buffer, L"[EnemySpawner] Created cube enemy at (%.1f, %.1f, %.1f)\n",
        position.x, position.y, position.z);
    OutputDebugString(buffer);

    return pEnemy;
}

void EnemySpawner::SetupEnemyComponents(GameObject* pEnemy, const EnemySpawnData& data, CRoom* pRoom, GameObject* pTarget)
{
    if (!pEnemy) return;

    // Add EnemyComponent
    auto* pEnemyComp = pEnemy->AddComponent<EnemyComponent>();
    pEnemyComp->SetStats(data.m_Stats);
    pEnemyComp->SetTarget(pTarget);
    pEnemyComp->SetRoom(pRoom);
    pEnemyComp->SetAttackOriginForwardOffset(data.m_fAttackOriginForwardOffset);

    // Set flying mode if enabled
    if (data.m_bIsFlying)
    {
        pEnemyComp->SetFlying(true, data.m_fFlyHeight);
    }

    // Stationary mode (고정형 보스)
    if (data.m_bStationary)
    {
        pEnemyComp->SetStationary(true);
    }
    pEnemyComp->SetRotationSpeed(data.m_fRotationSpeed);

    // 기본 애니 재생속도 전달 (공격 override 후 복원에 사용)
    pEnemyComp->SetBaseAnimPlaybackSpeed(
        (data.m_fAnimationPlaybackSpeed > 0.0f) ? data.m_fAnimationPlaybackSpeed : 1.0f);

    // Create attack behavior (+ store factory for per-use recreation)
    if (data.m_fnCreateAttack)
    {
        pEnemyComp->SetAttackBehavior(data.m_fnCreateAttack());
        pEnemyComp->SetAttackFactory(data.m_fnCreateAttack);
    }
    else
    {
        // Default melee attack
        pEnemyComp->SetAttackBehavior(std::make_unique<MeleeAttackBehavior>());
    }

    // Boss settings
    if (data.m_bIsBoss)
    {
        pEnemyComp->SetBoss(true);
        pEnemyComp->SetSpecialAttackCooldown(data.m_fSpecialAttackCooldown);
        pEnemyComp->SetSpecialAttackChance(data.m_nSpecialAttackChance);
        pEnemyComp->SetFlyingAttackCooldown(data.m_fFlyingAttackCooldown);
        pEnemyComp->SetFlyingAttackChance(data.m_nFlyingAttackChance);

        // Create special attack behavior + store factory for per-use recreation
        if (data.m_fnCreateSpecialAttack)
        {
            pEnemyComp->SetSpecialAttackBehavior(data.m_fnCreateSpecialAttack());
            pEnemyComp->SetSpecialAttackFactory(data.m_fnCreateSpecialAttack);
            OutputDebugString(L"[EnemySpawner] Boss special attack behavior set\n");
        }

        // Setup boss phase controller if config factory is provided
        if (data.m_fnCreateBossPhaseConfig)
        {
            auto pPhaseConfig = data.m_fnCreateBossPhaseConfig();
            if (pPhaseConfig)
            {
                auto pPhaseController = std::make_unique<BossPhaseController>(pEnemyComp);
                pPhaseController->SetPhaseConfig(std::move(pPhaseConfig));
                pEnemyComp->SetBossPhaseController(std::move(pPhaseController));
                OutputDebugString(L"[EnemySpawner] Boss phase controller set\n");
            }
        }
    }

    // 상태이상 파티클 VFX
    if (m_pVFXManager)
        pEnemyComp->SetVFXManager(m_pVFXManager);

    // 타입 식별 메쉬 마커 (마법진) — 사용자 요청으로 일시 비활성화. 코드 보존.
    if (false)
    {
        const std::string& atk = data.m_strAttackTypeId;
        if (!atk.empty() && atk != "Melee" && m_pRingMesh && m_pDiscMesh)
        {
            // 외곽링 material (강한 emissive)
            MATERIAL outerMat;
            outerMat.m_cAmbient  = XMFLOAT4(0.10f, 0.05f, 0.20f, 1.0f);
            outerMat.m_cDiffuse  = XMFLOAT4(0.30f, 0.20f, 0.50f, 1.0f);
            outerMat.m_cSpecular = XMFLOAT4(0.0f,  0.0f,  0.0f,  1.0f);

            // 다크판타지 톤: 보라 베이스, 자폭만 적색 위험, 저격은 금색
            XMFLOAT4 outerEmis = { 1.20f, 0.50f, 2.20f, 1.0f };
            XMFLOAT4 innerEmis = { 2.20f, 1.50f, 2.80f, 1.0f };   // 내부는 더 밝게 (코어 광원)
            float    footScale = 5.0f;  // 기본값 — 적 본체 스케일(5.5x) 대비 큼
            if      (atk == "SuicideExplode") {
                outerEmis = { 2.50f, 0.30f, 0.10f, 1.0f };
                innerEmis = { 3.20f, 0.80f, 0.20f, 1.0f };
                footScale = 9.0f;   // 위험 — 매우 큼
            }
            else if (atk == "ChargedShot") {
                outerEmis = { 2.00f, 1.60f, 0.35f, 1.0f };
                innerEmis = { 2.80f, 2.40f, 0.80f, 1.0f };
                footScale = 6.0f;
            }
            else if (atk == "RushAoE") {
                outerEmis = { 1.40f, 0.55f, 2.20f, 1.0f };
                innerEmis = { 2.40f, 1.60f, 2.80f, 1.0f };
                footScale = 10.0f;  // 광역 — 매우 큼
            }
            else if (atk == "RushFront") {
                outerEmis = { 1.40f, 0.55f, 2.20f, 1.0f };
                innerEmis = { 2.40f, 1.60f, 2.80f, 1.0f };
                footScale = 6.5f;
            }
            else if (atk == "GrenadeThrow") {
                outerEmis = { 1.60f, 0.75f, 2.10f, 1.0f };
                innerEmis = { 2.60f, 1.80f, 2.70f, 1.0f };
                footScale = 7.5f;
            }
            else if (atk == "QuickJab") {
                outerEmis = { 1.10f, 0.45f, 2.00f, 1.0f };
                innerEmis = { 2.20f, 1.40f, 2.60f, 1.0f };
                footScale = 4.0f;   // 가장 작음
            }
            else if (atk == "Ranged") {
                outerEmis = { 1.20f, 0.50f, 2.00f, 1.0f };
                innerEmis = { 2.20f, 1.50f, 2.60f, 1.0f };
                footScale = 5.5f;
            }

            // 중간보스 — 마커 크기 1.6배 + 외곽 청록 발광(보스 식별색)
            if (data.m_bIsMiniBoss)
            {
                footScale *= 1.6f;
                outerEmis = { 0.30f, 2.40f, 2.60f, 1.0f };  // 청록 외곽
                innerEmis = { 1.50f, 3.20f, 3.20f, 1.0f };  // 밝은 청록 코어
            }
            outerMat.m_cEmissive = outerEmis;

            MATERIAL innerMat = outerMat;
            innerMat.m_cEmissive = innerEmis;

            auto applyMat = [](GameObject* pGO, const MATERIAL& m) {
                if (!pGO) return;
                pGO->SetMaterial(m);
                if (pGO->GetTransform()) pGO->GetTransform()->Update(0.0f);
                pGO->Update(0.0f);
            };

            // 발밑 이중 동심원만 사용 (헤드 마커 제거)
            GameObject* pFoot      = CreateIndicatorObject(pRoom, m_pRingMesh);
            GameObject* pFootInner = CreateIndicatorObject(pRoom, m_pRingMesh);
            applyMat(pFoot,      outerMat);
            applyMat(pFootInner, innerMat);

            pEnemyComp->SetFootMarker(pFoot);
            pEnemyComp->SetFootMarkerInner(pFootInner);

            pEnemyComp->SetMarkerScales(
                footScale,            // foot outer
                footScale * 0.55f,    // foot inner
                0.f,                  // head 사용 안 함
                0.f);
        }
    }

    // Set death callback to notify room
    pEnemyComp->SetOnDeathCallback([pRoom](EnemyComponent* pDeadEnemy) {
        if (pRoom)
        {
            pRoom->OnEnemyDeath(pDeadEnemy);
        }
    });

    // Register enemy with room
    if (pRoom)
    {
        pRoom->RegisterEnemy(pEnemyComp);
    }

    // Connect AnimationComponent if present
    auto* pAnimComp = pEnemy->GetComponent<AnimationComponent>();
    if (pAnimComp)
    {
        pEnemyComp->SetAnimationComponent(pAnimComp);
        pEnemyComp->SetAnimationConfig(data.m_AnimConfig);

        // Apply random time offset to desync animations between enemies
        float fRandomOffset = (float)(rand() % 1000) / 100.0f;  // 0.0 ~ 10.0 seconds
        pAnimComp->SetTimeOffset(fRandomOffset);

        pAnimComp->Play(data.m_AnimConfig.m_strIdleClip, data.m_AnimConfig.m_bLoopIdle);
    }

    // Create attack indicators
    if (data.m_IndicatorConfig.m_eType != IndicatorType::None && pRoom)
    {
        SetupAttackIndicators(pEnemy, pEnemyComp, data.m_IndicatorConfig, pRoom);
    }

    OutputDebugString(L"[EnemySpawner] Setup enemy components complete\n");
}

GameObject* EnemySpawner::CreateMeshEnemy(CRoom* pRoom, const XMFLOAT3& position, const EnemySpawnData& data)
{
    if (!m_pDevice || !m_pCommandList || !m_pScene) return nullptr;

    // Temporarily set current room to place enemy in room
    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(pRoom);

    // Load mesh from file
    GameObject* pEnemy = MeshLoader::LoadGeometryFromFile(m_pScene, m_pDevice, m_pCommandList, NULL, data.m_strMeshPath.c_str());

    // Restore previous room
    m_pScene->SetCurrentRoom(pPrevRoom);

    if (!pEnemy)
    {
        wchar_t buffer[256];
        swprintf_s(buffer, L"[EnemySpawner] Failed to load mesh: %hs\n", data.m_strMeshPath.c_str());
        OutputDebugString(buffer);
        return nullptr;
    }

    // Set position and scale
    TransformComponent* pTransform = pEnemy->GetTransform();
    if (pTransform)
    {
        pTransform->SetPosition(position);
        pTransform->SetScale(data.m_xmf3Scale);
    }

    // Add AnimationComponent and load animation
    if (!data.m_strAnimationPath.empty())
    {
        auto* pAnimComp = pEnemy->AddComponent<AnimationComponent>();
        pAnimComp->Init(m_pDevice, m_pCommandList);
        pAnimComp->LoadAnimation(data.m_strAnimationPath.c_str());
        // 고정형 보스 등은 1.0 보다 낮게 → 무거운 애니메이션 느낌
        if (data.m_fAnimationPlaybackSpeed > 0.0f && data.m_fAnimationPlaybackSpeed != 1.0f)
            pAnimComp->SetPlaybackSpeed(data.m_fAnimationPlaybackSpeed);
    }

    // Add RenderComponents to hierarchy
    AddRenderComponentsToHierarchy(pEnemy);

    // Load texture if specified
    if (!data.m_strTexturePath.empty())
    {
        // 보스 스폰 마커 — 세션 식별 쉬움
        {
            std::ofstream f("vfx_debug.log", std::ios::app);
            if (f.is_open()) f << "\n=== Enemy spawn: default tex='" << data.m_strTexturePath
                               << "' overrides=" << data.m_vTextureOverrides.size() << " ===\n";
        }
        // m_xmf4Color 를 텍스처 위 tint 로 전달 (카테고리별 색 구분)
        const std::vector<std::pair<std::string, std::string>>* pOv =
            data.m_vTextureOverrides.empty() ? nullptr : &data.m_vTextureOverrides;
        LoadTextureToHierarchy(pEnemy, data.m_strTexturePath, data.m_xmf4Color, pOv);
    }
    else
    {
        // Apply color tint to all meshes in hierarchy (only if no texture)
        ApplyColorToHierarchy(pEnemy, data.m_xmf4Color);
    }

    // Add ColliderComponent (reduced size to avoid getting stuck on terrain)
    auto* pCollider = pEnemy->AddComponent<ColliderComponent>();
    float colliderScale = data.m_xmf3Scale.x;
    if (data.m_xmf3Scale.y > colliderScale) colliderScale = data.m_xmf3Scale.y;
    if (data.m_xmf3Scale.z > colliderScale) colliderScale = data.m_xmf3Scale.z;
    float xzMult = (data.m_fColliderXZMultiplier > 0.0f) ? data.m_fColliderXZMultiplier : 0.3f;
    pCollider->SetExtents(colliderScale * xzMult, colliderScale * 1.0f, colliderScale * xzMult);
    pCollider->SetCenter(0.0f, colliderScale * 1.0f, 0.0f);
    pCollider->SetLayer(CollisionLayer::Enemy);
    pCollider->SetCollisionMask(CollisionMask::Enemy);

    wchar_t buffer[256];
    swprintf_s(buffer, L"[EnemySpawner] Created mesh enemy at (%.1f, %.1f, %.1f) from %hs\n",
        position.x, position.y, position.z, data.m_strMeshPath.c_str());
    OutputDebugString(buffer);

    return pEnemy;
}

void EnemySpawner::AddRenderComponentsToHierarchy(GameObject* pGameObject)
{
    if (!pGameObject || !m_pShader) return;

    // Skip Unity export helper nodes (BlobShadow, shadow projectors, etc.)
    {
        const char* name = pGameObject->m_pstrFrameName;
        bool bSkip = false;
        if (name)
        {
            // Case-insensitive substring check for known Unity helper node names
            std::string sName(name);
            for (char& c : sName) c = (char)tolower((unsigned char)c);
            if (sName == "bs" ||
                sName.find("shadow") != std::string::npos ||
                sName.find("blobshadow") != std::string::npos ||
                sName.find("projector") != std::string::npos)
            {
                bSkip = true;
                wchar_t buf[128];
                swprintf_s(buf, L"[EnemySpawner] Skipped helper node: %hs\n", name);
                OutputDebugString(buf);
            }
        }
        if (bSkip)
        {
            // Still traverse siblings (don't traverse children of skipped node)
            if (pGameObject->m_pSibling)
                AddRenderComponentsToHierarchy(pGameObject->m_pSibling);
            return;
        }
    }

    if (pGameObject->GetMesh())
    {
        auto* pRenderComp = pGameObject->AddComponent<RenderComponent>();
        pRenderComp->SetMesh(pGameObject->GetMesh());
        pRenderComp->SetCastsShadow(true);  // Enemies cast shadows
        m_pShader->AddRenderComponent(pRenderComp);

        wchar_t buffer[128];
        swprintf_s(buffer, L"[EnemySpawner] Added RenderComponent to: %hs\n", pGameObject->m_pstrFrameName);
        OutputDebugString(buffer);
    }

    if (pGameObject->m_pChild)
    {
        AddRenderComponentsToHierarchy(pGameObject->m_pChild);
    }
    if (pGameObject->m_pSibling)
    {
        AddRenderComponentsToHierarchy(pGameObject->m_pSibling);
    }
}

void EnemySpawner::ApplyColorToHierarchy(GameObject* pGameObject, const XMFLOAT4& color)
{
    if (!pGameObject) return;

    MATERIAL material;
    material.m_cAmbient = XMFLOAT4(color.x * 0.3f, color.y * 0.3f, color.z * 0.3f, 1.0f);
    material.m_cDiffuse = color;
    material.m_cSpecular = XMFLOAT4(0.5f, 0.5f, 0.5f, 32.0f);
    material.m_cEmissive = XMFLOAT4(color.x * 0.1f, color.y * 0.1f, color.z * 0.1f, 1.0f);
    pGameObject->SetMaterial(material);

    if (pGameObject->m_pChild)
    {
        ApplyColorToHierarchy(pGameObject->m_pChild, color);
    }
    if (pGameObject->m_pSibling)
    {
        ApplyColorToHierarchy(pGameObject->m_pSibling, color);
    }
}

void EnemySpawner::LoadTextureToHierarchy(GameObject* pGameObject, const std::string& texturePath,
                                          const XMFLOAT4& tint,
                                          const std::vector<std::pair<std::string, std::string>>* pOverrides)
{
    if (!pGameObject || !m_pDevice || !m_pCommandList || !m_pScene) return;

    // Load texture for objects with mesh
    if (pGameObject->GetMesh())
    {
        // 프레임명 substring 매칭으로 override 우선 선택
        std::string actualPath = texturePath;
        const char* matchedKey = "(default)";
        if (pOverrides && pGameObject->m_pstrFrameName && pGameObject->m_pstrFrameName[0])
        {
            for (const auto& kv : *pOverrides)
            {
                if (strstr(pGameObject->m_pstrFrameName, kv.first.c_str()) != nullptr)
                {
                    actualPath = kv.second;
                    matchedKey = kv.first.c_str();
                    break;
                }
            }
        }
        // 파일 존재 확인 — 없으면 LoadTexture 가 fail-fallback 으로 mesh 내장 텍스처(skin3 등) 유지하기 쉬움
        bool bFileExists = false;
        {
            std::ifstream check(actualPath);
            bFileExists = check.good();
        }
        pGameObject->SetTextureName(actualPath.c_str());

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
        m_pScene->AllocateDescriptor(&cpuHandle, &gpuHandle);

        // LoadTexture 호출 전후의 m_pd3dTexture 상태 + cpu/gpu handle 로그
        bool bHadTexBefore = pGameObject->HasTexture();
        pGameObject->LoadTexture(m_pDevice, m_pCommandList, cpuHandle);
        bool bHasTexAfter = pGameObject->HasTexture();
        pGameObject->SetSrvGpuDescriptorHandle(gpuHandle);

        {
            std::ofstream f("vfx_debug.log", std::ios::app);
            if (f.is_open())
            {
                f << "[TexMap] frame='" << (pGameObject->m_pstrFrameName ? pGameObject->m_pstrFrameName : "(null)")
                  << "' match='" << matchedKey
                  << "' exists=" << (bFileExists ? 1 : 0)
                  << " hadTex=" << (bHadTexBefore ? 1 : 0)
                  << " hasTex=" << (bHasTexAfter ? 1 : 0)
                  << " cpuH=" << cpuHandle.ptr
                  << " gpuH=" << gpuHandle.ptr
                  << " tex='" << actualPath << "'\n";
            }
        }

        // diffuse 에 tint 곱해 텍스처 위에 카테고리 색을 입힘.
        // tint == (1,1,1,1) 이면 기존 동작과 동일.
        // 카테고리 풀채도 틴트 × 텍스처 풀채도 = 이중 채도 → 다중 적 동시 등장 시 무지개감.
        // → diffuse 만 neutral grey(0.70) 쪽으로 mute (0.55 비중). 카테고리 identity 유지 +
        //   텍스처 채도와 곱해질 때 과도한 발색 억제. ambient 는 원본 유지 (어두운 영역 보존).
        const XMFLOAT3 kNeutralGrey = { 0.70f, 0.70f, 0.70f };
        const float    kTintBlend   = 0.55f;
        XMFLOAT4 mutedTint = {
            kNeutralGrey.x + (tint.x - kNeutralGrey.x) * kTintBlend,
            kNeutralGrey.y + (tint.y - kNeutralGrey.y) * kTintBlend,
            kNeutralGrey.z + (tint.z - kNeutralGrey.z) * kTintBlend,
            tint.w
        };
        MATERIAL material;
        material.m_cAmbient = XMFLOAT4(tint.x * 0.3f, tint.y * 0.3f, tint.z * 0.3f, 1.0f);
        material.m_cDiffuse = mutedTint;
        material.m_cSpecular = XMFLOAT4(0.3f, 0.3f, 0.3f, 32.0f);
        material.m_cEmissive = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
        pGameObject->SetMaterial(material);

        wchar_t buffer[256];
        swprintf_s(buffer, L"[EnemySpawner] Loaded texture for: %hs\n", pGameObject->m_pstrFrameName);
        OutputDebugString(buffer);
    }

    if (pGameObject->m_pChild)
    {
        LoadTextureToHierarchy(pGameObject->m_pChild, texturePath, tint, pOverrides);
    }
    if (pGameObject->m_pSibling)
    {
        LoadTextureToHierarchy(pGameObject->m_pSibling, texturePath, tint, pOverrides);
    }
}

// 검기 / 글로우 슬래시용 임시 메쉬 GameObject. CreateIndicatorObject 와 거의 동일하지만
//   초기 emissive 를 호출자가 지정 (원소색). Behavior 가 lifetime/스케일/페이드 트래킹.
GameObject* EnemySpawner::SpawnSlashMesh(CRoom* pRoom, Mesh* pMesh,
                                          const XMFLOAT3& pos,
                                          const XMFLOAT3& rotDeg,
                                          const XMFLOAT3& scale,
                                          const XMFLOAT4& emissive,
                                          const std::string& strSlashTexture)
{
    if (!m_pDevice || !m_pCommandList || !m_pScene || !pMesh || !m_pShader) return nullptr;

    CRoom* pPrev = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(pRoom);
    GameObject* pObj = m_pScene->CreateGameObject(m_pDevice, m_pCommandList);
    m_pScene->SetCurrentRoom(pPrev);
    if (!pObj) return nullptr;

    TransformComponent* pT = pObj->GetTransform();
    if (pT)
    {
        pT->SetPosition(pos);
        pT->SetRotation(rotDeg.x, rotDeg.y, rotDeg.z);
        pT->SetScale(scale);
    }

    pMesh->AddRef();
    pObj->SetMesh(pMesh);

    // diffuse / ambient 어둡게 → emissive 가 라이팅 무시하고 환하게 표시
    MATERIAL mat;
    mat.m_cAmbient  = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    mat.m_cDiffuse  = XMFLOAT4(0.0f, 0.0f, 0.0f, emissive.w);
    mat.m_cSpecular = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    mat.m_cEmissive = emissive;
    pObj->SetMaterial(mat);

    // 슬래시 알파 마스크 텍스처 (옵션) — Kenney scratch_01 등. 셰이더가 sample alpha 로 검기 모양 변형.
    if (!strSlashTexture.empty())
    {
        pObj->SetTextureName(strSlashTexture.c_str());
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
        m_pScene->AllocateDescriptor(&cpuHandle, &gpuHandle);
        pObj->LoadTexture(m_pDevice, m_pCommandList, cpuHandle);
        pObj->SetSrvGpuDescriptorHandle(gpuHandle);
    }

    auto* pRC = pObj->AddComponent<RenderComponent>();
    pRC->SetMesh(pMesh);
    pRC->SetCastsShadow(false);    // ellipse 들의 그림자가 ground 에 spike fan 형성하던 문제 차단
    // ★ Overlay 플래그 — opaque PSO 는 BlendEnable=FALSE 라 decalA=0 영역에 검은색이 그대로 덮임.
    //   indicator PSO 는 SrcAlpha/InvSrcAlpha 블렌딩 + DepthWrite=ZERO 라 슬래시 알파가 정상 반영.
    pRC->SetOverlay(true);
    m_pShader->AddRenderComponent(pRC);

    // 첫 프레임 CB 동기화 (Room 이 Inactive 일 때 Update skip 되어 ZeroMemory CB 방지)
    pT->Update(0.0f);
    pObj->Update(0.0f);

    // SetDecal — 라이팅 우회 + emissive 가산 표시. 글로우 슬래시 핵심.
    pObj->SetDecal(true);

    return pObj;
}

GameObject* EnemySpawner::CreateIndicatorObject(CRoom* pRoom, Mesh* pMesh)
{
    if (!m_pDevice || !m_pCommandList || !m_pScene || !pMesh || !m_pShader) return nullptr;

    CRoom* pPrevRoom = m_pScene->GetCurrentRoom();
    m_pScene->SetCurrentRoom(pRoom);

    GameObject* pIndicator = m_pScene->CreateGameObject(m_pDevice, m_pCommandList);

    m_pScene->SetCurrentRoom(pPrevRoom);

    if (!pIndicator) return nullptr;

    // Start hidden (below ground)
    TransformComponent* pTransform = pIndicator->GetTransform();
    if (pTransform)
    {
        pTransform->SetPosition(0.0f, -1000.0f, 0.0f);
    }

    // Set mesh
    pMesh->AddRef();
    pIndicator->SetMesh(pMesh);

    // 초기 머티리얼 — 매 프레임 EnemyComponent 가 덮어쓰므로 첫 프레임용 기본값.
    // 톤다운: emissive 1.6 → 0.6 (이전엔 bloom 과다, 스티커 느낌 가속).
    MATERIAL redMaterial;
    redMaterial.m_cAmbient  = XMFLOAT4(0.3f, 0.05f, 0.02f, 1.0f);
    redMaterial.m_cDiffuse  = XMFLOAT4(0.85f, 0.20f, 0.12f, 1.0f);
    redMaterial.m_cSpecular = XMFLOAT4(0.0f,  0.0f,  0.0f,  1.0f);
    redMaterial.m_cEmissive = XMFLOAT4(0.60f, 0.12f, 0.05f, 1.0f);
    pIndicator->SetMaterial(redMaterial);

    // Add render component — 오버레이 플래그로 맨 위에 렌더 (depth=LESS 유지, 알파 블렌딩).
    auto* pRenderComp = pIndicator->AddComponent<RenderComponent>();
    pRenderComp->SetMesh(pMesh);
    pRenderComp->SetOverlay(true);
    m_pShader->AddRenderComponent(pRenderComp);

    // FIX: Room이 Inactive일 때 Room::Update가 early return하여 인디케이터의
    // GameObject::Update가 호출되지 않아 CBV가 ZeroMemory 상태로 렌더링되는 버그 방지.
    // 생성 직후 Transform과 CB를 한 번 강제 동기화.
    pIndicator->GetTransform()->Update(0.0f);
    pIndicator->Update(0.0f);

    // 데칼 플래그 — 셰이더 bIsDecal 패스에서 lighting 우회 + UV V축 soft edge 적용.
    // 반드시 Update 후 호출 (Update 안에서 CB가 ZeroMemory 될 수 있음).
    pIndicator->SetDecal(true);

    return pIndicator;
}

void EnemySpawner::SetupAttackIndicators(GameObject* pEnemy, EnemyComponent* pEnemyComp,
                                          const AttackIndicatorConfig& config, CRoom* pRoom)
{
    pEnemyComp->SetIndicatorConfig(config);

    if (config.m_eType == IndicatorType::Circle)
    {
        // 테두리 링 (고정 크기, 공격 범위 윤곽)
        GameObject* pBorder = CreateIndicatorObject(pRoom, m_pRingMesh);
        if (pBorder)
        {
            pEnemyComp->SetHitZoneIndicator(pBorder);
        }
        // 내부 fill 원판 (windup 동안 0→1 차오름)
        GameObject* pFill = CreateIndicatorObject(pRoom, m_pDiscMesh);
        if (pFill)
        {
            pEnemyComp->SetHitZoneFillIndicator(pFill);
        }
    }
    else if (config.m_eType == IndicatorType::ForwardBox)
    {
        // 전방 직사각형: 외곽 flat box (border 역할 — 살짝 큼) + 내부 fill (차오름)
        GameObject* pBorder = CreateIndicatorObject(pRoom, m_pBoxMesh);
        if (pBorder)
        {
            pEnemyComp->SetHitZoneIndicator(pBorder);
        }
        GameObject* pFill = CreateIndicatorObject(pRoom, m_pBoxMesh);
        if (pFill)
        {
            pEnemyComp->SetHitZoneFillIndicator(pFill);
        }
    }
    else if (config.m_eType == IndicatorType::RushCircle)
    {
        // Rush + 360 AoE: line + ring at destination
        GameObject* pLine = CreateIndicatorObject(pRoom, m_pLineMesh);
        if (pLine)
        {
            pEnemyComp->SetRushLineIndicator(pLine);
        }

        GameObject* pHitZone = CreateIndicatorObject(pRoom, m_pRingMesh);
        if (pHitZone)
        {
            pEnemyComp->SetHitZoneIndicator(pHitZone);
        }
    }
    else if (config.m_eType == IndicatorType::RushCone)
    {
        // Rush + cone: line + fan at destination
        GameObject* pLine = CreateIndicatorObject(pRoom, m_pLineMesh);
        if (pLine)
        {
            pEnemyComp->SetRushLineIndicator(pLine);
        }

        GameObject* pHitZone = CreateIndicatorObject(pRoom, m_pFanMesh);
        if (pHitZone)
        {
            pEnemyComp->SetHitZoneIndicator(pHitZone);
        }
    }
}

EnemySpawner::NetBossIndicatorSet EnemySpawner::CreateNetBossIndicators()
{
    // 네트워크 보스용 — Room 소속 X (전역 GameObject). CreateIndicatorObject 가 nullptr Room 도 처리.
    NetBossIndicatorSet set;
    set.circleBorder = CreateIndicatorObject(nullptr, m_pRingMesh);
    set.circleFill   = CreateIndicatorObject(nullptr, m_pDiscMesh);
    set.boxBorder    = CreateIndicatorObject(nullptr, m_pBoxMesh);
    set.boxFill      = CreateIndicatorObject(nullptr, m_pBoxMesh);
    return set;
}
