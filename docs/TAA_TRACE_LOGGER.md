# Sonic Frontiers TAA Trace Logger

This build adds a DX11 trace logger focused on the HE2 `TemporalUpscalerJob` / TAA path discovered through HE2 DevTools.

DevTools evidence we are targeting:

- `TemporalUpscalerJob` exists in the live rendering pipeline.
- `taa.enableUpscaling` exists.
- `taa.velocityVarianceBasedWeightBias`, `taa.velocityVarianceMin`, and `taa.velocityVarianceMax` exist.
- `taa.enableCharaStencilMask` exists.

The logger does not implement FSR3 yet. Its job is to identify the resources that the temporal/upscaler pass consumes.

## What the logger hooks

- `ID3D11Device::CreateTexture2D`
- `ID3D11DeviceContext::OMSetRenderTargets`
- `ID3D11DeviceContext::OMSetRenderTargetsAndUnorderedAccessViews`
- `ID3D11DeviceContext::PSSetShaderResources`
- `ID3D11DeviceContext::CSSetShaderResources`
- `ID3D11DeviceContext::CSSetUnorderedAccessViews`
- `ID3D11DeviceContext::PSSetShader`
- `ID3D11DeviceContext::CSSetShader`
- `Draw`, `DrawIndexed`, instanced draw calls
- `Dispatch`

## What to search for in `fsr-injector.log`

Search for:

```text
[taa-trace] candidate
```

A useful candidate should show a late fullscreen draw or compute dispatch with a group like:

```text
RTV/UAV: final output target
DSV or SRV: depth
PS_SRV/CS_SRV: HDR/scene color
PS_SRV/CS_SRV: history-like color
PS_SRV/CS_SRV: velocity-like R8G8/RG16/RGBA8 texture
```

Also search for:

```text
[taa-trace] create2d
```

This gives the high-value textures created by the game: format, resolution, bind flags, and resource pointer.

## Test procedure

1. Use a clean Sonic Frontiers install with this `dxgi.dll`.
2. Start gameplay in a normal island area.
3. Move the camera for 5-10 seconds.
4. Open the HE2 DevTools rendering window and confirm TAA/upscaler is enabled.
5. Close the game.
6. Upload/paste the `fsr-injector.log`, especially `[taa-trace] candidate` blocks.

Do not judge this build by visual quality. It is a discovery build.
