#pragma once

#include <string>

#include <sol/state_view.hpp>

namespace teez::worker {

/// Raises a Lua assertion error without triggering sol2's C++ exception stderr logging.
inline void assert_fail(const sol::state_view& lua, const std::string& message) {
    luaL_error(lua.lua_state(), "%s", message.c_str());
}

} // namespace teez::worker
