// advapi32 (no registry) and wsock32 (single player only: sockets cannot be opened, the game
// uses its loopback connection).
#include "hle.h"

WIN32API(advapi32, RegOpenKeyExA, 5) { RET(2); } // ERROR_FILE_NOT_FOUND
WIN32API(advapi32, RegOpenKeyA, 3) { RET(2); }
WIN32API(advapi32, RegQueryValueExA, 6) { RET(2); }
WIN32API(advapi32, RegCloseKey, 1) { RET(0); }
WIN32API(advapi32, RegCreateKeyExA, 9) { RET(5); }
WIN32API(advapi32, RegSetValueExA, 6) { RET(5); }

namespace
{
    constexpr uint32_t kSocketError = 0xFFFFFFFFu;
    constexpr uint32_t kNetDown = 10050; // WSAENETDOWN
}

WIN32API(wsock32, WSAStartup, 2)
{
    const uint32_t d = ARG(1);
    memset(g_mem + d, 0, 400);
    wr16(d, 0x0101);
    wr16(d + 2, 0x0202);
    strcpy(gstr(d + 4), "macOS host (no network)");
    RET(0);
}
WIN32API(wsock32, WSACleanup, 0) { RET(0); }
WIN32API(wsock32, WSAGetLastError, 0) { RET(kNetDown); }
WIN32API(wsock32, socket, 3) { RET(kSocketError); }
WIN32API(wsock32, closesocket, 1) { RET(0); }
WIN32API(wsock32, ioctlsocket, 3) { RET(kSocketError); }
WIN32API(wsock32, setsockopt, 5) { RET(kSocketError); }
WIN32API(wsock32, bind, 3) { RET(kSocketError); }
WIN32API(wsock32, connect, 3) { RET(kSocketError); }
WIN32API(wsock32, send, 4) { RET(kSocketError); }
WIN32API(wsock32, sendto, 6) { RET(kSocketError); }
WIN32API(wsock32, recvfrom, 6) { RET(kSocketError); }
WIN32API(wsock32, select, 5) { RET(0); }
WIN32API(wsock32, gethostbyname, 1) { RET(0); }
WIN32API(wsock32, gethostname, 2)
{
    strncpy(gstr(ARG(0)), "localhost", ARG(1));
    RET(0);
}
WIN32API(wsock32, inet_addr, 1)
{
    unsigned a, b, c2, d;
    if (sscanf(gstr(ARG(0)), "%u.%u.%u.%u", &a, &b, &c2, &d) != 4)
        RET(0xFFFFFFFFu);
    RET(a | b << 8 | c2 << 16 | d << 24);
}
WIN32API(wsock32, htons, 1) { RET(__builtin_bswap16((uint16_t)ARG(0))); }
WIN32API(wsock32, ntohs, 1) { RET(__builtin_bswap16((uint16_t)ARG(0))); }
WIN32API(wsock32, htonl, 1) { RET(__builtin_bswap32(ARG(0))); }
WIN32API(wsock32, ntohl, 1) { RET(__builtin_bswap32(ARG(0))); }
