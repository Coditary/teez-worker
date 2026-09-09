#pragma once

#include <sol/sol.hpp>

namespace teez::worker {

void register_mock_file_assertions(sol::table& assertions, sol::state& lua);

} // namespace teez::worker
