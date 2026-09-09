#include "teez/worker/embedded_runtime.hpp"
#include "teez/worker/runtime_config.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <cstdlib>
#include <filesystem>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "teez/core/coverage.hpp"
#include "teez/core/discovery.hpp"
#include "teez/core/lua_helpers.hpp"
#include "teez/core/runner_config.hpp"
#include "teez/core/teez_config.hpp"
#include "teez/core/test_filter.hpp"
#include "teez/worker/assert_fail.hpp"
#include "teez/worker/harness.hpp"
#include "teez/worker/mock_binary.hpp"
#include "teez/worker/mock_file.hpp"
#include "teez/worker/mock_http.hpp"
#include <coditary/lua/register.hpp>
#include <coditary/utils/path.hpp>

namespace teez::worker {

namespace {

void emit_ndjson(std::ostream& out, const nlohmann::json& event, bool* saw_failure = nullptr) {
    if (saw_failure != nullptr && event.is_object() && event.contains("event")) {
        const std::string type = event.at("event").get<std::string>();
        if (type == "fail" || type == "error") {
            *saw_failure = true;
        }
    }
    out << event.dump() << '\n';
    out.flush();
}

void emit_phase(std::ostream& out, const std::string& id, const std::string& phase,
                const std::string& state) {
    emit_ndjson(out, {{"event", "phase"}, {"id", id}, {"phase", phase}, {"state", state}});
}

class LiveCoverageEmitter {
  public:
    LiveCoverageEmitter(std::ostream& out, const std::filesystem::path& project_root)
        : out_(out), project_root_(project_root) {
        nlohmann::json config = teez::core::active_config().data();
        if (!config.is_object()) {
            config = nlohmann::json::object();
        }

        const auto& coverage = config.contains("coverage") && config["coverage"].is_object()
                                   ? config["coverage"]
                                   : nlohmann::json::object();
        const auto& live = coverage.contains("live") && coverage["live"].is_object()
                               ? coverage["live"]
                               : nlohmann::json::object();

        use_progress_ = live.value("use_test_progress", false);
        if (live.contains("lines_found") && live["lines_found"].is_number()) {
            progress_total_ = live["lines_found"].get<int>();
        } else if (config.contains("bulk_count") && config["bulk_count"].is_number()) {
            progress_total_ = config["bulk_count"].get<int>();
        }

        if (const auto report = live.value("report", std::string{}); !report.empty()) {
            use_file_ = true;
            report_path_ = project_root_ / report;
        }

        if (live.contains("delay_ms") && live["delay_ms"].is_number()) {
            delay_ms_ = std::max(0, live["delay_ms"].get<int>());
        }
    }

    bool enabled() const {
        return use_file_ || (use_progress_ && progress_total_ > 0);
    }

    void start() {
        if (!enabled()) {
            return;
        }
        emit_progress_event();
    }

    void on_test_completed() {
        if (!enabled()) {
            return;
        }
        ++executed_;
        maybe_emit();
        if (delay_ms_ > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        }
    }

    void finish() {
        maybe_emit(true);
    }

  private:
    static constexpr auto kEmitInterval = std::chrono::milliseconds(50);

    void maybe_emit(bool force = false) {
        if (!force) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_emit_ < kEmitInterval) {
                return;
            }
            last_emit_ = now;
        }

        if (use_file_) {
            std::error_code ec;
            if (!std::filesystem::exists(report_path_, ec)) {
                if (use_progress_ && progress_total_ > 0) {
                    emit_progress_event();
                }
                return;
            }
            try {
                emit_ndjson(out_, teez::core::coverage_table_to_event(
                                      teez::core::load_coverage_table(report_path_)));
                return;
            } catch (const std::exception&) {
                if (!use_progress_ || progress_total_ <= 0) {
                    return;
                }
            }
        }

        if (use_progress_ && progress_total_ > 0) {
            emit_progress_event();
        }
    }

    void emit_progress_event() {
        const int lines_hit = std::min(executed_, progress_total_);
        const double line_rate = progress_total_ > 0 ? static_cast<double>(lines_hit) /
                                                           static_cast<double>(progress_total_)
                                                     : 0.0;
        emit_ndjson(out_, nlohmann::json{{"event", "coverage"},
                                         {"line_rate", line_rate},
                                         {"lines_hit", lines_hit},
                                         {"lines_found", progress_total_},
                                         {"source", "progress"}});
    }

    std::ostream& out_;
    std::filesystem::path project_root_;
    std::filesystem::path report_path_;
    std::chrono::steady_clock::time_point last_emit_{};
    int executed_ = 0;
    int progress_total_ = 0;
    int delay_ms_ = 0;
    bool use_file_ = false;
    bool use_progress_ = false;
};

std::string object_to_string(sol::state& lua, const sol::object& value) {
    sol::protected_function tostring = lua["tostring"];
    const auto result = tostring(value);
    if (!result.valid()) {
        return "<value>";
    }
    sol::object obj = result;
    if (obj.get_type() == sol::type::string) {
        return obj.as<std::string>();
    }
    return "<value>";
}

bool lua_equal(sol::state& lua, const sol::object& lhs, const sol::object& rhs) {
    lua["__teez_assert_lhs"] = lhs;
    lua["__teez_assert_rhs"] = rhs;
    const auto result = lua.safe_script("return __teez_assert_lhs == __teez_assert_rhs");
    return result.valid() && result.get<bool>();
}

bool lua_truthy(const sol::object& value) {
    if (value.get_type() == sol::type::lua_nil || value.get_type() == sol::type::none) {
        return false;
    }
    if (value.is<bool>()) {
        return value.as<bool>();
    }
    return true;
}

bool snapshots_should_update() {
    const char* flag = std::getenv("TEEZ_UPDATE_SNAPSHOTS");
    return flag != nullptr && flag[0] != '\0' && flag[0] != '0';
}

std::string sanitize_snapshot_name(sol::state_view lua, const std::string& name) {
    std::string sanitized;
    sanitized.reserve(name.size());
    for (const char ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':') {
            sanitized.push_back('_');
        } else {
            sanitized.push_back(ch);
        }
    }
    if (sanitized.empty()) {
        assert_fail(lua, "assert_match_snapshot failed: snapshot name must not be empty");
    }
    return sanitized;
}

void write_snapshot_file(sol::state_view lua, const std::filesystem::path& path,
                         const std::string& actual) {
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path());
    }
    std::ofstream file(path);
    if (!file.is_open()) {
        assert_fail(lua, "snapshot write failed: " + path.string());
    }
    file << actual;
}

void compare_snapshot_file(sol::state_view lua, const std::filesystem::path& path,
                           const std::string& actual) {
    if (!std::filesystem::exists(path)) {
        assert_fail(lua, "snapshot not found: " + path.string() +
                             " (run with --update-snapshots to create it)");
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        assert_fail(lua, "snapshot read failed: " + path.string());
    }
    const std::string expected = {std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    if (actual != expected) {
        assert_fail(lua, "snapshot mismatch: " + path.string() +
                             " (run with --update-snapshots to accept changes)");
    }
}

void assert_snapshot_content(sol::state_view lua, const std::string& actual,
                             const std::filesystem::path& path) {
    if (snapshots_should_update()) {
        write_snapshot_file(lua, path, actual);
        return;
    }
    compare_snapshot_file(lua, path, actual);
}

int process_exit_code(sol::state_view lua, const sol::table& result) {
    const sol::object code = result["exit_code"];
    if (code.get_type() != sol::type::number) {
        assert_fail(lua, "process result is missing exit_code");
    }
    return code.as<int>();
}

std::string process_stream(sol::state_view lua, const sol::table& result, const char* key) {
    const sol::object value = result[key];
    if (value.get_type() != sol::type::string) {
        assert_fail(lua, std::string("process result is missing ") + key);
    }
    return value.as<std::string>();
}

sol::table make_assertions(sol::state& lua) {
    sol::table assertions = lua.create_table();
    assertions["defer"] = [&lua](sol::protected_function fn) {
        sol::protected_function register_defer = lua["__teez_register_defer"];
        register_defer(fn);
    };
    assertions["assert_eq"] = [&lua](const sol::object& actual, const sol::object& expected) {
        if (!lua_equal(lua, actual, expected)) {
            assert_fail(lua, "assert_eq failed: " + object_to_string(lua, actual) +
                                 " != " + object_to_string(lua, expected));
        }
    };
    assertions["assert_not_eq"] = [&lua](const sol::object& actual, const sol::object& expected) {
        if (lua_equal(lua, actual, expected)) {
            assert_fail(lua, "assert_not_eq failed: " + object_to_string(lua, actual) +
                                 " == " + object_to_string(lua, expected));
        }
    };
    assertions["assert_true"] = [&lua](const sol::object& value) {
        if (!lua_truthy(value)) {
            assert_fail(lua, "assert_true failed: expected truthy value, got " +
                                 object_to_string(lua, value));
        }
    };
    assertions["assert_false"] = [&lua](const sol::object& value) {
        if (lua_truthy(value)) {
            assert_fail(lua, "assert_false failed: expected falsy value, got " +
                                 object_to_string(lua, value));
        }
    };
    assertions["assert_contains"] = [&lua](const std::string& haystack, const std::string& needle) {
        if (haystack.find(needle) == std::string::npos) {
            assert_fail(lua, "assert_contains failed: '" + haystack + "' does not contain '" +
                                 needle + "'");
        }
    };
    assertions["assert_starts_with"] = [&lua](const std::string& value, const std::string& prefix) {
        if (value.rfind(prefix, 0) != 0) {
            assert_fail(lua, "assert_starts_with failed: '" + value + "' does not start with '" +
                                 prefix + "'");
        }
    };
    assertions["assert_ends_with"] = [&lua](const std::string& value, const std::string& suffix) {
        if (value.size() < suffix.size() ||
            value.compare(value.size() - suffix.size(), suffix.size(), suffix) != 0) {
            assert_fail(lua, "assert_ends_with failed: '" + value + "' does not end with '" +
                                 suffix + "'");
        }
    };
    assertions["assert_match"] = [&lua](const std::string& value, const std::string& pattern) {
        lua["__teez_match_value"] = value;
        lua["__teez_match_pattern"] = pattern;
        const auto result = lua.safe_script(R"(
            if type(__teez_match_value) ~= "string" then
                error("assert_match requires a string value")
            end
            return string.match(__teez_match_value, __teez_match_pattern) ~= nil
        )");
        if (!result.valid()) {
            sol::error err = result;
            assert_fail(lua, err.what());
        }
        if (!result.get<bool>()) {
            assert_fail(lua, "assert_match failed: '" + value + "' does not match pattern '" +
                                 pattern + "'");
        }
    };
    assertions["assert_empty"] = [&lua](const sol::object& value) {
        lua["__teez_empty_value"] = value;
        const auto result = lua.safe_script(R"(
            local v = __teez_empty_value
            if v == nil then
                return true
            end
            local kind = type(v)
            if kind == "string" then
                return v == ""
            end
            if kind == "table" then
                return next(v) == nil
            end
            return false
        )");
        if (!result.valid()) {
            sol::error err = result;
            assert_fail(lua, err.what());
        }
        if (!result.get<bool>()) {
            assert_fail(lua, "assert_empty failed: value is not empty (" +
                                 object_to_string(lua, value) + ")");
        }
    };
    assertions["assert_not_empty"] = [&lua](const sol::object& value) {
        lua["__teez_empty_value"] = value;
        const auto result = lua.safe_script(R"(
            local v = __teez_empty_value
            if v == nil then
                return false
            end
            local kind = type(v)
            if kind == "string" then
                return v ~= ""
            end
            if kind == "table" then
                return next(v) ~= nil
            end
            return true
        )");
        if (!result.valid()) {
            sol::error err = result;
            assert_fail(lua, err.what());
        }
        if (!result.get<bool>()) {
            assert_fail(lua, "assert_not_empty failed: value is empty");
        }
    };
    assertions["assert_table_has"] = [&lua](const sol::table& table, const sol::object& key,
                                            sol::optional<sol::object> expected) {
        lua["__teez_table_has_t"] = table;
        lua["__teez_table_has_k"] = key;
        if (expected.has_value()) {
            lua["__teez_table_has_e"] = *expected;
            const auto result = lua.safe_script(R"(
                if type(__teez_table_has_t) ~= "table" then
                    error("assert_table_has requires a table")
                end
                local actual = __teez_table_has_t[__teez_table_has_k]
                if actual == nil then
                    error("assert_table_has failed: key not found")
                end
                if actual ~= __teez_table_has_e then
                    error("assert_table_has failed: value mismatch")
                end
                return true
            )");
            if (!result.valid()) {
                sol::error err = result;
                assert_fail(lua, err.what());
            }
            return;
        }

        const auto result = lua.safe_script(R"(
            if type(__teez_table_has_t) ~= "table" then
                error("assert_table_has requires a table")
            end
            if __teez_table_has_t[__teez_table_has_k] == nil then
                error("assert_table_has failed: key not found")
            end
            return true
        )");
        if (!result.valid()) {
            sol::error err = result;
            assert_fail(lua, err.what());
        }
    };
    assertions["assert_http_status"] = [&lua](const sol::table& response, long expected_status) {
        const sol::object status = response["status"];
        if (status.get_type() != sol::type::number) {
            assert_fail(lua, "assert_http_status failed: response.status is missing");
        }
        const long actual_status = status.as<long>();
        if (actual_status != expected_status) {
            assert_fail(lua, "assert_http_status failed: expected " +
                                 std::to_string(expected_status) + ", got " +
                                 std::to_string(actual_status));
        }
    };
    assertions["assert_file_exists"] = [&lua](const std::string& path) {
        if (!std::filesystem::exists(path)) {
            assert_fail(lua, "assert_file_exists failed: " + path);
        }
    };
    assertions["assert_exit_code"] = [&lua](const sol::table& result, int expected) {
        const int actual = process_exit_code(lua, result);
        if (actual != expected) {
            assert_fail(lua, "assert_exit_code failed: expected " + std::to_string(expected) +
                                 ", got " + std::to_string(actual));
        }
    };
    assertions["assert_exit_success"] = [&lua](const sol::table& result) {
        const int actual = process_exit_code(lua, result);
        if (actual != 0) {
            assert_fail(lua,
                        "assert_exit_success failed: expected 0, got " + std::to_string(actual));
        }
    };
    assertions["assert_coverage_rate"] = [&lua](const sol::table& result, double minimum) {
        const sol::object line_rate = result["line_rate"];
        if (line_rate.get_type() != sol::type::number) {
            assert_fail(lua, "assert_coverage_rate failed: result has no line_rate");
        }
        const double actual = line_rate.as<double>();
        if (actual < minimum) {
            assert_fail(lua, "assert_coverage_rate failed: line_rate " + std::to_string(actual) +
                                 " < " + std::to_string(minimum));
        }
    };
    assertions["assert_coverage_threshold"] = [&lua](const sol::table& result) {
        const sol::object passed = result["threshold_passed"];
        if (passed.get_type() != sol::type::boolean || !passed.as<bool>()) {
            std::string message = "assert_coverage_threshold failed";
            const sol::object failures = result["threshold_failures"];
            if (failures.get_type() == sol::type::table) {
                for (const auto& pair : failures.as<sol::table>()) {
                    if (pair.second.get_type() == sol::type::string) {
                        message += ": " + pair.second.as<std::string>();
                    }
                }
            }
            assert_fail(lua, message);
        }
    };
    assertions["assert_exit_not"] = [&lua](const sol::table& result, int forbidden) {
        const int actual = process_exit_code(lua, result);
        if (actual == forbidden) {
            assert_fail(lua, "assert_exit_not failed: exit code must not be " +
                                 std::to_string(forbidden));
        }
    };
    assertions["assert_stdout_contains"] = [&lua](const sol::table& result,
                                                  const std::string& needle) {
        const std::string stdout_text = process_stream(lua, result, "stdout");
        if (stdout_text.find(needle) == std::string::npos) {
            assert_fail(lua,
                        "assert_stdout_contains failed: stdout does not contain '" + needle + "'");
        }
    };
    assertions["assert_stderr_contains"] = [&lua](const sol::table& result,
                                                  const std::string& needle) {
        const std::string stderr_text = process_stream(lua, result, "stderr");
        if (stderr_text.find(needle) == std::string::npos) {
            assert_fail(lua,
                        "assert_stderr_contains failed: stderr does not contain '" + needle + "'");
        }
    };
    assertions["assert_stdout_empty"] = [&lua](const sol::table& result) {
        const std::string stdout_text = process_stream(lua, result, "stdout");
        if (!stdout_text.empty()) {
            assert_fail(lua, "assert_stdout_empty failed: stdout is not empty");
        }
    };
    assertions["assert_stderr_empty"] = [&lua](const sol::table& result) {
        const std::string stderr_text = process_stream(lua, result, "stderr");
        if (!stderr_text.empty()) {
            assert_fail(lua, "assert_stderr_empty failed: stderr is not empty");
        }
    };
    assertions["assert_in"] = [&lua](const sol::table& list, const sol::object& value) {
        lua["__teez_in_list"] = list;
        lua["__teez_in_value"] = value;
        const auto result = lua.safe_script(R"(
            if type(__teez_in_list) ~= "table" then
                error("assert_in requires a table")
            end
            for _, item in ipairs(__teez_in_list) do
                if item == __teez_in_value then
                    return true
                end
            end
            for _, item in pairs(__teez_in_list) do
                if item == __teez_in_value then
                    return true
                end
            end
            return false
        )");
        if (!result.valid()) {
            sol::error err = result;
            assert_fail(lua, err.what());
        }
        if (!result.get<bool>()) {
            assert_fail(lua, "assert_in failed: value not found in table (" +
                                 object_to_string(lua, value) + ")");
        }
    };
    assertions["assert_type"] = [&lua](const sol::object& value, const std::string& expected_type) {
        lua["__teez_type_value"] = value;
        const auto actual = lua.safe_script("return type(__teez_type_value)");
        if (!actual.valid()) {
            assert_fail(lua, "assert_type failed: could not determine value type");
        }
        const std::string actual_type = actual.get<std::string>();
        if (actual_type != expected_type) {
            assert_fail(lua,
                        "assert_type failed: expected " + expected_type + ", got " + actual_type);
        }
    };
    assertions["assert_snapshot"] = [&lua](const std::string& actual,
                                           const std::string& snapshot_path) {
        assert_snapshot_content(lua, actual, std::filesystem::path(snapshot_path));
    };
    assertions["assert_match_snapshot"] = [&lua](const std::string& actual,
                                                 const std::string& snapshot_name) {
        const sol::object root = lua["__teez_snapshots_root"];
        if (root.get_type() != sol::type::string) {
            assert_fail(lua, "assert_match_snapshot failed: snapshots root is missing");
        }
        const auto path = std::filesystem::path(root.as<std::string>()) /
                          (sanitize_snapshot_name(lua, snapshot_name) + ".snap");
        assert_snapshot_content(lua, actual, path);
    };
    assertions["assert_eventually"] = [&lua](sol::protected_function fn,
                                             sol::optional<double> timeout_seconds) {
        const double timeout = timeout_seconds.value_or(5.0);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(timeout));
        std::string last_error = "callback returned false";

        while (std::chrono::steady_clock::now() < deadline) {
            const auto result = fn();
            if (result.valid()) {
                if (result.get_type() == sol::type::boolean && !result.get<bool>()) {
                    last_error = "callback returned false";
                } else {
                    return;
                }
            } else {
                sol::error err = result;
                last_error = err.what();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        assert_fail(lua, "assert_eventually timed out after " + std::to_string(timeout) +
                             "s: " + last_error);
    };
    assertions["assert_near"] = [&lua](const sol::object& actual, const sol::object& expected,
                                       double epsilon) {
        lua["__teez_near_actual"] = actual;
        lua["__teez_near_expected"] = expected;
        lua["__teez_near_eps"] = epsilon;
        const auto result = lua.safe_script(R"(
            if type(__teez_near_actual) ~= "number" or type(__teez_near_expected) ~= "number" then
                error("assert_near requires numeric values")
            end
            return math.abs(__teez_near_actual - __teez_near_expected) <= __teez_near_eps
        )");
        if (!result.valid()) {
            sol::error err = result;
            assert_fail(lua, err.what());
        }
        if (!result.get<bool>()) {
            assert_fail(lua, "assert_near failed: " + object_to_string(lua, actual) +
                                 " != " + object_to_string(lua, expected) +
                                 " (eps=" + std::to_string(epsilon) + ")");
        }
    };
    assertions["assert_throws"] = [&lua](sol::protected_function fn,
                                         sol::optional<std::string> expected_message) {
        bool threw = false;
        std::string message;

        const auto result = fn();
        if (!result.valid()) {
            threw = true;
            sol::error err = result;
            message = err.what();
        }

        if (!threw) {
            assert_fail(lua, "assert_throws failed: function did not throw");
        }
        if (expected_message.has_value() && message.find(*expected_message) == std::string::npos) {
            assert_fail(lua, "assert_throws failed: expected message containing '" +
                                 *expected_message + "', got '" + message + "'");
        }
    };
    register_mock_binary_assertions(assertions, lua);
    register_mock_http_assertions(assertions, lua);
    register_mock_file_assertions(assertions, lua);
    return assertions;
}

void register_http(sol::state& lua) {
    coditary::lua::RegisterOptions options;
    coditary::lua::register_net(lua, options);
}

void register_test_runtime(sol::state& lua, const std::string& current_file,
                           const std::filesystem::path& snapshots_root,
                           const teez::core::TestFilters& filters, std::ostream& out,
                           LiveCoverageEmitter& coverage_emitter, bool* saw_failure) {
    lua["__teez_current_file"] = current_file;
    lua["__teez_snapshots_root"] = snapshots_root.string();
    lua["__teez_active_suite_stack"] = lua.create_table();
    lua["__teez_cleanup_errors"] = lua.create_table();
    if (!filters.empty()) {
        lua["__teez_filters"] = teez::core::filters_to_lua(lua, filters);
    }

    lua["__teez_emit_phase"] = [&out](const std::string& id, const std::string& phase,
                                      const std::string& state) {
        emit_phase(out, id, phase, state);
    };
    lua["__teez_sleep_ms"] = [](int milliseconds) {
        if (milliseconds > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
        }
    };

    lua["__teez_skip_it"] = [&out, &coverage_emitter, saw_failure](const std::string& test_id) {
        const auto started_at = std::chrono::steady_clock::now();
        emit_ndjson(out, {{"event", "start"}, {"id", test_id}}, saw_failure);
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started_at)
                                     .count();
        emit_ndjson(out, {{"event", "skip"}, {"id", test_id}, {"duration_ms", duration_ms}},
                    saw_failure);
        coverage_emitter.on_test_completed();
    };

    lua["__teez_todo_it"] = [&out, &coverage_emitter, saw_failure](const std::string& test_id) {
        const auto started_at = std::chrono::steady_clock::now();
        emit_ndjson(out, {{"event", "start"}, {"id", test_id}}, saw_failure);
        const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - started_at)
                                     .count();
        emit_ndjson(out, {{"event", "todo"}, {"id", test_id}, {"duration_ms", duration_ms}},
                    saw_failure);
        coverage_emitter.on_test_completed();
    };

    lua["__teez_run_it"] = [&lua, &out, &coverage_emitter, saw_failure](
                               const std::string& test_id, sol::protected_function fn,
                               sol::optional<std::string> mode, sol::optional<int> retry) {
        const std::string run_mode = mode.value_or("normal");
        int max_attempts = retry.value_or(1);
        if (max_attempts < 1) {
            max_attempts = 1;
        }

        const auto test_started_at = std::chrono::steady_clock::now();
        emit_ndjson(out, {{"event", "start"}, {"id", test_id}}, saw_failure);

        const auto elapsed_ms = [&]() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - test_started_at)
                .count();
        };

        auto run_attempt = [&]() -> std::pair<bool, std::string> {
            bool failed = false;
            std::string message;
            bool test_phase_active = false;

            try {
                const sol::protected_function before_test = lua["__teez_before_test"];
                const sol::protected_function_result before_result = before_test();
                if (!before_result.valid()) {
                    sol::error err = before_result;
                    assert_fail(lua, std::string("before_test hook failed: ") + err.what());
                }

                emit_phase(out, test_id, "test", "start");
                test_phase_active = true;
                sol::table assertions = make_assertions(lua);
                lua["__teez_active_assertions"] = assertions;
                const auto result = fn(assertions);
                lua["__teez_active_assertions"] = sol::lua_nil;
                emit_phase(out, test_id, "test", "end");
                test_phase_active = false;
                if (!result.valid()) {
                    sol::error err = result;
                    failed = true;
                    message = err.what();
                }
            } catch (const std::exception& ex) {
                if (test_phase_active) {
                    emit_phase(out, test_id, "test", "end");
                }
                failed = true;
                message = ex.what();
            }

            return {failed, message};
        };

        auto finalize_attempt = [&](bool finalize) -> std::pair<bool, std::string> {
            bool failed = false;
            std::string message;

            try {
                const sol::protected_function after_test = lua["__teez_after_test"];
                const sol::protected_function_result after_result = after_test(finalize);
                if (!after_result.valid()) {
                    sol::error err = after_result;
                    failed = true;
                    message = std::string("after_test hook failed: ") + err.what();
                } else if (after_result.get_type() == sol::type::table) {
                    sol::table errors = after_result;
                    for (const auto& pair : errors) {
                        if (pair.second.get_type() != sol::type::string) {
                            continue;
                        }
                        failed = true;
                        message = "cleanup failed: " + pair.second.as<std::string>();
                    }
                }
            } catch (const std::exception& ex) {
                failed = true;
                message = std::string("after_test hook failed: ") + ex.what();
            }

            return {failed, message};
        };

        std::string last_message;

        for (int attempt = 1; attempt <= max_attempts; ++attempt) {
            const auto [attempt_failed, attempt_message] = run_attempt();
            last_message = attempt_message;

            const bool is_last_attempt = attempt == max_attempts;
            const auto [cleanup_failed, cleanup_message] = finalize_attempt(is_last_attempt);
            if (cleanup_failed) {
                last_message = cleanup_message;
            }

            const bool failed = attempt_failed || cleanup_failed;

            if (run_mode == "fails") {
                if (failed) {
                    emit_ndjson(out,
                                {{"event", "pass"}, {"id", test_id}, {"duration_ms", elapsed_ms()}},
                                saw_failure);
                    coverage_emitter.on_test_completed();
                    return;
                }
                if (is_last_attempt) {
                    emit_ndjson(out,
                                {{"event", "fail"},
                                 {"id", test_id},
                                 {"msg", "expected test to fail but it passed"},
                                 {"duration_ms", elapsed_ms()}},
                                saw_failure);
                    coverage_emitter.on_test_completed();
                    return;
                }
                emit_ndjson(out,
                            {{"event", "retry"},
                             {"id", test_id},
                             {"attempt", attempt},
                             {"max", max_attempts},
                             {"msg", "expected failure did not occur yet"}},
                            saw_failure);
                continue;
            }

            if (!failed) {
                emit_ndjson(out,
                            {{"event", "pass"}, {"id", test_id}, {"duration_ms", elapsed_ms()}},
                            saw_failure);
                coverage_emitter.on_test_completed();
                return;
            }

            if (!is_last_attempt) {
                emit_ndjson(out,
                            {{"event", "retry"},
                             {"id", test_id},
                             {"attempt", attempt},
                             {"max", max_attempts},
                             {"msg", last_message}},
                            saw_failure);
                continue;
            }
        }

        emit_ndjson(out,
                    {{"event", "fail"},
                     {"id", test_id},
                     {"msg", last_message},
                     {"duration_ms", elapsed_ms()}},
                    saw_failure);
        coverage_emitter.on_test_completed();
    };
}

void load_embedded_runtime(sol::state& lua) {
    const auto load_script = [&](const char* source, const char* label) {
        const auto result = lua.script(source);
        if (!result.valid()) {
            sol::error err = result;
            throw std::runtime_error(std::string("failed to load embedded ") + label + ": " +
                                     err.what());
        }
    };

    load_script(embedded::kTeezWorkerLua, "teez-worker.lua");
    load_script(embedded::kExpectLua, "expect.lua");
}

void load_runtime_library(sol::state& lua, const std::filesystem::path& runtime_dir) {
    if (use_embedded_runtime()) {
        load_embedded_runtime(lua);
        return;
    }

    const auto runtime_path = runtime_dir / "teez-worker.lua";
    if (!std::filesystem::exists(runtime_path)) {
        throw std::runtime_error("worker runtime not found: " + runtime_path.string());
    }

    const auto result = lua.script_file(runtime_path.string());
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("failed to load worker runtime: " + std::string(err.what()));
    }

    const auto expect_path = runtime_dir / "expect.lua";
    if (!std::filesystem::exists(expect_path)) {
        throw std::runtime_error("worker runtime not found: " + expect_path.string());
    }

    const auto expect_result = lua.script_file(expect_path.string());
    if (!expect_result.valid()) {
        sol::error err = expect_result;
        throw std::runtime_error("failed to load expect runtime: " + std::string(err.what()));
    }
}

std::filesystem::path find_project_root(const std::filesystem::path& target_path) {
    const auto config = coditary::utils::find_upward(target_path, "teez.config.lua");
    if (!config.empty()) {
        return config.parent_path();
    }
    return std::filesystem::absolute(target_path);
}

teez::core::TestFilters load_filters_from_env() {
    const char* raw = std::getenv("TEEZ_FILTERS");
    if (raw == nullptr || raw[0] == '\0') {
        return {};
    }

    const auto json = nlohmann::json::parse(raw, nullptr, false);
    if (json.is_discarded()) {
        return {};
    }
    return teez::core::filters_from_json(json);
}

std::string relative_file_path(const std::filesystem::path& file_path,
                               const std::filesystem::path& project_root) {
    std::error_code ec;
    const auto relative = std::filesystem::relative(file_path, project_root, ec);
    if (ec) {
        return file_path.filename().string();
    }
    return relative.generic_string();
}

#ifndef TEEZ_PLUGIN_DIR
#define TEEZ_PLUGIN_DIR "plugins"
#endif

bool run_test_file(const std::filesystem::path& file_path,
                   const std::filesystem::path& project_root,
                   const std::filesystem::path& runtime_dir, const teez::core::TestFilters& filters,
                   std::ostream& out) {
    bool saw_failure = false;
    LiveCoverageEmitter coverage_emitter(out, project_root);
    sol::state lua;
    teez::core::register_lua_helpers(lua, project_root);
    register_http(lua);

    const std::string current_file = relative_file_path(file_path, project_root);
    const auto snapshots_root =
        file_path.parent_path() / "__snapshots__" / file_path.filename().string();
    register_test_runtime(lua, current_file, snapshots_root, filters, out, coverage_emitter,
                          &saw_failure);
    register_harness(lua, project_root, TEEZ_PLUGIN_DIR, out);
    load_runtime_library(lua, runtime_dir);

    coverage_emitter.start();

    const auto result = lua.script_file(file_path.string(), sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        emit_ndjson(out,
                    {{"event", "fail"},
                     {"id", current_file},
                     {"msg", std::string("failed to load test file: ") + err.what()}},
                    &saw_failure);
        return true;
    }

    const sol::protected_function run_all = lua["__teez_run_all_tests"];
    const sol::protected_function_result run_all_result = run_all();
    if (!run_all_result.valid()) {
        sol::error err = run_all_result;
        emit_ndjson(out,
                    {{"event", "fail"},
                     {"id", current_file},
                     {"msg", std::string("failed to run tests: ") + err.what()}},
                    &saw_failure);
    }

    coverage_emitter.finish();
    return saw_failure;
}

std::vector<std::string> collect_tests_from_file(const std::filesystem::path& file_path,
                                                 const std::filesystem::path& project_root,
                                                 const std::filesystem::path& runtime_dir,
                                                 const teez::core::TestFilters& filters) {
    std::ostringstream discard;
    bool saw_failure = false;
    LiveCoverageEmitter coverage_emitter(discard, project_root);
    sol::state lua;
    teez::core::register_lua_helpers(lua, project_root);
    register_http(lua);

    const std::string current_file = relative_file_path(file_path, project_root);
    const auto snapshots_root =
        file_path.parent_path() / "__snapshots__" / file_path.filename().string();
    register_test_runtime(lua, current_file, snapshots_root, filters, discard, coverage_emitter,
                          &saw_failure);
    register_harness(lua, project_root, TEEZ_PLUGIN_DIR, discard);
    load_runtime_library(lua, runtime_dir);

    const auto result = lua.script_file(file_path.string(), sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        throw std::runtime_error("failed to load test file: " + file_path.string() + ": " +
                                 err.what());
    }

    const sol::protected_function collect_all = lua["__teez_collect_all_tests"];
    const sol::protected_function_result collect_result = collect_all();
    if (!collect_result.valid()) {
        sol::error err = collect_result;
        throw std::runtime_error("failed to collect tests from: " + file_path.string() + ": " +
                                 err.what());
    }

    std::vector<std::string> tests;
    if (collect_result.get_type() != sol::type::table) {
        return tests;
    }

    const sol::table tests_table = collect_result;
    for (const auto& pair : tests_table) {
        if (pair.second.get_type() == sol::type::string) {
            tests.push_back(pair.second.as<std::string>());
        }
    }
    return tests;
}

} // namespace

std::vector<std::string> list_worker(const std::filesystem::path& target_path,
                                     const std::filesystem::path& runtime_dir) {
    const auto project_root = find_project_root(target_path);
    teez::core::set_active_config(teez::core::TeezConfig::resolve(
        {.search_dir = project_root, .target_path = target_path, .config_file = std::nullopt}));
    const auto filters = load_filters_from_env();
    auto files = teez::core::glob_match(target_path, "*.teez.lua");
    files =
        teez::core::apply_runner_options(files, target_path, teez::core::runner_options_from_env());
    if (files.empty()) {
        throw std::runtime_error("no .teez.lua files found in: " + target_path.string());
    }

    std::vector<std::string> tests;
    for (const auto& file : files) {
        const auto file_tests = collect_tests_from_file(file, project_root, runtime_dir, filters);
        tests.insert(tests.end(), file_tests.begin(), file_tests.end());
    }
    return tests;
}

int run_worker(const std::filesystem::path& target_path, const std::filesystem::path& runtime_dir,
               std::ostream& out) {
    const auto project_root = find_project_root(target_path);
    teez::core::set_active_config(teez::core::TeezConfig::resolve(
        {.search_dir = project_root, .target_path = target_path, .config_file = std::nullopt}));
    const auto filters = load_filters_from_env();
    auto files = teez::core::glob_match(target_path, "*.teez.lua");
    files =
        teez::core::apply_runner_options(files, target_path, teez::core::runner_options_from_env());
    if (files.empty()) {
        throw std::runtime_error("no .teez.lua files found in: " + target_path.string());
    }

    bool saw_failure = false;
    for (const auto& file : files) {
        saw_failure = run_test_file(file, project_root, runtime_dir, filters, out) || saw_failure;
    }

    return saw_failure ? 1 : 0;
}

} // namespace teez::worker
