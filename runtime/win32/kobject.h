// Kernel objects behind Win32 handles, and guest threads.
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

#include "hle.h"

// A waitable object state (events, mutexes, threads).
struct WaitableState
{
    std::mutex m;
    std::condition_variable cv;
    bool signaled = false;
    bool manualReset = true;

    void signal(bool on)
    {
        std::lock_guard lock(m);
        signaled = on;
        if (on)
            cv.notify_all();
    }
    bool isSignaled()
    {
        std::lock_guard lock(m);
        return signaled;
    }
    // Waits up to ms (0xFFFFFFFF = forever); consumes the signal of auto-reset objects.
    bool wait(uint32_t ms)
    {
        std::unique_lock lock(m);
        auto ready = [&] { return signaled; };
        if (ms == 0xFFFFFFFFu)
            cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(ms), ready))
            return false;
        if (!manualReset)
            signaled = false;
        return true;
    }
};

struct KObject
{
    virtual ~KObject() = default;
    virtual WaitableState* waitable() { return nullptr; }
};

uint32_t handleNew(std::shared_ptr<KObject> o);
std::shared_ptr<KObject> handleGet(uint32_t h);
bool handleClose(uint32_t h);

struct GuestThread
{
    Cpu cpu;
    uint32_t id = 0, teb = 0, stackLo = 0, stackHi = 0;
    uint32_t lastError = 0, exitCode = 0;
    uint32_t tls[64] = {};
};

GuestThread* guestThreadCreate(uint32_t stackSize);
void guestThreadEnter(GuestThread* t);
uint32_t msSinceStart();
