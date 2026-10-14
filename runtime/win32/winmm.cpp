// winmm: timers, waveOut on the platform audio output, and MCI CD audio played from the
// disc image's audio tracks.
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <regex>
#include <sstream>
#include <vector>

#include "../platform/platform.h"
#include "kobject.h"

std::string g_cuePath;
void dsoundMix(float* out, uint32_t frames);
void postMessageHost(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);

WIN32API(winmm, timeGetTime, 0) { RET(msSinceStart()); }
WIN32API(winmm, timeBeginPeriod, 1) { RET(0); }
WIN32API(winmm, timeEndPeriod, 1) { RET(0); }

// ---------------------------------------------------------------- audio output

namespace
{
    constexpr double kOutRate = 44100.0;

    struct WaveOut
    {
        bool open = false;
        uint32_t rate = 22050, channels = 2, bits = 16;
        std::deque<uint32_t> queue; // WAVEHDR guest addresses
        uint32_t pos = 0;           // byte offset in the front buffer
        double frac = 0;            // resampling phase
    };

    struct CdTrack
    {
        std::string file;
        uint64_t offset = 0, bytes = 0; // audio data after the pregap
        bool audio = false;
    };

    struct Cd
    {
        std::vector<CdTrack> tracks; // index 0 = track 1
        FILE* f = nullptr;
        uint64_t pos = 0, end = 0;
        bool playing = false;
        int track = 0;
        uint32_t notifyHwnd = 0;
        float volume = 0.7f;
    };

    std::mutex g_audioMutex;
    WaveOut g_wave;
    Cd g_cd;
    std::atomic<uint32_t> g_pendingNotify{ 0 };

    float sampleAt(const uint8_t* p, uint32_t bits)
    {
        if (bits == 8)
            return ((int)*p - 128) / 128.0f;
        int16_t s;
        memcpy(&s, p, 2);
        return s / 32768.0f;
    }

    // Mixes waveOut (resampled) and CD audio into interleaved stereo floats.
    void render(float* out, uint32_t frames)
    {
        memset(out, 0, frames * 8);
        std::lock_guard lock(g_audioMutex);
        if (g_wave.open)
        {
            const uint32_t frameBytes = g_wave.channels * g_wave.bits / 8;
            const double step = g_wave.rate / kOutRate;
            for (uint32_t i = 0; i < frames && !g_wave.queue.empty(); i++)
            {
                const uint32_t hdr = g_wave.queue.front();
                const uint32_t data = rd32(hdr), len = rd32(hdr + 4);
                const uint8_t* p = g_mem + data + g_wave.pos;
                const float l = sampleAt(p, g_wave.bits);
                const float r = g_wave.channels > 1 ? sampleAt(p + g_wave.bits / 8, g_wave.bits) : l;
                out[2 * i] += l;
                out[2 * i + 1] += r;
                g_wave.frac += step;
                while (g_wave.frac >= 1.0)
                {
                    g_wave.frac -= 1.0;
                    g_wave.pos += frameBytes;
                    if (g_wave.pos >= len)
                    {
                        g_wave.pos = 0;
                        wr32(hdr + 16, (rd32(hdr + 16) | 1) & ~0x10u); // WHDR_DONE, not INQUEUE
                        g_wave.queue.pop_front();
                        if (g_wave.queue.empty())
                            break;
                    }
                }
            }
        }
        if (g_cd.playing && g_cd.f)
        {
            int16_t buf[4096];
            uint32_t done = 0;
            while (done < frames)
            {
                const uint32_t want = std::min<uint32_t>(frames - done, 2048);
                const uint64_t left = (g_cd.end - g_cd.pos) / 4;
                const uint32_t n = (uint32_t)std::min<uint64_t>(want, left);
                if (n == 0)
                {
                    g_cd.playing = false;
                    g_pendingNotify = g_cd.notifyHwnd ? g_cd.notifyHwnd : 0xFFFFFFFFu;
                    break;
                }
                fseeko(g_cd.f, (off_t)g_cd.pos, SEEK_SET);
                const size_t got = fread(buf, 4, n, g_cd.f);
                for (size_t k = 0; k < got; k++)
                {
                    out[2 * (done + k)] += buf[2 * k] / 32768.0f * g_cd.volume;
                    out[2 * (done + k) + 1] += buf[2 * k + 1] / 32768.0f * g_cd.volume;
                }
                g_cd.pos += 4 * got;
                done += (uint32_t)got;
                if (got < n)
                {
                    g_cd.playing = false;
                    break;
                }
            }
        }
        dsoundMix(out, frames);
        static FILE* dump = getenv("KP_AUDIO_DUMP") ? fopen(getenv("KP_AUDIO_DUMP"), "wb") : nullptr;
        if (dump)
        {
            fwrite(out, 8, frames, dump);
            fflush(dump);
        }
        static float peak = 0;
        static uint64_t total = 0;
        for (uint32_t i = 0; i < frames * 2; i++)
        {
            out[i] = std::clamp(out[i], -1.0f, 1.0f);
            peak = std::max(peak, std::fabs(out[i]));
        }
        total += frames;
        if (total >= 44100 * 10)
        {
            logf("[audio] peak %.3f over the last 10 s (wave queue %zu, cd %s)", peak, g_wave.queue.size(), g_cd.playing ? "playing" : "idle");
            peak = 0;
            total = 0;
        }
    }
}

void audioStartOutput()
{
    static bool started = false;
    if (started || getenv("KP_NO_AUDIO"))
        return;
    started = true;
    platform::audioStart(render);
}

// Delivers MM_MCINOTIFY for a finished CD track (called from the message pump side).
void winmmPoll()
{
    const uint32_t h = g_pendingNotify.exchange(0);
    if (h && h != 0xFFFFFFFFu)
        postMessageHost(h, 0x03B9, 1, 1); // MM_MCINOTIFY, MCI_NOTIFY_SUCCESSFUL, device 1
}

WIN32API(winmm, waveOutOpen, 6)
{
    const uint32_t fmt = ARG(2);
    if (ARG(5) & 1) // WAVE_FORMAT_QUERY
        RET(0);
    // Output starts before the lock is taken: starting it may wait for the first render
    // (RemoteIO), which takes the lock.
    audioStartOutput();
    std::lock_guard lock(g_audioMutex);
    g_wave = WaveOut{};
    g_wave.open = true;
    g_wave.channels = rd16(fmt + 2);
    g_wave.rate = rd32(fmt + 4);
    g_wave.bits = rd16(fmt + 14);
    if (ARG(0))
        wr32(ARG(0), 0x0000A0D0);
    logf("[audio] waveOutOpen %u Hz, %u channels, %u bits", g_wave.rate, g_wave.channels, g_wave.bits);
    RET(0);
}
WIN32API(winmm, waveOutPrepareHeader, 3)
{
    wr32(ARG(1) + 16, rd32(ARG(1) + 16) | 2);
    RET(0);
}
WIN32API(winmm, waveOutUnprepareHeader, 3)
{
    wr32(ARG(1) + 16, rd32(ARG(1) + 16) & ~2u);
    RET(0);
}
WIN32API(winmm, waveOutWrite, 3)
{
    std::lock_guard lock(g_audioMutex);
    const uint32_t h = ARG(1);
    wr32(h + 16, (rd32(h + 16) & ~1u) | 0x10);
    g_wave.queue.push_back(h);
    RET(0);
}
WIN32API(winmm, waveOutReset, 1)
{
    std::lock_guard lock(g_audioMutex);
    for (uint32_t h : g_wave.queue)
        wr32(h + 16, (rd32(h + 16) | 1) & ~0x10u);
    g_wave.queue.clear();
    g_wave.pos = 0;
    RET(0);
}
WIN32API(winmm, waveOutClose, 1)
{
    std::lock_guard lock(g_audioMutex);
    g_wave.open = false;
    g_wave.queue.clear();
    RET(0);
}
WIN32API(winmm, waveOutGetNumDevs, 0) { RET(1); }
WIN32API(winmm, waveOutGetPosition, 3) { RET(0); }

// ---------------------------------------------------------------- MCI CD audio

static void loadCue()
{
    if (!g_cd.tracks.empty())
        return;
    std::string cue = g_cuePath;
    if (const char* env = getenv("KP_CUE"))
        cue = env;
    std::ifstream f(cue);
    if (!f)
    {
        logf("[cd] no cue sheet found (set KP_CUE); CD music disabled");
        return;
    }
    const std::string dir = cue.substr(0, cue.find_last_of('/') + 1);
    struct stat st{};
    const bool discImage = stat(g_discDir.c_str(), &st) == 0 && S_ISREG(st.st_mode);
    const uint64_t discImageBytes = (uint64_t)st.st_size;
    std::string line, file;
    std::regex reFile("FILE \"(.+)\""), reTrack("TRACK (\\d+) (\\w+)"), reIndex("INDEX 01 (\\d+):(\\d+):(\\d+)");
    std::smatch m;
    while (std::getline(f, line))
    {
        if (std::regex_search(line, m, reFile))
            file = dir + m[1].str();
        else if (std::regex_search(line, m, reTrack))
        {
            CdTrack t;
            t.file = file;
            t.audio = m[2].str() == "AUDIO";
            g_cd.tracks.push_back(t);
        }
        else if (std::regex_search(line, m, reIndex) && !g_cd.tracks.empty())
        {
            const uint64_t frames = (std::stoull(m[1]) * 60 + std::stoull(m[2])) * 75 + std::stoull(m[3]);
            auto& t = g_cd.tracks.back();
            t.offset = frames * 2352;
            if (FILE* fp = fopen(t.file.c_str(), "rb"))
            {
                fseeko(fp, 0, SEEK_END);
                t.bytes = (uint64_t)ftello(fp) - t.offset;
                fclose(fp);
            }
            else if (!t.audio && discImage)
                t.bytes = discImageBytes / 2048 * 2352; // the data track is present as its .iso image
        }
    }
    logf("[cd] %zu tracks from %s", g_cd.tracks.size(), cue.c_str());
}

WIN32API(winmm, mciSendCommandA, 4)
{
    const uint32_t msg = ARG(1), flags = ARG(2), p = ARG(3);
    if (msg == 0x0803) // MCI_OPEN: start the output first (outside the lock, see waveOutOpen)
        audioStartOutput();
    std::lock_guard lock(g_audioMutex);
    switch (msg)
    {
    case 0x0803: // MCI_OPEN
        loadCue();
        if (g_cd.tracks.empty())
            RET(275); // MCIERR_DEVICE_NOT_READY-ish
        wr32(p + 4, 1);
        RET(0);
    case 0x0804: // MCI_CLOSE
        g_cd.playing = false;
        if (g_cd.f)
            fclose(g_cd.f);
        g_cd.f = nullptr;
        RET(0);
    case 0x080D: // MCI_SET
        RET(0);
    case 0x0814: // MCI_STATUS
    {
        const uint32_t item = rd32(p + 8), track = rd32(p + 12);
        uint32_t v = 0;
        switch (item)
        {
        case 7: v = 1; break;                                 // READY
        case 5: v = 1; break;                                 // MEDIA_PRESENT
        case 3: v = (uint32_t)g_cd.tracks.size(); break;      // NUMBER_OF_TRACKS
        case 0x4001:                                          // MCI_CDA_STATUS_TYPE_TRACK
            v = track >= 1 && track <= g_cd.tracks.size() && g_cd.tracks[track - 1].audio ? 0x440 : 0x441;
            break;
        case 1: // LENGTH (MSF)
        {
            uint64_t frames = 0;
            if (!(flags & 0x10)) // no MCI_TRACK: length of the whole disc
            {
                for (const auto& t : g_cd.tracks)
                    frames += (t.offset + t.bytes) / 2352;
            }
            else if (track >= 1 && track <= g_cd.tracks.size())
                frames = g_cd.tracks[track - 1].bytes / 2352;
            v = (uint32_t)(frames / 75 / 60) | (uint32_t)(frames / 75 % 60) << 8 | (uint32_t)(frames % 75) << 16;
            break;
        }
        case 4: v = g_cd.playing ? 526 : 525; break; // MODE: playing / stopped
        default: break;
        }
        wr32(p + 4, v);
        RET(0);
    }
    case 0x0806: // MCI_PLAY
    {
        const uint32_t from = rd32(p + 4), to = rd32(p + 8);
        const int track = (flags & 4) ? (int)(from & 0xFF) : g_cd.track; // MCI_FROM
        if (track < 1 || track > (int)g_cd.tracks.size() || !g_cd.tracks[track - 1].audio)
            RET(277);
        const auto& t = g_cd.tracks[track - 1];
        if (g_cd.f)
            fclose(g_cd.f);
        g_cd.f = fopen(t.file.c_str(), "rb");
        g_cd.pos = t.offset;
        g_cd.end = t.offset + t.bytes;
        if (flags & 8) // MCI_TO (TMSF): stop at that track position
        {
            const uint32_t tt = to & 0xFF;
            if ((int)tt == track && (to >> 8))
            {
                const uint64_t frames = ((to >> 8 & 0xFF) * 60 + (to >> 16 & 0xFF)) * 75 + (to >> 24 & 0xFF);
                g_cd.end = std::min(g_cd.end, t.offset + frames * 2352);
            }
        }
        g_cd.track = track;
        g_cd.playing = g_cd.f != nullptr;
        g_cd.notifyHwnd = (flags & 1) ? rd32(p) : 0; // MCI_NOTIFY -> dwCallback = hwnd
        logf("[cd] play track %d", track);
        RET(0);
    }
    case 0x0808: // MCI_STOP
    case 0x0809: // MCI_PAUSE
        g_cd.playing = false;
        RET(0);
    case 0x0855: // MCI_RESUME
        g_cd.playing = g_cd.f != nullptr && g_cd.pos < g_cd.end;
        RET(0);
    default:
        logf("[cd] mciSendCommand %04X ignored", msg);
        RET(0);
    }
}

WIN32API(winmm, mciGetErrorStringA, 3)
{
    if (ARG(1) && ARG(2))
        strncpy(gstr(ARG(1)), "MCI error", ARG(2));
    RET(1);
}
