#pragma once
#include "stdafx.h"
#include "EnemyComponent.h"
#include "IAttackBehavior.h"
#include <string>
#include <functional>
#include <memory>

class BossPhaseConfig;

struct EnemySpawnData
{
    // Visual
    std::string m_strMeshPath;          // Empty = use CubeMesh
    std::string m_strAnimationPath;     // Empty = no animation
    std::string m_strTexturePath;       // Empty = no texture override
    // 프레임명 substring → 텍스처 경로 매핑. 매칭 시 default 대신 사용.
    //   예: {{"Sword", "T_Skin3_Sword.png"}, {"Helm", "T_Skin3_Helm.png"}}
    //   첫 매치 우선. 매칭 없는 프레임은 m_strTexturePath 사용.
    std::vector<std::pair<std::string, std::string>> m_vTextureOverrides;
    XMFLOAT3 m_xmf3Scale = { 1.0f, 1.0f, 1.0f };
    XMFLOAT4 m_xmf4Color = { 1.0f, 0.0f, 0.0f, 1.0f }; // Red by default

    // Stats
    EnemyStats m_Stats;

    // Attack behavior factory
    std::function<std::unique_ptr<IAttackBehavior>()> m_fnCreateAttack;

    // Special attack behavior factory (for bosses)
    std::function<std::unique_ptr<IAttackBehavior>()> m_fnCreateSpecialAttack;

    // Animation config
    EnemyAnimationConfig m_AnimConfig;

    // Attack indicator config
    AttackIndicatorConfig m_IndicatorConfig;

    // Flying mode
    bool m_bIsFlying = false;
    float m_fFlyHeight = 15.0f;

    // Stationary mode — 고정형 보스 (이동/회전 모두 봉쇄, 방사형 패턴 전용)
    bool m_bStationary = false;
    float m_fRotationSpeed = 180.0f;   // 기본 180 deg/s (Stationary 에선 무시됨)

    // 애니메이션 재생속도 — 1.0 기본, 낮추면 무거운 느낌 (골렘 등), 높이면 빠른 느낌
    float m_fAnimationPlaybackSpeed = 1.0f;

    // Collider override (0이면 scale 기반 자동 계산)
    float m_fColliderXZMultiplier = 0.0f;

    // 공격 원점 forward 오프셋 (크라켄처럼 몸 앞쪽 촉수가 실제 공격 원점인 경우)
    float m_fAttackOriginForwardOffset = 0.0f;

    // Boss settings
    bool m_bIsBoss = false;
    float m_fSpecialAttackCooldown = 10.0f;
    float m_fFlyingAttackCooldown = 12.0f;
    int m_nSpecialAttackChance = 30;  // Percentage (0-100)
    int m_nFlyingAttackChance = 30;   // Percentage (0-100)

    // Boss phase config factory (for multi-phase bosses)
    std::function<std::unique_ptr<BossPhaseConfig>()> m_fnCreateBossPhaseConfig;

    // Type marker VFX (적 타입 식별용 상시 표시). 빈 문자열이면 마커 없음.
    std::string m_strHeadMarkerEffect;
    std::string m_strFootMarkerEffect;

    // 메쉬 기반 타입 마커용 attackType ID. EnemySpawner 에서 색/크기 매핑에 사용.
    std::string m_strAttackTypeId;

    // 중간보스 플래그 — true 면 마커 크기·색 강조 (방 중앙 단독 배치되는 강한 적)
    bool m_bIsMiniBoss = false;

    // Constructor with defaults for test enemy
    EnemySpawnData()
    {
        m_Stats.m_fMaxHP = 100.0f;
        m_Stats.m_fCurrentHP = 100.0f;
        m_Stats.m_fMoveSpeed = 5.0f;
        m_Stats.m_fAttackRange = 3.0f;
        m_Stats.m_fAttackCooldown = 2.0f;
        m_Stats.m_fDetectionRange = 50.0f;
    }
};

// 한 웨이브의 스폰 정보. 다중 웨이브 룸에서 사용.
//   trigger 는 직전 웨이브 대비 조건 (첫 웨이브는 Immediate 권장).
//   AfterTimer:        직전 웨이브 스폰 시점부터 fTriggerValue 초 경과
//   AfterPrevCleared:  직전 웨이브의 적이 전부 사망
//   AfterKillN:        직전 웨이브에서 nKillThreshold 마리 처치
struct EnemyWave
{
    enum class TriggerType
    {
        Immediate,
        AfterTimer,
        AfterPrevCleared,
        AfterKillN
    };

    std::vector<std::pair<std::string, XMFLOAT3>> spawns;
    TriggerType trigger        = TriggerType::Immediate;
    float       fTriggerValue  = 0.0f;   // AfterTimer 의 초
    int         nKillThreshold = 0;      // AfterKillN 의 임계 수

    void AddSpawn(const std::string& presetName, const XMFLOAT3& position)
    {
        spawns.push_back({ presetName, position });
    }
    void AddSpawn(const std::string& presetName, float x, float y, float z)
    {
        spawns.push_back({ presetName, XMFLOAT3(x, y, z) });
    }
};

// Configuration for spawning enemies in a room
//   - 단일 웨이브 (기존): m_vEnemySpawns 만 채우면 됨
//   - 다중 웨이브: m_vWaves 를 채우면 우선 사용됨 (m_vEnemySpawns 는 무시)
struct RoomSpawnConfig
{
    std::vector<std::pair<std::string, XMFLOAT3>> m_vEnemySpawns;   // legacy 호환
    std::vector<EnemyWave>                        m_vWaves;          // 신규 다중 웨이브

    void AddSpawn(const std::string& presetName, const XMFLOAT3& position)
    {
        m_vEnemySpawns.push_back({ presetName, position });
    }

    void AddSpawn(const std::string& presetName, float x, float y, float z)
    {
        m_vEnemySpawns.push_back({ presetName, XMFLOAT3(x, y, z) });
    }

    // 다중 웨이브 추가 헬퍼 — 빈 EnemyWave 를 push 후 reference 리턴
    EnemyWave& AddWave(EnemyWave::TriggerType trigger = EnemyWave::TriggerType::Immediate,
                       float fTriggerValue = 0.0f, int nKillThreshold = 0)
    {
        EnemyWave w;
        w.trigger        = trigger;
        w.fTriggerValue  = fTriggerValue;
        w.nKillThreshold = nKillThreshold;
        m_vWaves.push_back(std::move(w));
        return m_vWaves.back();
    }

    void Clear()
    {
        m_vEnemySpawns.clear();
        m_vWaves.clear();
    }

    // 호환성: m_vWaves 가 비어있으면 m_vEnemySpawns 를 단일 Immediate 웨이브로 취급
    bool HasMultiWave() const { return !m_vWaves.empty(); }
};
