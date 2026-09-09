#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "teez/core/process.hpp"

#ifndef TEEZ_WORKER_BIN
#error "TEEZ_WORKER_BIN must be defined"
#endif

namespace {

struct ProcessOutput {
    int exit_code = 0;
    std::string stdout_text;
    std::string stderr_text;
};

ProcessOutput run_worker_bin(const std::vector<std::string>& args) {
    teez::core::CommandSpec spec;
    spec.command = TEEZ_WORKER_BIN;
    spec.args = args;

    const auto result = teez::core::run_command_capture(spec);
    return {result.exit_code, result.stdout_text, result.stderr_text};
}

std::filesystem::path write_worker_test_dir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "smoke.teez.lua") << R"(
test.it("passes", function(t)
    t.assert_true(true)
end)
)";
    return dir;
}

}  // namespace

TEST_CASE("teez-worker prints usage without directory argument", "[worker][integration]") {
    const auto output = run_worker_bin({});

    REQUIRE(output.exit_code == 1);
    REQUIRE(output.stderr_text.find("usage:") != std::string::npos);
}

TEST_CASE("teez-worker runs tests from directory", "[worker][integration]") {
    const auto dir = write_worker_test_dir("teez_worker_main_run_test");
    const auto output = run_worker_bin({dir.string()});

    REQUIRE(output.exit_code == 0);
    REQUIRE(output.stdout_text.find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("teez-worker accepts --update-snapshots flag", "[worker][integration]") {
    const auto dir = write_worker_test_dir("teez_worker_main_snapshot_flag_test");
    const auto output = run_worker_bin({"--update-snapshots", dir.string()});

    REQUIRE(output.exit_code == 0);
    REQUIRE(output.stdout_text.find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("teez-worker --list prints discovered test ids", "[worker][integration]") {
    const auto dir = write_worker_test_dir("teez_worker_main_list_test");
    const auto output = run_worker_bin({"--list", dir.string()});

    REQUIRE(output.exit_code == 0);
    REQUIRE(output.stdout_text.find("passes") != std::string::npos);
}

TEST_CASE("teez-worker reports missing test files", "[worker][integration]") {
    const auto empty_dir = std::filesystem::temp_directory_path() / "teez_worker_main_empty";
    std::filesystem::remove_all(empty_dir);
    std::filesystem::create_directories(empty_dir);

    const auto output = run_worker_bin({empty_dir.string()});

    REQUIRE(output.exit_code == 1);
    REQUIRE(output.stderr_text.find("error:") != std::string::npos);
}
