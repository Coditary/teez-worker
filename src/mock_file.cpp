#include "teez/worker/mock_file.hpp"

#include <atomic>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#include <filesystem>

#include <sol/sol.hpp>

namespace teez::worker {

namespace {

std::atomic<int> g_mock_file_counter{0};

struct MockFileInstance {
    std::filesystem::path root;
    std::filesystem::path file_path;
};

void validate_relative_path(const std::string& relative_path) {
    if (relative_path.empty()) {
        throw std::runtime_error("mock_file failed: path must not be empty");
    }
    if (relative_path.front() == '/') {
        throw std::runtime_error("mock_file failed: path must be relative");
    }
    if (relative_path.find("..") != std::string::npos) {
        throw std::runtime_error("mock_file failed: path must not contain '..'");
    }
}

std::filesystem::path create_mock_root(const std::string& relative_path) {
    const auto leaf = std::filesystem::path(relative_path).filename().string();
    const auto root = std::filesystem::temp_directory_path() /
                      ("teez_mock_file_" + std::to_string(++g_mock_file_counter) + "_" + leaf);
    std::filesystem::create_directories(root);
    return root;
}

void cleanup_mock_file(const std::shared_ptr<MockFileInstance>& instance) {
    if (instance == nullptr) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(instance->root, error);
}

void register_auto_cleanup(sol::state& lua, const std::shared_ptr<MockFileInstance>& instance) {
    sol::protected_function register_defer = lua["__teez_register_defer"];
    register_defer([instance]() { cleanup_mock_file(instance); });
}

std::string read_file_text(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        throw std::runtime_error("mock_file failed: could not read " + path.string());
    }
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

sol::table create_mock_file(sol::state& lua, const std::string& relative_path,
                            const std::string& content, sol::optional<sol::table> options) {
    validate_relative_path(relative_path);

    auto instance = std::make_shared<MockFileInstance>();
    instance->root = create_mock_root(relative_path);
    instance->file_path = instance->root / relative_path;
    std::filesystem::create_directories(instance->file_path.parent_path());

    {
        std::ofstream file(instance->file_path);
        if (!file.is_open()) {
            throw std::runtime_error("mock_file failed: could not write " +
                                     instance->file_path.string());
        }
        file << content;
    }

    register_auto_cleanup(lua, instance);

    sol::table mock = lua.create_table();
    mock["path"] = instance->file_path.string();
    mock["name"] = relative_path;
    mock["dir"] = instance->root.string();
    mock["read"] = [instance]() { return read_file_text(instance->file_path); };

    if (options.has_value()) {
        const sol::table config = *options;
        if (const sol::object env_key = config["env_key"];
            env_key.get_type() == sol::type::string) {
            sol::table env = lua.create_table();
            env[env_key.as<std::string>()] = instance->file_path.string();
            mock["env"] = env;
        }
    }

    return mock;
}

} // namespace

void register_mock_file_assertions(sol::table& assertions, sol::state& lua) {
    assertions["mock_file"] = [&lua](const std::string& relative_path, const std::string& content,
                                     sol::optional<sol::table> options) {
        return create_mock_file(lua, relative_path, content, options);
    };
}

} // namespace teez::worker
