#include "dsound_impl.h"
#include <algorithm>
#include <cstring>

namespace {
struct MixLock {
    MixLock() { Mixer::Lock(); }
    ~MixLock() { Mixer::Unlock(); }
};
}  // namespace

// ---------------------------------------------------------------------------------------------
// Sound buffer
// ---------------------------------------------------------------------------------------------
SoundBuffer::SoundBuffer() { notify.owner = this; }

SoundBuffer::~SoundBuffer() { Mixer::Unregister(this); }

HRESULT SoundBuffer::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectSoundBuffer || riid == IID_IDirectSoundBuffer8) *ppv = this;
    else if (riid == IID_IDirectSoundNotify) *ppv = &notify;
    else {
        LOG_VERBOSE("buffer QI for unsupported interface %08lX", riid.Data1);
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}
ULONG SoundBuffer::AddRef() { return ++refs; }
ULONG SoundBuffer::Release() { LONG r = --refs; if (!r) delete this; return (ULONG)r; }

HRESULT SoundBuffer::GetCaps(LPDSBCAPS caps) {
    if (!caps) return DSERR_INVALIDPARAM;
    caps->dwFlags = flags;
    caps->dwBufferBytes = Bytes();
    caps->dwUnlockTransferRate = 0;
    caps->dwPlayCpuOverhead = 0;
    return DS_OK;
}

HRESULT SoundBuffer::GetCurrentPosition(LPDWORD play, LPDWORD write) {
    MixLock lock;
    DWORD bytes = Bytes();
    DWORD p = bytes ? ((DWORD)pos * fmt.nBlockAlign) % bytes : 0;
    if (play) *play = p;
    if (write) *write = bytes ? (p + (playing ? Mixer::WriteAheadBytes(this) : 0)) % bytes : 0;
    return DS_OK;
}

HRESULT SoundBuffer::GetFormat(LPWAVEFORMATEX out, DWORD size, LPDWORD written) {
    DWORD need = sizeof(WAVEFORMATEX);
    if (written) *written = need;
    if (!out) return written ? DS_OK : DSERR_INVALIDPARAM;
    memcpy(out, &fmt, std::min(size, need));
    return DS_OK;
}

HRESULT SoundBuffer::GetVolume(LPLONG v) { if (!v) return DSERR_INVALIDPARAM; *v = volume; return DS_OK; }
HRESULT SoundBuffer::GetPan(LPLONG p) { if (!p) return DSERR_INVALIDPARAM; *p = pan; return DS_OK; }
HRESULT SoundBuffer::GetFrequency(LPDWORD f) { if (!f) return DSERR_INVALIDPARAM; *f = frequency ? frequency : fmt.nSamplesPerSec; return DS_OK; }

HRESULT SoundBuffer::GetStatus(LPDWORD s) {
    if (!s) return DSERR_INVALIDPARAM;
    MixLock lock;
    *s = playing ? (DSBSTATUS_PLAYING | (looping ? DSBSTATUS_LOOPING : 0)) : 0;
    return DS_OK;
}

HRESULT SoundBuffer::Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) { return DSERR_ALREADYINITIALIZED; }

HRESULT SoundBuffer::Lock(DWORD offset, DWORD bytes, LPVOID* p1, LPDWORD n1, LPVOID* p2, LPDWORD n2, DWORD lockFlags) {
    if (primary) return DSERR_PRIOLEVELNEEDED;
    DWORD size = Bytes();
    if (!size || !p1 || !n1) return DSERR_INVALIDPARAM;
    if (lockFlags & DSBLOCK_FROMWRITECURSOR) GetCurrentPosition(nullptr, &offset);
    if (lockFlags & DSBLOCK_ENTIREBUFFER) bytes = size;
    if (offset >= size || bytes > size) return DSERR_INVALIDPARAM;
    uint8_t* base = data->data();
    *p1 = base + offset;
    if (offset + bytes <= size) {
        *n1 = bytes;
        if (p2) *p2 = nullptr;
        if (n2) *n2 = 0;
    } else {
        *n1 = size - offset;
        if (p2) *p2 = base;
        if (n2) *n2 = bytes - *n1;
    }
    return DS_OK;
}

HRESULT SoundBuffer::Play(DWORD, DWORD, DWORD playFlags) {
    MixLock lock;
    playing = true;
    looping = (playFlags & DSBPLAY_LOOPING) != 0;
    return DS_OK;
}

HRESULT SoundBuffer::SetCurrentPosition(DWORD p) {
    MixLock lock;
    if (fmt.nBlockAlign && p < Bytes()) pos = (double)(p / fmt.nBlockAlign);
    return DS_OK;
}

HRESULT SoundBuffer::SetFormat(LPCWAVEFORMATEX f) {
    if (!f) return DSERR_INVALIDPARAM;
    if (!primary) return DSERR_INVALIDCALL;
    fmt = *f;  // the primary format only affects what we report; mixing is always float
    return DS_OK;
}

HRESULT SoundBuffer::SetVolume(LONG v) {
    MixLock lock;
    volume = std::clamp<LONG>(v, DSBVOLUME_MIN, DSBVOLUME_MAX);
    if (primary) Mixer::SetMasterVolume(volume);
    return DS_OK;
}

HRESULT SoundBuffer::SetPan(LONG p) { MixLock lock; pan = std::clamp<LONG>(p, DSBPAN_LEFT, DSBPAN_RIGHT); return DS_OK; }

HRESULT SoundBuffer::SetFrequency(DWORD f) {
    MixLock lock;
    frequency = f == DSBFREQUENCY_ORIGINAL ? 0 : std::clamp<DWORD>(f, DSBFREQUENCY_MIN, 200000);
    return DS_OK;
}

HRESULT SoundBuffer::Stop() {
    MixLock lock;
    playing = false;
    for (auto& n : notifies)
        if (n.dwOffset == DSBPN_OFFSETSTOP && n.hEventNotify) SetEvent(n.hEventNotify);
    return DS_OK;
}

HRESULT SoundBuffer::Unlock(LPVOID, DWORD, LPVOID, DWORD) { return DS_OK; }
HRESULT SoundBuffer::Restore() { return DS_OK; }
HRESULT SoundBuffer::SetFX(DWORD, LPDSEFFECTDESC, LPDWORD) { LOG_UNIMPL("SetFX"); return DSERR_CONTROLUNAVAIL; }
HRESULT SoundBuffer::AcquireResources(DWORD, DWORD, LPDWORD) { return DS_OK; }
HRESULT SoundBuffer::GetObjectInPath(REFGUID, DWORD, REFGUID, LPVOID*) { return DSERR_OBJECTNOTFOUND; }

HRESULT NotifyFace::QueryInterface(REFIID riid, void** ppv) { return owner->QueryInterface(riid, ppv); }
ULONG NotifyFace::AddRef() { return owner->AddRef(); }
ULONG NotifyFace::Release() { return owner->Release(); }
HRESULT NotifyFace::SetNotificationPositions(DWORD count, LPCDSBPOSITIONNOTIFY positions) {
    MixLock lock;
    owner->notifies.assign(positions, positions + count);
    return DS_OK;
}

// ---------------------------------------------------------------------------------------------
// DirectSound object
// ---------------------------------------------------------------------------------------------
DirectSound::~DirectSound() {
    if (primaryBuffer) primaryBuffer->Release();
}

HRESULT DirectSound::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDirectSound || riid == IID_IDirectSound8) {
        *ppv = this;
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG DirectSound::AddRef() { return ++refs; }
ULONG DirectSound::Release() { LONG r = --refs; if (!r) delete this; return (ULONG)r; }

HRESULT DirectSound::CreateSoundBuffer(LPCDSBUFFERDESC desc, LPDIRECTSOUNDBUFFER* out, LPUNKNOWN) {
    if (!desc || !out) return DSERR_INVALIDPARAM;
    *out = nullptr;
    if (desc->dwFlags & DSBCAPS_PRIMARYBUFFER) {
        if (!primaryBuffer) {
            primaryBuffer = new SoundBuffer;
            primaryBuffer->primary = true;
            primaryBuffer->flags = desc->dwFlags;
            WAVEFORMATEX& f = primaryBuffer->fmt;
            f.wFormatTag = WAVE_FORMAT_PCM;
            f.nChannels = 2;
            f.nSamplesPerSec = 22050;
            f.wBitsPerSample = 16;
            f.nBlockAlign = 4;
            f.nAvgBytesPerSec = 22050 * 4;
        }
        primaryBuffer->AddRef();
        *out = primaryBuffer;
        return DS_OK;
    }
    const WAVEFORMATEX* wf = desc->lpwfxFormat;
    if (!wf || !desc->dwBufferBytes) return DSERR_INVALIDPARAM;
    bool pcm = wf->wFormatTag == WAVE_FORMAT_PCM ||
               (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22);
    if (!pcm || (wf->wBitsPerSample != 8 && wf->wBitsPerSample != 16) || wf->nChannels < 1 || wf->nChannels > 2) {
        LOG_INFO("unsupported buffer format tag=%u bits=%u ch=%u", wf->wFormatTag, wf->wBitsPerSample, wf->nChannels);
        return DSERR_BADFORMAT;
    }
    auto b = new SoundBuffer;
    b->flags = desc->dwFlags;
    b->fmt = *wf;
    b->fmt.wFormatTag = WAVE_FORMAT_PCM;
    b->fmt.cbSize = 0;
    b->fmt.nBlockAlign = (WORD)(b->fmt.nChannels * b->fmt.wBitsPerSample / 8);
    b->fmt.nAvgBytesPerSec = b->fmt.nSamplesPerSec * b->fmt.nBlockAlign;
    // Silence is 0x80 for 8-bit PCM, 0 for 16-bit.
    b->data = std::make_shared<std::vector<uint8_t>>(desc->dwBufferBytes, (uint8_t)(wf->wBitsPerSample == 8 ? 0x80 : 0));
    Mixer::Register(b);
    *out = b;
    return DS_OK;
}

HRESULT DirectSound::GetCaps(LPDSCAPS caps) {
    if (!caps) return DSERR_INVALIDPARAM;
    DWORD size = caps->dwSize ? caps->dwSize : sizeof(DSCAPS);
    memset(caps, 0, size);
    caps->dwSize = size;
    caps->dwFlags = DSCAPS_PRIMARYMONO | DSCAPS_PRIMARYSTEREO | DSCAPS_PRIMARY8BIT | DSCAPS_PRIMARY16BIT |
                    DSCAPS_SECONDARYMONO | DSCAPS_SECONDARYSTEREO | DSCAPS_SECONDARY8BIT | DSCAPS_SECONDARY16BIT |
                    DSCAPS_CONTINUOUSRATE | DSCAPS_CERTIFIED;
    caps->dwMinSecondarySampleRate = DSBFREQUENCY_MIN;
    caps->dwMaxSecondarySampleRate = 200000;
    caps->dwPrimaryBuffers = 1;
    caps->dwFreeHwMixingAllBuffers = 0;
    return DS_OK;
}

HRESULT DirectSound::DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER orig, LPDIRECTSOUNDBUFFER* out) {
    auto src = (SoundBuffer*)orig;
    if (!src || !out || src->primary) return DSERR_INVALIDPARAM;
    auto b = new SoundBuffer;
    {
        MixLock lock;
        b->flags = src->flags;
        b->fmt = src->fmt;
        b->data = src->data;
        b->volume = src->volume;
        b->pan = src->pan;
        b->frequency = src->frequency;
    }
    Mixer::Register(b);
    *out = b;
    return DS_OK;
}

HRESULT DirectSound::SetCooperativeLevel(HWND, DWORD) { return DS_OK; }
HRESULT DirectSound::Compact() { return DS_OK; }
HRESULT DirectSound::GetSpeakerConfig(LPDWORD c) { if (c) *c = DSSPEAKER_STEREO; return DS_OK; }
HRESULT DirectSound::SetSpeakerConfig(DWORD) { return DS_OK; }
HRESULT DirectSound::Initialize(LPCGUID) { return DS_OK; }
HRESULT DirectSound::VerifyCertification(LPDWORD c) { if (c) *c = DS_CERTIFIED; return DS_OK; }

// ---------------------------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------------------------
BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        LOG_INFO("sb2modern dsound.dll loaded");
    }
    return TRUE;
}

extern "C" {

HRESULT WINAPI DirectSoundCreate(LPCGUID, LPDIRECTSOUND* out, LPUNKNOWN) {
    if (!out) return DSERR_INVALIDPARAM;
    Mixer::Start();
    *out = new DirectSound;
    return DS_OK;
}

HRESULT WINAPI DirectSoundCreate8(LPCGUID, LPDIRECTSOUND8* out, LPUNKNOWN) {
    if (!out) return DSERR_INVALIDPARAM;
    Mixer::Start();
    *out = new DirectSound;
    return DS_OK;
}

HRESULT WINAPI DirectSoundEnumerateA(LPDSENUMCALLBACKA cb, LPVOID ctx) {
    if (cb) cb(nullptr, "Primary Sound Driver", "", ctx);
    return DS_OK;
}

HRESULT WINAPI DirectSoundEnumerateW(LPDSENUMCALLBACKW cb, LPVOID ctx) {
    if (cb) cb(nullptr, L"Primary Sound Driver", L"", ctx);
    return DS_OK;
}

HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID, LPDIRECTSOUNDCAPTURE*, LPUNKNOWN) { return DSERR_NODRIVER; }
HRESULT WINAPI DirectSoundCaptureCreate8(LPCGUID, LPDIRECTSOUNDCAPTURE8*, LPUNKNOWN) { return DSERR_NODRIVER; }
HRESULT WINAPI DirectSoundCaptureEnumerateA(LPDSENUMCALLBACKA, LPVOID) { return DS_OK; }
HRESULT WINAPI DirectSoundCaptureEnumerateW(LPDSENUMCALLBACKW, LPVOID) { return DS_OK; }
HRESULT WINAPI GetDeviceID(LPCGUID src, LPGUID dst) {
    if (!dst) return DSERR_INVALIDPARAM;
    *dst = src ? *src : GUID{};
    return DS_OK;
}
HRESULT WINAPI DirectSoundFullDuplexCreate(LPCGUID, LPCGUID, LPCDSCBUFFERDESC, LPCDSBUFFERDESC, HWND, DWORD,
                                           LPDIRECTSOUNDFULLDUPLEX*, LPDIRECTSOUNDCAPTUREBUFFER8*,
                                           LPDIRECTSOUNDBUFFER8*, LPUNKNOWN) {
    return DSERR_NODRIVER;
}
HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID, REFIID, LPVOID* out) { if (out) *out = nullptr; return CLASS_E_CLASSNOTAVAILABLE; }

}  // extern "C"
