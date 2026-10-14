// Read-only ISO 9660 image access (primary volume descriptor, 2048-byte sectors).
#include "iso9660.h"

#include <fcntl.h>
#include <strings.h>
#include <unistd.h>

#include <cstring>

namespace iso
{
    namespace
    {
        int g_fd = -1;
        Entry g_root;

        uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

        // Directory record -> entry (name without the ";1" version and a trailing dot).
        Entry parse(const uint8_t* r)
        {
            Entry e;
            e.offset = (uint64_t)le32(r + 2) * 2048;
            e.size = le32(r + 10);
            e.dir = (r[25] & 2) != 0;
            e.name.assign((const char*)r + 33, r[32]);
            if (const size_t s = e.name.find(';'); s != std::string::npos)
                e.name.resize(s);
            if (!e.name.empty() && e.name.back() == '.')
                e.name.pop_back();
            return e;
        }
    }

    int64_t read(uint64_t offset, void* buf, uint64_t n) { return pread(g_fd, buf, n, (off_t)offset); }

    bool open(const std::string& imagePath)
    {
        const int fd = ::open(imagePath.c_str(), O_RDONLY);
        if (fd < 0)
            return false;
        uint8_t pvd[2048];
        if (pread(fd, pvd, sizeof pvd, 16 * 2048) != (ssize_t)sizeof pvd || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0)
        {
            close(fd);
            return false;
        }
        g_fd = fd;
        g_root = parse(pvd + 156);
        return true;
    }

    std::vector<Entry> list(const Entry& dir)
    {
        std::vector<Entry> out;
        std::vector<uint8_t> data(dir.size);
        if (read(dir.offset, data.data(), dir.size) != (int64_t)dir.size)
            return out;
        for (size_t p = 0; p < data.size();)
        {
            const uint8_t len = data[p];
            if (len == 0) // records do not cross sectors: skip to the next one
            {
                p = (p / 2048 + 1) * 2048;
                continue;
            }
            if (p + len > data.size())
                break;
            const uint8_t* r = &data[p];
            if (!(r[32] == 1 && (r[33] == 0 || r[33] == 1))) // skip "." and ".."
                out.push_back(parse(r));
            p += len;
        }
        return out;
    }

    bool lookup(const std::string& rel, Entry& e)
    {
        if (g_fd < 0)
            return false;
        Entry cur = g_root;
        size_t pos = 0;
        while (pos <= rel.size())
        {
            size_t end = rel.find('/', pos);
            if (end == std::string::npos)
                end = rel.size();
            const std::string part = rel.substr(pos, end - pos);
            pos = end + 1;
            if (part.empty() || part == ".")
                continue;
            if (!cur.dir)
                return false;
            bool found = false;
            for (const Entry& c : list(cur))
                if (strcasecmp(c.name.c_str(), part.c_str()) == 0)
                {
                    cur = c;
                    found = true;
                    break;
                }
            if (!found)
                return false;
        }
        e = cur;
        return true;
    }
}
