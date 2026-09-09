#include "teez/worker/harness.hpp"

#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <sol/sol.hpp>

#include "teez/core/discovery.hpp"
#include "teez/core/lua_helpers.hpp"
#include "teez/core/plugin.hpp"
#include "teez/core/plugin_config.hpp"
#include "teez/core/process.hpp"
#include "teez/core/teez_config.hpp"
#include "teez/core/test_filter.hpp"

namespace teez::worker {

namespace {

using teez::core::CommandSpec;

void emit_harness_event(std::ostream& out, const nlohmann::json& event) {
    out << event.dump() << '\n';
    out.flush();
}

CommandSpec command_spec_from_lua(const std::string& command, sol::table args_table) {
    CommandSpec spec;
    spec.command = command;
    if (args_table.valid() && args_table.get_type() == sol::type::table) {
        for (const auto& pair : args_table) {
            if (pair.second.get_type() == sol::type::string) {
                spec.args.push_back(pair.second.as<std::string>());
            }
        }
    }
    return spec;
}

void apply_run_options(CommandSpec& spec, const sol::table& options) {
    const sol::object env = options["env"];
    if (env.get_type() == sol::type::table) {
        const sol::table env_table = env;
        for (const auto& pair : env_table) {
            if (pair.second.get_type() != sol::type::string) {
                continue;
            }
            std::string key;
            if (pair.first.get_type() == sol::type::string) {
                key = pair.first.as<std::string>();
            } else if (pair.first.get_type() == sol::type::number) {
                key = std::to_string(pair.first.as<int>());
            } else {
                continue;
            }
            spec.env.emplace_back(key, pair.second.as<std::string>());
        }
    }

    const sol::object timeout = options["timeout"];
    if (timeout.get_type() == sol::type::number) {
        spec.timeout_ms = timeout.as<int>();
    }
}

sol::table background_handle_to_lua(sol::state& lua,
                                    const teez::core::BackgroundProcessHandlePtr& handle) {
    sol::table table = lua.create_table();
    table["pid"] = handle->pid();
    table["running"] = sol::property([handle]() { return handle->running(); });
    table["stop"] = [handle]() { handle->stop(); };
    table["lines"] = [handle]() { return handle->lines(); };
    table["count"] = [handle]() { return handle->line_count(); };
    return table;
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

class HarnessRegistry {
  public:
    HarnessRegistry(sol::state& lua, const std::filesystem::path& project_root,
                    const std::filesystem::path& plugins_dir, std::ostream& out)
        : lua_(lua), project_root_(project_root),
          discovery_(teez::core::make_discovery_context(teez::core::active_config(), plugins_dir)),
          out_(out) {}

    sol::table make_api() {
        sol::table api = lua_.create_table();
        api["provide"] = [this](const std::string& name, sol::table module) {
            loaded_[name] = module;
        };
        api["defer"] = [this](sol::protected_function fn) {
            sol::protected_function register_defer = lua_["__teez_register_defer"];
            register_defer(fn);
        };
        api["emit"] = [this](sol::table event) {
            nlohmann::json payload = {{"event", "harness"}};
            for (const auto& pair : event) {
                if (pair.first.get_type() != sol::type::string) {
                    continue;
                }
                const std::string key = pair.first.as<std::string>();
                const sol::object value = pair.second;
                if (value.is<std::string>()) {
                    payload[key] = value.as<std::string>();
                } else if (value.is<bool>()) {
                    payload[key] = value.as<bool>();
                } else if (value.is<int>()) {
                    payload[key] = value.as<int>();
                } else if (value.is<double>()) {
                    payload[key] = value.as<double>();
                }
            }
            emit_harness_event(out_, payload);
        };

        sol::table probe = lua_.create_table();
        probe["tcp"] = [this](const std::string& host, int port,
                              sol::optional<sol::table> options) {
            int timeout_ms = 1000;
            if (options.has_value()) {
                const sol::object timeout = (*options)["timeout"];
                if (timeout.get_type() == sol::type::number) {
                    timeout_ms = static_cast<int>(timeout.as<double>() * 1000.0);
                }
                const sol::object timeout_ms_value = (*options)["timeout_ms"];
                if (timeout_ms_value.get_type() == sol::type::number) {
                    timeout_ms = timeout_ms_value.as<int>();
                }
            }
            return teez::core::probe_tcp_open(host, port, timeout_ms);
        };
        probe["exec"] = [](const std::string& command, sol::table args,
                           sol::optional<sol::table> options) -> bool {
            CommandSpec spec = command_spec_from_lua(command, args);
            if (options.has_value()) {
                apply_run_options(spec, *options);
            }
            const auto result = teez::core::run_command_capture(spec);
            return result.exit_code == 0;
        };
        probe["until"] = [this](sol::protected_function fn, sol::optional<double> timeout_seconds) {
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
                        return true;
                    }
                } else {
                    sol::error err = result;
                    last_error = err.what();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            throw std::runtime_error("harness.probe.until timed out after " +
                                     std::to_string(timeout) + "s: " + last_error);
        };
        api["probe"] = probe;

        sol::table process = lua_.create_table();
        process["spawn"] = [this](const std::string& command, sol::table args,
                                  sol::optional<sol::table> options) -> sol::table {
            CommandSpec spec = command_spec_from_lua(command, args);
            if (options.has_value()) {
                apply_run_options(spec, *options);
            }
            const sol::object capture = options ? (*options)["capture"] : sol::lua_nil;
            const bool capture_output =
                capture.get_type() == sol::type::boolean ? capture.as<bool>() : true;
            auto handle = teez::core::spawn_background_process(spec, capture_output);
            sol::protected_function register_defer = lua_["__teez_register_defer"];
            const teez::core::BackgroundProcessHandlePtr captured = handle;
            register_defer([captured]() { captured->stop(); });
            return background_handle_to_lua(lua_, handle);
        };
        api["process"] = process;

        api["require_plugin"] = [this](const std::string& name) { return use(name); };

        const sol::object harness = lua_["harness"];
        if (harness.get_type() == sol::type::table) {
            sol::table harness_table = harness;
            api["vars"] = harness_table["vars"];
            api["capture"] = harness_table["capture"];
            api["sleep"] = harness_table["sleep"];
        }

        return api;
    }

    sol::table use(const std::string& name) {
        if (loaded_.contains(name)) {
            return loaded_[name];
        }

        const auto manifest = teez::core::find_harness_manifest(name, discovery_, project_root_);
        if (!manifest.has_value()) {
            throw std::runtime_error("harness plugin not found: " + name);
        }

        for (const auto& requirement : manifest->tool_requires) {
            if (!teez::core::command_exists(requirement)) {
                throw std::runtime_error("required tool not found on PATH: " + requirement);
            }
        }

        sol::table api = make_api();
        const auto load_result = lua_.script_file(manifest->plugin_file.string());
        if (!load_result.valid()) {
            sol::error err = load_result;
            throw std::runtime_error("failed to load harness plugin '" + name + "': " + err.what());
        }

        sol::protected_function register_fn = lua_["register"];
        if (register_fn.get_type() != sol::type::function) {
            throw std::runtime_error("harness plugin missing register(): " + name);
        }

        const auto result = register_fn(api);
        lua_["register"] = sol::lua_nil;
        if (!result.valid()) {
            sol::error err = result;
            throw std::runtime_error("failed to register harness plugin '" + name +
                                     "': " + err.what());
        }

        if (!loaded_.contains(name)) {
            throw std::runtime_error("harness plugin did not export module: " + name);
        }

        emit_harness_event(out_, {{"event", "harness_loaded"}, {"name", name}});
        return loaded_[name];
    }

  private:
    sol::state& lua_;
    std::filesystem::path project_root_;
    teez::core::DiscoveryContext discovery_;
    std::ostream& out_;
    std::unordered_map<std::string, sol::table> loaded_;
};

} // namespace

void register_harness(sol::state& lua, const std::filesystem::path& project_root,
                      const std::filesystem::path& plugins_dir, std::ostream& out) {
    auto registry = std::make_shared<HarnessRegistry>(lua, project_root, plugins_dir, out);
    sol::table engine_api = registry->make_api();

    sol::table harness = lua.create_named_table("harness");

    harness["use"] = [registry](const std::string& name) { return registry->use(name); };
    harness["probe"] = engine_api["probe"];
    harness["process"] = engine_api["process"];

    harness["tool"] = lua.create_table();
    harness["tool"]["require"] = [](sol::object tools_arg) {
        std::vector<std::string> tools;
        if (tools_arg.get_type() == sol::type::table) {
            for (const auto& pair : tools_arg.as<sol::table>()) {
                if (pair.second.get_type() == sol::type::string) {
                    tools.push_back(pair.second.as<std::string>());
                }
            }
        } else if (tools_arg.get_type() == sol::type::string) {
            tools.push_back(tools_arg.as<std::string>());
        }

        for (const auto& tool : tools) {
            if (!teez::core::command_exists(tool)) {
                throw std::runtime_error("required tool not found on PATH: " + tool);
            }
        }
    };
    harness["tool"]["exists"] = [](const std::string& tool) {
        return teez::core::command_exists(tool);
    };

    harness["phase"] = [&out](const std::string& name, sol::protected_function fn) {
        emit_harness_event(out, {{"event", "harness_phase"}, {"name", name}, {"state", "start"}});
        const auto result = fn();
        if (!result.valid()) {
            sol::error err = result;
            emit_harness_event(out, {{"event", "harness_phase"},
                                     {"name", name},
                                     {"state", "error"},
                                     {"msg", err.what()}});
            throw std::runtime_error(std::string("harness.phase failed: ") + err.what());
        }
        emit_harness_event(out, {{"event", "harness_phase"}, {"name", name}, {"state", "end"}});
    };

    harness["sleep"] = [](double seconds) {
        std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::duration<double>(seconds)));
    };

    harness["vars"] = lua.create_table();
    harness["vars"]["apply"] = [](const std::string& template_text, sol::table vars) {
        std::string output = template_text;
        for (const auto& pair : vars) {
            if (pair.first.get_type() != sol::type::string ||
                pair.second.get_type() != sol::type::string) {
                continue;
            }
            const std::string key = pair.first.as<std::string>();
            const std::string value = pair.second.as<std::string>();
            const std::string token = "${" + key + "}";
            std::size_t pos = 0;
            while ((pos = output.find(token, pos)) != std::string::npos) {
                output.replace(pos, token.size(), value);
                pos += value.size();
            }
        }
        return output;
    };

    sol::table format = lua.create_table();
    format["test_id"] = [](const sol::table& descriptor_table) {
        return teez::core::serialize_id(teez::core::descriptor_from_lua(descriptor_table),
                                        teez::core::default_test_id_format());
    };
    format["apply"] = [](const std::string& template_text, const sol::table& descriptor_table) {
        return teez::core::apply_output_template(template_text,
                                                 teez::core::descriptor_from_lua(descriptor_table),
                                                 teez::core::default_test_id_format());
    };
    harness["format"] = format;

    harness["capture"] = lua.create_table();
    harness["capture"]["for_duration"] = [&lua, &out](double seconds, sol::table options,
                                                      sol::protected_function collect) {
        double every = 1.0;
        const sol::object every_value = options["every"];
        if (every_value.get_type() == sol::type::number) {
            every = every_value.as<double>();
        }
        const sol::object file = options["file"];
        const sol::object header = options["header"];
        const sol::object tag = options["tag"];

        std::ofstream stream;
        if (file.get_type() == sol::type::string) {
            const auto path = std::filesystem::path(file.as<std::string>());
            if (path.has_parent_path()) {
                std::filesystem::create_directories(path.parent_path());
            }
            const bool write_header =
                header.get_type() == sol::type::string && !std::filesystem::exists(path);
            stream.open(path, std::ios::app);
            if (!stream.is_open()) {
                throw std::runtime_error("failed to open capture file: " + path.string());
            }
            if (write_header) {
                stream << header.as<std::string>() << '\n';
            }
        }

        sol::table samples = lua.create_table();
        int index = 0;
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(seconds));

        while (std::chrono::steady_clock::now() < deadline) {
            const auto sample_result = collect();
            if (!sample_result.valid()) {
                sol::error err = sample_result;
                throw std::runtime_error(std::string("capture.collect failed: ") + err.what());
            }

            sol::object sample = sample_result;
            ++index;
            samples[index] = sample;
            if (stream.is_open() && sample.get_type() == sol::type::string) {
                stream << sample.as<std::string>() << '\n';
            }

            nlohmann::json event = {{"event", "harness_capture"}, {"index", index}};
            if (tag.get_type() == sol::type::string) {
                event["tag"] = tag.as<std::string>();
            }
            if (sample.get_type() == sol::type::string) {
                event["sample"] = sample.as<std::string>();
            }
            emit_harness_event(out, event);

            std::this_thread::sleep_for(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::duration<double>(every)));
        }

        sol::table handle = lua.create_table();
        handle["samples"] = samples;
        handle["count"] = index;
        return handle;
    };

    harness["capture"]["spawn"] = [registry](const std::string& command, sol::table args,
                                             sol::optional<sol::table> options) {
        return registry->make_api()["process"]["spawn"](command, args, options);
    };
}

} // namespace teez::worker
