#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include <sol/sol.hpp>

#include "teez/worker/mock_file.hpp"

namespace {

struct MockFileFixture {
    sol::state lua;
    sol::table assertions;
    std::vector<sol::protected_function> deferred;

    MockFileFixture() {
        lua.open_libraries(sol::lib::base, sol::lib::string, sol::lib::table);
        lua["__teez_register_defer"] = [this](sol::protected_function fn) { deferred.push_back(fn); };
        assertions = lua.create_table();
        teez::worker::register_mock_file_assertions(assertions, lua);
    }

    ~MockFileFixture() {
        for (auto it = deferred.rbegin(); it != deferred.rend(); ++it) {
            (*it)();
        }
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

TEST_CASE("mock_file creates readable temporary files", "[mock_file]") {
    MockFileFixture fixture;
    const sol::protected_function_result created =
        fixture.fn("mock_file")("config/app.json", R"({"enabled":true})");
    REQUIRE(created.valid());
    const sol::table mock = created;

    REQUIRE(std::filesystem::exists(mock["path"].get<std::string>()));
    REQUIRE(mock["read"]().get<std::string>().find("enabled") != std::string::npos);
    REQUIRE(mock["name"].get<std::string>() == "config/app.json");
}

TEST_CASE("mock_file supports env_key injection", "[mock_file]") {
    MockFileFixture fixture;
    sol::table options = fixture.lua.create_table();
    options["env_key"] = "TEEZ_MOCK_FILE";
    const sol::protected_function_result created =
        fixture.fn("mock_file")("secrets.txt", "token", options);
    REQUIRE(created.valid());
    const sol::table mock = created;

    REQUIRE(mock["env"]["TEEZ_MOCK_FILE"].get<std::string>() == mock["path"].get<std::string>());
}

TEST_CASE("mock_file rejects invalid relative paths", "[mock_file]") {
    MockFileFixture fixture;

    const auto empty = fixture.fn("mock_file")("", "content");
    REQUIRE_FALSE(empty.valid());
    REQUIRE(MockFileFixture::error_message(empty).find("path must not be empty") !=
              std::string::npos);

    const auto absolute = fixture.fn("mock_file")("/etc/passwd", "content");
    REQUIRE_FALSE(absolute.valid());
    REQUIRE(MockFileFixture::error_message(absolute).find("path must be relative") !=
              std::string::npos);

    const auto traversal = fixture.fn("mock_file")("../escape.txt", "content");
    REQUIRE_FALSE(traversal.valid());
    REQUIRE(MockFileFixture::error_message(traversal).find("must not contain '..'") !=
              std::string::npos);
}
