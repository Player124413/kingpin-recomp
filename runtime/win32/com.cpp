// See com.h.
#include "com.h"

#include <map>
#include <mutex>
#include <vector>

namespace
{
    std::mutex g_comMutex;
    std::map<std::string, std::vector<std::string>>& interfaces()
    {
        static std::map<std::string, std::vector<std::string>> m;
        return m;
    }
    std::map<std::string, uint32_t> g_vtables;
}

bool comRegisterInterface(const char* name, std::initializer_list<const char*> methods)
{
    std::lock_guard lock(g_comMutex);
    interfaces()[name].assign(methods.begin(), methods.end());
    return true;
}

uint32_t comNew(const char* iface)
{
    std::lock_guard lock(g_comMutex);
    uint32_t& vt = g_vtables[iface];
    if (!vt)
    {
        const auto& ms = interfaces()[iface];
        vt = heapAlloc((uint32_t)ms.size() * 4);
        for (size_t k = 0; k < ms.size(); k++)
            wr32(vt + 4 * (uint32_t)k, hleThunk(iface, ms[k]));
    }
    const uint32_t obj = heapAlloc(8, true);
    wr32(obj, vt);
    wr32(obj + 4, 1);
    return obj;
}
