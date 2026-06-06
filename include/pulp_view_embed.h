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

/* Bump when the struct/function layout changes incompatibly. */
#define PULP_VIEW_EMBED_ABI_VERSION 1u

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
    void*       host_ctx;        /* opaque; reserved for future param/meter cbs */
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
