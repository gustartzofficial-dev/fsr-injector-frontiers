#pragma once

namespace capture::dx11log {

// Installs a targeted DX11 trace logger for Sonic Frontiers / HE2.
// It does not modify rendering; it only records resource bindings around
// temporal-upscaler/TAA-like fullscreen passes so we can identify color,
// depth, history, and velocity inputs.
bool install();
void shutdown();

} // namespace capture::dx11log
