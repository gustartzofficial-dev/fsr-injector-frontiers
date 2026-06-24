#pragma once

namespace capture::dx11log {

// Sonic Frontiers HE2 TemporalUpscaler / TAA trace logger.
// This is intentionally a data-gathering tool: it does not modify rendering.
bool install();
void shutdown();

} // namespace capture::dx11log
