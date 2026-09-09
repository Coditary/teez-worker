#pragma once

#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <vector>

namespace teez::worker {

/// Executes all *.teez.lua files in target_path and streams NDJSON to out.
int run_worker(const std::filesystem::path& target_path, const std::filesystem::path& runtime_dir,
               std::ostream& out = std::cout);

/// Collects test ids from *.teez.lua files without executing them.
std::vector<std::string> list_worker(const std::filesystem::path& target_path,
                                     const std::filesystem::path& runtime_dir);

} // namespace teez::worker
