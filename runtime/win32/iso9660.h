// Read-only ISO 9660 image access: lets D:\ be served from the disc's data track image directly
// (platforms without a way to mount it, e.g. iOS and Android).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iso
{
    struct Entry
    {
        bool dir = false;
        uint64_t offset = 0, size = 0; // byte range in the image
        std::string name;
    };
    bool open(const std::string& imagePath);       // true if it is a readable ISO 9660 image
    bool lookup(const std::string& rel, Entry& e); // rel: "/kingpin/data1.cab", any case
    std::vector<Entry> list(const Entry& dir);
    int64_t read(uint64_t offset, void* buf, uint64_t n);
}
