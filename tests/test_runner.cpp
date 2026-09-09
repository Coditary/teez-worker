#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "teez/worker/runner.hpp"
#include "teez/worker/runtime_config.hpp"
#include "teez/core/process.hpp"

#ifndef TEEZ_WORKER_RUNTIME_DIR
#error "TEEZ_WORKER_RUNTIME_DIR must be defined"
#endif

namespace {

const std::filesystem::path kRuntimeDir = TEEZ_WORKER_RUNTIME_DIR;

std::filesystem::path write_test_file(const std::filesystem::path& dir, const std::string& content) {
    std::filesystem::create_directories(dir);
    const auto path = dir / "smoke.teez.lua";
    std::ofstream(path) << content;
    return path;
}

class StderrCapture {
  public:
    StderrCapture() {
        saved_stderr_ = dup(STDERR_FILENO);
        int fds[2] = {-1, -1};
        if (pipe(fds) != 0) {
            throw std::runtime_error("failed to create stderr capture pipe");
        }
        read_fd_ = fds[0];
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
    }

    ~StderrCapture() {
        restore();
        if (read_fd_ >= 0) {
            close(read_fd_);
            read_fd_ = -1;
        }
    }

    std::string drain() {
        restore();
        std::string captured;
        char buffer[512];
        ssize_t bytes = 0;
        while (read_fd_ >= 0 && (bytes = read(read_fd_, buffer, sizeof(buffer))) > 0) {
            captured.append(buffer, static_cast<std::size_t>(bytes));
        }
        return captured;
    }

  private:
    void restore() {
        if (saved_stderr_ >= 0) {
            dup2(saved_stderr_, STDERR_FILENO);
            close(saved_stderr_);
            saved_stderr_ = -1;
        }
    }

    int saved_stderr_ = -1;
    int read_fd_ = -1;
};

}  // namespace

TEST_CASE("run_worker executes teez.lua tests and emits NDJSON", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_run_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Shell", function()
    test.it("runs echo", function(t)
        local out = sys.exec("bash", {"-c", "echo worker-ok"})
        t.assert_contains(out, "worker-ok")
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"start\"") != std::string::npos);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(output.str().find("smoke.teez.lua::default::Shell > runs echo") != std::string::npos);
}

TEST_CASE("run_worker reports assertion failures", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_fail_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("fails assertion", function(t)
    t.assert_eq(1, 2)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 1);
    REQUIRE(output.str().find("\"event\":\"fail\"") != std::string::npos);
}

TEST_CASE("run_worker supports assert_true and assert_false", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_truthy_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("checks truthiness", function(t)
    t.assert_true(1)
    t.assert_true("ok")
    t.assert_false(false)
    t.assert_false(nil)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("list_worker collects test ids without executing them", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_list_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Suite", function()
    test.it("alpha", function(t)
        t.assert_true(false)
    end)
    test.skip("skipped case", function(t)
        t.assert_true(false)
    end)
end)
)");

    const auto tests = teez::worker::list_worker(temp_dir, kRuntimeDir);

    REQUIRE(tests.size() == 2);
    REQUIRE(tests[0].find("alpha") != std::string::npos);
    REQUIRE(tests[1].find("skipped case") != std::string::npos);
}

TEST_CASE("run_worker supports assert_not_eq", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_not_eq_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("checks inequality", function(t)
    t.assert_not_eq(1, 2)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
}

TEST_CASE("run_worker supports assert_near", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_near_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("checks approximate equality", function(t)
    t.assert_near(0.1 + 0.2, 0.3, 0.0001)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
}

TEST_CASE("run_worker supports assert_throws", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_throws_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("expects errors", function(t)
    t.assert_throws(function()
        error("boom")
    end, "boom")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
}

TEST_CASE("run_worker reports assert_throws failure when no error is raised", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_throws_fail_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("expects a throw", function(t)
    t.assert_throws(function()
        local x = 1
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 1);
    REQUIRE(output.str().find("assert_throws failed") != std::string::npos);
}

TEST_CASE("run_worker supports prio2 string and table assertions", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_prio2_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("checks strings tables and files", function(t)
    t.assert_starts_with("hello world", "hello")
    t.assert_ends_with("hello world", "world")
    t.assert_match("status=200 ok", "status=%d+")
    t.assert_empty("")
    t.assert_empty({})
    t.assert_not_empty("data")
    t.assert_not_empty({ value = 1 })

    local data = json.decode('{"status":"ok","code":200}')
    t.assert_table_has(data, "status")
    t.assert_table_has(data, "code", 200)

    t.assert_http_status({ status = 201, body = "created" }, 201)
    t.assert_file_exists("/etc/hosts")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker supports prio3 membership type snapshot and retry assertions", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_prio3_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    const auto snapshot_path = temp_dir / "output.snap";
    std::ofstream(snapshot_path) << "expected-output\n";

    write_test_file(temp_dir, std::string(R"(
attempts = 0

test.it("checks membership type snapshots and retries", function(t)
    t.assert_in({ "a", "b", "c" }, "b")
    t.assert_in({ id = 42, name = "teez" }, 42)
    t.assert_type("hello", "string")
    t.assert_type(123, "number")
    t.assert_snapshot("expected-output\n", ")") + snapshot_path.string() + R"(")

    t.assert_eventually(function()
        attempts = attempts + 1
        if attempts < 2 then
            error("not ready yet")
        end
    end, 1)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker supports process result assertions", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_process_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("checks exit code and streams", function(t)
    local ok = sys.run("bash", {"-c", "echo ok-out"})
    t.assert_exit_success(ok)
    t.assert_stdout_contains(ok, "ok-out")
    t.assert_stderr_empty(ok)

    local fail = sys.run("bash", {"-c", "echo bad >&2; exit 3"})
    t.assert_exit_code(fail, 3)
    t.assert_stderr_contains(fail, "bad")
    t.assert_exit_not(fail, 0)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker runs defer and lifecycle hooks even when tests fail", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_lifecycle_test";
    std::filesystem::remove_all(temp_dir);
    const auto marker = temp_dir / "lifecycle.log";
    std::filesystem::create_directories(temp_dir);

    write_test_file(temp_dir, std::string(R"(
local marker = ")") + marker.string() + R"("

local function mark(tag)
    sys.run("bash", {"-c", "echo " .. tag .. " >> " .. marker})
end

test.describe("Lifecycle", function()
    test.beforeAll(function()
        mark("beforeAll")
    end)

    test.afterAll(function()
        mark("afterAll")
    end)

    test.beforeEach(function()
        mark("beforeEach")
    end)

    test.afterEach(function()
        mark("afterEach")
    end)

    test.it("runs defer in LIFO order on failure", function(t)
        mark("test-start")
        t.defer(function() mark("defer-1") end)
        t.defer(function() mark("defer-2") end)
        t.assert_eq(1, 2)
    end)

    test.it("passes on second run", function(t)
        mark("test-pass")
        t.assert_true(true)
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 1);
    REQUIRE(output.str().find("\"event\":\"fail\"") != std::string::npos);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);

    std::ifstream log(marker);
    std::string log_text((std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>());

    REQUIRE(log_text.find("beforeAll") != std::string::npos);
    REQUIRE(log_text.find("beforeEach") != std::string::npos);
    REQUIRE(log_text.find("test-start") != std::string::npos);
    REQUIRE(log_text.find("defer-2") != std::string::npos);
    REQUIRE(log_text.find("defer-1") != std::string::npos);
    REQUIRE(log_text.find("afterEach") != std::string::npos);
    REQUIRE(log_text.find("test-pass") != std::string::npos);
    REQUIRE(log_text.find("afterAll") != std::string::npos);

    const auto before_all_pos = log_text.find("beforeAll");
    const auto before_all_again = log_text.find("beforeAll", before_all_pos + 1);
    REQUIRE(before_all_again == std::string::npos);

    const auto defer_2_pos = log_text.find("defer-2");
    const auto defer_1_pos = log_text.find("defer-1");
    REQUIRE(defer_2_pos < defer_1_pos);
}

TEST_CASE("run_worker emits phase events for lifecycle hooks", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_phase_events_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Phase hooks", function()
    test.beforeEach(function()
    end)

    test.afterEach(function()
    end)

    test.it("runs", function(t)
        t.assert_true(true)
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string text = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(text.find("\"event\":\"phase\"") != std::string::npos);
    REQUIRE(text.find("\"phase\":\"beforeEach\"") != std::string::npos);
    REQUIRE(text.find("\"phase\":\"afterEach\"") != std::string::npos);
    REQUIRE(text.find("\"phase\":\"test\"") != std::string::npos);
    REQUIRE(text.find("\"state\":\"start\"") != std::string::npos);
    REQUIRE(text.find("\"state\":\"end\"") != std::string::npos);
}

TEST_CASE("run_worker supports parameterized test.each cases", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_each_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.each({
    { "alice@example.com", 200 },
    { "invalid-email", 400 },
    { "", 400 },
})("POST /users returns %s for status %s", function(t, email, expected_status)
    t.assert_true(type(email) == "string")
    t.assert_true(expected_status == 200 or expected_status == 400)
    if expected_status == 200 then
        t.assert_not_empty(email)
    end
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("alice@example.com") != std::string::npos);
    REQUIRE(events.find("invalid-email") != std::string::npos);
    REQUIRE(events.find("\"event\":\"pass\"") != std::string::npos);

    std::size_t pass_count = 0;
    std::size_t pos = 0;
    while ((pos = events.find("\"event\":\"pass\"", pos)) != std::string::npos) {
        ++pass_count;
        ++pos;
    }
    REQUIRE(pass_count == 3);
}

TEST_CASE("run_worker supports test.each.only for focused cases", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_each_only_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("ignored single test", function(t)
    t.assert_eq(1, 2)
end)

test.each.only({
    { "focused-a", 1 },
    { "focused-b", 2 },
})("case %s value %s", function(t, label, value)
    t.assert_true(value > 0)
    t.assert_contains(label, "focused")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("focused-a") != std::string::npos);
    REQUIRE(events.find("focused-b") != std::string::npos);
    REQUIRE(events.find("ignored single test") == std::string::npos);
}

TEST_CASE("run_worker supports test flow control modifiers", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_modifiers_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.skip("skipped test", function(t)
    t.assert_eq(1, 2)
end)

test.todo("future test")

test.fails("expected failure", function(t)
    t.assert_eq(1, 2)
end)

test.fails("unexpected pass", function(t)
    t.assert_true(true)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 1);
    REQUIRE(events.find("\"event\":\"skip\"") != std::string::npos);
    REQUIRE(events.find("skipped test") != std::string::npos);
    REQUIRE(events.find("\"event\":\"todo\"") != std::string::npos);
    REQUIRE(events.find("future test") != std::string::npos);
    REQUIRE(events.find("expected failure") != std::string::npos);
    REQUIRE(events.find("expected test to fail but it passed") != std::string::npos);
}

TEST_CASE("run_worker supports test.only filtering", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_only_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("ignored when only is set elsewhere", function(t)
    t.assert_eq(1, 2)
end)

test.only("runs exclusively", function(t)
    t.assert_true(true)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("runs exclusively") != std::string::npos);
    REQUIRE(events.find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(events.find("ignored when only") == std::string::npos);
}

TEST_CASE("run_worker filters tests via TEEZ_FILTERS", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_filter_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("return 9999", function(t)
    t.assert_true(true)
end)

test.it("other test", function(t)
    t.assert_true(true)
end)
)");

    setenv("TEEZ_FILTERS", R"({"name_glob":"return 999*"})", 1);

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    unsetenv("TEEZ_FILTERS");

    const std::string events = output.str();
    REQUIRE(exit_code == 0);
    REQUIRE(events.find("return 9999") != std::string::npos);
    REQUIRE(events.find("other test") == std::string::npos);
}

TEST_CASE("run_worker supports describe.only blocks", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_describe_only_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Ignored", function()
    test.it("never runs", function(t)
        t.assert_eq(1, 2)
    end)
end)

test.describe.only("Focused", function()
    test.it("runs inside only describe", function(t)
        t.assert_true(true)
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("runs inside only describe") != std::string::npos);
    REQUIRE(events.find("never runs") == std::string::npos);
}

TEST_CASE("run_worker retries flaky tests until they pass", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_retry_pass_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
attempts = 0

test("flaky health check", { retry = 3 }, function(t)
    attempts = attempts + 1
    if attempts < 3 then
        t.assert_eq(1, 2)
    end
    t.assert_true(true)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("\"event\":\"retry\"") != std::string::npos);
    REQUIRE(events.find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(events.find("flaky health check") != std::string::npos);
}

TEST_CASE("run_worker fails after retry attempts are exhausted", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_retry_fail_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("always failing", { retry = 2 }, function(t)
    t.assert_eq(1, 2)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 1);
    REQUIRE(events.find("\"event\":\"retry\"") != std::string::npos);
    REQUIRE(events.find("\"event\":\"fail\"") != std::string::npos);

    std::size_t retry_count = 0;
    std::size_t pos = 0;
    while ((pos = events.find("\"event\":\"retry\"", pos)) != std::string::npos) {
        ++retry_count;
        ++pos;
    }
    REQUIRE(retry_count == 1);
}

TEST_CASE("run_worker supports assert_match_snapshot golden files", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_match_snapshot_test";
    std::filesystem::remove_all(temp_dir);
    const auto snapshot_path =
        temp_dir / "__snapshots__" / "smoke.teez.lua" / "cli_help_output.snap";

    write_test_file(temp_dir, R"(
test.it("stores CLI output snapshot", function(t)
    local r = sys.run("bash", {"-c", "echo teez-cli-help-v1"})
    t.assert_exit_success(r)
    t.assert_match_snapshot(r.stdout, "cli_help_output")
end)
)");

    setenv("TEEZ_UPDATE_SNAPSHOTS", "1", 1);
    {
        std::ostringstream output;
        const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
        REQUIRE(exit_code == 0);
    }
    REQUIRE(std::filesystem::exists(snapshot_path));

    unsetenv("TEEZ_UPDATE_SNAPSHOTS");
    {
        std::ostringstream output;
        const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
        REQUIRE(exit_code == 0);
    }

    write_test_file(temp_dir, R"(
test.it("stores CLI output snapshot", function(t)
    local r = sys.run("bash", {"-c", "echo changed-output"})
    t.assert_exit_success(r)
    t.assert_match_snapshot(r.stdout, "cli_help_output")
end)
)");
    {
        std::ostringstream output;
        const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
        REQUIRE(exit_code == 1);
        REQUIRE(output.str().find("snapshot mismatch") != std::string::npos);
    }
}

TEST_CASE("run_worker supports mock_binary spy assertions", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_mock_binary_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("tracks binary calls like vitest", function(t)
    local docker = t.mock_binary("docker", { exit_code = 0, stdout = "Mocked Output" })

    sys.run("docker", {"run", "-d", "redis"}, { env = docker.env })
    sys.run("docker", {"stop", "redis"}, { env = docker.env })

    t.assert_called(docker)
    t.assert_called_times(docker, 2)
    t.assert_called_with(docker, 1, {"run", "-d", "redis"})
    t.assert_called_with(docker, 2, {"stop", "redis"})

    local calls = docker.get_calls()
    t.assert_eq(calls[1][1], "run")
    t.assert_eq(calls[2][1], "stop")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker supports mock_http_server env injection", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_mock_http_server_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("mocks via environment variable", function(t)
    local server = t.mock_http_server()
    server.route("POST", "/data", { status = 201, body = '{"ok": true}' })

    local r = sys.run("bash", {"-c", "curl -s -X POST -d 'payload' \"$API_URL/data\""}, {
        env = { API_URL = server.url }
    })

    t.assert_exit_success(r)
    t.assert_stdout_contains(r, "ok")

    local requests = server.get_requests("POST", "/data")
    t.assert_eq(#requests, 1)
    t.assert_eq(requests[1].body, "payload")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker supports mock_http_proxy interception", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_mock_http_proxy_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("intercepts hardcoded urls via proxy", function(t)
    local proxy = t.mock_http_proxy()
    proxy.intercept("example.com", { status = 200, body = '{"user": "fake"}' })

    local r = sys.run("curl", {"-s", "http://example.com/users/1"}, {
        env = proxy.env
    })

    t.assert_exit_success(r)
    t.assert_stdout_contains(r, "fake")

    local requests = proxy.get_requests("GET", "/users/1")
    t.assert_eq(#requests, 1)
    t.assert_eq(requests[1].host, "example.com")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker supports sys.run auto_respond hooks", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_auto_respond_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("mocks interaction via auto-responder", function(t)
    local r = sys.run("bash", {"-c", [[
        read -r -p "Username: " username
        read -r -s -p "Password: " password
        echo
        read -r -p "Do you want to continue? [y/N] " answer
        echo "Setup finished successfully"
    ]]}, {
        auto_respond = {
            ["Username:"] = "admin\n",
            ["Password:"] = "geheim123\n",
            ["Do you want to continue? [y/N]"] = "y\n",
        },
        timeout = 5000,
    })

    t.assert_exit_success(r)
    t.assert_stdout_contains(r, "Setup finished successfully")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker supports mock_file temporary config files", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_mock_file_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("mocks configuration files", function(t)
    local fake_config = t.mock_file("config.json", [[ {"token": "fake"} ]])

    local r = sys.run("bash", {"-c", "grep -q fake " .. fake_config.path .. " && echo ok"})
    t.assert_exit_success(r)
    t.assert_stdout_contains(r, "ok")
    t.assert_eq(fake_config.read(), [[ {"token": "fake"} ]])
end)

test.it("mocks nested config paths via env_key", function(t)
    local creds = t.mock_file(".aws/credentials", "token=fake\n", { env_key = "AWS_SHARED_CREDENTIALS_FILE" })

    local r = sys.run("bash", {"-c", "grep -q fake \"$AWS_SHARED_CREDENTIALS_FILE\" && echo ok"}, {
        env = creds.env
    })
    t.assert_exit_success(r)
    t.assert_stdout_contains(r, "ok")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker supports vitest expect notation", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_expect_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("uses vitest expect matchers", function(t)
    expect(200).toBe(200)
    expect({ id = 1, name = "teez" }).toEqual({ id = 1, name = "teez" })
    expect("hello").toContain("ell")
    expect("status=200 ok").toMatch("status=%d+")
    expect({ "a", "b", "c" }).toHaveLength(3)
    expect({ data = { id = 5 } }).toHaveProperty("data.id", 5)
    expect(0.1 + 0.2).toBeCloseTo(0.3, 5)
    expect("value").toBeDefined()
    expect(nil).toBeNull()
    expect("").toBeFalsy()
    expect("ok").toBeTruthy()
    expect(10).toBeGreaterThan(0)
    expect(3).toBeLessThan(5)
    expect(function() error("boom") end).toThrow()
    expect(function() error("Timeout") end).toThrowError("Timeout")
    expect["not"](1).toBe(2)
end)

test.it("supports expect mock spy matchers", function(t)
    local docker = t.mock_binary("echo", { exit_code = 0 })

    sys.run("echo", {"hello"}, { env = docker.env })

    expect(docker).toHaveBeenCalled()
    expect(docker).toHaveBeenCalledTimes(1)
    expect(docker).toHaveBeenCalledWith("hello")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker rejects missing runtime directory", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_missing_runtime";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("ignored", function(t)
    t.assert_true(true)
end)
)");

    std::ostringstream output;
    REQUIRE_THROWS_AS(
        teez::worker::run_worker(temp_dir, "/tmp/teez-worker-runtime-missing", output),
        std::runtime_error);
}

TEST_CASE("run_worker reports assertion helper failures", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_assert_failures";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("fails assert_match", function(t)
    t.assert_match("status=500", "status=%d+ ok")
end)

test.it("fails assert_empty", function(t)
    t.assert_empty("not-empty")
end)

test.it("fails assert_near", function(t)
    t.assert_near(1.0, 2.0, 0.01)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 1);
    REQUIRE(events.find("assert_match failed") != std::string::npos);
    REQUIRE(events.find("assert_empty failed") != std::string::npos);
    REQUIRE(events.find("assert_near failed") != std::string::npos);
}

TEST_CASE("run_worker reports additional assertion helper failures", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_more_assert_failures";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("fails assert_http_status", function(t)
    t.assert_http_status({ status = 404, body = "missing" }, 200)
end)

test.it("fails assert_in", function(t)
    t.assert_in({ "a", "b" }, "c")
end)

test.it("fails assert_type", function(t)
    t.assert_type("hello", "number")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 1);
    REQUIRE(events.find("assert_http_status failed") != std::string::npos);
    REQUIRE(events.find("assert_in failed") != std::string::npos);
    REQUIRE(events.find("assert_type failed") != std::string::npos);
}

TEST_CASE("run_worker reports core assertion failures", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_core_assert_failures";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("fails assert_not_eq", function(t)
    t.assert_not_eq(1, 1)
end)

test.it("fails assert_true", function(t)
    t.assert_true(false)
end)

test.it("fails assert_false", function(t)
    t.assert_false(true)
end)

test.it("fails assert_contains", function(t)
    t.assert_contains("hello", "bye")
end)

test.it("fails assert_starts_with", function(t)
    t.assert_starts_with("hello", "bye")
end)

test.it("fails assert_ends_with", function(t)
    t.assert_ends_with("hello", "bye")
end)

test.it("fails assert_not_empty", function(t)
    t.assert_not_empty("")
end)

test.it("fails assert_exit_code", function(t)
    local result = sys.run("bash", {"-c", "exit 2"})
    t.assert_exit_code(result, 3)
end)

test.it("fails assert_stdout_empty", function(t)
    local result = sys.run("bash", {"-c", "echo noisy"})
    t.assert_stdout_empty(result)
end)

test.it("fails assert_stderr_empty", function(t)
    local result = sys.run("bash", {"-c", "echo warn >&2"})
    t.assert_stderr_empty(result)
end)

test.it("fails assert_table_has", function(t)
    t.assert_table_has({ id = 1 }, "missing")
end)

test.it("fails assert_eventually", function(t)
    t.assert_eventually(function()
        error("still failing")
    end, 1)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 1);
    REQUIRE(events.find("assert_not_eq failed") != std::string::npos);
    REQUIRE(events.find("assert_true failed") != std::string::npos);
    REQUIRE(events.find("assert_false failed") != std::string::npos);
    REQUIRE(events.find("assert_contains failed") != std::string::npos);
    REQUIRE(events.find("assert_starts_with failed") != std::string::npos);
    REQUIRE(events.find("assert_ends_with failed") != std::string::npos);
    REQUIRE(events.find("assert_not_empty failed") != std::string::npos);
    REQUIRE(events.find("assert_exit_code failed") != std::string::npos);
    REQUIRE(events.find("assert_stdout_empty failed") != std::string::npos);
    REQUIRE(events.find("assert_stderr_empty failed") != std::string::npos);
    REQUIRE(events.find("assert_table_has failed") != std::string::npos);
    REQUIRE(events.find("assert_eventually timed out") != std::string::npos);
}

TEST_CASE("run_worker reports stream and membership assertion failures", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_stream_assert_failures";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("fails assert_stdout_contains", function(t)
    local result = sys.run("bash", {"-c", "echo hello"})
    t.assert_stdout_contains(result, "missing")
end)

test.it("fails assert_stderr_contains", function(t)
    local result = sys.run("bash", {"-c", "echo warn >&2"})
    t.assert_stderr_contains(result, "missing")
end)

test.it("fails assert_table_has value mismatch", function(t)
    t.assert_table_has({ id = 1 }, "id", 2)
end)

test.it("fails assert_throws message mismatch", function(t)
    t.assert_throws(function()
        error("boom")
    end, "different")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 1);
    REQUIRE(events.find("assert_stdout_contains failed") != std::string::npos);
    REQUIRE(events.find("assert_stderr_contains failed") != std::string::npos);
    REQUIRE(events.find("assert_table_has failed") != std::string::npos);
    REQUIRE(events.find("assert_throws failed") != std::string::npos);
}

TEST_CASE("run_worker reports snapshot and process validation failures", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_snapshot_failures";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.it("fails on missing snapshot", function(t)
    t.assert_match_snapshot("actual-output", "missing-snapshot")
end)

test.it("fails on empty snapshot name", function(t)
    t.assert_match_snapshot("actual-output", "")
end)

test.it("fails when process result lacks exit code", function(t)
    t.assert_exit_success({ stdout = "only stdout" })
end)

test.it("fails assert_file_exists for missing path", function(t)
    t.assert_file_exists("/tmp/teez-worker-missing-file-xyz")
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 1);
    REQUIRE(events.find("snapshot not found") != std::string::npos);
    REQUIRE(events.find("snapshot name must not be empty") != std::string::npos);
    REQUIRE(events.find("process result is missing exit_code") != std::string::npos);
    REQUIRE(events.find("assert_file_exists failed") != std::string::npos);
}

TEST_CASE("run_worker reports invalid test file load errors", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_invalid_lua";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::ofstream(temp_dir / "broken.teez.lua") << "this is not valid lua {{{";

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 1);
    REQUIRE(output.str().find("failed to load test file") != std::string::npos);
}

TEST_CASE("run_worker reports test.fails retry progress", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_fails_retry";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
attempts = 0

test.fails("expected to fail twice", { retry = 2 }, function(t)
    attempts = attempts + 1
    if attempts < 2 then
        t.assert_true(true)
        return
    end
    t.assert_eq(1, 2)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("\"event\":\"retry\"") != std::string::npos);
    REQUIRE(events.find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker exposes coverage.run_profile from teez.config.lua", "[worker][coverage]") {
    const auto workspace_root =
        std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    const auto sample_lcov =
        workspace_root / "teez-core/tests/fixtures/coverage/sample.lcov";
    REQUIRE(std::filesystem::exists(sample_lcov));

    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_coverage_profile_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::filesystem::copy_file(sample_lcov, temp_dir / "coverage.lcov");

    {
        std::ofstream config_file(temp_dir / "teez.config.lua");
        config_file << "return {\n"
                       "  coverage = {\n"
                       "    reporter = \"msgpack\",\n"
                       "    min_line_rate = 0.5,\n"
                       "    profiles = {\n"
                       "      demo = {\n"
                       "        command = \"true\",\n"
                       "        cwd = \"" +
                           temp_dir.string() + "\",\n"
                                               "        report = \"coverage.lcov\",\n"
                                               "        output = \"coverage.msgpack\",\n"
                                               "      },\n"
                                               "    },\n"
                                               "  },\n"
                                               "}\n";
    }

    write_test_file(temp_dir, R"(
test.it("runs configured coverage profile", function(t)
    local result = coverage.run_profile("demo")
    t.assert_coverage_threshold(result)
    t.assert_coverage_rate(result, 0.5)
    t.assert_file_exists(result.exported_path)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(std::filesystem::exists(temp_dir / "coverage.msgpack"));
}

TEST_CASE("run_worker exposes harness engine and process plugin", "[worker][harness]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_harness_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Harness", { type = "system" }, function()
    test.it("runs process plugin", function(t)
        local process = harness.use("process")
        local result = process.run({
            command = "echo",
            args = { "hello-harness" },
        })
        t.assert_contains(result.stdout, "hello-harness")
    end)

    test.it("applies template vars", function(t)
        local rendered = harness.vars.apply("hello ${NAME}", { NAME = "world" })
        t.assert_eq(rendered, "hello world")
    end)

    test.it("runs phases", function(t)
        harness.phase("setup", function()
            harness.sleep(0.01)
        end)
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(events.find("harness_loaded") != std::string::npos);
    REQUIRE(events.find("harness_phase") != std::string::npos);
}

TEST_CASE("run_worker applies configurable test id output format", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_output_format_test";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);
    std::ofstream(temp_dir / "teez.config.lua") << R"(
return {
    output = {
        test_id = {
            file = false,
        },
    },
}
)";
    write_test_file(temp_dir, R"(
test.describe("Output format", { type = "system" }, function()
    test.it("omits file name from config", function(t)
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("smoke.teez.lua::system::") == std::string::npos);
    REQUIRE(events.find("system::Output format > omits file name from config") != std::string::npos);
}

TEST_CASE("run_worker does not leak sol2 stderr logs for test.fails", "[worker]") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_stderr_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Fails", function()
    test.fails("expected assertion failure", function(t)
        t.assert_eq(1, 2)
    end)
end)
)");

    StderrCapture stderr_capture;
    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string stderr_text = stderr_capture.drain();

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("expected assertion failure") != std::string::npos);
    REQUIRE(stderr_text.find("[sol2]") == std::string::npos);
    REQUIRE(stderr_text.find("assert_eq failed") == std::string::npos);
}

TEST_CASE("run_worker hyperfine harness benchmarks commands", "[worker][harness]") {
    if (!teez::core::command_exists("hyperfine")) {
        SUCCEED("hyperfine not installed");
        return;
    }

    const auto temp_dir = std::filesystem::temp_directory_path() / "teez_worker_hyperfine_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Hyperfine", { type = "experiment" }, function()
    test.it("compares two commands", function(t)
        local hyperfine = harness.use("hyperfine")
        local report = hyperfine.run({
            commands = { "sleep 0.01", "sleep 0.02" },
            warmup = 1,
            runs = 3,
        })

        t.assert_eq(#report.results, 2)
        t.assert_true(report.mean(1) < report.mean(2))
        t.assert_contains(report.fastest().command, "sleep 0.01")
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(events.find("harness_loaded") != std::string::npos);
    REQUIRE(events.find("\"name\":\"hyperfine\"") != std::string::npos);
}

TEST_CASE("run_worker harness probe until and exec helpers", "[worker][harness]") {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "teez_worker_harness_probe_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Harness probe", { type = "system" }, function()
    test.it("waits until callback succeeds", function(t)
        local attempts = 0
        harness.probe["until"](function()
            attempts = attempts + 1
            return attempts >= 2
        end, 1)
        t.assert_true(attempts >= 2)
    end)

    test.it("exec helper checks command success", function(t)
        t.assert_true(harness.probe.exec("true", {}))
        t.assert_false(harness.probe.exec("bash", {"-c", "exit 1"}))
    end)
end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("run_worker harness phase errors and capture file output", "[worker][harness]") {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "teez_worker_harness_phase_capture_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Harness phase and capture", { type = "system" }, function()
    test.it("writes capture samples to file", function(t)
        local capture_path = os.getenv("TEEZ_CAPTURE_PATH")
        local result = harness.capture.for_duration(0.05, {
            every = 0.01,
            tag = "metrics",
            file = capture_path,
            header = "value",
        }, function()
            return "sample"
        end)
        t.assert_true(result.count >= 1)
        local file = io.open(capture_path, "r")
        t.assert_true(file ~= nil)
        local content = file:read("*a")
        file:close()
        t.assert_contains(content, "value")
        t.assert_contains(content, "sample")
    end)

    test.fails("phase errors propagate", function(t)
        harness.phase("broken", function()
            error("phase boom")
        end)
    end)
end)
)");

    setenv("TEEZ_CAPTURE_PATH", (temp_dir / "capture.csv").string().c_str(), 1);
    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("harness_phase") != std::string::npos);
    REQUIRE(events.find("\"state\":\"error\"") != std::string::npos);
    REQUIRE(events.find("harness_capture") != std::string::npos);
    REQUIRE(std::filesystem::exists(temp_dir / "capture.csv"));
}

TEST_CASE("run_worker harness probe timeout and env options", "[worker][harness]") {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "teez_worker_harness_probe_options_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Harness probe options", { type = "system" }, function()
    test.it("uses timeout seconds for tcp probe", function(t)
        local open = harness.probe.tcp("127.0.0.1", 9, { timeout = 0.05 })
        t.assert_eq(open, false)
    end)

    test.it("passes env through probe exec", function(t)
        t.assert_true(harness.probe.exec("bash", {"-c", "test -n \"$TEEZ_PROBE_ENV\""}, {
            env = { TEEZ_PROBE_ENV = "1" },
        }))
    end)

    test.fails("probe until times out", function(t)
        harness.probe["until"](function()
            return false
        end, 0.1)
    end)
end)
)");

    StderrCapture stderr_capture;
    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string stderr_text = stderr_capture.drain();

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(stderr_text.find("timed out") != std::string::npos);
}

TEST_CASE("run_worker harness format apply and unknown plugin errors", "[worker][harness]") {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "teez_worker_harness_format_error_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Harness format", { type = "system" }, function()
    test.it("applies output template", function(t)
        local rendered = harness.format.apply("${name}", {
            file = "demo.teez.lua",
            type = "system",
            suites = { "Suite" },
            name = "example",
        })
        t.assert_eq(rendered, "example")
    end)

    test.fails("missing harness plugin", function(t)
        harness.use("missing-harness-plugin")
    end)

    test.fails("missing required tool", function(t)
        harness.tool.require({ "teez-definitely-missing-tool-12345" })
    end)
end)
)");

    StderrCapture stderr_capture;
    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string stderr_text = stderr_capture.drain();

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
    REQUIRE(stderr_text.find("missing-harness-plugin") != std::string::npos);
    REQUIRE(stderr_text.find("teez-definitely-missing-tool-12345") != std::string::npos);
}

TEST_CASE("run_worker exercises harness probe process and tool helpers", "[worker][harness]") {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "teez_worker_harness_extended_test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(
test.describe("Harness extended", { type = "system" }, function()
    test.it("probes tcp helper", function(t)
        local open = harness.probe.tcp("127.0.0.1", 9, { timeout_ms = 50 })
        t.assert_eq(open, false)
    end)

    test.it("checks tool helpers", function(t)
        t.assert_true(harness.tool.exists("echo"))
        harness.tool.require("echo")
        harness.tool.require({ "echo" })
    end)

    test.it("captures timed samples", function(t)
        local result = harness.capture.for_duration(0.05, { every = 0.01, tag = "cpu" }, function()
            return "sample"
        end)
        t.assert_true(result.count >= 1)
    end)

    test.it("formats test ids", function(t)
        local id = harness.format.test_id({
            file = "demo.teez.lua",
            type = "system",
            suites = { "Suite" },
            name = "example",
        })
        t.assert_contains(id, "example")
    end)

end)
)");

    std::ostringstream output;
    const int exit_code = teez::worker::run_worker(temp_dir, kRuntimeDir, output);
    const std::string events = output.str();

    REQUIRE(exit_code == 0);
    REQUIRE(events.find("\"event\":\"pass\"") != std::string::npos);
}

TEST_CASE("embedded worker runtime runs without runtime files on disk", "[worker]") {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "teez-embedded-runtime-test";
    std::filesystem::remove_all(temp_dir);
    write_test_file(temp_dir, R"(test.describe("embedded", function()
    test.it("passes", function(t)
        t.assert_eq(1, 1)
    end)
end)
)");

    teez::worker::set_use_embedded_runtime(true);
    std::ostringstream output;
    const int exit_code =
        teez::worker::run_worker(temp_dir, "/tmp/teez-missing-runtime-dir", output);
    teez::worker::set_use_embedded_runtime(false);

    REQUIRE(exit_code == 0);
    REQUIRE(output.str().find("\"event\":\"pass\"") != std::string::npos);
}
