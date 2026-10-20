#pragma once
// DirectSound emulation: buffers are mixed in software and played through WASAPI shared mode.
#include <windows.h>
#include <mmreg.h>
#include <dsound.h>
#include <atomic>
#include <memory>
#include <vector>
#include "log.h"

#define LOG_MODULE "dsound"

struct SoundBuffer;

struct NotifyFace : IDirectSoundNotify {
    SoundBuffer* owner;
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(SetNotificationPositions)(DWORD count, LPCDSBPOSITIONNOTIFY positions) override;
};

struct SoundBuffer : IDirectSoundBuffer8 {
    std::atomic<LONG> refs{1};
    bool primary = false;
    DWORD flags = 0;
    WAVEFORMATEX fmt{};
    std::shared_ptr<std::vector<uint8_t>> data;  // shared with duplicates
    NotifyFace notify;

    // Playback state; guarded by the mixer lock.
    bool playing = false, looping = false;
    double pos = 0.0;        // in frames
    LONG volume = 0;         // hundredths of a dB, -10000..0
    LONG pan = 0;            // -10000..10000
    DWORD frequency = 0;     // 0 = the format's own rate
    std::vector<DSBPOSITIONNOTIFY> notifies;

    SoundBuffer();
    ~SoundBuffer();
    DWORD Bytes() const { return data ? (DWORD)data->size() : 0; }
    DWORD Frames() const { return fmt.nBlockAlign ? Bytes() / fmt.nBlockAlign : 0; }

    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(GetCaps)(LPDSBCAPS caps) override;
    STDMETHOD(GetCurrentPosition)(LPDWORD play, LPDWORD write) override;
    STDMETHOD(GetFormat)(LPWAVEFORMATEX fmt, DWORD size, LPDWORD written) override;
    STDMETHOD(GetVolume)(LPLONG v) override;
    STDMETHOD(GetPan)(LPLONG p) override;
    STDMETHOD(GetFrequency)(LPDWORD f) override;
    STDMETHOD(GetStatus)(LPDWORD s) override;
    STDMETHOD(Initialize)(LPDIRECTSOUND, LPCDSBUFFERDESC) override;
    STDMETHOD(Lock)(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD flags) override;
    STDMETHOD(Play)(DWORD, DWORD priority, DWORD flags) override;
    STDMETHOD(SetCurrentPosition)(DWORD pos) override;
    STDMETHOD(SetFormat)(LPCWAVEFORMATEX fmt) override;
    STDMETHOD(SetVolume)(LONG v) override;
    STDMETHOD(SetPan)(LONG p) override;
    STDMETHOD(SetFrequency)(DWORD f) override;
    STDMETHOD(Stop)() override;
    STDMETHOD(Unlock)(LPVOID, DWORD, LPVOID, DWORD) override;
    STDMETHOD(Restore)() override;
    STDMETHOD(SetFX)(DWORD, LPDSEFFECTDESC, LPDWORD) override;
    STDMETHOD(AcquireResources)(DWORD, DWORD, LPDWORD) override;
    STDMETHOD(GetObjectInPath)(REFGUID, DWORD, REFGUID, LPVOID*) override;
};

struct DirectSound : IDirectSound8 {
    std::atomic<LONG> refs{1};
    SoundBuffer* primaryBuffer = nullptr;

    ~DirectSound();
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override;
    STDMETHOD_(ULONG, AddRef)() override;
    STDMETHOD_(ULONG, Release)() override;
    STDMETHOD(CreateSoundBuffer)(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN) override;
    STDMETHOD(GetCaps)(LPDSCAPS caps) override;
    STDMETHOD(DuplicateSoundBuffer)(LPDIRECTSOUNDBUFFER orig, LPDIRECTSOUNDBUFFER* out) override;
    STDMETHOD(SetCooperativeLevel)(HWND, DWORD) override;
    STDMETHOD(Compact)() override;
    STDMETHOD(GetSpeakerConfig)(LPDWORD) override;
    STDMETHOD(SetSpeakerConfig)(DWORD) override;
    STDMETHOD(Initialize)(LPCGUID) override;
    STDMETHOD(VerifyCertification)(LPDWORD) override;
};

// Software mixer feeding the current Windows output device.
namespace Mixer {
    void Start();
    void Register(SoundBuffer* b);
    void Unregister(SoundBuffer* b);
    void Lock();
    void Unlock();
    DWORD OutputRate();          // device sample rate (or 48000 while no device is present)
    DWORD WriteAheadBytes(const SoundBuffer* b);  // distance the play cursor will advance next mix
    void SetMasterVolume(LONG mB);
}
