# Third-Party Asset Credits

검기/VFX/SFX 시스템에 사용된 외부 자료 목록.
모두 상업 사용 허용 라이선스. 크레딧 표기 의무 없는 것까지 명시함 (후속 추가/교체 시 참조용).

## VFX Textures

### Kenney Particle Pack (1.1) — `Assets/Textures/VFX_Source/kenney_particle-pack/`
- 출처: https://kenney.nl/assets/particle-pack
- 작가: Kenney Vleugels (https://kenney.nl)
- 라이선스: **CC0 (Public Domain)** — 크레딧 의무 없음
- 내용: 80+ 입자/슬래시/연기/마법 sprite, 512×512 PNG (transparent + black bg + rotated variants)
- 사용 부분: 슬래시 알파 마스크 (slash_01~04), spark / star / magic / flame / smoke 입자

### Kenney Smoke Particles — `Assets/Textures/VFX_Source/kenney_smoke-particles/`
- 출처: https://kenney.nl/assets/smoke-particles
- 작가: Kenney Vleugels
- 라이선스: **CC0**
- 내용: 5 카테고리 (Black smoke / Explosion / Fart / Flash / White puff)

### Screaming Brain Studios — 768 Noise Texture Pack (256×256) — `Assets/Textures/VFX_Source/Noise/sbs_noise_256/`
- 출처: https://opengameart.org/content/700-noise-textures
- 원본: Screaming Brain Studios (https://screamingbrainstudios.com)
- 라이선스: **CC0**
- 내용: 18 카테고리 × 약 15장 = 262 PNG. Perlin / Voronoi / Cracks / Turbulence / Vein / Marble / Manifold / Swirl / Streak / Gabor / Spokes / Techno / Super 등
- 사용 부분: 셰이더 dissolve/UV displacement 노이즈, 검기 내부 텍스처 변동

## SFX

### OpenGameArt — 20 Sword Sound Effects (StarNinjas) — `Assets/SFX_Source/sword_attacks/`, `sword_clash/`
- 출처: https://opengameart.org/content/20-sword-sound-effects-attacks-and-clashes
- 작가: StarNinjas (계정 미상)
- 라이선스: **CC0**
- 내용: sword.1~10.ogg (검 휘두름 10종) + sword_clash.1~10.ogg (검 충돌 10종)
- 사용 부분: 검기 windup/swing/impact SFX

### OpenGameArt — RPG Sound Pack — `Assets/SFX_Source/rpg_sound_pack/`
- 출처: https://opengameart.org/content/rpg-sound-pack
- 라이선스: **CC0**
- 내용: 192 WAV — battle (magic1, spell, swing×3, sword-unsheathe×5), interface, inventory, NPC, world
- 사용 부분: 추가 검 휘두름 / 마법 차징 / 아이템 SFX

## Unity Asset Store (구매 — 클라이언트 내부 사용만 / 재배포 금지)

Unity Asset Store EULA: 구매한 라이선스로 게임에 사용 OK, 단 원본 에셋 재배포·sublicense 금지.
저장소 공개시 `Assets/Textures/VFX/namu_slash_sheet.png`, `free_*.png` 등은 .gitignore 처리 또는 빌드 시점에만 포함 필요.

### NamuFX — Simple Stylized Slash Pack 2 — `Assets/Textures/VFX/namu_slash_sheet.png`
- 출처: Unity Asset Store
- 작가: NamuFX
- 라이선스: **Unity Asset Store EULA** (구매한 게임 내 사용 OK, 원본 재배포 금지)
- 내용: T_SlashSheet01 — 3×3 hand-painted 슬래시 브러시 시트 (셀당 1/3×1/3 UV)
- 사용 부분: 보스 검기 crescent + 동적 ribbon — shader 가 원소 ID 로 셀 sub-UV 샘플

### Free Slash VFX (Unity Asset Store) — `Assets/Textures/VFX/free_noise.png`, `free_cut.png`, `free_slash_noise.png`
- 출처: Unity Asset Store (Free)
- 라이선스: **Unity Asset Store EULA** (Free Tier — 게임 내 사용 OK)
- 내용: Noise.png(marble smooth noise), Cut.png(8-point lens flare star), SlashNoise.png(블루 노이즈)
- 사용 부분: 향후 impact burst / inner noise modulation 보강용 (현재는 namu_slash_sheet 단독)

## 추후 추가 예정 (수동 다운로드 권장)

### Sonniss GDC 2026 Game Audio Bundle
- 출처: https://gdc.sonniss.com/ (페이지 직접 방문)
- 라이선스: Sonniss EULA — 게임 상업 사용 OK, 크레딧 의무 없음, AI/ML 훈련만 금지
- 7.47GB / 347 WAV / 24bit-96kHz. 검기 element-specific impact (fire/water/earth/wind) 보강용
- 자동 다운로드 불가 (사이트 게이트). 페이지 방문 후 클릭 다운로드 필요

### Freesound.org 픽
- 출처: https://freesound.org/ (CC0 필터: https://freesound.org/search/?f=license:%22Creative+Commons+0%22 )
- 라이선스: 개별 확인 (CC0 / CC-BY)
- 보스 windup 마법 차징 / 원소별 sting 보강용
- 로그인 필요 (무료)

### Effect Texture Maker
- 출처: https://mebiusbox.github.io/contents/EffectTextureMaker/
- 라이선스: 생성한 텍스처 자유 사용 (도구 자체 무료)
- 커스텀 슬래시/링/번개/연기 텍스처 즉석 생성용 (셰이더에 맞는 변형 텍스처가 필요할 때 활용)

---

라이선스 추적 원칙: 새 외부 자료 추가 시 반드시 이 파일에 (출처 URL, 작가, 라이선스, 다운로드 시점) 4종 기록.
