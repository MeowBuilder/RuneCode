# 서버 보스 로직 보강 참고 문서 (오프라인 클라 기준)

## 목적
현재 서버(`RuneCode_Server/GameServer/Monster.cpp`)의 보스 패턴 로직은 거리 기반 단일 분기로 단순화되어 있어, 클라 오프라인 모드의 다양한 패턴/페이즈/타이밍을 재현하지 못합니다. 이 문서는 서버에서 각 보스의 오프라인 거동을 동일하게 구현할 때 클라 어떤 파일/라인을 참고하면 되는지 정리합니다.

## 클라 보스 시스템 구조 요약
- **`EnemyComponent`** (gaym/EnemyComponent.cpp): 보스/적 FSM(Idle→Chase→Attack→Stagger→Dead) + 페이즈 컨트롤러 호출
- **`BossPhaseController`** (gaym/BossPhaseController.cpp): HP% 기준으로 페이즈 자동 전환, 페이즈별 공격 팩토리(`m_fnPrimaryAttack`/`m_fnSpecialAttack`/`m_fnFlyingAttack`/`m_fnTransitionAttack`) 호출
- **`IAttackBehavior`** (gaym/IAttackBehavior.h): 공격 1회의 lifecycle (`Execute → Update(dt) → IsFinished`). windup → execute → recovery 페이즈 분리.
- **`EnemySpawner`** (gaym/EnemySpawner.cpp): 보스별 preset 등록 — 메쉬/애니/스탯/페이즈/공격 팩토리를 한 곳에 묶음

서버는 `IAttackBehavior` 그대로 포팅할 필요는 없습니다. 핵심은 **(1) 페이즈 전환 트리거 (HP%), (2) 페이즈별 공격 풀 + 확률, (3) 공격별 windup/duration/cooldown 타이밍** 을 동일하게 맞추는 것.

## 1. Red Dragon (`MonsterType::Dragon = 6`)

### 페이즈 구성
| 페이즈 | HP 임계값 | 속도 배수 | 공격속도 배수 | 비행 가능 | 특수기 확률 | 비행 확률 |
|--------|----------|----------|--------------|----------|------------|----------|
| 1 | 100~70% | 1.0x | 1.0x | X | 35% | - |
| 2 | 70~35% | 1.3x | 0.85x | O | 45% | 55% |
| 3 | 35~0% | 1.6x | 0.7x | O | 55% | 65% |

**참고 파일**: `gaym/EnemySpawner.cpp` 라인 178~384

### Phase 1 — Ground Combat
- **Primary**: `BreathAttackBehavior(damage=30, speed=35, count=4, spread=45°, windup=0.4s, breathDur=0.8s, recovery=0.3s)`
- **Special (50/50)**:
  - `JumpSlamAttackBehavior(damage=45, jumpHeight=10, jumpDur=0.45, slamRadius=7, windup=0.25, recovery=0.4, trackTarget=true)`
  - `ComboAttackBehavior::CreateLightCombo()` (3연타)
- 참고 라인: `EnemySpawner.cpp:231~256`

### Phase 2 — Aerial Assault
- **Primary**: `BreathAttackBehavior(35, 38, 5, 50°, 0.35, 0.75, 0.25)`
- **Special (50/50)**: JumpSlam(50dmg) 또는 `ComboAttackBehavior::CreateHeavyCombo()`
- **Flying (50/50)**:
  - `FlyingStrafeAttackBehavior(30, 22, 18, 22, 0.15, 2, 12, 0.4, 0.4)` — 직선 활공+2발 burst
  - `FlyingCircleAttackBehavior(28, 22, 18, 100, 280, 0.18, 2, 12, 0.4, 0.4)` — 원형 비행+2발/턴
- **Transition Attack** (페이즈 진입 시 1회): `MegaBreathAttackBehavior(15, 0.2, 20, 3.0, 5.5, 6.5, 1.2, 3.0)` — 벽 이동 + 5.5s 충전 + 6.5s 분사 + 1.2s 회복
- `bInvincibleDuringTransition = true`
- 참고 라인: `EnemySpawner.cpp:258~313`

### Phase 3 — Fury Mode
- **Primary**: `BreathAttackBehavior(42, 42, 6, 55°, 0.25, 0.65, 0.2)` — 더 빠른 분사
- **Special (50/50)**: `FuryCombo` 또는 JumpSlam(60dmg)
- **Flying (4종 균등)**:
  - `DiveBombAttackBehavior(38, 32, 36, 40, 7, 3, 0.1, 18, 0.4, 0.25)` — 급강하 폭격
  - `FlyingSweepAttackBehavior(28, 28, 18, 28, 100, 200, 0.08, 2, 10, 0.35, 0.35)` — 일직선 휩쓸기
  - `FlyingBarrageAttackBehavior(42, 20, 6, 3, 0.35, 16, 0.5, 0.5)` — 대량 탄막
  - `FlyingStrafeAttackBehavior(33, 26, 22, 25, 0.12, 2, 12, 0.35, 0.35)` — 빠른 활공
- **Transition Attack**: `MegaBreathAttackBehavior(25, 0.15, 25, 2.5, 4.8, 7.5, 1.0, 3.5)` — 강화 버전
- 참고 라인: `EnemySpawner.cpp:315~382`

### 핵심 참고 클라 파일
- 페이즈 팩토리: `EnemySpawner.cpp:178~384` (Dragon preset 전체)
- BreathAttackBehavior 구현: `BreathAttackBehavior.cpp` (windup→breath→recovery 상태머신, projectile fan)
- JumpSlamAttackBehavior 구현: `JumpSlamAttackBehavior.cpp` (windup→jump→slam→recovery, AoE damage at slam frame)
- MegaBreathAttackBehavior 구현: `MegaBreathAttackBehavior.cpp` (TakeOff→MoveToWall→Landing→SpawnCover→Windup→Breath→Recovery→Return) — **벽 이동 컷씬** 포함
- ComboAttackBehavior 구현: `ComboAttackBehavior.cpp` (`CreateLightCombo/HeavyCombo/FuryCombo` 정적 팩토리에 hits 정의)
- 비행 패턴들: `FlyingStrafeAttackBehavior.cpp`, `FlyingCircleAttackBehavior.cpp`, `DiveBombAttackBehavior.cpp`, `FlyingSweepAttackBehavior.cpp`, `FlyingBarrageAttackBehavior.cpp`

### 서버 측 마이그레이션 우선순위
1. **페이즈 자동 전환** (HP 70%, 35%) — 이미 부분 구현됨
2. **페이즈별 패턴 풀** — 현재는 거리 기반 3패턴 고정. Phase 1/2/3 마다 다른 풀로 분기 필요.
3. **공격별 정확한 windupSec** — 클라 IAttackBehavior 의 windup 값 그대로 (Breath 0.4, JumpSlam 0.25 등)
4. **MegaBreath 의 다단계 시퀀스** — 벽 이동/충전/분사/회복 단계 모두 별도 패킷으로 클라에 알려야 컷씬 재현 가능 (현재는 단일 attackType=6 만 보냄)
5. **비행 모드 진입/종료** — Phase 2/3 진입 시 비행 상태 진입 패킷 필요

## 2. Blue Dragon (`MonsterType::BlueDragon = 10`)

물 보스 Phase 1 (이후 Kraken 으로 전환됨).

**참고 파일**: `gaym/EnemySpawner.cpp` 라인 720~782

### 단일 페이즈 (HP 80)
- **Primary**: `BreathAttackBehavior(32, 38, 5, 50°, 1.0, 1.2, 0.5, 1.0, 3.0, ElementType::Water, "Fireball Shoot")`
  - **windup 1.0s** (Red Dragon 보다 길게 — 뚱뚱한 몸집 표현)
  - 클립: `"Fireball Shoot"` 명시 오버라이드
- **Special Cooldown**: 3.0s, 확률 60% (4종 랜덤)
  - `TailSweepAttackBehavior(32, 0.65, 0.45, 0.6, 10, 200°, true)`
  - `JumpSlamAttackBehavior(42, 6, 0.7, 8, 0.5, 0.7, true)` — 묵직한 점프
  - `ComboAttackBehavior::CreateHeavyCombo()` — 묵직한 3연타
  - `RushFrontAttackBehavior(40, 10, 1.0, 0.35, 0.35, 0.5, 6, 60)` — 짧은 거리 느린 돌진

### 사망 후 처리 (클라 측)
- `Scene::m_bPendingKrakenSpawn = true` 로 설정 → Kraken 컷씬 트리거 (`Scene.h:290`).
- 서버는 Blue Dragon 사망 시 **별도 패킷**(예: `S_BOSS_PHASE_TRANSITION` 또는 새로운 컷씬 이벤트) 으로 Kraken 등장 트리거 필요.

### 서버 측 차이점
- HP 가 **80** 으로 매우 낮음 — 의도적으로 빠르게 죽고 Kraken 으로 넘어가는 페이즈 1 디자인.
- `m_AnimConfig.m_bLoopAttack = true` — 행동 지속 동안 공격 포즈 유지

## 3. Kraken (`MonsterType::Kraken = 7`)

물 보스 Phase 2 (Blue Dragon 사망 후 등장).

**참고 파일**: `gaym/EnemySpawner.cpp` 라인 386~519

### 단일 페이즈 (HP 1000, MoveSpeed 5.0 — 느릿)
- **Primary**: `BreathAttackBehavior(7, 34, 10, 55°, 0.4, 1.1, 0.2, 0.6, 1.0, ElementType::Water, "Attack_Forward_RM", varied=true)`
  - **10발 잉크 다발** ±55° 스프레이
  - **bVariedProjectiles=true** — 크기/속도/각/발사위치 ±랜덤 (오프라인 BreathAttackBehavior 의 `m_bVariedProjectiles` 플래그)
  - 견제 역할: 자주 쏘지만 데미지 낮음 (7)

- **Special Cooldown**: 4.5s, 확률 70% (4종 가중 랜덤)
  - **35%** TailSweep (`Sweep_Attack` 클립): rect 14×30, dmg 55
  - **25%** 3연타 콤보 (`Sweep_Smash_Attack_3_HIt_Combo`): 30→55→85 데미지, rect 14×30
  - **20%** SideSmashAttackBehavior: 좌/우 45° 틀어 내려찍기, dmg 60, windup 1.2s
  - **20%** 360° 탄막 BreathAttackBehavior: 16발 360° spread, `"Unreal Take"` 클립, dmg 10

- `m_fAttackOriginForwardOffset = 0.0f` — 사각형 판정이 보스 중심부터 전방으로

### 핵심 참고 클라 파일
- Kraken preset 전체: `EnemySpawner.cpp:386~519`
- TailSweepAttackBehavior 의 rect 모드: `TailSweepAttackBehavior.cpp` (`m_fRectWidthHalf`/`m_fRectLength` 인자 있을 때 사각형 판정)
- ComboAttackBehavior 의 `ComboHit` 구조체: `ComboAttackBehavior.h` (히트별 damage/windup/hit/recovery 시간)
- SideSmashAttackBehavior: `SideSmashAttackBehavior.cpp` (Side::Left/Right 자동 선택, 회피 방향 강제)
- BreathAttackBehavior 의 varied 분기: `BreathAttackBehavior.cpp` 의 `FireBreathProjectile` 내 `m_bVariedProjectiles` 분기

### 서버 측 마이그레이션 핵심
1. **bVariedProjectiles** 모드의 랜덤 변주 — 서버는 attackType 만 보내므로, 변주값(스케일/속도/각도)을 별도 필드로 보내거나 클라가 결정 가능 (시각만)
2. **SideSmash 의 Left/Right 결정** — 서버에서 플레이어 위치 기준 자동 선택 후 attackType + side 정보 같이 전송
3. **Kraken 인트로 컷씬** (`Scene.h:298~309` `KrakenCutsceneStage`) — Rumble→Rise→Burst→Reveal→Roar→Jump→Slam→WaterRise 8단계. 각 단계마다 별도 이벤트 패킷 필요

## 4. Golem (`MonsterType::Golem = 8`)

흙 보스. **완전 고정형** (`m_bStationary=true`, MoveSpeed=0). 방사형 광역 패턴으로만 전장 통제.

**참고 파일**: `gaym/EnemySpawner.cpp` 라인 521~680

### 단일 페이즈 (HP 2500, AttackCooldown 4.5s)
- 애니 재생속도 0.7× ("무거운 석상" 체감)
- `m_bLoopAttack = true`, `m_bLoopStagger = true` — 어떤 행동도 freeze 안 됨

- **Primary**: `JumpSlamAttackBehavior(160, 0, 0.25, 50, 3.8, 1.3, false, 2.5, 0.5, "Golem_battle_attack01_ge")`
  - **windup 3.8s** (애니 slam 피크 ~45% = 3.6s 에 정확히 맞춤)
  - jumpHeight 0 (고정형 — 안 뜀), slamRadius 50 (광역)
  - cameraShake 강도 2.5 / 지속 0.5

- **Special Cooldown 0** (매 공격 시도) **확률 75%** — **6종 균등** (직전 패턴 제외 재추첨)
  - **0**: 점프 진동 — `JumpSlamAttackBehavior(140, 6.5, 1.8, 60, 1.3, 0.7, false, 3.6, 0.7, "Golem_jump_ge", 0.5)`
  - **1**: 광역 내려찍기 — `JumpSlamAttackBehavior(150, 0, 0.3, 85, 3.6, 1.8, false, 3.4, 0.65, "Golem_battle_attack01_ge")` (radius 85!)
  - **2**: 바위 발사 (`RockBarrageAttackBehavior`) — 12개 바위 궤도 → 발사, summon 2.6s + charge 0.6s + fire interval 0.18s + recovery 2.2s
  - **3**: 바위 낙하 (`RockFallAttackBehavior`) — 10개 바위 (반경 18~65) windup 2.0s drop 0.8s recovery 2.0s
  - **4**: 십자/X 균열 (`GroundRuptureAttackBehavior`) — Cross 또는 XDiag 랜덤, 길이 70 반폭 4, windup 2.2s impact 0.6s
  - **5**: 순차 십자 폭발 (`SequentialCrossAttackBehavior`) — 0°/30°/60° 3개 십자가 0.65s 간격 폭발, 막대 반길이 70 반폭 9

### 핵심 참고 클라 파일
- Golem preset: `EnemySpawner.cpp:521~680`
- JumpSlamAttackBehavior: `JumpSlamAttackBehavior.cpp`
- RockBarrageAttackBehavior: `RockBarrageAttackBehavior.cpp` (Summon→Charge→Fire→Recovery 단계, orbit motion → 발사)
- RockFallAttackBehavior: `RockFallAttackBehavior.cpp` (낙하 위치 사전 표시 → 떨어짐)
- GroundRuptureAttackBehavior: `GroundRuptureAttackBehavior.cpp` (Cross 또는 XDiag 형태 균열)
- SequentialCrossAttackBehavior: `SequentialCrossAttackBehavior.cpp` (시간차 폭발)

### 서버 측 마이그레이션 핵심
1. **고정형 보스** — 서버에서 MoveSpeed=0 + isStationary 플래그
2. **애니 재생 속도** — 서버가 클라에 0.7× 재생속도 통보 필요 (또는 클라가 monsterType=Golem 으로 자동 적용)
3. **6종 패턴 + 직전 패턴 제외** — `static int s_lastIndex = -1; do { r = rand() % 6; } while (r == s_lastIndex);` 로직 그대로 서버에 포팅
4. **RockBarrage/RockFall/GroundRupture/SequentialCross** — 모두 attackType 별도 enum 필요. 현재 서버 enum (`MonsterAttackType`) 에 RockFall, SequentialCross 가 없음. 추가 필요.
5. **windup 정확성** — Golem 의 3.8s windup 같은 긴 텔레그래프가 게임플레이의 핵심. 서버 windupSec 가 실제 클라 애니 피크 타이밍과 정확히 일치해야 데미지가 애니/VFX 와 동기화됨.

## 5. 추가로 확인해야 할 클라 시스템

### BossPhaseConfig (페이즈 팩토리 시스템)
**파일**: `gaym/BossPhaseController.h`, `gaym/BossPhaseController.cpp`
- 페이즈별 `m_fnPrimaryAttack` / `m_fnSpecialAttack` / `m_fnFlyingAttack` / `m_fnTransitionAttack` 람다
- HP% 도달 시 자동 전환 + 전환 공격 호출
- `m_bInvincibleDuringTransition`, `m_fTransitionDuration` 등 페이즈 메타

### IAttackBehavior 인터페이스
**파일**: `gaym/IAttackBehavior.h`
```cpp
virtual void Execute(EnemyComponent*) = 0;       // 공격 시작 (windup 진입)
virtual void Update(float dt, EnemyComponent*) = 0;  // 매 프레임 페이즈 전환 처리
virtual bool IsFinished() const = 0;
virtual const char* GetAnimClipName() const;     // 재생할 클립
virtual float GetTimeToHit() const;              // 인디케이터 fill 시간
virtual float GetIndicatorRadius() const;        // 지면 인디케이터 반경
virtual bool ShouldShowHitZone() const;          // 인디케이터 표시 여부 (360° spread 면 false)
```

서버는 `IAttackBehavior` 인스턴스를 만들 필요 없이, 위 인자들의 값만 패킷에 담아 클라에 전달하면 됨. 이미 있는 `S_MONSTER_ATTACK` 의 `windupSec` 외에 다음을 추가하면 패리티 ↑:
- `attackDuration` (breath duration / sweep duration / barrage duration 등)
- `recoveryTime` (다음 공격까지의 후딜)
- `indicatorRadius` (override 값, 0 이면 preset 기본값)
- `indicatorLength` (ForwardBox 용)
- `cameraShakeIntensity`, `cameraShakeDuration`

### 페이즈 전환 컷씬 (현재 클라만 처리, 서버 패킷 필요)
- **Dragon Phase 2 진입**: 비행 시작 — 서버가 보스 위치를 공중으로 보내야 함 (또는 클라가 자체 처리)
- **Dragon Phase 3 진입**: Fury 모드 — 시각 변화 (붉은 오라 등) 트리거 패킷 필요
- **Blue Dragon → Kraken**: 위 Kraken 항목 참고
- **Golem 페이즈** 없음 (단일 페이즈)

## 정리: 서버 측 우선 작업 체크리스트
- [ ] `MonsterAttackType` enum 확장: `RockFall`, `SequentialCross`, `LightCombo`, `HeavyCombo`, `FuryCombo`, `SideSmashLeft`, `SideSmashRight`, `FlyingStrafe`, `FlyingCircle`, `DiveBomb`, `FlyingSweep`, `FlyingBarrage` 추가
- [ ] `Monster::SelectBossAttack` 페이즈별 분기 (현재 단일 분기)
- [ ] 페이즈별 `specialChance`/`flyingChance` 적용
- [ ] `m_fSpeedMultiplier`/`m_fAttackSpeedMultiplier` 페이즈별 (1.0/1.3/1.6, 1.0/0.85/0.7)
- [ ] `windupSec` 값을 클라 IAttackBehavior 와 정확히 일치시키기
- [ ] `S_MONSTER_ATTACK` 에 `attackDuration`, `recoveryTime`, `indicatorRadius`, `indicatorLength`, `cameraShakeIntensity` 필드 추가 (선택)
- [ ] Golem `attackPlaybackSpeed` 통보
- [ ] Kraken 인트로 8단계 컷씬 패킷 시퀀스
