# Sonic Frontiers TAA Trace Logger v2

This build adds a stricter DX11 trace logger for the Hedgehog Engine 2 temporal pipeline.

## Why this exists

HE2 DevTools confirmed these engine-level facts:

- `TemporalUpscalerJob` exists in the main rendering pipeline.
- `taa.enableUpscaling` exists.
- `taa.velocityVarianceBasedWeightBias`, `velocityVarianceMin`, and `velocityVarianceMax` exist.
- `taa.enableCharaStencilMask` exists.

That means the renderer has a velocity-aware temporal upscaler/TAA path. The DX11 DLL cannot read those engine object names directly, so this logger watches the DX11 command stream and tries to catch the GPU state that corresponds to that temporal pass.

## What v2 logs

The logger hooks:

- `ID3D11Device::CreateTexture2D`
- `ID3D11DeviceContext::OMSetRenderTargets`
- `ID3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews`
- `ID3D11DeviceContext::PSSetShaderResources`
- `ID3D11DeviceContext::CSSetShaderResources`
- `ID3D11DeviceContext::PSSetShader`
- `ID3D11DeviceContext::CSSetShader`
- `Draw`, `DrawIndexed`, instanced draw calls
- `Dispatch`

It records texture format, resolution, bind flags, and resource pointer for likely temporal resources.

## What changed from v1

v1 was too loose. It flagged early simple copy/upscale passes such as:

```text
RTV0 = R8G8B8A8_UNORM 1920x1080
PS_SRV0 = R8G8B8A8_UNORM 1280x720
```

v2 ignores that pattern.

v2 only emits `[taa-trace] candidate` when the currently bound state looks like a real temporal pass:

- output target exists, or compute dispatch has rich SRVs
- HDR/scene-color-like resource is present
- depth/depth-stencil-like resource is present
- multiple history/color-like resources are present
- velocity-like or packed-data-like resource is present

## What to search for after testing

Open `fsr-injector.log` and search for:

```text
[taa-trace] candidate
```

Useful candidate lines will include groups like:

```text
RTV0
DSV0
PS_SRV0
PS_SRV1
PS_SRV2
CS_SRV0
```

The goal is to identify:

- scene color / HDR input
- depth / stencil input
- TAA history input
- velocity / motion vector candidate
- temporal upscaler output

## Important

This is not the FSR3 implementation yet. It is the trace tool needed before FSR3 can be wired correctly.
