# Dark Hunting Ground (漆黑猎场) — 슬레이트 시스템 분석

## 게임 개요

- **원제**: 漆黑猎场 (Qī Hēi Liè Chǎng)
- **개발사**: BingX / The Bueno Interactive
- **플랫폼**: Windows, macOS (Steam)
- **장르**: Indie Action RPG, Looter ARPG, Roguelike 요소
- **출시**: Early Access 2024.11, 정식 v1.0 2025.09
- **설계 철학**: "군더더기 없는 순수한 전투와 빌드 중심 성장"
- **비교 게임**: Path of Exile (슬레이트 = PoE의 서포트 젬)

---

## 스킬 시스템

### 6개 직업 (클래스)
Warrior / Assassin / Scholar(Mage) / Ranger / Summoner / Guardian

**핵심**: 스킬은 클래스 고정이 아님 — 모든 직업의 스킬을 자유롭게 조합 가능.  
총 18개 핵심 스킬 (Flicker Strike, Slash, Sweep, Stab, Scatter, Shooting, Roll, Flash, Holy Judgement 등)

### 스킬 데미지 공식
```
damage = base × (1 + inc1 + inc2 + ...) × (1 + More1) × (1 + More2) × ...
```
- 같은 버킷 내: 가산 합산
- 버킷 간: 곱산 (PoE와 동일)
- 모든 스킬이 **Skill Power** 하나로 스케일링

### 상태이상 (원소별)
- Fire → Ignite (30% 화염 데미지를 DoT로 추가)
- Cold → Freeze (이동 불가)
- Lightning → Shock (데미지 증폭)
- Physical → Armor Break

---

## 슬레이트 시스템 (핵심)

슬레이트 = 이 게임의 "룬". Path of Exile의 서포트 젬에서 직접 영감.

### 기본 규칙
- 스킬 당 **7개 슬롯** (잠금 해제 방식으로 점진적 개방)
- 슬레이트는 **장착된 스킬에만** 효과 (다른 스킬에 영향 없음)
- **총 60종** 존재
- 드롭 품질: 흰색(기본) ~ 보라(강화) — 낮은 품질도 Skill Power 기본 부여
- **비원소 슬레이트는 중복 장착 가능** (Armor Break x2 = 효과 두 배)
- **원소 슬레이트는 스킬 당 1개만** (중복 무효)

### 슬레이트 카테고리 (6종)

| 카테고리 | 예시 | 설명 |
|---|---|---|
| **원소 변환** | Magma, Lightning Strike, Cold/Frozen, Poison | 스킬에 원소 추가/변환, 스킬 당 1개 한정 |
| **행동 변환** | Charge, Automatic, Turret | 스킬 동작 방식 자체를 바꿈 |
| **수치 증폭** | Crit Rate, Crit Damage, Armor Break, Elemental Disintegration | 스킬 수치 강화 |
| **군중 제어** | Frozen, Trample, ARC | 상태이상/넉백 추가 |
| **경제/지속** | Mana Draw, Cooldown/Cooldown Charges, Haste | 자원/쿨다운 조절 |
| **타격 방식** | Fast, Multi-Hit | 공격 속도/횟수 변경 |

### 주목할 슬레이트들

| 슬레이트 | 효과 | 설계 의의 |
|---|---|---|
| **Magma** | 증폭 스킬 적중 시 마그마 구슬 3개 폭발 (Skill Power 1000%) | 단순 수치가 아닌 새 발사체 생성 |
| **Charge** | 근접 사거리 시 자동으로 적에게 돌진 | 이동 스킬이 아닌 스킬에도 적용 가능 |
| **Automatic** | 스킬/소환수가 자동으로 시전 | 플레이어 입력 없이 동작 변환 |
| **Turret** | 소환수/스킬을 설치형 포탑으로 변환 | 발동 방식을 완전히 재정의 |
| **Slate-Pure** | 스킬을 순수 물리 데미지로 변환 | 원소 제거 |
| **Mana Draw** | 마나 코스트 제거 대신 데미지 감소 | 트레이드오프형 경제 룬 |
| **ARC** | 번개 연쇄 효과 | 타격 범위/전파 방식 변환 |

---

## 시너지 구조

### 스킬 내 시너지 (슬레이트 콤보)
예) Flicker Strike: Crit Rate + Crit Damage + Armor Break + Elemental Disintegration + Cohesion + Lightning Strike  
→ 방어력 깎기 + 번개 변환 + 높은 치명타 = 단일 스킬이 완결된 딜 사이클

### 스킬 간 시너지
- Slash에 Charge 슬레이트 → Flicker Strike의 근접 포지셔닝 해결
- Mikiri(방어기) + Automatic → 메인 스킬이 쿨다운 도는 동안 자동 수동 방어

### 외부 시스템과 시너지
- **렐릭 (80종)**: 스킬 동작 자체를 근본적으로 바꿈
  - "Ornate Crescent Blade": Shooting 스킬을 날아다니는 칼날로 변환
  - "Dancer's Costume": Strafe 스킬 부여
  - "Elemental Summons": 걷는 소환수를 원거리 공격으로 변환
- **Abyssal Eyes (35종)**: 별도 강화 카테고리 (치명타 배율, 데미지 조건부 증폭 등)
- **패시브 트리**: 직업별 수동 스탯 (흡혈, 회피, 분노 등)

---

## VFX와의 관계

픽셀 아트 스타일이라 정교한 VFX 레이어 시스템은 없지만:
- 원소 슬레이트 장착 → 원소별 시각 효과 (불꽃, 얼음 결정, 번개 지시자)
- Magma 슬레이트 → 별도 마그마 구슬 발사체 오브젝트 생성
- Ornate Crescent Blade 렐릭 → 투사체 형태 자체가 칼날로 변경
- **VFX 변화는 순수하게 기능적** — 외관만 바뀌는 코스메틱 레이어 없음

---

## 설계 원칙 요약 (참조용)

1. **자유로운 조합**: 클래스 제한 없이 어떤 스킬에든 어떤 슬레이트든 장착 가능
2. **스킬 독립성**: 슬레이트는 해당 스킬에만 작용
3. **행동 변환 슬레이트**: 수치만 올리는 게 아니라 스킬 동작 방식 자체를 바꾸는 슬레이트가 핵심 빌드 다양성의 원천
4. **중복 허용 + 원소 1개 제한**: 증폭 슬레이트는 중복 가능, 원소만 1개 제한으로 밸런스
5. **트레이드오프**: Mana Draw처럼 뭔가를 얻으면 뭔가를 잃는 구조
6. **외부 시스템(렐릭)과 분리**: 슬레이트 = 스킬 내부 조율, 렐릭 = 스킬 외부 변환
