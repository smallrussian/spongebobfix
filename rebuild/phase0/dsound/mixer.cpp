// Software mixer for emulated DirectSound buffers, output through WASAPI shared mode.
// Follows the Windows default output device as it changes, survives devices being unplugged,
// and keeps play cursors advancing (silently) while no output device exists at all.
#include "dsound_impl.h"
#include "config.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <initguid.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::recursive_mutex g_mx;
std::vector<SoundBuffer*> g_buffers;
std::once_flag g_startOnce;
HANDLE g_wake;                       // signalled by WASAPI when it wants more data, or by device events
std::atomic<bool> g_reopen{false};
std::atomic<DWORD> g_rate{48000};
float g_masterGain = 1.0f;           // primary-buffer volume (game controlled)
float g_userGain = 1.0f;             // MasterVolume from the ini

IMMDeviceEnumerator* g_enum;
IAudioClient* g_client;
IAudioRenderClient* g_render;
UINT32 g_bufferFrames;

inline float GainFromMillibels(LONG mB) {
    return mB <= DSBVOLUME_MIN ? 0.0f : powf(10.0f, mB / 2000.0f);
}

std::wstring DeviceName(IMMDevice* dev) {
    std::wstring name;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
        props->Release();
    }
    return name;
}

std::string Narrow(const std::wstring& w) {
    char buf[512];
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

// Device-change notifications arrive on a system thread; we only flag and wake the mixer.
struct DeviceWatcher : IMMNotificationClient {
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) { *ppv = this; return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 1; }
    STDMETHOD_(ULONG, Release)() override { return 1; }
    STDMETHOD(OnDefaultDeviceChanged)(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole && GetConfig().audioDevice.empty()) Reopen();
        return S_OK;
    }
    STDMETHOD(OnDeviceAdded)(LPCWSTR) override { if (!g_client || !GetConfig().audioDevice.empty()) Reopen(); return S_OK; }
    STDMETHOD(OnDeviceRemoved)(LPCWSTR) override { return S_OK; }
    STDMETHOD(OnDeviceStateChanged)(LPCWSTR, DWORD) override { if (!g_client || !GetConfig().audioDevice.empty()) Reopen(); return S_OK; }
    STDMETHOD(OnPropertyValueChanged)(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    void Reopen() { g_reopen = true; SetEvent(g_wake); }
} g_watcher;

void CloseDevice() {
    if (g_client) g_client->Stop();
    if (g_render) { g_render->Release(); g_render = nullptr; }
    if (g_client) { g_client->Release(); g_client = nullptr; }
}

IMMDevice* PickDevice() {
    const std::wstring& want = GetConfig().audioDevice;
    if (!want.empty()) {
        IMMDeviceCollection* list = nullptr;
        if (SUCCEEDED(g_enum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list))) {
            UINT count = 0;
            list->GetCount(&count);
            std::wstring needle = want;
            std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* d = nullptr;
                if (FAILED(list->Item(i, &d))) continue;
                std::wstring name = DeviceName(d);
                std::transform(name.begin(), name.end(), name.begin(), ::towlower);
                if (name.find(needle) != std::wstring::npos) { list->Release(); return d; }
                d->Release();
            }
            list->Release();
        }
        LOG_INFO("configured audio device \"%s\" not present; using the Windows default", Narrow(want).c_str());
    }
    IMMDevice* d = nullptr;
    g_enum->GetDefaultAudioEndpoint(eRender, eConsole, &d);
    return d;
}

bool OpenDevice() {
    CloseDevice();
    IMMDevice* dev = PickDevice();
    if (!dev) return false;
    std::string name = Narrow(DeviceName(dev));
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&g_client);
    dev->Release();
    if (FAILED(hr)) { g_client = nullptr; return false; }

    WAVEFORMATEX* mix = nullptr;
    g_client->GetMixFormat(&mix);
    DWORD rate = mix ? mix->nSamplesPerSec : 48000;
    CoTaskMemFree(mix);

    // Always render float stereo; Windows converts to the device's own layout and rate.
    WAVEFORMATEXTENSIBLE wf{};
    wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    wf.Format.nChannels = 2;
    wf.Format.nSamplesPerSec = rate;
    wf.Format.wBitsPerSample = 32;
    wf.Format.nBlockAlign = 8;
    wf.Format.nAvgBytesPerSec = rate * 8;
    wf.Format.cbSize = sizeof(wf) - sizeof(WAVEFORMATEX);
    wf.Samples.wValidBitsPerSample = 32;
    wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    wf.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    REFERENCE_TIME duration = (REFERENCE_TIME)std::clamp(GetConfig().audioLatencyMs, 5, 200) * 10000;
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = g_client->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, duration, 0, (WAVEFORMATEX*)&wf, nullptr);
    if (FAILED(hr)) {
        LOG_INFO("audio device \"%s\" rejected the stream (%08lX)", name.c_str(), hr);
        CloseDevice();
        return false;
    }
    g_client->SetEventHandle(g_wake);
    g_client->GetBufferSize(&g_bufferFrames);
    g_client->GetService(__uuidof(IAudioRenderClient), (void**)&g_render);
    g_rate = rate;
    g_client->Start();
    LOG_INFO("audio output: \"%s\" at %lu Hz, %u-frame buffer", name.c_str(), rate, g_bufferFrames);
    return true;
}

inline float Sample(const SoundBuffer* b, DWORD frame, int ch) {
    const uint8_t* p = b->data->data() + frame * b->fmt.nBlockAlign;
    if (b->fmt.wBitsPerSample == 8) return ((int)p[ch] - 128) / 128.0f;
    return ((const int16_t*)p)[ch] / 32768.0f;
}

void FireNotifies(SoundBuffer* b, DWORD fromByte, DWORD toByte, bool wrapped) {
    for (auto& n : b->notifies) {
        if (n.dwOffset == DSBPN_OFFSETSTOP || !n.hEventNotify) continue;
        bool hit = wrapped ? (n.dwOffset >= fromByte || n.dwOffset < toByte) : (n.dwOffset >= fromByte && n.dwOffset < toByte);
        if (hit) SetEvent(n.hEventNotify);
    }
}

// Mixes every playing buffer into `out` (interleaved stereo float). Caller holds the lock.
void MixLocked(float* out, UINT32 frames, DWORD rate) {
    std::fill(out, out + frames * 2, 0.0f);
    const float master = g_masterGain * g_userGain;
    for (SoundBuffer* b : g_buffers) {
        if (!b->playing || b->primary || !b->data || !b->fmt.nBlockAlign) continue;
        const DWORD total = b->Frames();
        if (!total) { b->playing = false; continue; }
        const double step = (double)(b->frequency ? b->frequency : b->fmt.nSamplesPerSec) / rate;
        const float gain = GainFromMillibels(b->volume) * master;
        const float gl = gain * (b->pan > 0 ? GainFromMillibels(-b->pan) : 1.0f);
        const float gr = gain * (b->pan < 0 ? GainFromMillibels(b->pan) : 1.0f);
        const bool stereo = b->fmt.nChannels == 2;
        const DWORD startByte = (DWORD)b->pos * b->fmt.nBlockAlign;
        double pos = b->pos;
        bool wrapped = false, ended = false;

        for (UINT32 i = 0; i < frames; ++i) {
            DWORD f0 = (DWORD)pos;
            float t = (float)(pos - f0);
            DWORD f1 = f0 + 1;
            if (f1 >= total) f1 = b->looping ? 0 : total - 1;
            float l = Sample(b, f0, 0) * (1 - t) + Sample(b, f1, 0) * t;
            float r = stereo ? Sample(b, f0, 1) * (1 - t) + Sample(b, f1, 1) * t : l;
            out[i * 2] += l * gl;
            out[i * 2 + 1] += r * gr;
            pos += step;
            if (pos >= total) {
                if (b->looping) { pos -= total * std::floor(pos / total); wrapped = true; }
                else { ended = true; break; }
            }
        }
        if (ended) {
            b->playing = false;
            b->pos = 0;
            FireNotifies(b, startByte, b->Bytes(), false);
            for (auto& n : b->notifies)
                if (n.dwOffset == DSBPN_OFFSETSTOP && n.hEventNotify) SetEvent(n.hEventNotify);
        } else {
            b->pos = pos;
            FireNotifies(b, startByte, (DWORD)pos * b->fmt.nBlockAlign, wrapped);
        }
    }
    for (UINT32 i = 0; i < frames * 2; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

void MixerThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&g_enum))) {
        LOG_INFO("no Windows audio service; sound disabled");
        return;
    }
    g_enum->RegisterEndpointNotificationCallback(&g_watcher);

    std::vector<float> scratch;
    LARGE_INTEGER freq, last;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    ULONGLONG nextRetry = 0;

    OpenDevice();
    for (;;) {
        if (g_reopen.exchange(false)) {
            LOG_INFO("audio device change detected; reopening output");
            OpenDevice();
        }
        if (!g_client) {
            // No output device: advance buffers in real time so the game's audio and movies don't stall.
            WaitForSingleObject(g_wake, 10);
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            UINT32 frames = (UINT32)((now.QuadPart - last.QuadPart) * g_rate / freq.QuadPart);
            if (frames) {
                last = now;
                scratch.resize(frames * 2);
                std::lock_guard<std::recursive_mutex> lock(g_mx);
                MixLocked(scratch.data(), frames, g_rate);
            }
            if (GetTickCount64() >= nextRetry) { nextRetry = GetTickCount64() + 1000; OpenDevice(); }
            continue;
        }
        WaitForSingleObject(g_wake, 200);
        QueryPerformanceCounter(&last);
        UINT32 padding = 0;
        HRESULT hr = g_client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            LOG_INFO("audio device lost (%08lX); switching to the current default", hr);
            CloseDevice();
            OpenDevice();
            continue;
        }
        UINT32 avail = g_bufferFrames - padding;
        if (!avail) continue;
        BYTE* p = nullptr;
        if (FAILED(g_render->GetBuffer(avail, &p))) continue;
        {
            std::lock_guard<std::recursive_mutex> lock(g_mx);
            MixLocked((float*)p, avail, g_rate);
        }
        g_render->ReleaseBuffer(avail, 0);
    }
}

}  // namespace

namespace Mixer {

void Start() {
    std::call_once(g_startOnce, [] {
        g_userGain = std::clamp(GetConfig().masterVolume, 0, 200) / 100.0f;
        g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        std::thread(MixerThread).detach();
    });
}

void Register(SoundBuffer* b) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    g_buffers.push_back(b);
}

void Unregister(SoundBuffer* b) {
    std::lock_guard<std::recursive_mutex> lock(g_mx);
    g_buffers.erase(std::remove(g_buffers.begin(), g_buffers.end(), b), g_buffers.end());
}

void Lock() { g_mx.lock(); }
void Unlock() { g_mx.unlock(); }
DWORD OutputRate() { return g_rate; }

DWORD WriteAheadBytes(const SoundBuffer* b) {
    // The mixer consumes up to one device buffer ahead of what is audible.
    DWORD srcRate = b->frequency ? b->frequency : b->fmt.nSamplesPerSec;
    DWORD frames = (DWORD)((uint64_t)std::max<UINT32>(g_bufferFrames, g_rate / 50) * srcRate / std::max<DWORD>(1, g_rate));
    return frames * b->fmt.nBlockAlign;
}

void SetMasterVolume(LONG mB) { g_masterGain = GainFromMillibels(mB); }

}  // namespace Mixer
