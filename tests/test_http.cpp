#include <catch2/catch_test_macros.hpp>

#include "teez/worker/http.hpp"

TEST_CASE("http.get fetches a public endpoint", "[worker][http]") {
    try {
        const auto response = teez::worker::http_get("https://httpbin.org/get");
        REQUIRE(response.status == 200);
        REQUIRE(response.body.find("httpbin.org") != std::string::npos);
    } catch (const std::exception& ex) {
        WARN(std::string("network unavailable, skipping: ") + ex.what());
    }
}

TEST_CASE("http.post sends request body", "[worker][http]") {
    try {
        const auto response =
            teez::worker::http_post("https://httpbin.org/post", R"({"hello":"teez"})");
        REQUIRE(response.status == 200);
        REQUIRE(response.body.find("hello") != std::string::npos);
    } catch (const std::exception& ex) {
        WARN(std::string("network unavailable, skipping: ") + ex.what());
    }
}
