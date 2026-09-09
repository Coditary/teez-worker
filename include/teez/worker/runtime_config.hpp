#pragma once

namespace teez::worker {

/// When true, worker loads bundled Lua runtime instead of files from disk.
void set_use_embedded_runtime(bool enabled);
bool use_embedded_runtime();

} // namespace teez::worker
