// DirectSound (version 1 interfaces): secondary buffers in guest memory, mixed into the
// CoreAudio output by winmm.cpp (dsoundMix).
#include <cmath>
#include <map>
#include <mutex>

#include "com.h"

void audioStartOutput(); // winmm.cpp

namespace
{
    struct Buffer
    {
        bool primary = false;
        uint32_t mem = 0, bytes = 0;
        uint32_t rate = 22050, channels = 2, bits = 16;
        bool playing = false, looping = false;
        double pos = 0; // play position in frames (fractional while resampling)
        float volume = 1.0f;
        uint32_t frequency = 0; // 0 = format rate
    };
    std::mutex g_dsMutex;
    std::map<uint32_t, Buffer> g_buffers;

    uint32_t frameBytes(const Buffer& b) { return b.channels * b.bits / 8; }
    uint32_t playCursor(const Buffer& b) { return b.bytes ? ((uint32_t)b.pos * frameBytes(b)) % b.bytes : 0; }
}

void dsoundReset()
{
    std::lock_guard lock(g_dsMutex);
    g_buffers.clear();
}

void dsoundMix(float* out, uint32_t frames)
{
    std::lock_guard lock(g_dsMutex);
    for (auto& [obj, b] : g_buffers)
    {
        if (b.primary || !b.playing || !b.bytes)
            continue;
        const uint32_t fb = frameBytes(b), total = b.bytes / fb;
        const double step = (b.frequency ? b.frequency : b.rate) / 44100.0;
        for (uint32_t i = 0; i < frames; i++)
        {
            uint32_t f = (uint32_t)b.pos;
            if (f >= total)
            {
                if (!b.looping)
                {
                    b.playing = false;
                    break;
                }
                b.pos -= total;
                f = (uint32_t)b.pos;
            }
            const uint8_t* p = g_mem + b.mem + (size_t)f * fb;
            float l, r;
            if (b.bits == 16)
            {
                int16_t s0, s1;
                memcpy(&s0, p, 2);
                s1 = s0;
                if (b.channels > 1)
                    memcpy(&s1, p + 2, 2);
                l = s0 / 32768.0f;
                r = s1 / 32768.0f;
            }
            else
            {
                l = ((int)p[0] - 128) / 128.0f;
                r = b.channels > 1 ? ((int)p[1] - 128) / 128.0f : l;
            }
            out[2 * i] += l * b.volume;
            out[2 * i + 1] += r * b.volume;
            b.pos += step;
        }
    }
}

COM_INTERFACE(IDirectSound, "QueryInterface", "AddRef", "Release", "CreateSoundBuffer", "GetCaps",
              "DuplicateSoundBuffer", "SetCooperativeLevel", "Compact", "GetSpeakerConfig", "SetSpeakerConfig",
              "Initialize");
COM_INTERFACE(IDirectSoundBuffer, "QueryInterface", "AddRef", "Release", "GetCaps", "GetCurrentPosition",
              "GetFormat", "GetVolume", "GetPan", "GetFrequency", "GetStatus", "Initialize", "Lock", "Play",
              "SetCurrentPosition", "SetFormat", "SetVolume", "SetPan", "SetFrequency", "Stop", "Unlock", "Restore");

WIN32API(dsound, DirectSoundCreate, 3)
{
    wr32(ARG(1), comNew("IDirectSound"));
    audioStartOutput();
    logf("[dsound] DirectSoundCreate");
    RET(0);
}

COMMETHOD(IDirectSound, QueryInterface, 3)
{
    wr32(ARG(2), 0);
    RET(0x80004002u);
}
COMMETHOD(IDirectSound, AddRef, 1) { RET(1); }
COMMETHOD(IDirectSound, Release, 1) { RET(0); }
COMMETHOD(IDirectSound, SetCooperativeLevel, 3) { RET(0); }
COMMETHOD(IDirectSound, Compact, 1) { RET(0); }
COMMETHOD(IDirectSound, GetSpeakerConfig, 2)
{
    wr32(ARG(1), 4); // DSSPEAKER_STEREO
    RET(0);
}
COMMETHOD(IDirectSound, SetSpeakerConfig, 2) { RET(0); }
COMMETHOD(IDirectSound, GetCaps, 2)
{
    const uint32_t size = rd32(ARG(1)) ? rd32(ARG(1)) : 96;
    memset(g_mem + ARG(1), 0, size);
    wr32(ARG(1), size);
    wr32(ARG(1) + 4, 0x400 | 0x200 | 0x10 | 0x20 | 0x1 | 0x2); // stereo/mono 8/16-bit primary+secondary
    RET(0);
}

COMMETHOD(IDirectSound, CreateSoundBuffer, 4)
{
    const uint32_t d = ARG(1);
    const uint32_t flags = rd32(d + 4), bytes = rd32(d + 8), fmt = rd32(d + 16);
    const uint32_t obj = comNew("IDirectSoundBuffer");
    Buffer b;
    b.primary = flags & 1;
    if (!b.primary)
    {
        b.bytes = bytes;
        b.mem = heapAlloc(bytes, true);
        if (fmt)
        {
            b.channels = rd16(fmt + 2);
            b.rate = rd32(fmt + 4);
            b.bits = rd16(fmt + 14);
        }
        logf("[dsound] buffer %u bytes, %u Hz, %u ch, %u bits", bytes, b.rate, b.channels, b.bits);
    }
    {
        std::lock_guard lock(g_dsMutex);
        g_buffers[obj] = b;
    }
    wr32(ARG(2), obj);
    RET(0);
}

COMMETHOD(IDirectSoundBuffer, QueryInterface, 3)
{
    wr32(ARG(2), 0);
    RET(0x80004002u);
}
COMMETHOD(IDirectSoundBuffer, AddRef, 1) { RET(1); }
COMMETHOD(IDirectSoundBuffer, Release, 1)
{
    std::lock_guard lock(g_dsMutex);
    auto it = g_buffers.find(ARG(0));
    if (it != g_buffers.end())
    {
        it->second.playing = false;
        if (it->second.mem)
            heapFree(it->second.mem);
        g_buffers.erase(it);
    }
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, Restore, 1) { RET(0); }
COMMETHOD(IDirectSoundBuffer, SetFormat, 2) { RET(0); }
COMMETHOD(IDirectSoundBuffer, GetFormat, 4)
{
    std::lock_guard lock(g_dsMutex);
    const Buffer& b = g_buffers[ARG(0)];
    if (ARG(1))
    {
        wr16(ARG(1), 1);
        wr16(ARG(1) + 2, (uint16_t)b.channels);
        wr32(ARG(1) + 4, b.rate);
        wr32(ARG(1) + 8, b.rate * frameBytes(b));
        wr16(ARG(1) + 12, (uint16_t)frameBytes(b));
        wr16(ARG(1) + 14, (uint16_t)b.bits);
    }
    if (ARG(3))
        wr32(ARG(3), 16);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, GetCaps, 2)
{
    std::lock_guard lock(g_dsMutex);
    const Buffer& b = g_buffers[ARG(0)];
    memset(g_mem + ARG(1), 0, 20);
    wr32(ARG(1), 20);
    wr32(ARG(1) + 8, b.bytes);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, GetCurrentPosition, 3)
{
    std::lock_guard lock(g_dsMutex);
    const Buffer& b = g_buffers[ARG(0)];
    const uint32_t play = playCursor(b);
    if (ARG(1))
        wr32(ARG(1), play);
    if (ARG(2)) // write cursor ~15 ms ahead of play
        wr32(ARG(2), b.bytes ? (play + (b.rate * frameBytes(b) * 15 / 1000) / frameBytes(b) * frameBytes(b)) % b.bytes : 0);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, SetCurrentPosition, 2)
{
    std::lock_guard lock(g_dsMutex);
    Buffer& b = g_buffers[ARG(0)];
    b.pos = frameBytes(b) ? ARG(1) / frameBytes(b) : 0;
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, GetStatus, 2)
{
    std::lock_guard lock(g_dsMutex);
    const Buffer& b = g_buffers[ARG(0)];
    wr32(ARG(1), (b.playing ? 1 : 0) | (b.playing && b.looping ? 4 : 0));
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, Play, 4)
{
    std::lock_guard lock(g_dsMutex);
    Buffer& b = g_buffers[ARG(0)];
    b.playing = true;
    b.looping = ARG(3) & 1;
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, Stop, 1)
{
    std::lock_guard lock(g_dsMutex);
    g_buffers[ARG(0)].playing = false;
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, SetVolume, 2)
{
    std::lock_guard lock(g_dsMutex);
    const int32_t mb = (int32_t)ARG(1); // hundredths of a dB, <= 0
    g_buffers[ARG(0)].volume = mb <= -10000 ? 0.0f : std::pow(10.0f, mb / 2000.0f);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, GetVolume, 2)
{
    wr32(ARG(1), 0);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, SetPan, 2) { RET(0); }
COMMETHOD(IDirectSoundBuffer, GetPan, 2)
{
    wr32(ARG(1), 0);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, SetFrequency, 2)
{
    std::lock_guard lock(g_dsMutex);
    g_buffers[ARG(0)].frequency = ARG(1);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, GetFrequency, 2)
{
    std::lock_guard lock(g_dsMutex);
    const Buffer& b = g_buffers[ARG(0)];
    wr32(ARG(1), b.frequency ? b.frequency : b.rate);
    RET(0);
}

COMMETHOD(IDirectSoundBuffer, Lock, 8)
{
    const uint32_t off0 = ARG(1), flags = ARG(7);
    uint32_t len = ARG(2);
    std::lock_guard lock(g_dsMutex);
    const Buffer& b = g_buffers[ARG(0)];
    if (!b.bytes)
        RET(0x80070057u);
    uint32_t off = off0;
    if (flags & 1) // DSBLOCK_FROMWRITECURSOR
        off = (playCursor(b) + b.rate * frameBytes(b) * 15 / 1000) % b.bytes;
    if (flags & 2) // DSBLOCK_ENTIREBUFFER
        len = b.bytes;
    off %= b.bytes;
    len = std::min(len, b.bytes);
    const uint32_t first = std::min(len, b.bytes - off);
    wr32(ARG(3), b.mem + off);
    wr32(ARG(4), first);
    if (ARG(5))
        wr32(ARG(5), len > first ? b.mem : 0);
    if (ARG(6))
        wr32(ARG(6), len - first);
    RET(0);
}
COMMETHOD(IDirectSoundBuffer, Unlock, 5) { RET(0); }
