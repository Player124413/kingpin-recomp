// Guest memory allocators: Win32 heaps (HeapAlloc & co.) and VirtualAlloc regions.
#include <sys/mman.h>

#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "hle.h"

namespace
{
    // First-fit allocator over a guest address range with coalescing free ranges.
    class RangeAlloc
    {
    public:
        RangeAlloc(uint32_t lo, uint32_t hi) { free_[lo] = hi - lo; }

        uint32_t alloc(uint32_t size, uint32_t align, uint32_t want = 0)
        {
            std::lock_guard lock(m_);
            if (want)
            {
                // Exact placement inside a free range.
                auto it = free_.upper_bound(want);
                if (it == free_.begin())
                    return 0;
                --it;
                const uint32_t s = it->first, n = it->second;
                if (want < s || (uint64_t)want + size > (uint64_t)s + n)
                    return 0;
                free_.erase(it);
                if (want > s)
                    free_[s] = want - s;
                if ((uint64_t)want + size < (uint64_t)s + n)
                    free_[want + size] = (uint32_t)((uint64_t)s + n - want - size);
                used_[want] = size;
                return want;
            }
            for (auto it = free_.begin(); it != free_.end(); ++it)
            {
                const uint32_t s = it->first, n = it->second;
                const uint32_t a = (s + align - 1) & ~(align - 1);
                if (a - s > n || n - (a - s) < size)
                    continue;
                free_.erase(it);
                if (a > s)
                    free_[s] = a - s;
                if (n - (a - s) > size)
                    free_[a + size] = n - (a - s) - size;
                used_[a] = size;
                return a;
            }
            return 0;
        }

        uint32_t size(uint32_t a)
        {
            std::lock_guard lock(m_);
            auto it = used_.find(a);
            return it == used_.end() ? 0 : it->second;
        }

        // Grows or shrinks a block in place when the following range is free.
        bool resize(uint32_t a, uint32_t size)
        {
            std::lock_guard lock(m_);
            auto it = used_.find(a);
            if (it == used_.end())
                return false;
            const uint32_t old = it->second;
            if (size <= old)
            {
                it->second = size;
                insertFree(a + size, old - size);
                return true;
            }
            auto f = free_.find(a + old);
            if (f == free_.end() || f->second < size - old)
                return false;
            const uint32_t rest = f->second - (size - old);
            free_.erase(f);
            if (rest)
                free_[a + size] = rest;
            it->second = size;
            return true;
        }

        bool release(uint32_t a)
        {
            std::lock_guard lock(m_);
            auto it = used_.find(a);
            if (it == used_.end())
                return false;
            const uint32_t n = it->second;
            used_.erase(it);
            insertFree(a, n);
            return true;
        }

        // Block containing address a (for VirtualAlloc commits inside a reservation).
        bool containing(uint32_t a, uint32_t& start, uint32_t& n)
        {
            std::lock_guard lock(m_);
            auto it = used_.upper_bound(a);
            if (it == used_.begin())
                return false;
            --it;
            if (a >= it->first && a - it->first < it->second)
            {
                start = it->first;
                n = it->second;
                return true;
            }
            return false;
        }

    private:
        void insertFree(uint32_t a, uint32_t n)
        {
            if (!n)
                return;
            auto next = free_.find(a + n);
            if (next != free_.end())
            {
                n += next->second;
                free_.erase(next);
            }
            auto prev = free_.lower_bound(a);
            if (prev != free_.begin())
            {
                --prev;
                if (prev->first + prev->second == a)
                {
                    prev->second += n;
                    return;
                }
            }
            free_[a] = n;
        }

        std::mutex m_;
        std::map<uint32_t, uint32_t> free_;
        std::map<uint32_t, uint32_t> used_;
    };

    RangeAlloc& heapArena()
    {
        static RangeAlloc a(0x30000000u, 0x70000000u);
        return a;
    }
    RangeAlloc& vaArena()
    {
        static RangeAlloc a(0x70000000u, 0xEF000000u);
        return a;
    }

    void zeroRange(uint32_t a, uint32_t n)
    {
        // Fresh anonymous pages read as zero and release physical memory.
        const uint32_t lo = (a + 0x3FFF) & ~0x3FFFu, hi = (a + n) & ~0x3FFFu;
        if (hi > lo)
        {
            mmap(g_mem + lo, hi - lo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
            memset(g_mem + a, 0, lo - a);
            memset(g_mem + hi, 0, a + n - hi);
        }
        else
            memset(g_mem + a, 0, n);
    }

    std::mutex g_heapsMutex;
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> g_heaps; // heap handle -> blocks
    std::unordered_map<uint32_t, uint32_t> g_reqSize;                   // block -> requested size
}

uint32_t heapAlloc(uint32_t size, bool zero)
{
    const uint32_t n = (std::max<uint32_t>(size, 1) + 15) & ~15u;
    const uint32_t a = heapArena().alloc(n, 16);
    if (!a)
    {
        logf("[mem] guest heap exhausted (%u bytes)", size);
        return 0;
    }
    if (zero)
        memset(g_mem + a, 0, n);
    std::lock_guard lock(g_heapsMutex);
    g_reqSize[a] = size;
    return a;
}

void heapFree(uint32_t a)
{
    if (!a)
        return;
    {
        std::lock_guard lock(g_heapsMutex);
        g_reqSize.erase(a);
    }
    heapArena().release(a);
}

uint32_t heapSize(uint32_t a)
{
    std::lock_guard lock(g_heapsMutex);
    auto it = g_reqSize.find(a);
    return it == g_reqSize.end() ? 0 : it->second;
}

uint32_t heapRealloc(uint32_t a, uint32_t size)
{
    if (!a)
        return heapAlloc(size);
    const uint32_t n = (std::max<uint32_t>(size, 1) + 15) & ~15u;
    if (heapArena().resize(a, n))
    {
        std::lock_guard lock(g_heapsMutex);
        g_reqSize[a] = size;
        return a;
    }
    const uint32_t old = heapSize(a);
    const uint32_t b = heapAlloc(size);
    if (!b)
        return 0;
    memcpy(g_mem + b, g_mem + a, std::min(old, size));
    heapFree(a);
    return b;
}

uint32_t vaReserve(uint32_t size, uint32_t want)
{
    const uint32_t n = (size + 0xFFFF) & ~0xFFFFu;
    return vaArena().alloc(n, 0x10000, want ? want & ~0xFFFFu : 0);
}

bool vaRelease(uint32_t a)
{
    uint32_t start, n;
    if (!vaArena().containing(a, start, n) || start != a)
        return false;
    zeroRange(a, n);
    return vaArena().release(a);
}

uint32_t guestStrdup(const std::string& s)
{
    const uint32_t a = heapAlloc((uint32_t)s.size() + 1);
    memcpy(g_mem + a, s.c_str(), s.size() + 1);
    return a;
}

// ---------------------------------------------------------------- kernel32 heap API

WIN32API(kernel32, HeapCreate, 3)
{
    static uint32_t next = 0x00A00000;
    std::lock_guard lock(g_heapsMutex);
    const uint32_t h = next += 0x10;
    g_heaps[h];
    RET(h);
}

WIN32API(kernel32, HeapDestroy, 1)
{
    std::unordered_set<uint32_t> blocks;
    {
        std::lock_guard lock(g_heapsMutex);
        auto it = g_heaps.find(ARG(0));
        if (it == g_heaps.end())
            RET(0);
        blocks = std::move(it->second);
        g_heaps.erase(it);
    }
    for (uint32_t b : blocks)
        heapFree(b);
    RET(1);
}

static void track(uint32_t heap, uint32_t a, uint32_t old = 0)
{
    std::lock_guard lock(g_heapsMutex);
    auto it = g_heaps.find(heap);
    if (it == g_heaps.end())
        return;
    if (old)
        it->second.erase(old);
    if (a)
        it->second.insert(a);
}

WIN32API(kernel32, HeapAlloc, 3)
{
    const uint32_t a = heapAlloc(ARG(2), ARG(1) & 0x8);
    track(ARG(0), a);
    RET(a);
}

WIN32API(kernel32, HeapFree, 3)
{
    track(ARG(0), 0, ARG(2));
    heapFree(ARG(2));
    RET(1);
}

WIN32API(kernel32, HeapReAlloc, 4)
{
    const uint32_t heap = ARG(0), flags = ARG(1), a = ARG(2), size = ARG(3);
    const uint32_t old = heapSize(a);
    if (flags & 0x10) // HEAP_REALLOC_IN_PLACE_ONLY
    {
        const uint32_t n = (std::max<uint32_t>(size, 1) + 15) & ~15u;
        if (!heapArena().resize(a, n))
            RET(0);
        {
            std::lock_guard lock(g_heapsMutex);
            g_reqSize[a] = size;
        }
        if ((flags & 0x8) && size > old)
            memset(g_mem + a + old, 0, size - old);
        RET(a);
    }
    const uint32_t b = heapRealloc(a, size);
    if (b && (flags & 0x8) && size > old)
        memset(g_mem + b + old, 0, size - old);
    if (b)
        track(heap, b, a);
    RET(b);
}

WIN32API(kernel32, HeapSize, 3) { RET(heapSize(ARG(2))); }
WIN32API(kernel32, GetProcessHeap, 0)
{
    static uint32_t h = [] {
        std::lock_guard lock(g_heapsMutex);
        g_heaps[0x00900000];
        return 0x00900000u;
    }();
    RET(h);
}

WIN32API(kernel32, VirtualAlloc, 4)
{
    const uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (addr)
    {
        uint32_t start, n;
        if ((type & 0x1000) && !(type & 0x2000) && vaArena().containing(addr, start, n))
            RET(addr & ~0xFFFu); // commit inside a reservation
        if (vaArena().containing(addr, start, n))
            RET(addr & ~0xFFFu);
        const uint32_t a = vaReserve(size + (addr & 0xFFFF), addr);
        if (!a)
        {
            setLastError(487); // ERROR_INVALID_ADDRESS
            RET(0);
        }
        RET(addr & ~0xFFFu);
    }
    const uint32_t a = vaReserve(size);
    if (!a)
    {
        logf("[mem] VirtualAlloc of %u bytes failed", size);
        setLastError(8);
        RET(0);
    }
    RET(a);
}

WIN32API(kernel32, VirtualFree, 3)
{
    const uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (type & 0x8000) // MEM_RELEASE
        RET(vaRelease(addr) ? 1 : 0);
    if (type & 0x4000) // MEM_DECOMMIT
    {
        zeroRange(addr & ~0xFFFu, (size + (addr & 0xFFF) + 0xFFF) & ~0xFFFu);
        RET(1);
    }
    RET(1);
}

WIN32API(kernel32, GlobalAlloc, 2) { RET(heapAlloc(ARG(1), ARG(0) & 0x40)); }
WIN32API(kernel32, GlobalFree, 1)
{
    heapFree(ARG(0));
    RET(0);
}
WIN32API(kernel32, GlobalLock, 1) { RET(ARG(0)); }
WIN32API(kernel32, GlobalUnlock, 1) { RET(1); }
WIN32API(kernel32, GlobalSize, 1) { RET(heapSize(ARG(0))); }
WIN32API(kernel32, GlobalMemoryStatus, 1)
{
    const uint32_t p = ARG(0);
    wr32(p, 32);
    wr32(p + 4, 10);
    wr32(p + 8, 512u << 20);
    wr32(p + 12, 384u << 20);
    wr32(p + 16, 1024u << 20);
    wr32(p + 20, 900u << 20);
    wr32(p + 24, 2000u << 20);
    wr32(p + 28, 1800u << 20);
    RET(0);
}

WIN32API(kernel32, VirtualProtect, 4)
{
    if (ARG(3))
        wr32(ARG(3), 0x40); // PAGE_EXECUTE_READWRITE (guest memory is always writable)
    RET(1);
}
