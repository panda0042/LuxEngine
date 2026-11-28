#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace LuxEngine::Forge {

// Read a display-referred 3D .cube. Blue selects the strip's horizontal tile, green a row and
// red a column. Re-sample a non-unit input domain so the runtime sampler always addresses [0,1].
inline bool ReadColorGradingCube(const std::filesystem::path& path, uint32_t& size,
                                 std::vector<uint8_t>& pixels, std::string& error) {
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > 32 * 1024 * 1024) {
        error = "Cannot open color grading LUT or file exceeds 32 MiB";
        return false;
    }
    std::ifstream stream(path);
    using Color = std::array<float, 3>;
    Color lower{}, upper{1, 1, 1};
    std::vector<Color> table;
    std::string line;
    size = 0;
    auto invalid = [&] {
        error = "Invalid 3D color grading LUT";
        return false;
    };
    while (std::getline(stream, line)) {
        line.resize(std::min(line.find('#'), line.size()));
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::string tag;
        if (!(row >> tag) || tag == "TITLE")
            continue;
        if (tag == "LUT_3D_SIZE") {
            if (size || !(row >> size) || size < 2 || size > 64)
                return invalid();
            table.reserve(size * size * size);
        } else {
            Color value{};
            const bool domain = tag == "DOMAIN_MIN" || tag == "DOMAIN_MAX";
            if (!domain) {
                if (!size || table.size() == size * size * size)
                    return invalid();
                row.seekg(0);
            }
            for (auto& channel : value)
                if (!(row >> channel) || !std::isfinite(channel))
                    return invalid();
            if (domain)
                (tag == "DOMAIN_MIN" ? lower : upper) = value;
            else
                table.push_back(value);
        }
        if (row >> tag)
            return invalid();
    }
    if (!stream.eof() || !size || table.size() != size * size * size)
        return invalid();
    for (size_t i = 0; i < 3; ++i)
        if (upper[i] <= lower[i])
            return invalid();

    pixels.resize(table.size() * 4);
    for (uint32_t b = 0; b < size; ++b) {
        for (uint32_t g = 0; g < size; ++g) {
            for (uint32_t r = 0; r < size; ++r) {
                std::array<uint32_t, 3> lo{r, g, b}, hi{};
                Color frac{};
                for (size_t i = 0; i < 3; ++i) {
                    const double input = double(lo[i]) / (size - 1);
                    const double t =
                        std::clamp((input - lower[i]) / (double(upper[i]) - lower[i]), 0., 1.) *
                        (size - 1);
                    lo[i] = uint32_t(t);
                    hi[i] = std::min(lo[i] + 1, size - 1);
                    frac[i] = float(t - lo[i]);
                }
                auto* dst = pixels.data() + 4 * ((g * size + b) * size + r);
                for (size_t c = 0; c < 3; ++c) {
                    const auto x = [&](uint32_t y, uint32_t z) {
                        const auto offset = (z * size + y) * size;
                        return std::lerp(table[offset + lo[0]][c], table[offset + hi[0]][c],
                                         frac[0]);
                    };
                    const auto y = [&](uint32_t z) {
                        return std::lerp(x(lo[1], z), x(hi[1], z), frac[1]);
                    };
                    dst[c] = uint8_t(std::lround(
                        255.f * std::clamp(std::lerp(y(lo[2]), y(hi[2]), frac[2]), 0.f, 1.f)));
                }
                dst[3] = 255;
            }
        }
    }
    return true;
}

} // namespace LuxEngine::Forge
