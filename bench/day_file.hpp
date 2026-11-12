// day_file.hpp — a "trading day" on disk: a header and a flat array of Commands.
#pragma once

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "lob/types.hpp"

namespace lob::bench {

struct DayHeader {
    char          magic[8];   // "LOBDAY01"
    std::uint64_t count;
    std::uint64_t seed;
    BookConfig    cfg;
};

inline void write_day(const std::string& path, const DayHeader& h, const std::vector<Command>& cmds) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fwrite(&h, sizeof h, 1, f);
    std::fwrite(cmds.data(), sizeof(Command), cmds.size(), f);
    std::fclose(f);
}

inline std::vector<Command> read_day(const std::string& path, DayHeader& h) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path + " (run gen_day first)");
    if (std::fread(&h, sizeof h, 1, f) != 1 || std::memcmp(h.magic, "LOBDAY01", 8) != 0) {
        std::fclose(f);
        throw std::runtime_error(path + " is not a day file");
    }
    std::vector<Command> cmds(h.count);
    std::size_t got = std::fread(cmds.data(), sizeof(Command), h.count, f);
    std::fclose(f);
    if (got != h.count) throw std::runtime_error(path + " is truncated");
    return cmds;
}

}  // namespace lob::bench
