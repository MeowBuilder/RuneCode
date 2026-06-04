#include "stdafx.h"
#include "BossPhaseController.h"
#include "EnemyComponent.h"
#include "IAttackBehavior.h"
#include "AnimationComponent.h"
#include "Room.h"
#include "GameObject.h"

BossPhaseController::BossPhaseController(EnemyComponent* pOwner)
    : m_pOwner(pOwner)
{
}

void BossPhaseController::SetPhaseConfig(std::unique_ptr<BossPhaseConfig> pConfig)
{
    m_pPhaseConfig = std::move(pConfig);

    // 페이즈 트리거 기록 초기화
    if (m_pPhaseConfig)
    {
        m_vPhaseTriggered.resize(m_pPhaseConfig->GetPhaseCount(), false);
        m_vPhaseTriggered[0] = true;  // 첫 번째 페이즈는 이미 시작됨

        // 첫 번째 페이즈 적용
        if (m_pPhaseConfig->GetPhaseCount() > 0)
        {
            ApplyPhaseData(m_pPhaseConfig->GetPhase(0));
        }
    }
}

void BossPhaseController::Update(float deltaTime)
{
    if (!m_pOwner || !m_pPhaseConfig) return;

    switch (m_eTransitionState)
    {
    case PhaseTransitionState::TransitionAttack:
    {
        // 전환 공격 업데이트
        if (m_pTransitionAttack)
        {
            m_pTransitionAttack->Update(deltaTime, m_pOwner);

            if (m_pTransitionAttack->IsFinished())
            {
                OutputDebugString(L"[BossPhase] Transition attack finished\n");
                m_pTransitionAttack.reset();

                // 전환 애니메이션으로 이동
                const BossPhaseData& nextPhase = m_pPhaseConfig->GetPhase(m_nPendingPhase);
                if (nextPhase.m_fTransitionDuration > 0.0f)
                {
                    m_eTransitionState = PhaseTransitionState::TransitionAnim;
                    m_fTransitionTimer = 0.0f;

                    // 무적 상태 설정
                    if (nextPhase.m_bInvincibleDuringTransition)
                    {
                        m_pOwner->SetInvincible(true);
                    }

                    // 전환 애니메이션 재생
                    AnimationComponent* pAnim = m_pOwner->GetAnimationComponent();
                    if (pAnim && !nextPhase.m_strTransitionAnimation.empty())
                    {
                        pAnim->Play(nextPhase.m_strTransitionAnimation, false);
                    }
                }
                else
                {
                    // 애니메이션 없으면 바로 완료
                    FinishTransition();
                }
            }
        }
        break;
    }

    case PhaseTransitionState::TransitionAnim:
    {
        const BossPhaseData& nextPhase = m_pPhaseConfig->GetPhase(m_nPendingPhase);
        m_fTransitionTimer += deltaTime;

        if (m_fTransitionTimer >= nextPhase.m_fTransitionDuration)
        {
            FinishTransition();
        }
        break;
    }

    default:
        break;
    }
}

void BossPhaseController::OnHealthChanged(float fCurrentHP, float fMaxHP)
{
    if (!m_pPhaseConfig || m_pPhaseConfig->GetPhaseCount() == 0) return;

    // 이미 전환 중이면 무시
    if (IsInTransition()) return;

    float fHealthRatio = (fMaxHP > 0.0f) ? (fCurrentHP / fMaxHP) : 0.0f;
    int nNewPhase = m_pPhaseConfig->GetPhaseIndexForHealth(fHealthRatio);

    // 새로운 페이즈로 전환해야 하는지 확인
    if (nNewPhase > m_nCurrentPhase && !m_vPhaseTriggered[nNewPhase])
    {
#ifdef _DEBUG
        wchar_t buf[128];
        swprintf_s(buf, L"[BossPhase] Health %.1f%% -> Phase %d -> %d\n",
                   fHealthRatio * 100.0f, m_nCurrentPhase, nNewPhase);
        OutputDebugString(buf);
#endif

        TransitionToPhase(nNewPhase);
    }
}

void BossPhaseController::TransitionToPhase(int nNewPhase)
{
    if (!m_pPhaseConfig || nNewPhase < 0 || nNewPhase >= m_pPhaseConfig->GetPhaseCount()) return;

    m_nPendingPhase = nNewPhase;
    m_vPhaseTriggered[nNewPhase] = true;

    const BossPhaseData& nextPhase = m_pPhaseConfig->GetPhase(nNewPhase);

    // 전환 공격이 있으면 먼저 실행
    if (nextPhase.m_bHasTransitionAttack && nextPhase.m_fnTransitionAttack)
    {
        StartTransitionAttack(nextPhase);
    }
    else if (nextPhase.m_fTransitionDuration > 0.0f)
    {
        // 전환 공격 없고 애니메이션만 있으면 바로 애니메이션
        m_eTransitionState = PhaseTransitionState::TransitionAnim;
        m_fTransitionTimer = 0.0f;

        if (nextPhase.m_bInvincibleDuringTransition)
        {
            m_pOwner->SetInvincible(true);
        }

        AnimationComponent* pAnim = m_pOwner->GetAnimationComponent();
        if (pAnim && !nextPhase.m_strTransitionAnimation.empty())
        {
            pAnim->Play(nextPhase.m_strTransitionAnimation, false);
        }
    }
    else
    {
        // 둘 다 없으면 즉시 전환
        FinishTransition();
    }
}

void BossPhaseController::StartTransitionAttack(const BossPhaseData& phase)
{
    if (!m_pOwner) return;

    OutputDebugString(L"[BossPhase] Starting transition attack (e.g. Mega Breath)\n");

    m_eTransitionState = PhaseTransitionState::TransitionAttack;
    m_pTransitionAttack = phase.m_fnTransitionAttack();

    if (m_pTransitionAttack)
    {
        m_pTransitionAttack->Execute(m_pOwner);
    }
}

void BossPhaseController::ApplyPhaseData(const BossPhaseData& phase)
{
    if (!m_pOwner) return;

    // 속도 배율 적용
    // Note: EnemyComponent에 SetSpeedMultiplier 추가 필요
    // m_pOwner->SetSpeedMultiplier(phase.m_fSpeedMultiplier);

    // 특수 공격 확률 설정
    m_pOwner->SetSpecialAttackChance(phase.m_nSpecialAttackChance);

    // 공격 행동 교체
    //   주의: EnemyComponent::Update 가 매 공격마다 m_fnAttackFactory() 로 재생성하기 때문에
    //   factory 까지 페이즈 람다로 교체해야 다음 공격에도 페이즈 패턴이 유지된다.
    if (phase.m_fnPrimaryAttack)
    {
        m_pOwner->SetAttackBehavior(phase.m_fnPrimaryAttack());
        m_pOwner->SetAttackFactory(phase.m_fnPrimaryAttack);
    }

    if (phase.m_fnSpecialAttack)
    {
        m_pOwner->SetSpecialAttackBehavior(phase.m_fnSpecialAttack());
        m_pOwner->SetSpecialAttackFactory(phase.m_fnSpecialAttack);
    }

#ifdef _DEBUG
    wchar_t buf[128];
    swprintf_s(buf, L"[BossPhase] Applied phase data: speed=%.1fx, specialChance=%d%%\n",
               phase.m_fSpeedMultiplier, phase.m_nSpecialAttackChance);
    OutputDebugString(buf);
#endif
}

void BossPhaseController::SpawnPhaseAdds(const BossPhaseData& phase)
{
    if (!phase.m_bSpawnAdds || !m_pRoom) return;

    OutputDebugString(L"[BossPhase] Spawning phase adds...\n");

    // TODO: Room의 EnemySpawner를 통해 쫄 소환
    // for (const auto& [presetName, position] : phase.m_vAddSpawns)
    // {
    //     m_pRoom->SpawnEnemyAt(presetName, position);
    // }
}

void BossPhaseController::FinishTransition()
{
    if (!m_pPhaseConfig || m_nPendingPhase < 0) return;

    const BossPhaseData& nextPhase = m_pPhaseConfig->GetPhase(m_nPendingPhase);

    // 무적 해제
    if (m_pOwner)
    {
        m_pOwner->SetInvincible(false);

        // 상태를 Idle로 리셋하여 애니메이션이 자연스럽게 전환되도록
        m_pOwner->ChangeState(EnemyState::Idle);
    }

    // 페이즈 데이터 적용
    ApplyPhaseData(nextPhase);

    // 쫄 소환
    SpawnPhaseAdds(nextPhase);

    // 상태 업데이트
    m_nCurrentPhase = m_nPendingPhase;
    m_nPendingPhase = -1;
    m_eTransitionState = PhaseTransitionState::TransitionDone;
    m_fTransitionTimer = 0.0f;

#ifdef _DEBUG
    wchar_t buf[64];
    swprintf_s(buf, L"[BossPhase] Transition complete -> Phase %d\n", m_nCurrentPhase);
    OutputDebugString(buf);
#endif

    // 한 프레임 후 None 상태로 복귀 (다음 Update에서 처리)
    m_eTransitionState = PhaseTransitionState::None;
}

bool BossPhaseController::CanFly() const
{
    if (!m_pPhaseConfig || m_nCurrentPhase < 0 ||
        m_nCurrentPhase >= m_pPhaseConfig->GetPhaseCount()) return false;

    return m_pPhaseConfig->GetPhase(m_nCurrentPhase).m_bCanFly;
}

int BossPhaseController::GetFlyingAttackChance() const
{
    if (!m_pPhaseConfig || m_nCurrentPhase < 0 ||
        m_nCurrentPhase >= m_pPhaseConfig->GetPhaseCount()) return 0;

    return m_pPhaseConfig->GetPhase(m_nCurrentPhase).m_nFlyingAttackChance;
}
