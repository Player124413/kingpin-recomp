// kernel32 file API over the host file system.
//
// The game sees itself installed at C:\KINGPIN (its working directory); that maps to the host
// game directory. D:\ is the CD: a directory (the mounted data track) or the data track image
// itself (read with iso9660.cpp); other C:\ paths go to a sandbox directory inside the game
// directory.
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

#include "iso9660.h"
#include "kobject.h"

std::string g_gameDir;
std::string g_discDir;

namespace
{
    struct FileObj : KObject
    {
        int fd = -1;
        std::string path;
        bool console = false; // qconsole.log: also copied to the host log (logcat on Android)
        bool iso = false; // a file inside the disc image: byte range [isoBase, isoBase + isoSize)
        uint64_t isoBase = 0, isoSize = 0, isoPos = 0;
        ~FileObj() override
        {
            if (fd >= 0)
                close(fd);
        }
    };

    struct FindObj : KObject
    {
        std::string dir;
        std::vector<std::string> names;
        size_t next = 0;
    };

    bool iequalPrefix(const std::string& s, const char* p)
    {
        const size_t n = strlen(p);
        return s.size() >= n && strncasecmp(s.c_str(), p, n) == 0;
    }

    // Case-insensitive lookup of each path component below `root` (host file systems may be
    // case sensitive; the game mixes cases freely).
    std::string resolveCase(const std::string& root, const std::string& rel)
    {
        std::string cur = root;
        size_t pos = 0;
        while (pos <= rel.size())
        {
            size_t e = rel.find('/', pos);
            if (e == std::string::npos)
                e = rel.size();
            const std::string part = rel.substr(pos, e - pos);
            pos = e + 1;
            if (part.empty() || part == ".")
                continue;
            if (part == "..")
            {
                const size_t s = cur.find_last_of('/');
                if (s != std::string::npos && cur.size() > root.size())
                    cur.resize(s);
                continue;
            }
            std::string next = cur + "/" + part;
            struct stat st;
            if (stat(next.c_str(), &st) != 0)
            {
                if (DIR* d = opendir(cur.c_str()))
                {
                    while (dirent* de = readdir(d))
                        if (strcasecmp(de->d_name, part.c_str()) == 0)
                        {
                            next = cur + "/" + de->d_name;
                            break;
                        }
                    closedir(d);
                }
            }
            cur = next;
        }
        return cur;
    }

    // Paths inside the disc image are returned by hostPath() as "iso:<path>".
    constexpr const char* kIsoPrefix = "iso:";
    bool isIsoPath(const std::string& p) { return p.compare(0, 4, kIsoPrefix) == 0; }

    bool discIsImage()
    {
        static const bool image = [] {
            struct stat st;
            if (stat(g_discDir.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                return false;
            const bool ok = iso::open(g_discDir);
            logf("[files] disc image %s: %s", g_discDir.c_str(), ok ? "ISO 9660" : "not readable");
            return ok;
        }();
        return image;
    }

    uint64_t fileTime(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }

    void fillFindData(uint32_t p, const std::string& dir, const std::string& name)
    {
        memset(g_mem + p, 0, 320);
        struct stat st{};
        iso::Entry e;
        if (isIsoPath(dir) && iso::lookup(dir.substr(4) + "/" + name, e))
        {
            st.st_mode = e.dir ? S_IFDIR : S_IFREG;
            st.st_size = (off_t)e.size;
        }
        else
            stat((dir + "/" + name).c_str(), &st);
        wr32(p, S_ISDIR(st.st_mode) ? 0x10 : 0x80);
        const uint64_t ft = fileTime(st.st_mtime);
        wr64(p + 4, ft);
        wr64(p + 12, ft);
        wr64(p + 20, ft);
        wr32(p + 28, (uint32_t)((uint64_t)st.st_size >> 32));
        wr32(p + 32, (uint32_t)st.st_size);
        strncpy(gstr(p + 44), name.c_str(), 259);
    }
}

std::string hostPath(const std::string& guestPath)
{
    std::string p = guestPath;
    for (auto& ch : p)
        if (ch == '\\')
            ch = '/';
    std::string root = g_gameDir, rel;
    if (p.size() >= 2 && p[1] == ':')
    {
        const char drive = (char)toupper(p[0]);
        rel = p.substr(2);
        if (drive == 'D')
            root = g_discDir;
        else if (iequalPrefix(rel, "/KINGPIN/") || strcasecmp(rel.c_str(), "/KINGPIN") == 0)
            rel = rel.substr(8);
        else
            root = g_gameDir + "/drive_c";
    }
    else if (!p.empty() && p[0] == '/')
    {
        rel = iequalPrefix(p, "/KINGPIN/") ? p.substr(8) : p;
        if (!iequalPrefix(p, "/KINGPIN/"))
            root = g_gameDir + "/drive_c";
    }
    else
        rel = p;
    static const bool trace = getenv("KP_TRACE_DISC") != nullptr;
    if (trace && root == g_discDir)
        logf("[files] disc access %s", guestPath.c_str());
    if (root == g_discDir && discIsImage())
        return kIsoPrefix + rel;
    return resolveCase(root, rel);
}

WIN32API(kernel32, CreateFileA, 7)
{
    const std::string guest = gstr(ARG(0));
    const uint32_t access = ARG(1), disp = ARG(4);
    const std::string path = hostPath(guest);
    if (isIsoPath(path))
    {
        iso::Entry e;
        if (!iso::lookup(path.substr(4), e) || e.dir || (access & 0x40000000u))
        {
            setLastError(e.dir ? 5 : (access & 0x40000000u) ? 19 : 2); // ACCESS_DENIED / WRITE_PROTECT / FILE_NOT_FOUND
            RET(0xFFFFFFFFu);
        }
        auto f = std::make_shared<FileObj>();
        f->path = path;
        f->iso = true;
        f->isoBase = e.offset;
        f->isoSize = e.size;
        RET(handleNew(f));
    }
    int flags = (access & 0x40000000u) ? ((access & 0x80000000u) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disp)
    {
    case 1: flags |= O_CREAT | O_EXCL; break;
    case 2: flags |= O_CREAT | O_TRUNC; break;
    case 4: flags |= O_CREAT; break;
    case 5: flags |= O_TRUNC; break;
    default: break;
    }
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
    {
        setLastError(5);
        RET(0xFFFFFFFFu);
    }
    const int fd = open(path.c_str(), flags, 0644);
    static const bool trace = getenv("KP_TRACE_FILES") != nullptr;
    if (trace)
        logf("[files] open %s -> %s: %s", guest.c_str(), path.c_str(), fd < 0 ? strerror(errno) : "ok");
    if (fd < 0)
    {
        setLastError(errno == ENOENT ? 2 : errno == EEXIST ? 80 : 5);
        RET(0xFFFFFFFFu);
    }
    auto f = std::make_shared<FileObj>();
    f->fd = fd;
    f->path = path;
    f->console = path.size() >= 12 && strcasecmp(path.c_str() + path.size() - 12, "qconsole.log") == 0;
    setLastError((disp == 2 || disp == 4) && access ? 0 : 0);
    RET(handleNew(f));
}

static std::shared_ptr<FileObj> fileOf(uint32_t h) { return std::dynamic_pointer_cast<FileObj>(handleGet(h)); }

WIN32API(kernel32, ReadFile, 5)
{
    const uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), outN = ARG(3);
    auto f = fileOf(h);
    if (!f)
    {
        if (outN)
            wr32(outN, 0);
        RET(0);
    }
    ssize_t r;
    if (f->iso)
    {
        const uint64_t take = std::min<uint64_t>(n, f->isoSize - std::min(f->isoPos, f->isoSize));
        r = (ssize_t)iso::read(f->isoBase + f->isoPos, g_mem + buf, take);
        if (r > 0)
            f->isoPos += (uint64_t)r;
    }
    else
        r = read(f->fd, g_mem + buf, n);
    if (outN)
        wr32(outN, r > 0 ? (uint32_t)r : 0);
    RET(r >= 0 ? 1 : 0);
}

WIN32API(kernel32, WriteFile, 5)
{
    const uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), outN = ARG(3);
    if (h == 0x11 || h == 0x12)
    {
        fwrite(g_mem + buf, 1, n, stderr);
        if (outN)
            wr32(outN, n);
        RET(1);
    }
    auto f = fileOf(h);
    if (!f)
        RET(0);
    if (f->console && n)
    {
        std::string text((const char*)g_mem + buf, n);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (!text.empty())
            logf("[console] %s", text.c_str());
    }
    const ssize_t r = write(f->fd, g_mem + buf, n);
    if (outN)
        wr32(outN, r > 0 ? (uint32_t)r : 0);
    RET(r >= 0 ? 1 : 0);
}

WIN32API(kernel32, SetFilePointer, 4)
{
    auto f = fileOf(ARG(0));
    if (!f)
        RET(0xFFFFFFFFu);
    int64_t off = (int32_t)ARG(1);
    if (ARG(2))
        off = (int64_t)((uint64_t)rd32(ARG(2)) << 32 | ARG(1));
    const int whence = ARG(3) == 0 ? SEEK_SET : ARG(3) == 1 ? SEEK_CUR : SEEK_END;
    off_t r;
    if (f->iso)
    {
        r = (whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)f->isoPos : (off_t)f->isoSize) + off;
        if (r >= 0)
            f->isoPos = (uint64_t)r;
    }
    else
        r = lseek(f->fd, off, whence);
    if (r < 0)
        RET(0xFFFFFFFFu);
    if (ARG(2))
        wr32(ARG(2), (uint32_t)((uint64_t)r >> 32));
    RET((uint32_t)r);
}

WIN32API(kernel32, GetFileSize, 2)
{
    auto f = fileOf(ARG(0));
    struct stat st{};
    if (f && f->iso)
        st.st_size = (off_t)f->isoSize;
    else if (!f || fstat(f->fd, &st) != 0)
        RET(0xFFFFFFFFu);
    if (ARG(1))
        wr32(ARG(1), (uint32_t)((uint64_t)st.st_size >> 32));
    RET((uint32_t)st.st_size);
}

WIN32API(kernel32, FlushFileBuffers, 1) { RET(1); }
WIN32API(kernel32, SetEndOfFile, 1)
{
    auto f = fileOf(ARG(0));
    if (!f)
        RET(0);
    RET(ftruncate(f->fd, lseek(f->fd, 0, SEEK_CUR)) == 0);
}
WIN32API(kernel32, GetFileType, 1)
{
    const uint32_t h = ARG(0);
    if (h >= 0x10 && h <= 0x12)
        RET(2); // FILE_TYPE_CHAR
    RET(fileOf(h) ? 1 : 0);
}

WIN32API(kernel32, DeleteFileA, 1) { RET(unlink(hostPath(gstr(ARG(0))).c_str()) == 0); }
WIN32API(kernel32, MoveFileA, 2) { RET(rename(hostPath(gstr(ARG(0))).c_str(), hostPath(gstr(ARG(1))).c_str()) == 0); }
WIN32API(kernel32, CreateDirectoryA, 2)
{
    const int r = mkdir(hostPath(gstr(ARG(0))).c_str(), 0755);
    if (r != 0)
        setLastError(errno == EEXIST ? 183 : 3);
    RET(r == 0);
}
WIN32API(kernel32, RemoveDirectoryA, 1) { RET(rmdir(hostPath(gstr(ARG(0))).c_str()) == 0); }
WIN32API(kernel32, GetFileAttributesA, 1)
{
    const std::string path = hostPath(gstr(ARG(0)));
    if (isIsoPath(path))
    {
        iso::Entry e;
        if (!iso::lookup(path.substr(4), e))
        {
            setLastError(2);
            RET(0xFFFFFFFFu);
        }
        RET(e.dir ? 0x11 : 0x81); // read-only
    }
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
    {
        setLastError(2);
        RET(0xFFFFFFFFu);
    }
    RET(S_ISDIR(st.st_mode) ? 0x10 : 0x80);
}

WIN32API(kernel32, GetCurrentDirectoryA, 2)
{
    const char* d = "C:\\KINGPIN";
    if (ARG(0) > strlen(d))
        strcpy(gstr(ARG(1)), d);
    RET(strlen(d));
}
WIN32API(kernel32, SetCurrentDirectoryA, 1) { RET(1); }

WIN32API(kernel32, GetFullPathNameA, 4)
{
    std::string p = gstr(ARG(0));
    if (!(p.size() >= 2 && p[1] == ':'))
        p = (p.size() && (p[0] == '\\' || p[0] == '/')) ? "C:" + p : "C:\\KINGPIN\\" + p;
    for (auto& ch : p)
        if (ch == '/')
            ch = '\\';
    if (ARG(1) <= p.size())
        RET(p.size() + 1);
    strcpy(gstr(ARG(2)), p.c_str());
    if (ARG(3))
    {
        const size_t s = p.find_last_of('\\');
        wr32(ARG(3), ARG(2) + (uint32_t)(s == std::string::npos ? 0 : s + 1));
    }
    RET(p.size());
}

WIN32API(kernel32, GetDriveTypeA, 1)
{
    const char* r = ARG(0) ? gstr(ARG(0)) : "C:\\";
    const char d = (char)toupper(r[0]);
    RET(d == 'C' ? 3 : d == 'D' ? 5 : 1); // FIXED, CDROM, NO_ROOT_DIR
}
WIN32API(kernel32, GetLogicalDrives, 0) { RET(0xC); } // C: and D:
WIN32API(kernel32, GetVolumeInformationA, 8)
{
    if (ARG(1) && ARG(2))
        strncpy(gstr(ARG(1)), "KINGPIN", ARG(2));
    if (ARG(3))
        wr32(ARG(3), 0x19990617);
    if (ARG(4))
        wr32(ARG(4), 255);
    if (ARG(5))
        wr32(ARG(5), 0);
    if (ARG(6) && ARG(7))
        strncpy(gstr(ARG(6)), "CDFS", ARG(7));
    RET(1);
}

WIN32API(kernel32, FindFirstFileA, 2)
{
    std::string pat = gstr(ARG(0));
    for (auto& ch : pat)
        if (ch == '\\')
            ch = '/';
    const size_t s = pat.find_last_of('/');
    const std::string dirGuest = s == std::string::npos ? "." : pat.substr(0, s);
    std::string mask = s == std::string::npos ? pat : pat.substr(s + 1);
    if (mask == "*.*")
        mask = "*";
    auto f = std::make_shared<FindObj>();
    f->dir = hostPath(dirGuest);
    iso::Entry isoDir;
    if (isIsoPath(f->dir))
    {
        if (iso::lookup(f->dir.substr(4), isoDir) && isoDir.dir)
            for (const iso::Entry& e : iso::list(isoDir))
                if (fnmatch(mask.c_str(), e.name.c_str(), FNM_CASEFOLD) == 0)
                    f->names.push_back(e.name);
    }
    else if (DIR* d = opendir(f->dir.c_str()))
    {
        while (dirent* de = readdir(d))
            if (fnmatch(mask.c_str(), de->d_name, FNM_CASEFOLD) == 0)
                f->names.push_back(de->d_name);
        closedir(d);
    }
    std::sort(f->names.begin(), f->names.end());
    if (f->names.empty())
    {
        setLastError(2);
        RET(0xFFFFFFFFu);
    }
    fillFindData(ARG(1), f->dir, f->names[0]);
    f->next = 1;
    RET(handleNew(f));
}

WIN32API(kernel32, FindNextFileA, 2)
{
    auto f = std::dynamic_pointer_cast<FindObj>(handleGet(ARG(0)));
    if (!f || f->next >= f->names.size())
    {
        setLastError(18); // ERROR_NO_MORE_FILES
        RET(0);
    }
    fillFindData(ARG(1), f->dir, f->names[f->next++]);
    RET(1);
}

WIN32API(kernel32, FindClose, 1) { RET(handleClose(ARG(0))); }

// File mappings (only used for the dedicated server's console handshake).
WIN32API(kernel32, CreateFileMappingA, 6) { RET(0); }
WIN32API(kernel32, MapViewOfFile, 5) { RET(0); }
WIN32API(kernel32, UnmapViewOfFile, 1) { RET(1); }
