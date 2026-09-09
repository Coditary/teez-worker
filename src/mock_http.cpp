#include "teez/worker/mock_http.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <httplib.h>
#include <sol/sol.hpp>

namespace teez::worker {

namespace {

struct MockResponse {
    int status = 200;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

struct RecordedRequest {
    std::string method;
    std::string path;
    std::string host;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
};

enum class MockHttpMode { Server, Proxy };

struct MockHttpInstance {
    MockHttpMode mode = MockHttpMode::Server;
    std::unique_ptr<httplib::Server> server;
    std::thread thread;
    int port = 0;
    std::atomic<bool> stopped{false};
    mutable std::mutex mutex;
    std::vector<RecordedRequest> requests;
    std::vector<std::pair<std::pair<std::string, std::string>, MockResponse>> routes;
    std::vector<std::pair<std::string, MockResponse>> intercepts;
};

std::string to_upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    return value;
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::string strip_port(const std::string& host_header) {
    const auto colon = host_header.find(':');
    if (colon == std::string::npos) {
        return host_header;
    }
    return host_header.substr(0, colon);
}

struct ParsedTarget {
    std::string host;
    std::string path;
};

ParsedTarget parse_request_target(const std::string& target) {
    if (target.rfind("http://", 0) == 0) {
        const std::string rest = target.substr(7);
        const auto slash = rest.find('/');
        const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
        const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
        return {strip_port(host_port), path};
    }
    if (target.rfind("https://", 0) == 0) {
        const std::string rest = target.substr(8);
        const auto slash = rest.find('/');
        const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
        const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
        return {strip_port(host_port), path};
    }
    return {"", target.empty() ? "/" : target};
}

MockResponse parse_response_config(const sol::table& config) {
    MockResponse response;
    if (const sol::object status = config["status"]; status.get_type() == sol::type::number) {
        response.status = status.as<int>();
    }
    if (const sol::object body = config["body"]; body.get_type() == sol::type::string) {
        response.body = body.as<std::string>();
    }
    if (const sol::object headers = config["headers"]; headers.get_type() == sol::type::table) {
        for (const auto& pair : headers.as<sol::table>()) {
            if (pair.first.get_type() != sol::type::string ||
                pair.second.get_type() != sol::type::string) {
                continue;
            }
            response.headers.emplace_back(pair.first.as<std::string>(),
                                          pair.second.as<std::string>());
        }
    }
    return response;
}

RecordedRequest record_from_request(const httplib::Request& req, const ParsedTarget& target) {
    RecordedRequest recorded;
    recorded.method = to_upper(req.method);
    recorded.path = target.path.empty() ? req.path : target.path;
    recorded.body = req.body;

    if (!target.host.empty()) {
        recorded.host = target.host;
    } else {
        const auto host_it = req.headers.find("Host");
        if (host_it != req.headers.end()) {
            recorded.host = strip_port(host_it->second);
        }
    }

    for (const auto& header : req.headers) {
        recorded.headers.emplace_back(header.first, header.second);
    }
    return recorded;
}

void apply_response(httplib::Response& res, const MockResponse& mock) {
    res.status = mock.status;
    res.body = mock.body;
    for (const auto& header : mock.headers) {
        res.set_header(header.first, header.second);
    }
}

sol::table headers_to_lua(sol::state& lua,
                          const std::vector<std::pair<std::string, std::string>>& headers) {
    sol::table table = lua.create_table();
    for (const auto& header : headers) {
        table[header.first] = header.second;
    }
    return table;
}

sol::table request_to_lua(sol::state& lua, const RecordedRequest& request) {
    sol::table table = lua.create_table();
    table["method"] = request.method;
    table["path"] = request.path;
    table["host"] = request.host;
    table["body"] = request.body;
    table["headers"] = headers_to_lua(lua, request.headers);
    return table;
}

sol::table requests_to_lua(sol::state& lua, const std::vector<RecordedRequest>& requests) {
    sol::table table = lua.create_table();
    for (std::size_t index = 0; index < requests.size(); ++index) {
        table[index + 1] = request_to_lua(lua, requests[index]);
    }
    return table;
}

void stop_instance(const std::shared_ptr<MockHttpInstance>& instance) {
    if (instance->stopped.exchange(true)) {
        return;
    }
    if (instance->server != nullptr) {
        instance->server->stop();
    }
    if (instance->thread.joinable()) {
        instance->thread.join();
    }
}

void register_auto_stop(sol::state& lua, const std::shared_ptr<MockHttpInstance>& instance) {
    sol::protected_function register_defer = lua["__teez_register_defer"];
    register_defer([instance]() { stop_instance(instance); });
}

void handle_request(const std::shared_ptr<MockHttpInstance>& instance, const httplib::Request& req,
                    httplib::Response& res) {
    if (to_upper(req.method) == "CONNECT") {
        res.status = 501;
        res.set_content("HTTPS proxy interception is not supported in teez V1", "text/plain");
        return;
    }

    const ParsedTarget target = parse_request_target(req.path);
    MockResponse mock_response;
    bool matched = false;

    {
        std::lock_guard lock(instance->mutex);
        instance->requests.push_back(record_from_request(req, target));

        if (instance->mode == MockHttpMode::Server) {
            const std::string method = to_upper(req.method);
            const std::string path = target.path.empty() ? req.path : target.path;
            for (const auto& route : instance->routes) {
                if (route.first.first == method && route.first.second == path) {
                    mock_response = route.second;
                    matched = true;
                    break;
                }
            }
        } else {
            std::string host = target.host;
            if (host.empty()) {
                const auto host_it = req.headers.find("Host");
                if (host_it != req.headers.end()) {
                    host = strip_port(host_it->second);
                }
            }
            const std::string host_key = to_lower(host);
            for (const auto& intercept : instance->intercepts) {
                if (intercept.first == host_key) {
                    mock_response = intercept.second;
                    matched = true;
                    break;
                }
            }
        }
    }

    if (!matched) {
        res.status = 404;
        res.set_content("no mock route configured", "text/plain");
        return;
    }

    apply_response(res, mock_response);
}

void register_request_handler(const std::shared_ptr<MockHttpInstance>& instance) {
    const auto handler = [instance](const httplib::Request& req, httplib::Response& res) {
        handle_request(instance, req, res);
    };

    instance->server->Get(R"(.*)", handler);
    instance->server->Post(R"(.*)", handler);
    instance->server->Put(R"(.*)", handler);
    instance->server->Delete(R"(.*)", handler);
    instance->server->Patch(R"(.*)", handler);
    instance->server->Options(R"(.*)", handler);
}

void start_instance(const std::shared_ptr<MockHttpInstance>& instance) {
    instance->server = std::make_unique<httplib::Server>();
    register_request_handler(instance);

    instance->port = instance->server->bind_to_any_port("127.0.0.1");
    if (instance->port <= 0) {
        throw std::runtime_error("mock_http failed: could not bind to localhost");
    }

    instance->thread = std::thread([instance]() { instance->server->listen_after_bind(); });

    for (int attempt = 0; attempt < 200; ++attempt) {
        if (instance->server->is_running()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    stop_instance(instance);
    throw std::runtime_error("mock_http failed: server did not start");
}

sol::table make_instance_table(sol::state& lua, const std::shared_ptr<MockHttpInstance>& instance) {
    sol::table table = lua.create_table();
    table["port"] = instance->port;
    const std::string url = "http://127.0.0.1:" + std::to_string(instance->port);
    table["url"] = url;

    sol::table env = lua.create_table();
    env["http_proxy"] = url;
    env["https_proxy"] = url;
    env["HTTP_PROXY"] = url;
    env["HTTPS_PROXY"] = url;
    table["env"] = env;

    table["stop"] = [instance]() { stop_instance(instance); };

    table["get_requests"] = [&lua, instance](sol::optional<std::string> method,
                                             sol::optional<std::string> path) {
        std::vector<RecordedRequest> filtered;
        std::lock_guard lock(instance->mutex);
        for (const auto& request : instance->requests) {
            if (method.has_value() && to_upper(*method) != request.method) {
                continue;
            }
            if (path.has_value() && *path != request.path) {
                continue;
            }
            filtered.push_back(request);
        }
        return requests_to_lua(lua, filtered);
    };

    if (instance->mode == MockHttpMode::Server) {
        table["route"] = [instance](const std::string& method, const std::string& path,
                                    const sol::table& config) {
            const MockResponse response = parse_response_config(config);
            std::lock_guard lock(instance->mutex);
            instance->routes.emplace_back(std::make_pair(to_upper(method), path), response);
        };
    } else {
        table["intercept"] = [instance](const std::string& host, const sol::table& config) {
            const MockResponse response = parse_response_config(config);
            std::lock_guard lock(instance->mutex);
            instance->intercepts.emplace_back(to_lower(strip_port(host)), response);
        };
    }

    return table;
}

std::shared_ptr<MockHttpInstance> create_instance(sol::state& lua, MockHttpMode mode) {
    auto instance = std::make_shared<MockHttpInstance>();
    instance->mode = mode;
    start_instance(instance);
    register_auto_stop(lua, instance);
    return instance;
}

} // namespace

void register_mock_http_assertions(sol::table& assertions, sol::state& lua) {
    assertions["mock_http_server"] = [&lua]() {
        const auto instance = create_instance(lua, MockHttpMode::Server);
        return make_instance_table(lua, instance);
    };
    assertions["mock_http_proxy"] = [&lua]() {
        const auto instance = create_instance(lua, MockHttpMode::Proxy);
        return make_instance_table(lua, instance);
    };
}

} // namespace teez::worker
