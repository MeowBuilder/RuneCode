#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <Audio.h>

// ─── AudioManager ────────────────────────────────────────────────────────────
// DirectXTK12 오디오(XAudio2) 래퍼. 현재는 2D BGM 재생 담당.
//  - ogg 음원은 stb_vorbis 로 PCM 디코딩 후 메모리 SoundEffect 로 적재
//  - 오디오 장치가 없으면 'silent mode' 로 동작하며 게임 진행에는 영향 없음
class AudioManager
{
public:
    AudioManager() = default;
    ~AudioManager();

    bool Initialize();
    void Shutdown();

    // 매 프레임 호출 (장치 분실/복구 처리 포함)
    void Update();

    // 앱 포커스 손실/복귀 시
    void Suspend();
    void Resume();

    // BGM — name 은 확장자 없는 파일명 (Assets/Audio/BGM/<name>.ogg)
    // 동일 트랙이 이미 재생 중이면 무시 (매 프레임 호출해도 안전)
    void PlayBGM(const std::wstring& name, bool loop = true);
    void StopBGM();
    void SetBGMVolume(float v);   // 0.0 ~ 1.0

    void  SetMasterVolume(float v);
    float GetMasterVolume() const;

    bool IsAvailable() const { return m_engine != nullptr; }

private:
    DirectX::SoundEffect* GetOrLoadBGM(const std::wstring& name);

    std::unique_ptr<DirectX::AudioEngine>           m_engine;
    std::unordered_map<std::wstring,
        std::unique_ptr<DirectX::SoundEffect>>      m_bgmCache;
    std::unique_ptr<DirectX::SoundEffectInstance>   m_bgmInstance;
    std::wstring m_currentBGM;
    float        m_bgmVolume   = 0.6f;
    bool         m_retryAudio  = false;
};
