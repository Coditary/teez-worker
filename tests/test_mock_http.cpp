#include <catch2/catch_test_macros.hpp>

#include <sol/sol.hpp>

#include "teez/core/process.hpp"
#include "teez/worker/mock_http.hpp"

namespace {

struct MockHttpFixture {
    sol::state lua;
    sol::table assertions;

    MockHttpFixture() {
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table);
        assertions = lua.create_table();
        teez::worker::register_mock_http_assertions(assertions, lua);
    }

    sol::protected_function fn(const char* name) {
        return assertions[name];
    }
};

}  // namespace

TEST_CASE("mock_http_server records GET requests with custom headers", "[mock_http]") {
    MockHttpFixture fixture;
    const sol::protected_function_result created = fixture.fn("mock_http_server")();
    REQUIRE(created.valid());
    const sol::table server = created;

    sol::table headers = fixture.lua.create_table();
    headers["X-Custom"] = "teez";
    sol::table route_config = fixture.lua.create_table_with("status", 200, "body", "ok", "headers",
                                                            headers);
    server["route"]("GET", "/items", route_config);

    const auto result = teez::core::run_command_capture({
        .command = "curl",
        .args = {"-s", "-H", "X-Test: 1", server["url"].get<std::string>() + "/items"},
    });
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text.find("ok") != std::string::npos);

    const sol::table requests = server["get_requests"]("GET", "/items");
    REQUIRE(requests.size() == 1);
}

TEST_CASE("mock_http_proxy intercepts http hostnames", "[mock_http]") {
    MockHttpFixture fixture;
    const sol::protected_function_result created = fixture.fn("mock_http_proxy")();
    REQUIRE(created.valid());
    const sol::table proxy = created;

    sol::table response = fixture.lua.create_table_with("status", 200, "body", R"({"ok":true})");
    proxy["intercept"]("example.test", response);

    teez::core::CommandSpec spec;
    spec.command = "curl";
    spec.args = {"-s", "http://example.test/resource"};
    spec.env = {{"http_proxy", proxy["env"]["http_proxy"].get<std::string>()},
                {"HTTP_PROXY", proxy["env"]["HTTP_PROXY"].get<std::string>()},
                {"https_proxy", proxy["env"]["https_proxy"].get<std::string>()},
                {"HTTPS_PROXY", proxy["env"]["HTTPS_PROXY"].get<std::string>()}};

    const auto result = teez::core::run_command_capture(spec);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text.find("ok") != std::string::npos);
}
