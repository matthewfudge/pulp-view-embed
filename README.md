# pulp-view-embed

A flat **C ABI** for embedding a [Pulp](https://github.com/danielraffel/pulp)-imported
frontend (e.g. a design imported from Figma) as a rendered child view inside a
**foreign C++ host** — JUCE, iPlug2, or a bespoke shell — without the host
linking Pulp's C++ ABI.

> Status: **experiment**. macOS is working end to end: high-fidelity render
> **plus** an interactive host↔view parameter bridge (a dragged control moves a
> host parameter; host automation moves the control). Not for production yet.
> See `planning/2026-06-06-foreign-host-embedding-revised-plan.md` in the Pulp
> repo for the roadmap.

## Why

The host owns the native parent window; Pulp owns a child view and renders into
it. Only opaque handles, POD structs, and result codes cross the boundary
(`include/pulp_view_embed.h`) — no Pulp C++ type, exception, or STL object. The
shim wraps `pulp::format::ViewBridge` + `pulp::view::PluginViewHost` internally.

## Two render paths

| Entry point | Fidelity | Use it for |
|---|---|---|
| `pulp_embed_create_from_ui_bundle(desc, bundle_dir, out)` | **High — pixel-identical to the Pulp importer's own render** (rasterized images, skeuomorphic knobs, glass panels). Renders the importer's `--emit js` bundle through the scripted-UI pipeline (`ScriptedUiSession` + `WidgetBridge`). | Faithfully reproducing a Figma/imported design. **Recommended.** |
| `pulp_embed_create_from_design_json[_str](desc, …, out)` | Lightweight — flat native widgets, drops rasterized images. | A fast, dependency-light approximation. |

Generate the bundle with the importer (its `--emit js` output is `ui.js` + `assets/`):

```bash
pulp import-design --from figma-plugin --file scene.pulp.json --emit js --output bundle/ui.js
pulp_embed_create_from_ui_bundle(&desc, "bundle", &view);   # renders that bundle
```

## What works (v1, macOS)

- Both create paths above → open a `ViewBridge` → create a `PluginViewHost` (GPU
  Dawn/Skia, CPU fallback).
- `pulp_embed_attach` to a host `NSView*` (gated on a real attach via the
  `try_attach_to_parent`/`is_attached` seam, so a failed attach never fires the
  view-opened lifecycle), plus host-parents mode (`native_handle` +
  `notify_attached`) for JUCE `NSViewComponent` / iPlug2.
- `resize`, `tick`, `repaint`, `size_hints`, `active_backend` (GPU/CPU report).
- `pulp_embed_render_png` — deterministic headless Skia raster (thumbnails/tests).
- `pulp_embed_capture_png` — live GPU back-buffer capture.
- Strict C error model: `PulpEmbedResult` everywhere, `last_error` /
  `last_create_error`, idempotent NULL-safe `destroy` with correct teardown order.
- **Interactive parameter bridge (ABI v2)** — the design's controls are wired
  bidirectionally to the host's parameters:
  - `PulpEmbedDesc.host` carries the host callbacks (`set_param`, `get_param`,
    `begin_gesture`, `end_gesture`, `read_meters`), each passed the host's
    `host_ctx`.
  - Parameters are addressed by **string key** — the design's `pulpParamKey`
    when present, else the control's widget id. Enumerate them with
    `pulp_embed_param_count` / `pulp_embed_param_key` / `pulp_embed_param_widget_id`
    / `pulp_embed_param_value` and map each key to a host parameter once at
    create time.
  - UI → host: a dragged knob/fader fires `begin_gesture` → `set_param`(s) →
    `end_gesture` (normalized [0,1]); a toggle click fires begin/set/end
    atomically.
  - host → UI: `pulp_embed_param_changed(view, key, normalized)` pushes
    automation/preset values into the control and repaints — without echoing
    back to `set_param` (no feedback loop).
  - Mouse/keyboard already reach the controls through the GPU host's native
    child view (`plugin_view_host_mac.mm` forwards `mouseDown:`/`Dragged:`/`Up:`
    to the same widget handlers the bridge hooks); no extra forwarding needed.
  - `pulp_embed_simulate_param_drag(view, index, target)` drives a control
    through its real interaction path for headless host testing.

The smoke (`examples/macos-nsview-smoke`) drives the M1.1–M1.8 gates: synthetic +
Figma "VST Style" DesignIR, GPU attach/capture, CPU backend, 100× teardown, the
high-fidelity bundle render, and the bidirectional parameter bridge.

## Build

Requires an **installed Pulp SDK** (built from the embedding seam branch, which
adds the `PluginViewHost` attach-observability seam):

```bash
# In a Pulp checkout on the seam branch:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DPULP_BUILD_EXAMPLES=OFF
cmake --build build -j
cmake --install build --prefix /path/to/pulp-sdk-install

# Here:
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install
cmake --build build -j
ctest --test-dir build --output-on-failure   # runs the macOS embed smoke
```

`-DPULP_VIEW_EMBED_SHARED=ON` builds `libpulp_view_embed.dylib` (a stable ABI a
foreign host links without seeing Pulp C++ symbols); default is a static lib.

## Layout

```
include/pulp_view_embed.h            # the only header a foreign host includes
src/pulp_view_embed.cpp              # extern "C" shim over ViewBridge/PluginViewHost
examples/macos-nsview-smoke/         # AppKit host: smoke gates + bundle_render harness
fixtures/figma-vst-style/            # Figma "VST Style": DesignIR + bundle/ (ui.js + assets)
fixtures/synthetic/                  # tiny hand-authored DesignIR
```

Framework adapters (JUCE, iPlug2) live in their own repos and depend only on
`pulp_view_embed.h`.

## License

MIT (matches Pulp).
