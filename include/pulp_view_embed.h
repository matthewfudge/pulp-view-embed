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
 * desc. abi_version is therefore validated as "<= library version", not "==". */
#define PULP_VIEW_EMBED_ABI_VERSION 2u

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
    PULP_EMBED_ERR_INTERNAL         = 9   /* unexpected; a C++ exception was caught  */
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

typedef struct PulpEmbedHostCallbacks {
    PulpEmbedSetParamFn   set_param;     /* UI gesture -> host param write      */
    PulpEmbedGetParamFn   get_param;     /* host -> view initial-value pull     */
    PulpEmbedGestureFn    begin_gesture; /* UI gesture begin (undo grouping)    */
    PulpEmbedGestureFn    end_gesture;   /* UI gesture end                      */
    PulpEmbedReadMetersFn read_meters;   /* polled from tick(); may be NULL     */
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

/* Resize the embedded view. width/height are logical pixels; scale is the DPI
 * scale (pass the same width/height with a new scale to signal a DPI change). */
PulpEmbedResult pulp_embed_resize(PulpEmbedView* view, int32_t width, int32_t height, float scale);

/* Pump one host idle tick so a scripted UI's poll()/timers/rAF keep running.
 * GPU hosts also drive their own display-link; CPU hosts treat this as a
 * repaint request. Call from the host's timer/idle callback. */
PulpEmbedResult pulp_embed_tick(PulpEmbedView* view);

/* Request a repaint (e.g. after the host changed a value the view reflects). */
PulpEmbedResult pulp_embed_repaint(PulpEmbedView* view);

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
