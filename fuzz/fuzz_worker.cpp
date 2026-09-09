#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "fuzz_helpers.hpp"
#include "teez/worker/http.hpp"
#include "teez/worker/runner.hpp"

#ifndef TEEZ_WORKER_RUNTIME_DIR
#error "TEEZ_WORKER_RUNTIME_DIR must be defined"
#endif

namespace {

const std::filesystem::path kRuntimeDir = TEEZ_WORKER_RUNTIME_DIR;

void fuzz_run_lua(const std::string& payload) {
    const auto dir = teez::fuzz::make_temp_dir("teez-fuzz-worker", payload);
    std::ofstream(dir / "fuzz.teez.lua") << payload;
    std::ostringstream out;
    teez::worker::run_worker(dir, kRuntimeDir, out);
}

void fuzz_list_lua(const std::string& payload) {
    const auto dir = teez::fuzz::make_temp_dir("teez-fuzz-worker-list", payload);
    std::ofstream(dir / "fuzz.teez.lua") << payload;
    teez::worker::list_worker(dir, kRuntimeDir);
}

void fuzz_http(const std::string& payload) {
    const std::string url = payload.empty() ? "http://127.0.0.1:9" : payload;
    teez::worker::http_get(url);
    teez::worker::http_post(url, payload);
}

enum class FuzzOp : std::uint8_t {
    RunLua = 0,
    ListLua = 1,
    Http = 2,
    Count,
};

void dispatch(FuzzOp op, const std::string& payload) {
    switch (op) {
    case FuzzOp::RunLua:
        fuzz_run_lua(payload);
        break;
    case FuzzOp::ListLua:
        fuzz_list_lua(payload);
        break;
    case FuzzOp::Http:
        fuzz_http(payload);
        break;
    case FuzzOp::Count:
        break;
    }
}

FuzzOp decode_op(std::uint8_t byte) {
    return static_cast<FuzzOp>(byte % static_cast<std::uint8_t>(FuzzOp::Count));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size < 2) {
        return 0;
    }

    const auto op = decode_op(data[0]);
    const std::string payload = teez::fuzz::payload_as_string(data + 1, size - 1);
    teez::fuzz::invoke_safely([&]() { dispatch(op, payload); });
    return 0;
}
