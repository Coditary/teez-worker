#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace teez::fuzz {

template <typename Fn> void invoke_safely(Fn&& fn) {
    try {
        fn();
    } catch (...) {
    }
}

inline std::string payload_as_string(const uint8_t* data, size_t size) {
    return std::string(reinterpret_cast<const char*>(data), size);
}

inline std::filesystem::path write_temp_file(const std::string& prefix,
                                             const std::string& extension,
                                             std::string_view contents) {
    const auto path =
        std::filesystem::temp_directory_path() /
        (prefix + "-" + std::to_string(std::hash<std::string_view>{}(contents)) + extension);
    std::ofstream file(path, std::ios::binary);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    return path;
}

inline std::filesystem::path make_temp_dir(const std::string& prefix, std::string_view tag) {
    const auto path = std::filesystem::temp_directory_path() /
                      (prefix + "-" + std::to_string(std::hash<std::string_view>{}(tag)));
    std::filesystem::create_directories(path);
    return path;
}

} // namespace teez::fuzz
