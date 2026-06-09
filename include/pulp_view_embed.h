/*
 * pulp_view_embed.h — flat C ABI for embedding a Pulp-imported frontend
 * inside a foreign C++ host (JUCE, iPlug2, or a bespoke shell).
 *
 * Contract: the host owns the native parent window; Pulp owns a child view and
 * renders into it. NO Pulp C++ type, exception, or STL object crosses this
 * boundary — only opaque handles, POD structs, C strings, and result codes.
 * The implementation (libpulp_view_embed) is built in Pulp's toolchain and
 * wraps pulp::format::ViewBridge + pulp::view::PluginViewHost internally.
 *
 * Threading: every function here must be called on the host's UI/main thread
 * (the thread that owns the parent NSView/HWND). These wrap AppKit/UIKit
 * objects and a display-link render loop; none are safe to call concurrently.
 *
 * Ownership: the caller owns the PulpEmbedView handle and must call
 * pulp_embed_destroy() exactly once. Bytes returned through out-parameters are
 * copied into caller-provided buffers; nothing is borrowed across the boundary.
 *
 * Error model: every operation returns PulpEmbedResult. PULP_EMBED_OK == 0.
 * Per-handle detail is available via pulp_embed_last_error(); creation failures
 * (which produce no handle) report detail via pulp_embed_last_create_error().
 *
 * Compatibility: PulpEmbedDesc carries struct_size + abi_version so the shim
 * can accept structs from older/newer callers. Query pulp_embed_abi_version()
 * after dlopen, before constructing a desc.
 */
#ifndef PULP_VIEW_EMBED_H
#define PULP_VIEW_EMBED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the struct/function layout changes incompatibly.
 *
 * v2 (2026-06): added the host<->view parameter bridge. PulpEmbedDesc grew a
 * trailing `host` callbacks block (after `host_ctx`); the param-enumeration and
 * pulp_embed_param_changed() functions were added. The desc growth is
 * struct_size-gated, so a v1 caller's smaller desc is still accepted (the new
 * tail reads as zero/NULL = "no host bridge"), and a v2 library accepts a v1
 * desc. abi_version is therefore validated as "<= library version", not "==".
 *
 * v3 (2026-06): two deferred capabilities.
 *   (1) resolve_resource — a host resource-resolution callback APPENDED to the
 *       PulpEmbedHostCallbacks block. The shim consults it for each design asset
 *       BEFORE disk; bytes it returns are staged so the existing on-disk render
 *       path picks them up (NULL -> disk fallback). Because the field is at the
 *       END of the host block, a v2 caller's smaller struct_size simply stops
 *       before it (read as NULL = "no resource bridge"); the older host
 *       callbacks are still captured field-by-field.
 *   (2) offscreen/texture render mode — pulp_embed_create_offscreen() (no parent
 *       window) + pulp_embed_render_frame_rgba() (CPU-readable RGBA of the latest
 *       frame). These are new functions, not desc-layout changes.
 * The desc layout grows (host block gains resolve_resource), so this is a real
 * abi_version bump; v1/v2 callers remain accepted via struct_size gating. */
#define PULP_VIEW_EMBED_ABI_VERSION 7u

/* v7 (2026-06): missing-asset diagnostics. Adds pulp_embed_missing_asset_count()
 * + pulp_embed_missing_asset() so a host preflight can ask the materialized view
 * which faithful-vector frame assets failed to resolve on disk, instead of
 * re-parsing the DesignIR JSON + asset manifest by hand. The shim already walks
 * the manifest at create; this just exposes the result. NO desc-layout change and
 * NO new host callbacks — these are pure additive query functions, so v6 callers
 * keep working unchanged (the version bump is informational/monotonic). */

/* v6 (2026-06): text-field string bridge. A design's text_field controls are NOT
 * normalized params, so they ride a separate STRING side-channel: two callbacks
 * APPENDED to the host block (set_string UI->host, get_string host->view initial
 * pull) plus pulp_embed_string_param_count/_key/_get + pulp_embed_set_string.
 * Strings are app-host/plugin STATE (preset name, label) — saved with the
 * plugin, NOT a DAW-automatable parameter (VST3/AU/CLAP have no string param).
 * The host-block growth is struct_size-gated (v1..v5 callers' smaller desc stops
 * before the new fields = NULL); the rest are new functions. */

/* v5 (2026-06): richer parameter metadata. Adds PulpEmbedParamInfo +
 * pulp_embed_param_info() so a host can build a correct param from the design
 * (greenfield) or verify its own (existing plugin): control kind, discreteness,
 * option count, and imported default are available now from the materialized
 * design; name/unit/range fields are present but only populated once the
 * importer carries them (has_range == 0 until then). New function only — no desc
 * layout change — so v1..v4 callers are unaffected. */

/* Opaque embedded-view handle. */
typedef struct PulpEmbedView PulpEmbedView;

typedef enum PulpEmbedResult {
    PULP_EMBED_OK                   = 0,
    PULP_EMBED_ERR_INVALID_ARG      = 1,  /* null/zero arg, struct_size/abi mismatch */
    PULP_EMBED_ERR_PARSE            = 2,  /* DesignIR JSON failed to parse           */
    PULP_EMBED_ERR_MATERIALIZE      = 3,  /* native view-tree build failed           */
    PULP_EMBED_ERR_VIEW_OPEN        = 4,  /* ViewBridge::open() failed               */
    PULP_EMBED_ERR_HOST_CREATE      = 5,  /* no PluginViewHost (missing factory)     */
    PULP_EMBED_ERR_ATTACH           = 6,  /* attach to parent did not take           */
    PULP_EMBED_ERR_UNSUPPORTED      = 7,  /* capability absent (e.g. CPU capture)    */
    PULP_EMBED_ERR_BUFFER_TOO_SMALL = 8,  /* out buffer < required (see *out_len)    */
    PULP_EMBED_ERR_INTERNAL         = 9,  /* unexpected; a C++ exception was caught  */
    PULP_EMBED_ERR_WRONG_THREAD     = 10  /* called off the view's creator thread    */
} PulpEmbedResult;

/* Desired backend (PulpEmbedDesc.backend_pref). */
typedef enum PulpEmbedBackendPref {
    PULP_EMBED_BACKEND_PREF_AUTO = 0,  /* let Pulp choose (GPU if available)   */
    PULP_EMBED_BACKEND_PREF_GPU  = 1,
    PULP_EMBED_BACKEND_PREF_CPU  = 2
} PulpEmbedBackendPref;

/* Active backend (pulp_embed_active_backend return). */
typedef enum PulpEmbedBackend {
    PULP_EMBED_BACKEND_UNKNOWN = 0,  /* before creation succeeds             */
    PULP_EMBED_BACKEND_GPU     = 1,
    PULP_EMBED_BACKEND_CPU     = 2   /* incl. a GPU preference that fell back */
} PulpEmbedBackend;

/* ---- host parameter bridge (ABI v2) ----------------------------------- *
 *
 * The host wires a design's controls to ITS parameters by supplying these
 * callbacks in PulpEmbedDesc.host. Every callback carries the host's opaque
 * `host_ctx` (PulpEmbedDesc.host_ctx) back unchanged. Parameters are addressed
 * by their string KEY (the design's pulpParamKey, or — when a control carries
 * no binding metadata — its widget id). Use the param-enumeration functions
 * (pulp_embed_param_count / _key / _widget_id) to map each design key to a host
 * parameter once, right after creation.
 *
 * Direction UI -> host (a user dragging a knob): Pulp fires begin_gesture, then
 * one or more set_param(value), then end_gesture. `value` is NORMALIZED [0,1]
 * — the host denormalizes against its own range. A control without explicit
 * gesture phases (a toggle click) fires begin -> set -> end synchronously.
 *
 * Direction host -> UI (automation / preset recall): the host calls
 * pulp_embed_param_changed(view, key, value) to push a normalized value into
 * the matching control; the widget repaints. That path does NOT re-enter
 * set_param (no feedback loop).
 *
 * read_meters is polled from pulp_embed_tick(): the host writes up to `cap`
 * normalized [0,1] level samples into `out` and returns the count written.
 * Designs without meters ignore it.
 *
 * All callbacks are optional — a NULL slot disables that direction. Every
 * callback runs on the host UI thread (same constraint as the rest of the ABI).
 */
typedef void   (*PulpEmbedSetParamFn)(void* host_ctx, const char* key, double normalized);
typedef double (*PulpEmbedGetParamFn)(void* host_ctx, const char* key);
typedef void   (*PulpEmbedGestureFn)(void* host_ctx, const char* key);
typedef int32_t (*PulpEmbedReadMetersFn)(void* host_ctx, float* out, int32_t cap);

/* Host resource-resolution callback (ABI v3).
 *
 * The host serves an imported asset's bytes by id, so a design's images/fonts
 * can come from the host (an in-memory asset store, an encrypted bundle, a
 * project file) instead of disk. `id` is the design's asset identifier:
 *   - bundle path (pulp_embed_create_from_ui_bundle): the asset path as written
 *     in ui.js, e.g. "assets/<hash>.png".
 *   - DesignIR path (pulp_embed_create_from_design_json[_str]): the manifest
 *     asset's local_path (its on-disk-relative path), e.g. "assets/<hash>.png".
 *
 * Return BORROWED bytes valid until creation returns, and write the byte count
 * to *out_len. Return NULL (and the shim falls back to loading `id` from disk).
 * The shim copies any returned bytes immediately (it stages them to a temp file
 * the existing on-disk render path reads), so the host may free/recycle its
 * buffer as soon as the callback returns. Called once per asset, during
 * creation, on the host thread. NULL slot = no resource bridge (disk only). */
typedef const uint8_t* (*PulpEmbedResolveResourceFn)(void* host_ctx,
                                                     const char* id,
                                                     size_t* out_len);

/* Text-field string bridge (ABI v6).
 *
 * A design's text_field controls carry a STRING, not a normalized value, so they
 * are bridged separately from the numeric param bridge. set_string: the user
 * edited a text field -> the host stores the UTF-8 string (its own state; saved
 * with the plugin, NOT automatable). get_string: pulled once after creation to
 * seed the field from host state (preset recall) — write up to `cap` bytes
 * (incl. NUL) into `out`, return the full length excluding NUL, or a negative to
 * leave the field's imported placeholder/value. Both keyed by the text_field's
 * design key (source node id); both run on the host UI thread; NULL slot = no
 * string bridge. */
typedef void    (*PulpEmbedSetStringFn)(void* host_ctx, const char* key, const char* utf8);
typedef int32_t (*PulpEmbedGetStringFn)(void* host_ctx, const char* key,
                                        char* out, int32_t cap);

typedef struct PulpEmbedHostCallbacks {
    PulpEmbedSetParamFn   set_param;     /* UI gesture -> host param write      */
    PulpEmbedGetParamFn   get_param;     /* host -> view initial-value pull     */
    PulpEmbedGestureFn    begin_gesture; /* UI gesture begin (undo grouping)    */
    PulpEmbedGestureFn    end_gesture;   /* UI gesture end                      */
    PulpEmbedReadMetersFn read_meters;   /* polled from tick(); may be NULL     */
    /* ABI v3 tail — struct_size-gated. A v1/v2 caller's smaller desc stops
     * before this; the shim reads it as NULL = "no resource bridge". */
    PulpEmbedResolveResourceFn resolve_resource; /* host asset bytes by id      */
    /* ABI v6 tail — struct_size-gated (v1..v5 callers stop before this). */
    PulpEmbedSetStringFn set_string; /* UI text edit -> host string state       */
    PulpEmbedGetStringFn get_string; /* host -> view initial text (preset recall)*/
} PulpEmbedHostCallbacks;

/* Creation descriptor. Zero-initialize, then set struct_size = sizeof(*desc),
 * abi_version = PULP_VIEW_EMBED_ABI_VERSION, and the sizing fields. */
typedef struct PulpEmbedDesc {
    uint32_t    struct_size;     /* sizeof(PulpEmbedDesc) — fwd/back compat   */
    uint32_t    abi_version;     /* PULP_VIEW_EMBED_ABI_VERSION               */
    int32_t     logical_width;   /* initial logical size, > 0                 */
    int32_t     logical_height;
    float       scale_factor;    /* host DPI scale; <= 0 treated as 1.0       */
    int32_t     backend_pref;    /* PulpEmbedBackendPref                      */
    int32_t     design_width;    /* optional fixed design-viewport; 0 = off   */
    int32_t     design_height;
    const char* asset_base_path; /* base dir for relative asset paths when    */
                                 /* creating from a JSON string; may be NULL  */
    void*       host_ctx;        /* opaque; passed back to every host.* cb     */

    /* ABI v2 tail — struct_size-gated. A v1 caller (smaller struct_size) omits
     * this; the shim treats the absent block as all-NULL (no host bridge). */
    PulpEmbedHostCallbacks host; /* host parameter/meter callbacks; all opt.  */
} PulpEmbedDesc;

/* Resize constraints derived from the imported design (pulp_embed_size_hints). */
typedef struct PulpEmbedSizeHints {
    int32_t preferred_width;
    int32_t preferred_height;
    int32_t min_width;
    int32_t min_height;
    int32_t max_width;     /* 0 = unbounded */
    int32_t max_height;    /* 0 = unbounded */
    float   aspect_ratio;  /* 0 = unconstrained */
    int32_t resizable;     /* 0/1 */
} PulpEmbedSizeHints;

/* ---- library ---------------------------------------------------------- */

/* ABI version this library was built against. Query after dlopen, before
 * building a PulpEmbedDesc. */
uint32_t pulp_embed_abi_version(void);

/* ---- creation --------------------------------------------------------- */

/* Build the embedded view from a DesignIR JSON file. The view is constructed
 * offscreen and is NOT attached. On PULP_EMBED_OK, *out_view receives the
 * handle; otherwise *out_view is set to NULL and pulp_embed_last_create_error()
 * carries detail. */
PulpEmbedResult pulp_embed_create_from_design_json(const PulpEmbedDesc* desc,
                                                   const char* design_ir_json_path,
                                                   PulpEmbedView** out_view);

/* Same, from an in-memory JSON buffer (json need not be NUL-terminated).
 * Relative asset paths resolve against desc->asset_base_path. */
PulpEmbedResult pulp_embed_create_from_design_json_str(const PulpEmbedDesc* desc,
                                                       const char* json,
                                                       size_t json_len,
                                                       PulpEmbedView** out_view);

/* Build the embedded view from a Pulp importer JS bundle directory — the
 * `--emit js` output of `pulp import-design` (a `ui.js` driving the native
 * widget bridge: createCol/createImage/createKnob + setImageSource/setFlex,
 * with the rasterized assets referenced by absolute or bundle-relative path).
 *
 * This is the HIGH-FIDELITY path: it renders through the SAME scripted-UI
 * pipeline (`pulp::view::ScriptedUiSession` + WidgetBridge) that the Pulp
 * importer's own `--validate` render and real GPU-scripted plugins use, so the
 * embed reproduces the importer's render (rasterized 3D shapes, skeuomorphic
 * knobs, light glass panels) instead of the flattened native-widget fallback.
 *
 * `bundle_dir` must contain `ui.js`. Asset paths inside `ui.js` may be absolute
 * (as emitted by the CLI) or relative to `bundle_dir`. On PULP_EMBED_OK,
 * *out_view receives the handle; otherwise *out_view is NULL and
 * pulp_embed_last_create_error() carries detail. */
PulpEmbedResult pulp_embed_create_from_ui_bundle(const PulpEmbedDesc* desc,
                                                 const char* bundle_dir,
                                                 PulpEmbedView** out_view);

/* ---- offscreen / texture render mode (ABI v3) ------------------------- *
 *
 * For a host that composites Pulp's output ITSELF — no Pulp-owned child NSView.
 * The view is built exactly like the windowed create paths (same materializer /
 * scripted-UI pipeline, same parameter bridge, same resolve_resource staging),
 * but is never attached to a parent and never drives a display-link. The host
 * pulls finished frames on demand with pulp_embed_render_frame_rgba() and draws
 * them into its own surface.
 *
 * `source` is a DesignIR JSON file path when from_bundle == 0, or a bundle
 * directory (containing ui.js) when from_bundle != 0 — the offscreen analogues
 * of pulp_embed_create_from_design_json and pulp_embed_create_from_ui_bundle.
 * On PULP_EMBED_OK, *out_view receives the handle; otherwise *out_view is NULL
 * and pulp_embed_last_create_error() carries detail.
 *
 * The frame producer is Pulp's deterministic headless Skia renderer (the same
 * one behind pulp_embed_render_png), so offscreen output matches the windowed
 * embed's high-fidelity render. An offscreen view reports
 * PULP_EMBED_BACKEND_CPU from pulp_embed_active_backend (no live GPU host), but
 * still renders the full scripted UI. */
PulpEmbedResult pulp_embed_create_offscreen(const PulpEmbedDesc* desc,
                                            const char* source,
                                            int32_t from_bundle,
                                            PulpEmbedView** out_view);

/* Render the current view tree to a CPU-readable RGBA frame and copy it into
 * `out` (capacity `cap` bytes). The pixel format is tightly packed RGBA8
 * (R,G,B,A byte order), premultiplied alpha, sRGB, top-to-bottom rows, with
 * stride == *w * 4. Pixel dimensions are width*scale by height*scale; they are
 * always written to *w / *h (and *stride to the row byte count) so the caller
 * can size its buffer exactly.
 *
 * Two-call sizing pattern: pass out=NULL to learn *w, *h, *stride and the
 * required byte count (returned via (*stride) * (*h); check against your buffer),
 * then call again with cap >= (*stride) * (*h). Returns PULP_EMBED_ERR_BUFFER_TOO_SMALL if
 * out != NULL but cap < required, and PULP_EMBED_ERR_UNSUPPORTED when no Skia
 * raster backend is available. Works for BOTH offscreen and windowed views (it
 * renders deterministically; it does not read a live back buffer — use
 * pulp_embed_capture_png for the live host surface). */
PulpEmbedResult pulp_embed_render_frame_rgba(PulpEmbedView* view,
                                             int32_t width, int32_t height,
                                             float scale, uint8_t* out,
                                             size_t cap, int32_t* w,
                                             int32_t* h, int32_t* stride);

/* Copy the most recent creation error on THIS thread into buf (NUL-terminated,
 * truncated to cap). Returns the full length excluding the NUL. Thread-local:
 * read it immediately after a failed create on the same thread. */
size_t pulp_embed_last_create_error(char* buf, size_t cap);

/* ---- lifecycle -------------------------------------------------------- */
/*
 * Two legal attach modes — pick ONE per view, never mix them:
 *
 *   (A) Pulp-parents:  pulp_embed_attach(view, parent)
 *       Pulp adds its child into your parent NSView and fires the lifecycle.
 *       Best for bespoke hosts that have a parent native view to hand over.
 *
 *   (B) Host-parents:  pulp_embed_native_handle(view) -> notify_attached(view)
 *       You take Pulp's child native view and insert it into your own
 *       hierarchy (e.g. juce::NSViewComponent::setView, iPlug2 IGraphics),
 *       then call notify_attached() so Pulp fires the view-opened lifecycle.
 *       Required for frameworks whose native component owns parenting.
 */

/* The Pulp child view's native handle (NSView* on macOS). For mode (B): insert
 * it into your framework's native hierarchy, then call notify_attached().
 * Returns NULL before creation succeeds. The handle is owned by Pulp — do NOT
 * release it; null your wrapper's reference before pulp_embed_destroy(). */
void* pulp_embed_native_handle(PulpEmbedView* view);

/* Mode (B): tell Pulp the host has parented the child view. Fires the
 * view-opened lifecycle iff the child is actually in a native hierarchy
 * (superview != nil on macOS); otherwise returns PULP_EMBED_ERR_ATTACH and
 * does NOT fire it (keeping open/close balanced). Idempotent. */
PulpEmbedResult pulp_embed_notify_attached(PulpEmbedView* view);

/* Mode (A): attach the child view to parent_native_handle (NSView* on macOS)
 * and, on a confirmed attach, fire the view-opened lifecycle. If the attach
 * does not take (null/invalid parent), returns PULP_EMBED_ERR_ATTACH and does
 * NOT fire view-opened. Re-attaching an already-attached view is a no-op
 * returning PULP_EMBED_OK. */
PulpEmbedResult pulp_embed_attach(PulpEmbedView* view, void* parent_native_handle);

/* Detach the child view from its parent. Balances pulp_embed_attach and fires
 * view-closed if it had been opened. Safe (OK) if not attached. */
PulpEmbedResult pulp_embed_detach(PulpEmbedView* view);

/* Resize the embedded view. width/height are LOGICAL pixels.
 *
 * `scale` is validated (must be finite and > 0, else PULP_EMBED_ERR_INVALID_ARG)
 * but ADVISORY for this windowed/native-embed path: a Pulp-parented or
 * host-parented child surface derives its backing-store DPI from the host's
 * NSWindow (backingScaleFactor), which Pulp does not override here. Only the
 * deterministic capture APIs (pulp_embed_render_png / pulp_embed_render_frame_rgba)
 * honor a caller-supplied scale for pixel density. Pass the same width/height
 * with a new scale to signal a DPI change for hosts that treat it as a hint. */
PulpEmbedResult pulp_embed_resize(PulpEmbedView* view, int32_t width, int32_t height, float scale);

/* Pump one host idle tick so a scripted UI's poll()/timers/rAF keep running.
 * GPU hosts also drive their own display-link; CPU hosts treat this as a
 * repaint request. Call from the host's timer/idle callback. */
PulpEmbedResult pulp_embed_tick(PulpEmbedView* view);

/* Request a repaint (e.g. after the host changed a value the view reflects). */
PulpEmbedResult pulp_embed_repaint(PulpEmbedView* view);

/* Reload the embedded design in place (ABI v4). For the high-fidelity bundle
 * path, rebuilds the scripted UI from `bundle_dir`'s ui.js — or the CURRENT
 * bundle when `bundle_dir` is NULL — reusing the same native child view + GPU
 * surface and preserving widget values. The host keeps the same PulpEmbedView*,
 * the same attach/parent, the same host callbacks; the parameter list is rebuilt
 * by key (after a successful reload the host may re-enumerate pulp_embed_param_*).
 *
 * Last-good: the new code is validated before it replaces the live UI, so a
 * broken edit leaves the running design intact and this returns an error
 * (pulp_embed_last_error carries detail).
 *
 * Use it to drive an in-host "edit bundle -> reload" loop programmatically (the
 * complement to the PULP_EMBED_HOT_RELOAD file-watch dev flag). For dev editing
 * point the bundle at the importer's default absolute asset paths.
 *
 * Threading: must be called on the SAME thread that created the view (it rebuilds
 * views + touches the GPU surface); otherwise returns PULP_EMBED_ERR_WRONG_THREAD
 * and does nothing. Returns PULP_EMBED_ERR_UNSUPPORTED for a DesignIR/offscreen
 * view (only the scripted bundle path is reloadable). */
PulpEmbedResult pulp_embed_reload_bundle(PulpEmbedView* view, const char* bundle_dir);

/* Fill *out with the design's resize constraints (preferred/min/max/aspect). */
PulpEmbedResult pulp_embed_size_hints(PulpEmbedView* view, PulpEmbedSizeHints* out);

/* Active render backend after creation. Returns PulpEmbedBackend; a GPU
 * preference that failed to initialize reports PULP_EMBED_BACKEND_CPU. Returns
 * PULP_EMBED_BACKEND_UNKNOWN for a NULL/uninitialized view. */
int32_t pulp_embed_active_backend(PulpEmbedView* view);

/* ---- parameter bridge (ABI v2) --------------------------------------- *
 *
 * After creation, the view exposes an ORDERED, stable list of bindable
 * parameters discovered in the design (one per bindable control: knob, fader,
 * toggle). Each entry has a string KEY (the design's pulpParamKey when present,
 * else the control's widget id) and the widget id it drives. Index order is
 * stable for the lifetime of the view, so a host can enumerate once at create
 * time and address parameters by index internally if it prefers.
 *
 * Map design -> host like:
 *   int n = pulp_embed_param_count(view);
 *   for (int i = 0; i < n; ++i) {
 *       char key[128]; pulp_embed_param_key(view, i, key, sizeof key);
 *       host_bind(key);   // resolve key to a host parameter id
 *   }
 */

/* Number of bindable parameters discovered in the design (>= 0). */
int32_t pulp_embed_param_count(PulpEmbedView* view);

/* Copy the parameter's string key at `index` into buf (NUL-terminated,
 * truncated to cap). Returns the full key length excluding the NUL, or 0 for an
 * out-of-range index / NULL view. */
size_t pulp_embed_param_key(PulpEmbedView* view, int32_t index, char* buf, size_t cap);

/* Copy the widget id driven by the parameter at `index` (often identical to the
 * key). Same buffer/return contract as pulp_embed_param_key. */
size_t pulp_embed_param_widget_id(PulpEmbedView* view, int32_t index, char* buf, size_t cap);

/* Current NORMALIZED [0,1] value of the parameter at `index`, or a negative
 * value for an out-of-range index / NULL view. */
double pulp_embed_param_value(PulpEmbedView* view, int32_t index);

/* Richer parameter metadata (ABI v5) for the bindable control at `index`.
 *
 * Lets a host BUILD a correct host parameter from the design (greenfield/template
 * plugins) or VERIFY its own against the design (existing plugins — the binding
 * model keeps the plugin's params authoritative; this is advisory there). Fields:
 *   widget_kind   "knob" | "fader" | "toggle" | "dropdown" | "tab_group" |
 *                 "stepper" (the design control's real kind; NUL-terminated).
 *   is_discrete   1 for stepped/choice/toggle controls, 0 for continuous.
 *   option_count  number of discrete options for a choice control (dropdown /
 *                 tab_group / stepper), else 0. For a toggle, 2.
 *   default_norm  the imported default value, NORMALIZED [0,1].
 *   name, unit    display name / unit ("dB","Hz","%"). EMPTY until the importer
 *                 carries them; gate on has_meta, do not assume populated.
 *   has_range     1 iff min_value/max_value/step_count are meaningful (real
 *                 units imported); 0 = normalized-only (use [0,1]).
 *   min_value,
 *   max_value     denormalized range when has_range; else 0.
 *   step_count    discrete step count when known (0 = continuous / unknown).
 *   has_meta      1 iff name/unit were imported (so "" means "unset", not blank).
 *
 * Returns PULP_EMBED_OK and fills *out, or PULP_EMBED_ERR_INVALID_ARG for a NULL
 * view/out or out-of-range index (and zero-fills *out). */
typedef struct PulpEmbedParamInfo {
    char    widget_kind[16];
    int32_t is_discrete;
    int32_t option_count;
    double  default_norm;
    char    name[64];
    char    unit[16];
    int32_t has_range;
    double  min_value;
    double  max_value;
    int32_t step_count;
    int32_t has_meta;
} PulpEmbedParamInfo;

PulpEmbedResult pulp_embed_param_info(PulpEmbedView* view, int32_t index,
                                      PulpEmbedParamInfo* out);

/* Host -> view: push a NORMALIZED [0,1] value for the parameter identified by
 * `key` (host automation, preset recall, get_param sync). Updates the embed's
 * StateStore, sets the matching widget's value, and repaints. Does NOT call
 * back into host.set_param (no feedback loop). Returns PULP_EMBED_ERR_INVALID_ARG
 * for a NULL view/key, and PULP_EMBED_OK even if `key` matches no control (the
 * write is a no-op) so hosts can blind-push without first checking membership. */
PulpEmbedResult pulp_embed_param_changed(PulpEmbedView* view, const char* key, double normalized);

/* Headless input primitive (tests / automation): drive the control at `index`
 * to the NORMALIZED [0,1] target through its REAL interaction path — the same
 * gesture-begin -> value-change -> gesture-end sequence a mouse drag produces.
 * For a knob/fader this fires on_gesture_begin, one or more on_change, then
 * on_gesture_end (so the host sees begin_gesture / set_param(s) / end_gesture).
 * For a toggle it flips on/off when the target crosses 0.5. Returns
 * PULP_EMBED_ERR_INVALID_ARG for a NULL view or out-of-range index. This exists
 * so a headless host harness can prove the UI->host direction without a window
 * server; a real embedded UI is driven by the host's own mouse/touch events. */
PulpEmbedResult pulp_embed_simulate_param_drag(PulpEmbedView* view, int32_t index, double target_normalized);

/* Host -> view: tell the embed where the mouse pointer is, in ROOT-view
 * coordinates (top-left origin, logical pixels matching the bounds passed
 * to pulp_embed_resize). Triggers the same hover hit-test that drives CSS
 * :hover / `onMouseEnter` / `onMouseLeave` in non-embed Pulp windows. Host
 * adapters (pulp-embed-juce, future iPlug2/SDL wrappers) override their
 * platform mouseMove and forward each move here.
 *
 * Without this, `registerHover(id)` correctly arms the lambdas but no code
 * path ever calls `View::set_hovered(true)` in the embedded context, so the
 * JS `onMouseEnter` handler never fires. Symmetric counterpart to the
 * existing pulp_embed_simulate_* family but driven by the live host pointer,
 * not a test harness.
 *
 * Returns PULP_EMBED_OK on dispatch, PULP_EMBED_ERR_INVALID_ARG for a NULL
 * view. Coordinates outside the root bounds clear hover (matches Pulp's
 * own platform behaviour when the pointer leaves the window). */
PulpEmbedResult pulp_embed_dispatch_mouse_move(PulpEmbedView* view, double x, double y);

/* Host -> view: tell the embed the mouse pointer left the view entirely.
 * Equivalent to dispatching a mouse-move outside the root bounds — clears
 * any active hover state so `onMouseLeave` fires on the previously-hovered
 * widget. Host adapters call this from their platform mouseExit / when the
 * window loses focus. */
PulpEmbedResult pulp_embed_dispatch_mouse_exit(PulpEmbedView* view);

/* ---- text-field string bridge (ABI v6) ------------------------------- *
 *
 * Separate from the numeric param bridge: a design's text_field controls carry a
 * UTF-8 string (preset name, label, search text) bound to the HOST's own state —
 * saved/restored with the plugin, NOT a DAW-automatable parameter. Enumerated
 * like params: one entry per bindable text_field, keyed by its design key.
 *
 * Direction UI -> host (user types): the shim fires host.set_string(key, utf8).
 * Direction host -> view (preset recall): pulp_embed_set_string(view,key,utf8)
 * writes the field's text without echoing back through set_string (no loop). The
 * initial value is pulled once at create via host.get_string. */

/* Number of bindable text_field string controls discovered in the design. */
int32_t pulp_embed_string_param_count(PulpEmbedView* view);

/* Copy the string control's design key at `index` into buf (NUL-terminated,
 * truncated to cap). Returns the full key length excluding the NUL, or 0 for an
 * out-of-range index / NULL view. */
size_t pulp_embed_string_param_key(PulpEmbedView* view, int32_t index, char* buf, size_t cap);

/* Copy the current UTF-8 text of the string control identified by `key` into buf
 * (NUL-terminated, truncated to cap). Returns the full length excluding the NUL,
 * or 0 for an unknown key / NULL view. */
size_t pulp_embed_get_string(PulpEmbedView* view, const char* key, char* buf, size_t cap);

/* Host -> view: set the text of the string control identified by `key` (preset
 * recall / host state restore). Updates the field without re-entering
 * host.set_string. Returns PULP_EMBED_ERR_INVALID_ARG for a NULL view/key, and
 * PULP_EMBED_OK even if `key` matches no text_field (no-op), so a host can
 * blind-push. */
PulpEmbedResult pulp_embed_set_string(PulpEmbedView* view, const char* key, const char* utf8);

/* Headless input primitive (tests / automation): set the text_field at `index`
 * to `utf8` through its REAL edit path — fires host.set_string exactly as a user
 * typing would (unlike pulp_embed_set_string, which is the host->view push and
 * does NOT call back to the host). Returns PULP_EMBED_ERR_INVALID_ARG for a NULL
 * view or out-of-range index. */
PulpEmbedResult pulp_embed_simulate_text_input(PulpEmbedView* view, int32_t index, const char* utf8);

/* ---- capture (tests / thumbnails) ------------------------------------ */

/* Copy the current back buffer as PNG bytes into `out` (capacity `cap`).
 * Always writes the required size to *out_len. Two-call pattern: pass out=NULL
 * to size, then call again with cap >= *out_len. Returns
 * PULP_EMBED_ERR_BUFFER_TOO_SMALL if out != NULL but cap < required, and
 * PULP_EMBED_ERR_UNSUPPORTED on a CPU host (no back-buffer readback). */
PulpEmbedResult pulp_embed_capture_png(PulpEmbedView* view, uint8_t* out,
                                       size_t cap, size_t* out_len);

/* Deterministically rasterize the current view tree to a PNG at width x height
 * via Pulp's headless Skia renderer — no window, display-link, or live GPU
 * back buffer required. Same two-call sizing contract as pulp_embed_capture_png
 * (out=NULL sizes; cap<required -> BUFFER_TOO_SMALL). This is the reliable path
 * for thumbnails and tests; pulp_embed_capture_png reflects the live host
 * surface instead. */
PulpEmbedResult pulp_embed_render_png(PulpEmbedView* view, int32_t width,
                                      int32_t height, float scale,
                                      uint8_t* out, size_t cap, size_t* out_len);

/* ---- diagnostics ------------------------------------------------------ */

/* Missing-asset query (ABI v7). At create time the shim walks the DesignIR asset
 * manifest for the faithful-vector lane and records every frame asset (svg) whose
 * resolved local_path is absent on disk. A host preflight uses this to fail fast
 * with the offending paths, instead of re-parsing the JSON + manifest itself.
 * Only the faithful frame svg refs are flagged; non-faithful fallback rasters the
 * render never loads are intentionally NOT reported (no false positives). Bundle
 * (scripted) views resolve assets through the session, so this is always 0 there. */

/* Number of missing render assets recorded for this view (0 if none / NULL). */
int32_t pulp_embed_missing_asset_count(PulpEmbedView* view);

/* Copy the missing asset path at `index` into buf (NUL-terminated, truncated to
 * cap). Returns the full length excluding the NUL, or 0 for an out-of-range
 * index / NULL view. */
size_t pulp_embed_missing_asset(PulpEmbedView* view, int32_t index, char* buf, size_t cap);

/* Copy the handle's last error message into buf (NUL-terminated, truncated to
 * cap). Returns the full length excluding the NUL. view may be NULL (returns 0). */
size_t pulp_embed_last_error(PulpEmbedView* view, char* buf, size_t cap);

/* ---- teardown --------------------------------------------------------- */

/* Detach the surface, close the view bridge, and free everything. NULL-safe.
 * Call exactly once per handle. */
void pulp_embed_destroy(PulpEmbedView* view);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PULP_VIEW_EMBED_H */
