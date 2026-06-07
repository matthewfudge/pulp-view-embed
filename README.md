# pulp-view-embed

A flat **C ABI** for embedding a [Pulp](https://github.com/danielraffel/pulp)-imported
frontend (e.g. a design imported from Figma) as a rendered child view inside a
**foreign C++ host** — JUCE, iPlug2, or a bespoke shell — without the host
linking Pulp's C++ ABI.

> Status: **experiment**. macOS is working end to end: high-fidelity render,
> an interactive host↔view parameter bridge (a dragged control moves a host
> parameter; host automation moves the control), a host resource-resolution
> callback, and an offscreen/texture render mode. Not for production yet.
> See `planning/2026-06-06-foreign-host-embedding-revised-plan.md` in the Pulp
> repo for the roadmap.

## Status / what works / known limitations / roadmap

**What works (macOS, end to end):**

- Two create paths — high-fidelity importer JS bundle (rasterized images,
  skeuomorphic knobs, glass) and lightweight DesignIR — both opening a
  `ViewBridge` + `PluginViewHost` (GPU Dawn/Skia, CPU fallback).
- Lifecycle: pulp-parents (`attach`) and host-parents (`native_handle` +
  `notify_attached`); `resize` (with validated DPI scale), `tick`, `repaint`,
  `size_hints`, `active_backend`.
- **Interactive parameters (ABI v3):** controls bind bidirectionally to the
  host's parameters by **string key** (UI drag → host gesture/set; host
  automation → control, no feedback loop).
- **Plugin formats:** consumed by real VST3 / AU / CLAP plugins via the JUCE and
  iPlug2 adapter repos (the editor IS the embedded design).
- **Offscreen / texture render mode**, **`resolve_resource`** host-asset
  callback, **font** resolution + portable bundling, and a relocatable
  shared-library + tarball **packaging** story (`DISTRIBUTING.md`).
- Deterministic headless Skia render (`render_png` / `render_frame_rgba`) +
  live GPU back-buffer capture (`capture_png`).
- Smoke gates M1.1–M1.11 (create/attach/teardown stress, hi-fi bundle, param
  bridge, resolve_resource, offscreen, and a resize/scale/DPI stress sweep).

**Known limitations:**

- macOS only today (the GPU host and the offscreen RGBA producer are
  macOS-specific; Windows/Linux need a `PluginViewHost` factory + raw-pixel
  producer).
- Requires an installed Pulp SDK on `CMAKE_PREFIX_PATH` (the static build cannot
  stand alone; the shared-lib dist is the foreign-host path).
- `pulp_embed_resize`'s `scale` is validated but advisory for the windowed embed
  (the host NSWindow drives backing DPI); only the capture APIs honor it.
- Zero-copy GPU compositing (IOSurface/MTLTexture handle) is deferred — the
  offscreen path returns CPU RGBA today.

**Resolved design questions** (from the foreign-host-embedding plan):

- *Event-loop tick* — borrowed from the host: the host's display-link (GPU) plus
  a host timer drive `pulp_embed_tick`; the shim runs no loop of its own.
- *Parameter model* — string-key based, which works for both JUCE
  `AudioProcessorParameter` and iPlug2 `IParam` (the host maps its own
  parameters onto the design's keys once at create time).

**Roadmap:** Windows/Linux `PluginViewHost`; zero-copy GPU compositing;
`pulp add` foreign-host packaging integration.

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

### Host resource resolution (ABI v3)

A host can serve a design's assets from memory (an in-memory store, an
encrypted bundle, a project file) instead of from disk by supplying
`PulpEmbedDesc.host.resolve_resource`:

```c
const uint8_t* resolve_resource(void* host_ctx, const char* id, size_t* out_len);
```

The shim offers every asset the design references to this callback **before**
falling back to disk. Return borrowed bytes (valid until creation returns) +
write the byte count, or `NULL` to fall back to loading `id` from disk. The
asset `id` is the path as written in the design:

- bundle path (`pulp_embed_create_from_ui_bundle`): the path in `ui.js`, e.g.
  `assets/<hash>.png`.
- DesignIR path (`pulp_embed_create_from_design_json[_str]`): the manifest
  asset's `local_path`.

Bytes the host serves are staged to a temp file (removed on `destroy`) so the
existing on-disk render path picks them up — the host may free its buffer as
soon as the callback returns. (Image draws decode straight through Skia by
path, so staging is how the host's bytes reach every consumer uniformly.)

### Offscreen / texture render mode (ABI v3)

For a host that composites Pulp's output itself — no Pulp-owned child NSView —
create an offscreen view and pull finished frames on demand:

```c
PulpEmbedResult pulp_embed_create_offscreen(const PulpEmbedDesc* desc,
                                            const char* source,    // IR path or bundle dir
                                            int32_t from_bundle,   // 0 = DesignIR, 1 = bundle
                                            PulpEmbedView** out_view);

PulpEmbedResult pulp_embed_render_frame_rgba(PulpEmbedView* view,
                                             int32_t width, int32_t height, float scale,
                                             uint8_t* out, size_t cap,
                                             int32_t* w, int32_t* h, int32_t* stride);
```

The offscreen view is built through the same materializer / scripted-UI
pipeline, parameter bridge, and `resolve_resource` staging as the windowed
paths — it just has no parent window and no display-link. `render_frame_rgba`
hands back a CPU-readable **RGBA8** frame (R,G,B,A byte order, premultiplied
alpha, sRGB, top-to-bottom, `stride == w * 4`), pixel dimensions
`width*scale × height*scale`, via Pulp's deterministic headless Skia renderer —
so offscreen output matches the windowed embed's high-fidelity render exactly
(the M1.10 gate asserts a 0.00000 pixel diff vs the windowed render). Two-call
sizing pattern: pass `out=NULL` to learn `*w/*h/*stride`, then call again with
`cap >= *stride * *h`. `render_frame_rgba` also works on a windowed view (it
renders deterministically rather than reading a live back buffer — use
`pulp_embed_capture_png` for the live host surface). A zero-copy GPU
texture/IOSurface handle is deferred (see "Scoped out").

The smoke (`examples/macos-nsview-smoke`) drives the M1.1–M1.10 gates: synthetic
+ Figma "VST Style" DesignIR, GPU attach/capture, CPU backend, 100× teardown,
the high-fidelity bundle render, the bidirectional parameter bridge, the
resolve_resource host callback (same-bytes parity + different-bytes control),
and the offscreen render mode (matches the windowed render).

### Scoped out (this round)

- **Zero-copy GPU compositing** (`IOSurface` / `MTLTexture` handle): deferred.
  Codex's call — ship CPU RGBA readback first; a GPU handle needs a defined
  ownership/synchronization/resize-invalidation contract and a way to advance
  the offscreen scripted/GPU path without a display link.
- Non-macOS offscreen RGBA: `render_to_rgba` is macOS-only for now (the
  non-Apple screenshot stub has no portable raw-pixel producer).

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

## Distribution

To ship this to a foreign host that does **not** build Pulp from source, see
[`DISTRIBUTING.md`](DISTRIBUTING.md): the cargo-like package manifest
(`pulp-package.json`), the relocatable shared-library dist with a
`find_package(pulp_view_embed)` config and a symbol surface pinned to the C ABI,
the published-SDK tarball recipe (`tools/package-sdk.sh`), and the
codesign/notarize steps.

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
