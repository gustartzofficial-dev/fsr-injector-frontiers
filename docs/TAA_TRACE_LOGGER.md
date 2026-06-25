# Sonic Frontiers TAA Trace Logger v3

This build adds a stricter HE2 TemporalUpscaler/TAA tracer plus a lightweight pseudo frame snapshot.

## What v3 logs

Search the game log for:

```text
[frame-snapshot]
[taa-trace] candidate
CSSetShader
CS_UAV
CS_SRV
```

## Why v3 exists

v1 was too noisy and caught simple copy/upscale fullscreen draws.

v2 was clean but too strict and missed the real temporal path. It also did not reliably see compute-stage bindings because the compute vtable indices were wrong.

v3 fixes that by:

- tracking draw and dispatch events
- hooking compute shader resources
- hooking compute UAV outputs
- using corrected D3D11 immediate-context indices:
  - `CSSetShaderResources` = 67
  - `CSSetUnorderedAccessViews` = 68
  - `CSSetShader` = 69
- logging rich compute events as `[frame-snapshot]`
- still logging stronger matches as `[taa-trace] candidate`

## What we are trying to catch

The HE2 DevTools discovery proved the game has:

- `TemporalUpscalerJob`
- TAA `enableUpscaling`
- TAA velocity variance parameters
- character stencil mask support

The logger is now trying to catch the real DX11 resource bindings behind that temporal stage:

- HDR scene color
- depth/stencil
- history buffers
- velocity / motion-vector-like buffers
- compute UAV output target

## What to send back

After testing, send the full `fsr-injector.log`.

The most useful sections are:

```text
[frame-snapshot]
[taa-trace] candidate
CSSetShader
CS_UAV
CS_SRV
```

A good hit may look like:

```text
[frame-snapshot] reason=rich-compute-event kind=Dispatch ...
[taa-trace]   CS_UAV0 ...
[taa-trace]   CS_SRV0 ...
[taa-trace]   CS_SRV1 ...
[taa-trace]   CS_SRV2 ...
```

If v3 works correctly, it should show compute dispatches with their input textures and output UAVs, which is the missing information from v2.
