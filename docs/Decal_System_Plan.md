# 지면 데칼(Ground Decal) 시스템 구현 계획

## 목표
불 캐릭터 4가지 스킬(Q WaveSlash, E FireBeam, R Meteor, RC Fireball)이 지면에
VFX 텍스처 기반 스코치 마크/매직 써클을 남기도록 구현한다.

---

## 핵심 설계 결정

### 렌더링 방식
- **PSO**: 기존 Water PSO 재사용 (알파 블렌드 ON, 깊이 쓰기 OFF, 깊이 테스트 ON/LESS)
- **메쉬**: XZ 평면 1×1 사각형(QuadMesh) — 월드 행렬로 크기·회전·위치 지정
- **알파 마스크**: 셰이더에 `bIsDecal` 플래그 추가 → `return float4(texture.rgb, texture.a * diffuse.a)` 경로로 조명 우회
- **수명 페이드**: `mMaterial.diffuse.a = lifeRemain / lifeMax` 로 알파 페이드 아웃

### 데이터 관리
- **DecalManager**: MAX_DECALS(=32) 슬롯 풀, 프레임마다 수명 갱신
- **상수 버퍼**: ObjectConstants 레이아웃 기반, 슬롯당 8448바이트, 총 264KB
- **텍스처**: 4종 사전 로드 (SRV → Scene 디스크립터 힙)

---

## 사용 텍스처

| 인덱스 | 파일 | 용도 |
|--------|------|------|
| Scorch1 | `Assets/Textures/VFX/scorch_01.png` | 메테오 최종 착지 (대형) |
| Scorch2 | `Assets/Textures/VFX/scorch_02.png` | 파이어볼 폭발 / 빔 끝점 / 소형 메테오 |
| Scorch3 | `Assets/Textures/VFX/scorch_03.png` | WaveSlash 파도 진행로 |
| Magic2  | `Assets/Textures/VFX/magic_02.png`  | 메테오 최종 착지 (광원 서클) |

---

## 스킬별 데칼 배치

| 스킬 | 이벤트 | 텍스처 | 크기 | 수명 |
|------|--------|--------|------|------|
| R Meteor (소형) | 소형 메테오 착지 `OnSmallImpact()` | Scorch2 | 3.0 | 5s |
| R Meteor (최종) | 최종 메테오 착지 `OnFinalImpact()` | Scorch1 (크게) + Magic2 (더 크게) | 8 / 10 | 8s / 6s |
| Q WaveSlash | 발사 시 `Execute()` | Scorch3 | 4.0 | 4s |
| E FireBeam | 채널 종료 `Reset()` | Scorch2 | 2.0 | 3s |
| RC Fireball | 폭발 `SpawnExplosionParticles()` | Scorch2 | 폭발반경 | 4s |

---

## 변경 파일 목록 (17개)

### 셰이더 (1)
1. **`shaders.hlsl`**
   - `cbuffer cbGameObject`: `uint _gpad1;` → `uint bIsDecal;`
   - 픽셀셰이더 `baseColor` 계산 직후 조기 리턴 추가:
     ```hlsl
     if (bIsDecal)
         return float4(baseColor.rgb, albedoColor.a * gMaterial.m_cDiffuse.a);
     ```

### C++ 구조체 (1)
2. **`GameObject.h`** — ObjectConstants 구조체
   - `UINT m_grassPad1 = 0;` → `UINT m_bIsDecal = 0;` (주석 업데이트)

### 메쉬 (2)
3. **`Mesh.h`** — `QuadMesh` 클래스 선언 추가 (LineMesh 패턴 동일)
4. **`Mesh.cpp`** — `QuadMesh` 구현
   - 정점 4개: (-0.5,0,-0.5), (0.5,0,-0.5), (0.5,0,0.5), (-0.5,0,0.5)
   - UV: (0,1), (1,1), (1,0), (0,0)
   - 인덱스: 0,1,2 / 0,2,3

### 핵심 시스템 (2)
5. **`DecalManager.h`** — 신규
6. **`DecalManager.cpp`** — 신규

### Scene 연동 (2)
7. **`Scene.h`** — `m_pDecalManager` 멤버 + `GetDecalManager()` getter 추가
8. **`Scene.cpp`** — Init(텍스처 로드 + DecalManager 초기화), Update, Render(스킬 지오메트리 직후~VFX 전), 불 스킬 SetDecalManager 연결

### ISkillBehavior 인터페이스 (1)
9. **`ISkillBehavior.h`** — `virtual void SetDecalManager(class DecalManager*) {}` 기본 메서드 추가

### 불 스킬 Behavior (4)
10. **`MeteorBehavior.h`** — `SetDecalManager` override + `DecalManager*` 멤버
11. **`MeteorBehavior.cpp`** — `OnSmallImpact()`, `OnFinalImpact()`에서 데칼 스폰
12. **`WaveSlashBehavior.h`** — `SetDecalManager` override + 멤버
13. **`WaveSlashBehavior.cpp`** — `Execute()`에서 데칼 스폰
14. **`FireBeamBehavior.h`** — `SetDecalManager` override + 멤버
15. **`FireBeamBehavior.cpp`** — `Reset()`에서 데칼 스폰

### ProjectileManager (2)
16. **`ProjectileManager.h`** — `SetDecalManager(DecalManager*)` 메서드 추가
17. **`ProjectileManager.cpp`** — `SpawnExplosionParticles()`에서 Fire 속성 폭발 시 데칼 스폰

### 빌드 (1)
18. **`gaym.vcxproj`** — `DecalManager.h`, `DecalManager.cpp` 항목 추가

---

## DecalManager 내부 구조

```
DecalManager
├── m_pool[32]            -- 활성 데칼 풀
│     ├── pos, size, rotY
│     ├── lifeMax, lifeRemain
│     └── tex (DecalTexture enum)
├── m_pQuad               -- QuadMesh (1개 공유)
├── m_pCB                 -- 상수버퍼 (32 슬롯 × 8448 bytes)
├── m_pMappedCB           -- 영구 매핑 포인터
├── m_nCBVStart           -- 디스크립터 힙 CBV 시작 인덱스
├── m_texSlots[4]         -- 텍스처 리소스 + SRV GPU 핸들
├── m_pPSO                -- Water PSO (Shader에서 빌림)
└── m_pRootSig            -- Root Signature (Shader에서 빌림)

Init(device, cmdList, heap, nextIdx, shader)
  → CB 할당 + CBV×32 생성 + QuadMesh 생성

LoadTexture(device, cmdList, heap, nextIdx, type, path)
  → WICTextureLoader12 로드 + SRV 생성

Spawn(tex, pos, size, rotY, lifetime)
  → 빈 슬롯에 데칼 등록

Update(dt)
  → lifeRemain 감소, 0 되면 active=false

Render(cmdList, passCBV)
  → 활성 슬롯마다: CB 업데이트 → CBV 바인딩 → SRV 바인딩 → QuadMesh.Render()
```

---

## CB 레이아웃 (ObjectConstants 기반, 8368 bytes)

```
offset   0 ~ 63  : World matrix (transposed)
offset  64 ~ 111 : 플래그 (bHasTexture=1, bIsDecal=1, 나머지=0)
offset 112 ~ 175 : MATERIAL (ambient=0, diffuse={1,1,1,fade}, specular=0, emissive=0)
offset 176 ~     : BoneTransforms[128] (zero, bIsSkinned=0이므로 GPU 미참조)
```

---

## 렌더 삽입 위치 (Scene::Render)

```
[기존]  스킬 지오메트리 렌더 (MeteorBehavior::Render 등)
[신규]  DecalManager::Render(cmdList, passCBV)   ← 여기 삽입
[기존]  SSF/VFX 렌더링 (파티클이 데칼 위에 그려짐)
```

---

## 리스크 & 주의사항

1. **텍스처 알파 채널**: scorch_*.png / magic_*.png 에 실제로 알파 채널이 있어야 마스킹 작동. 없으면 흑백 사각형으로 보임 → 텍스처 확인 필요.
2. **Z-파이팅**: 데칼은 `pos.y + 0.1f` 오프셋으로 바닥면 위에 약간 띄워서 해결.
3. **BoneTransforms 크기**: `sizeof(ObjectConstants)`의 BoneTransforms[128]이 실제 HLSL cbuffer와 일치해야 함 (현재 모두 128개로 일치 확인됨).
4. **DecalManager 디스크립터 할당량**: CBV×32 + SRV×4 = 36 슬롯. `m_nNextDescriptorIndex += 36` 필요.
5. **워터 PSO 의존**: Shader 객체가 먼저 Build() 돼야 PSO 포인터 유효.

---

## 향후 확장 가능성
- 물/바람/대지 캐릭터용 추가 텍스처 로드 (현재는 불 캐릭터만)
- 데칼 회전 애니메이션 (rotY를 Update에서 증가)
- 데칼 스케일 인/아웃 (수명 초반에 size 확장)
- 최대 슬롯 수(32) 조정
