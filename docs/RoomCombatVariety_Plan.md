# 일반 몬스터 룸 전투 다양성 — 작업 계획

> 교수님 피드백: 일반 몬스터가 나오는 방들에서 전투 경험이 너무 똑같고 밋밋함.
> 본 문서는 그 해결을 위한 작업 분해(Tier 1 + Tier 2 + 신규 공격 타입)와 실행 순서를 정리한다.

## 0. 작성 배경 / 제약

- 멀티플레이 동기화가 전제 → **모든 변경은 서버 측 반영 필요**
- `RuneCode_Server`는 공동 관리 → 작업 단위마다 서버 담당자 전달용 요약 포함
- 우선순위: 서버 부담 ↓ + 체감 ↑ 항목부터

---

## 1. 작업 단위 분해

### Tier 1 — 서버 부담 최소 (기존 스폰/스탯 시스템 재활용)

| # | 작업 | 내용 | 서버 변경 | 추정 |
|---|---|---|---|---|
| 1.1 | **웨이브 스폰** | `RoomSpawnConfig`에 wave 배열 추가. 트리거: 타이머 / N마리 처치 / 직전 웨이브 클리어. 한 방에서도 페이즈 변화 체감 | 스폰 패킷에 `wave_index` 필드 1개 추가, 트리거 판정은 서버가 결정 | 2~3d |
| 1.2 | **엘리트 변형** | spawn 시 stat 멀티플라이어 (HP×2 / 속도×1.2 / 데미지×1.5). 외곽 발광 등 시각 강조 | spawn 패킷에 `is_elite` bool + (옵션) stat_multiplier 묶음. AI 로직 변경 X | 1~2d |
| 1.3 | **룸별 spawn mix 다양화** | preset 조합을 룸 타입별로 다르게 (rush 위주 / 원거리 위주 / 혼합 / 엘리트 섞임) | 룸 JSON에 spawn pool 다양화 — 프로토콜 무변경 | 1d |
| 1.4 | **단순 modifier (room affix)** | 룸 진입 시 affix 1개 표시. 종류: 분노(공속×1.5) / 재생(HP 회복) / 탱크(HP×1.5 dmg↓). 플레이어 전략 강제 | 룸 상태에 `modifier` enum 필드 + 스폰 시 stat 적용 | 2d |

### Tier 2 — 중간 (기존 패턴 시스템 확장)

| # | 작업 | 내용 | 서버 변경 | 추정 |
|---|---|---|---|---|
| 2.1 | **환경 hazard** | 보스 패턴 zone-damage 시스템 재활용. 종류: 용암 웅덩이(영구) / 가시 함정(주기) / 낙석(랜덤 위치) | hazard zone 스폰 패킷 + 주기 데미지 (기존 보스 시스템 응용) | 3~5d |
| 2.2 | **부서지는 오브젝트** | Interactable 확장. 보물통(드롭) / 폭발통(2.1 hazard 트리거) | HP-tracked Interactable + 파괴 패킷 추가 | 2~3d |
| 2.3 | **룸 클리어 조건 분기** | 표준 / 생존(N초 버티기, 무한 스폰) / 시간 제한(빨리 깨면 보너스) / 챔피언(미니보스 1마리) | Room state에 `room_type` 필드 + 클리어 조건 variant | 4~5d |

### 신규 몬스터 공격 타입 (가벼운 것부터)

기존 `IAttackBehavior` 그대로 확장 — 적의 `m_fnCreateAttack` / `m_fnCreateSpecialAttack`에 끼워넣기.

| # | 클래스 | 컨셉 | 서버 부담 | 추정 |
|---|---|---|---|---|
| 3.1 | **`ChargedShotBehavior`** (저격수) | 1.5~2s 차징 시 라인 인디케이터 표시 → 강한 단발 직선 발사. 회피는 라인 이탈 | 서버: AI 추가 + 발사 패킷 (기존 ranged 응용) | 2d |
| 3.2 | **`SuicideExplodeBehavior`** (자폭병) | 사거리 진입 시 1.5s 카운트다운 + 깜빡임 → 광역 폭발 (자기도 사망) | death + AoE 처리 (기존 패턴 응용) | 1.5d |
| 3.3 | **`GuardStanceBehavior`** (방패병) | N초마다 가드 자세 (전방 90° 데미지 70% 감소). 측면/뒤는 정상 → 위치잡기 강요 | 클라가 hit angle 판정, 서버는 가드 플래그 sync | 2d |
| 3.4 | **`SplitOnDeathBehavior`** (분열형) | 사망 hook — 작은 적 2~3마리 스폰 | 죽음 패킷 받아 서버가 미니 스폰 | 1.5d |
| (3.5) | **`HealAuraBehavior`** (힐러) | 주변 아군 HP 회복 — *서버에 buff 시스템 필요* | 무거움, 별도 합의 | 4d+ |
| (3.6) | **`DebuffShotBehavior`** (디버프 사수) | 맞으면 느려짐/스턴 — *상태이상 시스템 필요* | 무거움, 별도 합의 | 5d+ |

**3.5/3.6은 이번 범위 제외 권장.** buff/debuff 시스템 부재 상태에서 들어가면 서버 작업 부피 큼. 도입 시 별도 협의.

---

## 2. 실행 순서 권장

```
주차 1   →  1.1 웨이브 스폰 + 1.2 엘리트 변형            (서버 무부담, 즉시 체감↑)
주차 2   →  3.1 ChargedShot + 3.2 SuicideExplode        (적 다양성 핵심)
주차 3   →  1.3 mix 다양화 + 1.4 modifier + 3.3 GuardStance
주차 4   →  2.1 환경 hazard (보스 패턴 재활용)
주차 5+  →  2.2 부서지는 오브젝트, 2.3 룸 타입 분기, 3.4 SplitOnDeath
```

### 결정 포인트
- **3.5 / 3.6 (Heal / Debuff)**: 별도 합의 후 도입. 본 로드맵에 포함 X.
- **각 단계 종료 시점에 체감 평가 후 다음 단계 진입 판단** — 한꺼번에 다 넣지 말고 한 묶음씩 검증.

---

## 3. 서버 담당자 전달 요약

본 작업을 진행하려면 `RuneCode_Server` 측에 아래 변경이 필요. 사전 합의 후 단계적 반영.

### 1차 패킷 / 상태 변경 (Tier 1)
- **1.1 웨이브**: `S_MONSTER_SPAWN` 류 패킷에 `wave_index: uint8` 필드 추가. 웨이브 트리거 판정은 서버 (HP%/처치 수/타이머 중 룸 설정 기반).
- **1.2 엘리트**: 스폰 패킷에 `is_elite: bool` + (옵션) `stat_multiplier: { hp, speed, damage }` 묶음. 엘리트는 같은 archetype에 스탯 강화만.
- **1.3 mix**: 룸 정의 JSON의 spawn pool 다양화만 — 프로토콜 무변경. 서버는 그대로 룸 설정 따라 스폰.
- **1.4 modifier**: 룸 상태(`S_ROOM_STATE` 류)에 `modifier: enum {None, Frenzy, Regen, Tank}` 필드 추가. 스폰 시 modifier에 따라 스탯 보정.

### 2차 (Tier 2)
- **2.1 환경 hazard**: 보스 attack 패턴 zone-damage 시스템(`S_BOSS_ATTACK_*`)을 일반 룸 hazard로 분기 적용 가능한지 검토. 가능하면 hazard ID 기반 zone-damage 패킷 일반화.
- **2.2 부서지는 오브젝트**: Interactable에 `HP` + `OnBreak` 액션 추가. 파괴 시 보상 드롭 / 폭발 효과 트리거. 패킷: `S_INTERACTABLE_BREAK`.
- **2.3 룸 타입**: Room state에 `room_type: enum {Standard, Survival, TimeAttack, Champion}` + 클리어 조건 variant. 생존은 무한 스폰 루프, TimeAttack은 타이머 sync.

### 3차 (신규 공격 타입)
각 신규 attack은 클라/서버 동일 behavior 클래스 작성. 발동/판정은 기존 보스 attack과 동일 패턴.

- **3.1 ChargedShot**: 차징 시작/완료 + 발사 패킷. 라인 텔레그래프 sync.
- **3.2 SuicideExplode**: 카운트다운 시작 / 폭발(=죽음 + AoE) 패킷. 자기 사망 처리.
- **3.3 GuardStance**: 가드 진입/해제 플래그. 데미지 계산 시 hit angle 판정 (클라 또는 서버).
- **3.4 SplitOnDeath**: 적 사망 시 서버가 미니 적 N마리 스폰 (기존 스폰 패킷 재사용).

---

## 4. 클라 측 구현 메모

### 1.1 웨이브 — `RoomSpawnConfig` 확장
```cpp
struct WaveData {
    std::vector<EnemySpawnEntry> enemies;
    enum class TriggerType { Immediate, AfterTimer, AfterKillN, AfterPrevClear };
    TriggerType triggerType;
    float       timerValue = 0.0f;
    int         killThreshold = 0;
};
struct RoomSpawnConfig {
    std::vector<WaveData> waves;   // size 1 = 기존 동작과 동일
    // ...
};
```
`CRoom::Update`에서 wave 인덱스 추적, 트리거 조건 만족 시 다음 웨이브 스폰.

### 1.2 엘리트 — `EnemySpawnData` 확장
```cpp
struct StatMultipliers { float hp=1, speed=1, damage=1; };
EnemySpawner::SpawnEnemy(... bool bIsElite, const StatMultipliers& mul ...);
```
엘리트는 외곽 발광 (RenderComponent에 emissive boost 또는 별도 outline pass).

### 1.4 modifier — `Room` 멤버
```cpp
enum class RoomModifier { None, Frenzy, Regen, Tank };
RoomModifier m_eModifier = RoomModifier::None;
```
방 진입 시 UI 표시 (스킬 알림 시스템 재활용). 스폰 시 modifier 따라 stat 적용.

### 2.1 hazard — `EnvironmentalHazard` 클래스
보스의 `GroundRuptureAttackBehavior` / `ShockwaveRingAttackBehavior` 패턴 응용. 무한 지속하는 zone (용암) 또는 주기적 활성화(가시 함정).

### 3.x attack behaviors
기존 `IAttackBehavior` 그대로 상속. 신규 파일 → `gaym.vcxproj` 등록.

---

## 5. 진행 상황 추적

| 단계 | 항목 | 상태 | 비고 |
|---|---|---|---|
| 주차 1 | 1.1 웨이브 스폰 | 미착수 | |
| 주차 1 | 1.2 엘리트 변형 | 미착수 | |
| 주차 2 | 3.1 ChargedShot | 미착수 | |
| 주차 2 | 3.2 SuicideExplode | 미착수 | |
| 주차 3 | 1.3 mix 다양화 | 미착수 | |
| 주차 3 | 1.4 modifier | 미착수 | |
| 주차 3 | 3.3 GuardStance | 미착수 | |
| 주차 4 | 2.1 환경 hazard | 미착수 | |
| 주차 5+ | 2.2 부서지는 오브젝트 | 미착수 | |
| 주차 5+ | 2.3 룸 타입 분기 | 미착수 | |
| 주차 5+ | 3.4 SplitOnDeath | 미착수 | |
| 보류 | 3.5 HealAura | 보류 | buff 시스템 합의 필요 |
| 보류 | 3.6 DebuffShot | 보류 | 상태이상 시스템 합의 필요 |
