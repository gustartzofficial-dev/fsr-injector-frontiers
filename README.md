# FSR Injector Frontiers Experimental

This fork is the Sonic Frontiers / Hedgehog Engine 2 research build of the original FSR Injector project. The immediate goal is not finished FSR3 yet. The immediate goal is to inject into Sonic Frontiers, keep the existing DX11 overlay/upscaling/frame-generation experiments working, and collect the exact render data needed for a real FSR3/DX11 implementation.

Sonic Frontiers is a DX11 title, so official AMD FSR3 frame generation cannot be dropped in directly through the normal DX12/Vulkan FSR3 swapchain path. This project is currently aimed at discovering the game resources required for either:

- a DX11 FSR3 backport path, such as an OptiScaler/metarutaiga-style FidelityFX SDK DX11 route; or
- a later D3D11On12 bridge path, if that proves more stable.

## Current focus for Sonic Frontiers

We need logs that identify the resources FSR3 frame generation needs:

- **HUDless scene color**: the final 3D/post-process color before UI/HUD is drawn.
- **Depth**: the main scene depth buffer, including format, resolution, and whether it is SRV-readable or needs a private copy.
- **Motion vectors / velocity**: the texture read by the TAA/resolve pass, likely a full-resolution `R16G16_FLOAT`, `R32G32_FLOAT`, or sometimes `R16G16B16A16_FLOAT` texture.
- **TAA-like resolve pass clues**: pixel shader pointer, SRV slots, formats, and dimensions for color/history/depth/motion resources bound together.
- **Resolution / format data**: swapchain size, candidate texture dimensions, bind flags, sample count, and color format.

This build adds DX11 scout logging so we can run Sonic Frontiers and inspect `fsr_injector.log` instead of guessing blindly.

## What the DX11 scout logs

The Sonic Frontiers scout logs are intentionally diagnostic. They do not yet prove a candidate is the correct FSR3 input. They tell us what to inspect next.

Look for lines like:

```text
[depth] candidate 1920x1080 D32F bind=0x40 srv=no
[depth] readable private depth copy created 1920x1080 D32F
[dx11-scout] rtv/color-candidate slot=0/1 1920x1080 fmt=R16G16B16A16_FLOAT bind=0x28 samples=1 hasDSV=no note=possible-postprocess-or-hudless-color
[dx11-scout] motion-vector-candidate slot=5 tex=1920x1080 view=R16G16_FLOAT resource=R16G16_FLOAT bind=0x28 mips=1 samples=1 ps=0x00000123456789AB draw=1234
[dx11-scout] depth-srv-candidate slot=3 tex=1920x1080 view=R32_FLOAT resource=R32_TYPELESS bind=0x48 mips=1 samples=1 ps=0x00000123456789AB draw=1234
[dx11-scout] color/history-candidate slot=0 tex=1920x1080 view=R16G16B16A16_FLOAT resource=R16G16B16A16_FLOAT bind=0x28 mips=1 samples=1 ps=0x00000123456789AB draw=1234
```

Useful fields:

- `slot`: shader resource slot used by the current pixel shader.
- `tex`: texture dimensions.
- `view`: SRV view format.
- `resource`: underlying texture format.
- `bind`: D3D11 bind flags.
- `ps`: current pixel shader object pointer for this run. It is not a permanent hash yet, but matching groups of SRVs under the same `ps` are useful for finding the TAA pass.
- `draw`: approximate draw count when the resource was bound.
- `hasDSV=no` on a color RTV late in the frame is a clue for post-process or HUDless color.

## First Sonic Frontiers test flow

1. Build the repo with GitHub Actions.
2. Download the `dxgi.dll` artifact.
3. Place `dxgi.dll` next to `SonicFrontiers.exe`.
4. Launch Sonic Frontiers normally through Steam.
5. Load into gameplay, preferably in a moving scene.
6. Pan the camera or run around for 20-60 seconds.
7. Exit the game.
8. Open `fsr_injector.log` next to the DLL/game executable.
9. Search the log for:

```text
[depth]
[dx11-scout] motion-vector-candidate
[dx11-scout] depth-srv-candidate
[dx11-scout] color/history-candidate
[dx11-scout] rtv/color-candidate
```

The most important thing to paste back for analysis is the group of lines around the same `ps=0x...` where color/history/depth/motion-like resources appear together. That is probably the TAA/resolve area.

## Current controls

These are still inherited from the original experimental injector.

- `Home` = show/hide native menu.
- `End` = enable/disable DX12 post-process path.
- `PageUp/PageDown` = increase/decrease sharpness.
- `Insert/Delete` = increase/decrease internal test scale.
- `F1/F2/F3` = quality/balanced/performance presets.
- `F4` = motion/interpolation preview.
- `F5` = experimental generated-frame presentation.
- `F6` = experimental scout motion-vector candidate usage.

## Runtime environment options

Useful DX11 flags:

```bat
set FSRINJ_DX11_PACING=0
set FSRINJ_DX11_MENU_IN_GEN=0
```

Existing DX12 flags are still present for the global injector path:

```bat
set FSRINJ_DX12_SHARPEN=0
set FSRINJ_DX12_SHARPNESS=0.5
set FSRINJ_DX12_SCALE=0.77
set FSRINJ_DX12_GENPRESENT=1
set FSRINJ_DX12_SCOUT_MV=1
```

## What this build can do now

- Load as a DXGI proxy DLL named `dxgi.dll`.
- Keep the existing DX11 overlay, sharpening, and optical-flow frame-generation experiments.
- Detect DX11 scene depth candidates through `OMSetRenderTargets`.
- Create a private SRV-readable copy for DSV-only depth buffers when possible.
- Log DX11 pixel shader resource candidates that look like color/history, depth SRV, or motion vectors.
- Log DX11 render-target color candidates that may indicate scene color, post-process output, or HUDless color boundaries.

## What this build does not do yet

- It does not implement production FSR3 frame generation.
- It does not yet know Sonic Frontiers' exact motion-vector encoding.
- It does not yet know the final HUDless color boundary automatically.
- It does not yet provide stable shader bytecode hashes for pass fingerprinting.
- It does not yet convert the game's motion vectors into FSR3's expected convention.

## Data still needed before real FSR3 integration

For each useful candidate, we still need to determine:

```text
Resource role:
Format:
Width/height:
Bind flags:
Shader slot:
Current PS pointer from log:
Draw timing / pass order:
SRV-readable or private-copy needed:
Notes:
```

For motion vectors specifically, we still need:

```text
Units: pixels, UV, NDC, or clip-space delta
Sign: current - previous or previous - current
Jitter: includes TAA jitter or already jitter-cancelled
```

That part still needs RenderDoc or shader analysis after the log narrows down the right pass.

## Building

Use GitHub Actions on a Windows runner. After the build completes, download the `dxgi.dll` artifact and place it next to the Sonic Frontiers executable.

The intended GitHub artifact names are usually one of:

```text
fsr-injector-dxgi-dll
dxgi-dll
```

Inside the artifact should be:

```text
dxgi.dll
```

## Debug notes

If the game does not start with `dxgi.dll` present:

- Confirm the artifact is x64.
- Confirm the file is named exactly `dxgi.dll`.
- Remove other overlays while testing: Steam overlay, Discord overlay, NVIDIA overlay, RTSS/MSI Afterburner, Xbox Game Bar.
- Delete old `fsr_injector.log`, launch again, and see if a new log is created.
- If no log is created, the DLL probably is not loading or Windows rejected it before `DllMain`.

If the game starts but the log only has boot/hook lines, run around in-game for longer and make sure you are in actual 3D gameplay, not menus.
