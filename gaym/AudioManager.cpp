#include "stdafx.h"
#include "AudioManager.h"

#include <fstream>
#include <vector>
#include <cstdlib>
#include <cstring>

using namespace DirectX;

// stb_vorbis (ThirdParty/stb_vorbis.c, C 링키지) — 전체 ogg 메모리 디코딩
extern "C" int stb_vorbis_decode_memory(
    const unsigned char* mem, int len,
    int* channels, int* sample_rate, short** output);

namespace
{
    // ogg 바이트 → 인터리브 16bit PCM. 실패 시 nullptr 반환.
    // 성공 시 outPcm(stb 가 malloc) 소유권은 호출자에게 넘어감 (free 필요).
    short* DecodeOgg(const std::wstring& path, int& channels, int& sampleRate, int& frames)
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) return nullptr;

        std::streamsize size = f.tellg();
        if (size <= 0) return nullptr;
        f.seekg(0, std::ios::beg);

        std::vector<unsigned char> data(static_cast<size_t>(size));
        if (!f.read(reinterpret_cast<char*>(data.data()), size)) return nullptr;

        short* pcm = nullptr;
        frames = stb_vorbis_decode_memory(
            data.data(), static_cast<int>(size), &channels, &sampleRate, &pcm);

        if (frames <= 0 || !pcm)
        {
            if (pcm) free(pcm);
            return nullptr;
        }
        return pcm;
    }
}

AudioManager::~AudioManager()
{
    Shutdown();
}

bool AudioManager::Initialize()
{
    AUDIO_ENGINE_FLAGS flags = AudioEngine_Default;
#ifdef _DEBUG
    flags = flags | AudioEngine_Debug;
#endif
    try
    {
        m_engine = std::make_unique<AudioEngine>(flags);
    }
    catch (...)
    {
        m_engine.reset();
        return false;
    }
    return true;
}

void AudioManager::Shutdown()
{
    if (m_bgmInstance)
    {
        m_bgmInstance->Stop(true);
        m_bgmInstance.reset();
    }
    m_bgmCache.clear();
    m_currentBGM.clear();
    if (m_engine)
    {
        m_engine->Suspend();   // 보이스 정리 후 엔진 파괴
        m_engine.reset();
    }
}

void AudioManager::Update()
{
    if (!m_engine) return;

    if (m_retryAudio)
    {
        m_retryAudio = false;
        if (m_engine->Reset())
        {
            // 장치 복구됨 — 재생 중이던 BGM 복원
            if (!m_currentBGM.empty())
            {
                std::wstring track = m_currentBGM;
                m_currentBGM.clear();
                PlayBGM(track);
            }
        }
    }
    else if (!m_engine->Update())
    {
        // 'silent mode' — 장치가 사라졌으면 다음 프레임에 복구 시도
        if (m_engine->IsCriticalError())
            m_retryAudio = true;
    }
}

void AudioManager::Suspend()
{
    if (m_engine) m_engine->Suspend();
}

void AudioManager::Resume()
{
    if (m_engine) m_engine->Resume();
}

SoundEffect* AudioManager::GetOrLoadBGM(const std::wstring& name)
{
    auto it = m_bgmCache.find(name);
    if (it != m_bgmCache.end())
        return it->second.get();

    const std::wstring path = L"Assets\\Audio\\BGM\\" + name + L".ogg";

    int channels = 0, sampleRate = 0, frames = 0;
    short* pcm = DecodeOgg(path, channels, sampleRate, frames);
    if (!pcm)
        return nullptr;

    const size_t audioBytes = static_cast<size_t>(frames) * channels * sizeof(short);

    // [WAVEFORMATEX][PCM] 단일 블롭 — wfx/startAudio 포인터가 SoundEffect 수명 동안 유효해야 함
    const size_t fmtSize = sizeof(WAVEFORMATEX);
    auto blob = std::make_unique<uint8_t[]>(fmtSize + audioBytes);

    auto* wfx = reinterpret_cast<WAVEFORMATEX*>(blob.get());
    wfx->wFormatTag      = WAVE_FORMAT_PCM;
    wfx->nChannels       = static_cast<WORD>(channels);
    wfx->nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    wfx->wBitsPerSample  = 16;
    wfx->nBlockAlign     = static_cast<WORD>(channels * sizeof(short));
    wfx->nAvgBytesPerSec = wfx->nSamplesPerSec * wfx->nBlockAlign;
    wfx->cbSize          = 0;

    uint8_t* startAudio = blob.get() + fmtSize;
    memcpy(startAudio, pcm, audioBytes);
    free(pcm);

    std::unique_ptr<SoundEffect> effect;
    try
    {
        // blob 은 move 되지만 wfx/startAudio 가 가리키는 메모리는 그대로 유지됨
        effect = std::make_unique<SoundEffect>(
            m_engine.get(), blob, wfx, startAudio, audioBytes);
    }
    catch (...)
    {
        return nullptr;
    }

    SoundEffect* raw = effect.get();
    m_bgmCache.emplace(name, std::move(effect));
    return raw;
}

void AudioManager::PlayBGM(const std::wstring& name, bool loop)
{
    if (!m_engine) return;

    // 동일 트랙이 이미 재생 중이면 무시 (매 프레임 호출 안전)
    if (name == m_currentBGM && m_bgmInstance &&
        m_bgmInstance->GetState() != SoundState::STOPPED)
        return;

    SoundEffect* effect = GetOrLoadBGM(name);
    if (!effect) return;

    if (m_bgmInstance)
        m_bgmInstance->Stop(true);

    m_bgmInstance = effect->CreateInstance();
    if (!m_bgmInstance) return;

    m_bgmInstance->SetVolume(m_bgmVolume);
    m_bgmInstance->Play(loop);
    m_currentBGM = name;
}

void AudioManager::StopBGM()
{
    if (m_bgmInstance)
        m_bgmInstance->Stop(true);
    m_currentBGM.clear();
}

void AudioManager::SetBGMVolume(float v)
{
    m_bgmVolume = v;
    if (m_bgmInstance)
        m_bgmInstance->SetVolume(v);
}

void AudioManager::SetMasterVolume(float v)
{
    if (m_engine) m_engine->SetMasterVolume(v);
}

float AudioManager::GetMasterVolume() const
{
    return m_engine ? m_engine->GetMasterVolume() : 0.f;
}
