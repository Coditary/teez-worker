#include <iostream>
#include <string_view>

#include <cstdlib>

#include "teez/worker/runner.hpp"

#ifndef TEEZ_WORKER_RUNTIME_DIR
#error "TEEZ_WORKER_RUNTIME_DIR must be defined"
#endif

namespace {

bool arg_is_flag(std::string_view value) {
    return value == "--update-snapshots" || value == "--list";
}

} // namespace

int main(int argc, char** argv) {
    bool list_only = false;
    int arg_index = 1;
    while (arg_index < argc && arg_is_flag(argv[arg_index])) {
        if (std::string_view(argv[arg_index]) == "--update-snapshots") {
            setenv("TEEZ_UPDATE_SNAPSHOTS", "1", 1);
        } else if (std::string_view(argv[arg_index]) == "--list") {
            list_only = true;
        }
        ++arg_index;
    }

    if (arg_index >= argc) {
        std::cerr << "usage: teez-worker [--update-snapshots] [--list] <directory>\n";
        return 1;
    }

    try {
        if (list_only) {
            const auto tests = teez::worker::list_worker(argv[arg_index], TEEZ_WORKER_RUNTIME_DIR);
            for (const auto& id : tests) {
                std::cout << id << '\n';
            }
            return 0;
        }
        return teez::worker::run_worker(argv[arg_index], TEEZ_WORKER_RUNTIME_DIR, std::cout);
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << '\n';
        return 1;
    }
}
