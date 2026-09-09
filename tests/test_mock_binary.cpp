#include <catch2/catch_test_macros.hpp>

#include <sol/sol.hpp>

#include "teez/core/process.hpp"
#include "teez/worker/mock_binary.hpp"

namespace {

struct MockFixture {
    sol::state lua;
    sol::table assertions;

    MockFixture() {
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table);
        assertions = lua.create_table();
        teez::worker::register_mock_binary_assertions(assertions, lua);
    }

    sol::protected_function fn(const char* name) {
        return assertions[name];
    }

    static std::string error_message(const sol::protected_function_result& result) {
        sol::error err = result;
        return err.what();
    }
};

}  // namespace

TEST_CASE("mock_binary creates executable with custom exit code and streams", "[mock_binary]") {
    MockFixture fixture;
    sol::table options = fixture.lua.create_table();
    options["exit_code"] = 3;
    options["stdout"] = "mock-out";
    options["stderr"] = "mock-err";

    const sol::protected_function_result created =
        fixture.fn("mock_binary")("teez-mock-echo", options);
    REQUIRE(created.valid());
    const sol::table mock = created;

    teez::core::CommandSpec spec;
    spec.command = mock["name"].get<std::string>();
    spec.env = {{"PATH", mock["env"]["PATH"].get<std::string>()}};

    const auto result = teez::core::run_command_capture(spec);
    REQUIRE(result.exit_code == 3);
    REQUIRE(result.stdout_text.find("mock-out") != std::string::npos);
    REQUIRE(result.stderr_text.find("mock-err") != std::string::npos);
}

TEST_CASE("mock_binary rejects invalid binary names", "[mock_binary]") {
    MockFixture fixture;
    const auto result = fixture.fn("mock_binary")("bin/with/slash");
    REQUIRE_FALSE(result.valid());
    REQUIRE(MockFixture::error_message(result).find("binary name must be a simple filename") !=
            std::string::npos);
}

TEST_CASE("assert_called fails when mock was never invoked", "[mock_binary]") {
    MockFixture fixture;
    const sol::protected_function_result created = fixture.fn("mock_binary")("teez-mock-unused");
    REQUIRE(created.valid());
    const sol::table mock = created;

    const auto result = fixture.fn("assert_called")(mock);
    REQUIRE_FALSE(result.valid());
    REQUIRE(MockFixture::error_message(result).find("assert_called failed") != std::string::npos);
}

TEST_CASE("assert_called_times validates invocation count", "[mock_binary]") {
    MockFixture fixture;
    const sol::protected_function_result created = fixture.fn("mock_binary")("teez-mock-count");
    REQUIRE(created.valid());
    const sol::table mock = created;

    teez::core::CommandSpec spec;
    spec.command = mock["name"].get<std::string>();
    spec.args = {"one"};
    spec.env = {{"PATH", mock["env"]["PATH"].get<std::string>()}};
    REQUIRE(teez::core::run_command_capture(spec).exit_code == 0);

    REQUIRE(fixture.fn("assert_called_times")(mock, 1).valid());

    const auto mismatch = fixture.fn("assert_called_times")(mock, 2);
    REQUIRE_FALSE(mismatch.valid());
    REQUIRE(MockFixture::error_message(mismatch).find("assert_called_times failed") !=
            std::string::npos);
}

TEST_CASE("assert_called_with validates indexed arguments", "[mock_binary]") {
    MockFixture fixture;
    const sol::protected_function_result created = fixture.fn("mock_binary")("teez-mock-args");
    REQUIRE(created.valid());
    const sol::table mock = created;

    teez::core::CommandSpec spec;
    spec.command = mock["name"].get<std::string>();
    spec.args = {"run", "-d", "redis"};
    spec.env = {{"PATH", mock["env"]["PATH"].get<std::string>()}};
    REQUIRE(teez::core::run_command_capture(spec).exit_code == 0);

    sol::table expected = fixture.lua.create_table_with(1, "run", 2, "-d", 3, "redis");
    REQUIRE(fixture.fn("assert_called_with")(mock, 1, expected).valid());

    const auto out_of_range = fixture.fn("assert_called_with")(mock, 2, expected);
    REQUIRE_FALSE(out_of_range.valid());
    REQUIRE(MockFixture::error_message(out_of_range).find("out of range") != std::string::npos);

    const auto invalid_index = fixture.fn("assert_called_with")(mock, 0, expected);
    REQUIRE_FALSE(invalid_index.valid());
    REQUIRE(MockFixture::error_message(invalid_index).find("call index must be >= 1") !=
            std::string::npos);

    sol::table wrong = fixture.lua.create_table_with(1, "stop");
    const auto mismatch = fixture.fn("assert_called_with")(mock, 1, wrong);
    REQUIRE_FALSE(mismatch.valid());
    REQUIRE(MockFixture::error_message(mismatch).find("assert_called_with failed") !=
            std::string::npos);
}
