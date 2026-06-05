# 스킬/룬 아이콘 HUD 시스템

DirectX 12 기반 아이콘 HUD + 룬 인벤토리 + 룬 획득 모달 UI 문서.

## 1. 배경 / 목적

기존 스킬 HUD는 순수 텍스트였다. `Dx12App::RenderText()`에서 좌하단에
`[Q] Fireball  CD: 1.2s  DMG: 50 [불]` 식 텍스트를, 우하단에 `[Rune Combos]` 텍스트를
그렸고, 룬 획득(방 클리어 드롭)도 텍스트 목록 클릭이었다. 가독성이 낮고 게임 느낌이 약했다.

목표: 전체 룬/스킬 UI를 **아이콘 기반**으로 통일.

- 스킬 아이콘 + 조작키 + **방사형 쿨타임**(회색→시계방향 색 복구) + 장착 룬 아이콘
- **TAB 홀드** → 아이콘 확대 + 마우스 호버 시 스킬/룬 설명 툴팁
- **드래그&드롭** → 룬을 다른 스킬에 장착하거나 룬끼리 위치 교체
- **룬 획득 모달** → 방 클리어 후 룬 선택/장착도 동일한 아이콘 스타일

## 2. 구성 요소

새 클래스 3개를 추가하고 `Dx12App`의 텍스트 UI 코드를 대체했다.

| 파일 | 역할 |
|------|------|
| `gaym/SkillIconRenderer.{h,cpp}` | 저수준 — 아이콘 1장 렌더 + 방사형 쿨타임 셰이더 (자체 PSO) |
| `gaym/SkillHudUI.{h,cpp}` | 좌하단 HUD 레이아웃 + TAB 확대 + 호버 툴팁 + 드래그&드롭 |
| `gaym/RuneRewardUI.{h,cpp}` | 룬 획득 중앙 모달 (룬 선택 / 장착 슬롯 선택) |
| `gaym/Dx12App.{h,cpp}` | 초기화, 매 프레임 Update, RenderText 통합, 입력 라우팅 |

### 2.1 SkillIconRenderer (저수준 렌더러)

`DebugRenderer`와 동일 패턴: 자체 루트시그니처 + 인라인 `D3DCompile` VS/PS + PSO +
`SetGraphicsRoot32BitConstants`. **SpriteBatch와 별개의 패스**로 동작한다.

- **자체 디스크립터 힙**(shader-visible, 192슬롯)을 소유해 공유 폰트 힙(10슬롯 풀)을 건드리지 않음.
- 슬롯 0 = 1×1 흰 텍스처(폴백/단색용). 이후 슬롯에 아이콘 PNG SRV 등록.
- **아이콘 자동 로드**: init 시 `Assets/Textures/SkillIcons`, `RuneIcons` 폴더의 `*.png`를
  스캔해 파일명(확장자 제외)을 키로 등록(`std::filesystem`).
- **폴백**: 키로 PNG를 못 찾으면 흰 텍스처에 폴백색(원소색)을 곱한 단색 타일로 그린다.
  → PNG가 없어도 바로 동작하고, 나중에 폴더에 PNG를 넣으면 자동으로 진짜 아트로 교체.

#### 방사형 쿨타임 셰이더

draw 1회당 루트 32bit 상수 12개 전달: `gRect`(NDC 사각형) + `gTint`(rgba) +
`gCtl`(x=cooldownProgress, y=hasTexture).

픽셀 셰이더 핵심:

```hlsl
float prog = gCtl.x;            // 1=완료(색 100%), 0~1=복구 중
if (prog < 0.999) {
    float2 d = i.uv - 0.5;
    float ang = atan2(d.x, -d.y);                       // 12시=0, 시계방향 +, -pi..pi
    float a01 = ang / 6.2831853 + (ang < 0 ? 1.0 : 0.0); // 0..1 시계방향
    if (a01 > prog) {                                    // 아직 쿨다운인 부채꼴
        float lum = dot(c.rgb, float3(0.299,0.587,0.114));
        c.rgb = lum.xxx * 0.45;                          // 어두운 회색
    }
}
```

`cooldownProgress`는 `SkillComponent::GetCooldownProgress(slot)` (0=막 사용, 1=완료) 값을 그대로 사용.
정점은 정점버퍼 없이 `SV_VertexID` 4개로 트라이앵글 스트립 쿼드를 생성한다.

### 2.2 SkillHudUI (좌하단 HUD)

`Dx12App`가 소유, 매 프레임 `Update()` + 렌더 시 `RenderIcons()`(아이콘 패스) /
`RenderText()`(글자·툴팁 패스) 호출.

- **레이아웃**: 슬롯별 스킬 아이콘 + 조작키(Q/E/R/M2) + 장착 룬 3칸. 하단 기준 정렬.
- **방사형 쿨타임**: 쿨다운 중이면 `cooldownProgress`를 아이콘 draw에 전달.
- **TAB 확대**: `IsKeyDown(VK_TAB)`로 `m_tabZoom`을 0↔1 보간(`dt*12`). `IconScale = 1 + tabZoom*0.95`.
  레이아웃 함수가 확대를 반영하므로 호버 판정도 자동으로 일치.
- **호버 툴팁**: 확대 상태에서 마우스가 올라간 스킬/룬을 강조(금색 테두리) + 툴팁 박스.
  - 스킬: 이름(원소색) / 원소·활성화타입 / `피해 N · 쿨 X.Xs` (장착 룬 반영 `BuildSkillStats`)
  - 룬: 이름+등급(등급색) / 효과 설명(`BuildRuneDesc`, 자동 줄바꿈)
  - 툴팁 배경은 폰트 힙 슬롯3(1×1 흰 픽셀)을 SpriteBatch로 그린 반투명 사각형.
- **드래그&드롭**: 확대 상태에서 룬 칸을 좌클릭으로 집어 끌면 마우스를 따라 반투명 고스트.
  - 다른 룬 칸에 놓기 → 비었으면 이동, 차있으면 **교체(swap)** (스택 보존)
  - 스킬 아이콘에 놓기 → 그 스킬의 첫 빈 룬 칸에 장착
  - 영역 밖 → 취소
  - `SkillComponent::SetRuneSlot/ClearRuneSlot/GetRuneSlot`으로 **로컬** 변경.

### 2.3 RuneRewardUI (룬 획득 모달)

방 클리어 후 룬 드롭을 주울 때(F) 뜨는 중앙 모달. Scene의 상태머신은 그대로 두고 표현만 교체.

- **SelectingRune**: 화면을 어둡게 깔고 룬 3장을 **카드**로 표시(큰 아이콘 + 이름·등급 + 효과 설명).
  호버 카드는 금색 테두리. 클릭 또는 1/2/3 키로 선택.
- **SelectingSkill**: 상단에 선택한 룬, 아래에 4개 스킬 아이콘 + 각 3개 룬 칸 격자.
  룬 칸 호버 시 장착된 룬 툴팁. 칸 클릭으로 장착.

**핵심 설계 — 좌표 단일 소스화**: 카드/슬롯 위치를 계산하는 레이아웃 함수
(`RuneCardRect`, `SkillRuneRect` 등)를 **렌더링과 클릭 히트테스트가 공유**한다.
기존엔 그리는 좌표(`RenderText`)와 클릭 판정 좌표(입력 핸들러)가 따로 하드코딩돼 어긋날 위험이 있었는데,
이제 한 곳에서 관리해 항상 일치한다.

## 3. 렌더링 파이프라인 통합 (Dx12App::RenderText)

아이콘 패스와 SpriteBatch(텍스트) 패스는 **디스크립터 힙과 PSO가 다르므로 분리**한다.

```
RenderText():
  [아이콘 패스] SkillIconRenderer 자체 PSO/힙
    - 모달 비활성: SkillHudUI::RenderIcons (하단 HUD)
    - 모달 활성:   RuneRewardUI::Render*Icons (모달, 화면 어둡게 + 카드/격자)
  ── 폰트 디스크립터 힙 바인딩 ──
  m_spriteBatch->Begin()
    - HealthBar, 상호작용 프롬프트 등
    - 모달 비활성: SkillHudUI::RenderText (조작키/쿨다운/툴팁)
    - 모달 활성:   RuneRewardUI::Render*Text (제목/이름/툴팁)
  m_spriteBatch->End()
```

모달이 열리면 하단 HUD(아이콘+텍스트)는 숨긴다. 입력은 `Dx12App` 입력 핸들러에서
`RuneRewardUI::HitTestRuneOption / HitTestSkillSlot` 결과로 Scene 상태머신
(`SelectRuneByClick`, `SelectSkillSlot`)을 호출한다.

## 4. 에셋 규칙

- 스킬 아이콘: `gaym/Assets/Textures/SkillIcons/<SkillData.name>.png`
  (예: `Fireball.png`, `WaterPuddle.png`, `Tornado.png` …)
- 룬 아이콘: `gaym/Assets/Textures/RuneIcons/<RuneDef.id>.png`
  (예: `F02.png`, `ABY_INF.png` … `RuneRegistry.cpp`의 등록 id 참고)
- 정사각형, 투명 배경 PNG 권장. 파일이 없으면 원소색 단색 타일로 자동 폴백.
- 아이콘 생성 프롬프트: `docs/ImagePrompts_SkillIcons.md`.

## 5. 멀티플레이 동기화 한계 (중요)

드래그&드롭에 의한 룬 **재배치**는 현재 **로컬만** 반영한다. 기존 네트워크
`NetworkManager::SendRuneEquip`은 *드롭 보상 3개 중 optionIndex만* 서버에 보내고
서버가 `pendingRewardRunes`로 검증하는 구조라, 이미 장착된 룬의 임의 재배치를 표현할 수 없다.

→ 멀티플레이에서 재배치를 서버에 반영하려면 runeId 기반 재배치용 신규 프로토콜
(예: `C_RUNE_MOVE`) + 서버 핸들러 추가가 필요하다. **룬 획득 모달**의 정상 장착 경로는
기존 `SendRuneEquip`을 그대로 쓰므로 멀티플레이에서도 동작한다.

## 6. 빌드

```
"C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe" ^
  "gaym\gaym.sln" -p:Configuration=Release -p:Platform=x64 -m -v:minimal
```

신규 `.cpp/.h`는 `gaym.vcxproj`에 등록돼 있다. `C4819/C4828`(한글 코드페이지) 경고는 무해.

## 7. 향후 작업

- [ ] 실제 스킬/룬 아이콘 PNG 제작·삽입 (`docs/ImagePrompts_SkillIcons.md` 프롬프트)
- [ ] 드래그&드롭 재배치 멀티플레이 동기화 프로토콜
- [ ] 차지/채널 게이지의 아이콘 통합 시각화(현재 텍스트)
- [ ] 룬 우클릭 해제 등 추가 인벤토리 조작
