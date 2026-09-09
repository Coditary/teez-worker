#include "teez/worker/runtime_config.hpp"

namespace teez::worker {

namespace {

bool g_use_embedded_runtime = false;

} // namespace

void set_use_embedded_runtime(bool enabled) {
    g_use_embedded_runtime = enabled;
}

bool use_embedded_runtime() {
    return g_use_embedded_runtime;
}

} // namespace teez::worker
