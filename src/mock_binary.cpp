#include "teez/worker/mock_binary.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <filesystem>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "teez/worker/assert_fail.hpp"

namespace teez::worker {

namespace {

std::atomic<int> g_mock_counter{0};

std::string lua_long_string(const std::string& value) {
    return "[[" + value + "]]";
}

std::vector<std::vector<std::string>> read_mock_calls(const std::filesystem::path& log_path) {
    std::vector<std::vector<std::string>> calls;
    if (!std::filesystem::exists(log_path)) {
        return calls;
    }

    std::ifstream file(log_path);
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_array()) {
            throw std::runtime_error("mock_binary failed: invalid calls.log entry");
        }
        std::vector<std::string> args;
        for (const auto& item : parsed) {
            if (!item.is_string()) {
                throw std::runtime_error("mock_binary failed: call argument must be a string");
            }
            args.push_back(item.get<std::string>());
        }
        calls.push_back(std::move(args));
    }
    return calls;
}

sol::table calls_to_lua(sol::state& lua, const std::vector<std::vector<std::string>>& calls) {
    sol::table result = lua.create_table();
    for (std::size_t index = 0; index < calls.size(); ++index) {
        sol::table args = lua.create_table();
        for (std::size_t arg_index = 0; arg_index < calls[index].size(); ++arg_index) {
            args[arg_index + 1] = calls[index][arg_index];
        }
        result[index + 1] = args;
    }
    return result;
}

std::vector<std::vector<std::string>> get_calls_from_mock(sol::state& lua, const sol::table& mock) {
    const sol::object log_path = mock["log_path"];
    if (log_path.get_type() != sol::type::string) {
        throw std::runtime_error("mock object is missing log_path");
    }
    return read_mock_calls(log_path.as<std::string>());
}

bool args_equal(sol::state& lua, const std::vector<std::string>& actual,
                const sol::table& expected) {
    std::size_t expected_count = 0;
    for (const auto& pair : expected) {
        (void)pair;
        ++expected_count;
    }
    if (actual.size() != expected_count) {
        return false;
    }

    for (std::size_t index = 0; index < actual.size(); ++index) {
        lua["__teez_call_actual"] = actual[index];
        lua["__teez_call_expected"] = expected[index + 1];
        const auto result = lua.safe_script("return __teez_call_actual == __teez_call_expected");
        if (!result.valid() || !result.get<bool>()) {
            return false;
        }
    }
    return true;
}

std::string format_call_args(const std::vector<std::string>& args) {
    nlohmann::json json_args = nlohmann::json::array();
    for (const auto& arg : args) {
        json_args.push_back(arg);
    }
    return json_args.dump();
}

std::string format_expected_args(sol::state& lua, const sol::table& expected) {
    nlohmann::json json_args = nlohmann::json::array();
    for (const auto& pair : expected) {
        if (pair.second.get_type() == sol::type::string) {
            json_args.push_back(pair.second.as<std::string>());
        } else {
            json_args.push_back(pair.second.as<std::string>());
        }
    }
    return json_args.dump();
}

void validate_binary_name(const std::string& name) {
    if (name.empty() || name.find('/') != std::string::npos ||
        name.find('\\') != std::string::npos) {
        throw std::runtime_error("mock_binary failed: binary name must be a simple filename");
    }
}

std::filesystem::path create_mock_directory(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("teez_mock_" + std::to_string(++g_mock_counter) + "_" + name);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string build_path_env(const std::filesystem::path& mock_dir) {
    const char* current_path = std::getenv("PATH");
    if (current_path == nullptr || current_path[0] == '\0') {
        return mock_dir.string();
    }
    return mock_dir.string() + ":" + current_path;
}

void write_mock_executable(const std::filesystem::path& executable,
                           const std::filesystem::path& log_path, int exit_code,
                           const std::string& stdout_text, const std::string& stderr_text) {
    std::ostringstream script;
    script << "#!/usr/bin/env lua\n"
           << "local log_path = " << lua_long_string(log_path.string()) << "\n"
           << "local logfile = io.open(log_path, \"a\")\n"
           << "if not logfile then\n"
           << "  io.stderr:write(\"mock_binary: failed to open calls.log\\n\")\n"
           << "  os.exit(126)\n"
           << "end\n"
           << "local args = {}\n"
           << "for i = 1, #arg do\n"
           << "  args[i] = arg[i]\n"
           << "end\n"
           << "local function json_escape(value)\n"
           << "  return '\"' .. value:gsub('\\\\', '\\\\\\\\'):gsub('\"', '\\\\\"'):gsub('\\n', "
              "'\\\\n') .. '\"'\n"
           << "end\n"
           << "local encoded = {}\n"
           << "for i = 1, #args do\n"
           << "  encoded[i] = json_escape(args[i])\n"
           << "end\n"
           << "logfile:write(\"[\" .. table.concat(encoded, \",\") .. \"]\\n\")\n"
           << "logfile:close()\n"
           << "io.stdout:write(" << lua_long_string(stdout_text) << ")\n"
           << "io.stderr:write(" << lua_long_string(stderr_text) << ")\n"
           << "os.exit(" << exit_code << ")\n";

    std::ofstream file(executable);
    if (!file.is_open()) {
        throw std::runtime_error("mock_binary failed: could not write " + executable.string());
    }
    file << script.str();
    file.close();

    std::filesystem::permissions(
        executable, std::filesystem::perms::owner_all | std::filesystem::perms::group_read |
                        std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
                        std::filesystem::perms::others_exec);
}

sol::table create_mock_binary(sol::state& lua, const std::string& name,
                              sol::optional<sol::table> options) {
    validate_binary_name(name);

    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
    if (options.has_value()) {
        const sol::table config = *options;
        if (const sol::object code = config["exit_code"]; code.get_type() == sol::type::number) {
            exit_code = code.as<int>();
        }
        if (const sol::object out = config["stdout"]; out.get_type() == sol::type::string) {
            stdout_text = out.as<std::string>();
        }
        if (const sol::object err = config["stderr"]; err.get_type() == sol::type::string) {
            stderr_text = err.as<std::string>();
        }
    }

    const auto mock_dir = create_mock_directory(name);
    const auto log_path = mock_dir / "calls.log";
    const auto executable = mock_dir / name;
    std::ofstream(log_path).close();

    write_mock_executable(executable, log_path, exit_code, stdout_text, stderr_text);

    sol::table mock = lua.create_table();
    sol::table env = lua.create_table();
    env["PATH"] = build_path_env(mock_dir);
    mock["env"] = env;
    mock["name"] = name;
    mock["path"] = executable.string();
    mock["log_path"] = log_path.string();
    mock["get_calls"] = [&lua, log_path]() { return calls_to_lua(lua, read_mock_calls(log_path)); };
    return mock;
}

} // namespace

void register_mock_binary_assertions(sol::table& assertions, sol::state& lua) {
    assertions["mock_binary"] = [&lua](const std::string& name, sol::optional<sol::table> options) {
        return create_mock_binary(lua, name, options);
    };
    assertions["assert_called"] = [&lua](const sol::table& mock) {
        const auto calls = get_calls_from_mock(lua, mock);
        if (calls.empty()) {
            assert_fail(lua, "assert_called failed: mock was not called");
        }
    };
    assertions["assert_called_times"] = [&lua](const sol::table& mock, int expected_times) {
        const auto calls = get_calls_from_mock(lua, mock);
        if (static_cast<int>(calls.size()) != expected_times) {
            assert_fail(lua, "assert_called_times failed: expected " +
                                 std::to_string(expected_times) + " calls, got " +
                                 std::to_string(calls.size()));
        }
    };
    assertions["assert_called_with"] = [&lua](const sol::table& mock, int call_index,
                                              const sol::table& expected_args) {
        if (call_index < 1) {
            assert_fail(lua, "assert_called_with failed: call index must be >= 1");
        }
        const auto calls = get_calls_from_mock(lua, mock);
        if (call_index > static_cast<int>(calls.size())) {
            assert_fail(lua, "assert_called_with failed: call index " + std::to_string(call_index) +
                                 " out of range (got " + std::to_string(calls.size()) + " calls)");
        }
        if (!args_equal(lua, calls[static_cast<std::size_t>(call_index - 1)], expected_args)) {
            assert_fail(lua, "assert_called_with failed: call " + std::to_string(call_index) +
                                 " args " +
                                 format_call_args(calls[static_cast<std::size_t>(call_index - 1)]) +
                                 " != expected " + format_expected_args(lua, expected_args));
        }
    };
}

} // namespace teez::worker
