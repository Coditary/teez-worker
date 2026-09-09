#pragma once

#include <filesystem>
#include <iostream>
#include <ostream>

#include <sol/sol.hpp>

namespace teez::worker {

void register_harness(sol::state& lua, const std::filesystem::path& project_root,
                      const std::filesystem::path& plugins_dir, std::ostream& out = std::cout);

} // namespace teez::worker
