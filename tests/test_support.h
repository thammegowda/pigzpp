#pragma once

#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

namespace test_support {

inline std::string make_temp_dir(const std::string& prefix) {
    namespace fs = std::filesystem;
    fs::create_directories("tmp");
    std::random_device random;
    for (int attempt = 0; attempt < 100; ++attempt) {
        fs::path path = fs::path("tmp") / (prefix + std::to_string(random()) +
                                          "_" + std::to_string(random()));
        std::error_code error;
        if (fs::create_directory(path, error))
            return path.generic_string();
        if (error)
            throw fs::filesystem_error("Cannot create temporary directory", path, error);
    }
    throw std::runtime_error("Cannot create a unique temporary directory");
}

} // namespace test_support
