// macOS AppKit smoke for the pulp_view_embed C ABI. Exercises the M1.1–M1.5
// gates against a real NSWindow:
//   M1.1 create from synthetic DesignIR (detached tree)
//   M1.2 CPU attach: try_attach seam gate (failed attach never "opens")
//   M1.3 GPU attach + back-buffer PNG capture (GPU-only)
//   M1.4 Figma "VST Style" fixture renders
//   M1.5 100x create/attach/destroy teardown stress
//
// Exit 0 = all gates passed (or cleanly skipped where a GPU adapter / window
// server is unavailable). Non-zero = a gate failed.

#import <AppKit/AppKit.h>
#include "pulp_view_embed.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_failures;
}

std::string fixtures_dir() {
#ifdef PULP_EMBED_FIXTURES_DIR
    return PULP_EMBED_FIXTURES_DIR;
#else
    return "fixtures";
#endif
}

PulpEmbedDesc make_desc(int w, int h, PulpEmbedBackendPref pref) {
    PulpEmbedDesc d{};
    d.struct_size = sizeof(PulpEmbedDesc);
    d.abi_version = PULP_VIEW_EMBED_ABI_VERSION;
    d.logical_width = w;
    d.logical_height = h;
    d.scale_factor = 1.0f;
    d.backend_pref = pref;
    // Pin the design viewport to the imported design size so the fixed-size
    // tree gets aspect-correct bounds + paint scale (the documented imported-
    // design host gotcha) instead of rendering at native size off-surface.
    d.design_width = w;
    d.design_height = h;
    return d;
}

void pump(int frames) {
    // Run the run loop (not just dequeue events): the GPU host's CVDisplayLink
    // dispatches render work to the main queue, which runMode services.
    for (int i = 0; i < frames; ++i) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
}

// Deterministic render (Skia, no window) — the reliable content proof. Returns
// the PNG bytes and writes the file; empty on failure.
std::vector<uint8_t> render_to(PulpEmbedView* v, int w, int h, const char* path) {
    size_t need = 0;
    if (pulp_embed_render_png(v, w, h, 1.0f, nullptr, 0, &need) != PULP_EMBED_OK || !need)
        return {};
    std::vector<uint8_t> png(need);
    if (pulp_embed_render_png(v, w, h, 1.0f, png.data(), png.size(), &need) != PULP_EMBED_OK)
        return {};
    FILE* f = std::fopen(path, "wb");
    if (f) { std::fwrite(png.data(), 1, png.size(), f); std::fclose(f);
             std::printf("    wrote %s (%zu bytes)\n", path, need); }
    return png;
}

// Decode-free non-blank heuristic: a uniform image (e.g. all-white) compresses
// to far fewer bytes per pixel than one with real content. ~0.02 B/px is a
// generous floor that a blank frame falls under and a real UI clears.
bool looks_nonblank(const std::vector<uint8_t>& png, int w, int h) {
    if (png.size() < 8) return false;
    const double bytes_per_px = static_cast<double>(png.size()) / (double(w) * double(h));
    return bytes_per_px > 0.02;
}

NSWindow* make_offscreen_window(int w, int h) {
    // Position well off-screen but order it front so AppKit gives the view a
    // backing store and the GPU host's CVDisplayLink actually fires.
    NSRect frame = NSMakeRect(-20000, -20000, w, h);
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setReleasedWhenClosed:NO];
    [win orderFront:nil];
    return win;
}

const char* backend_name(int b) {
    switch (b) {
        case PULP_EMBED_BACKEND_GPU: return "GPU";
        case PULP_EMBED_BACKEND_CPU: return "CPU";
        default: return "UNKNOWN";
    }
}

// ── M1.8 host-callback recorder ──────────────────────────────────────────
// Captures the UI->host parameter traffic the embed forwards, so the gate can
// assert the bidirectional flow without a DAW.
struct HostParamLog {
    int begin_count = 0;
    int end_count = 0;
    int set_count = 0;
    std::string last_set_key;
    double last_set_value = -1.0;
    std::string last_begin_key;
    std::string last_end_key;
    // Host-side "automation state" keyed by param key (what a DAW would store).
    std::vector<std::pair<std::string, double>> store;

    double get(const std::string& key) const {
        for (auto& kv : store) if (kv.first == key) return kv.second;
        return -1.0;  // unknown
    }
    void put(const std::string& key, double v) {
        for (auto& kv : store) if (kv.first == key) { kv.second = v; return; }
        store.emplace_back(key, v);
    }
};

void host_set_param(void* ctx, const char* key, double v) {
    auto* log = static_cast<HostParamLog*>(ctx);
    ++log->set_count;
    log->last_set_key = key ? key : "";
    log->last_set_value = v;
    log->put(log->last_set_key, v);
}
double host_get_param(void* ctx, const char* key) {
    return static_cast<HostParamLog*>(ctx)->get(key ? key : "");
}
void host_begin_gesture(void* ctx, const char* key) {
    auto* log = static_cast<HostParamLog*>(ctx);
    ++log->begin_count;
    log->last_begin_key = key ? key : "";
}
void host_end_gesture(void* ctx, const char* key) {
    auto* log = static_cast<HostParamLog*>(ctx);
    ++log->end_count;
    log->last_end_key = key ? key : "";
}

// ── M1.9 resolve_resource recorder ───────────────────────────────────────
// Serves one design asset's bytes from memory; records which ids were asked
// for, so the gate can assert the host callback path drove asset loading.
struct ResourceServer {
    std::string serve_id;                 // the one id we answer
    std::vector<uint8_t> serve_bytes;     // bytes returned for serve_id
    std::vector<std::string> asked;       // every id the shim offered us
};

const uint8_t* host_resolve_resource(void* ctx, const char* id, size_t* out_len) {
    auto* rs = static_cast<ResourceServer*>(ctx);
    rs->asked.emplace_back(id ? id : "");
    if (id && rs->serve_id == id && !rs->serve_bytes.empty()) {
        if (out_len) *out_len = rs->serve_bytes.size();
        return rs->serve_bytes.data();   // borrowed; valid through creation
    }
    return nullptr;                       // disk fallback
}

std::vector<uint8_t> read_file_bytes(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> bytes(n > 0 ? static_cast<size_t>(n) : 0);
    if (!bytes.empty()) {
        size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
        bytes.resize(got);
    }
    std::fclose(f);
    return bytes;
}

// First setImageSource(...,'<path>') path argument in a ui.js — used as the
// asset id the host serves.
std::string first_image_asset_id(const std::string& ui_js) {
    const char* needle = "setImageSource(";
    size_t p = ui_js.find(needle);
    if (p == std::string::npos) return {};
    // skip to the comma after the id, then the opening quote of the path.
    size_t comma = ui_js.find(',', p);
    if (comma == std::string::npos) return {};
    size_t q1 = ui_js.find_first_of("'\"", comma);
    if (q1 == std::string::npos) return {};
    char quote = ui_js[q1];
    size_t q2 = ui_js.find(quote, q1 + 1);
    if (q2 == std::string::npos) return {};
    return ui_js.substr(q1 + 1, q2 - q1 - 1);
}

// Pull a deterministic RGBA frame. Returns pixels; writes w/h/stride. Works for
// both windowed and offscreen views (deterministic renderer, not a back buffer).
std::vector<uint8_t> pull_rgba(PulpEmbedView* v, int w, int h, float scale,
                               int* ow, int* oh, int* ostride) {
    int pw = 0, ph = 0, stride = 0;
    if (pulp_embed_render_frame_rgba(v, w, h, scale, nullptr, 0, &pw, &ph, &stride)
            != PULP_EMBED_OK || pw <= 0 || ph <= 0)
        return {};
    std::vector<uint8_t> px(static_cast<size_t>(stride) * static_cast<size_t>(ph));
    if (pulp_embed_render_frame_rgba(v, w, h, scale, px.data(), px.size(),
                                     &pw, &ph, &stride) != PULP_EMBED_OK)
        return {};
    if (ow) *ow = pw; if (oh) *oh = ph; if (ostride) *ostride = stride;
    return px;
}

// Fraction of pixels that differ by more than a small per-channel tolerance.
double rgba_diff_fraction(const std::vector<uint8_t>& a,
                          const std::vector<uint8_t>& b) {
    if (a.size() != b.size() || a.empty()) return 1.0;
    size_t differing = 0;
    const size_t pixels = a.size() / 4;
    for (size_t i = 0; i < pixels; ++i) {
        int da = std::abs(int(a[i*4+0]) - int(b[i*4+0]));
        int dg = std::abs(int(a[i*4+1]) - int(b[i*4+1]));
        int db = std::abs(int(a[i*4+2]) - int(b[i*4+2]));
        int dal = std::abs(int(a[i*4+3]) - int(b[i*4+3]));
        if (da > 4 || dg > 4 || db > 4 || dal > 4) ++differing;
    }
    return double(differing) / double(pixels);
}

// M1.1 — create from a DesignIR JSON, no attach. Returns handle or null.
PulpEmbedView* create_from(const std::string& path, PulpEmbedBackendPref pref,
                           int w, int h) {
    PulpEmbedDesc d = make_desc(w, h, pref);
    PulpEmbedView* v = nullptr;
    PulpEmbedResult r = pulp_embed_create_from_design_json(&d, path.c_str(), &v);
    if (r != PULP_EMBED_OK) {
        char buf[512];
        pulp_embed_last_create_error(buf, sizeof(buf));
        std::printf("    create(%s) failed: result=%d err=%s\n", path.c_str(), r, buf);
    }
    return v;
}

}  // namespace

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        const std::string fx = fixtures_dir();
        const std::string synthetic = fx + "/synthetic/design.ir.json";
        const std::string figma = fx + "/figma-vst-style/design.ir.json";

        std::printf("== pulp_view_embed smoke (ABI v%u) ==\n", pulp_embed_abi_version());
        check(pulp_embed_abi_version() == PULP_VIEW_EMBED_ABI_VERSION, "abi_version matches header");

        // ── M1.1: create synthetic, detached tree ────────────────────────
        std::printf("-- M1.1 create (synthetic) --\n");
        PulpEmbedView* v = create_from(synthetic, PULP_EMBED_BACKEND_PREF_AUTO, 320, 200);
        check(v != nullptr, "create_from_design_json(synthetic) succeeds");
        if (v) {
            PulpEmbedSizeHints hints{};
            check(pulp_embed_size_hints(v, &hints) == PULP_EMBED_OK, "size_hints OK");
            check(hints.preferred_width == 320 && hints.preferred_height == 200,
                  "size_hints preferred matches desc");
            std::printf("    active backend: %s\n",
                        backend_name(pulp_embed_active_backend(v)));
        }

        // ── M1.2: attach gate ────────────────────────────────────────────
        std::printf("-- M1.2 attach gate --\n");
        if (v) {
            // A null parent must NOT report attached (the try_attach seam).
            check(pulp_embed_attach(v, nullptr) == PULP_EMBED_ERR_ATTACH,
                  "attach(null) -> ERR_ATTACH (never opens)");

            NSWindow* win = make_offscreen_window(320, 200);
            check(pulp_embed_attach(v, (__bridge void*)win.contentView) == PULP_EMBED_OK,
                  "attach(real NSView) -> OK");
            // Idempotent re-attach.
            check(pulp_embed_attach(v, (__bridge void*)win.contentView) == PULP_EMBED_OK,
                  "re-attach is idempotent OK");
            check(pulp_embed_resize(v, 360, 220, 1.0f) == PULP_EMBED_OK, "resize OK");
            check(pulp_embed_tick(v) == PULP_EMBED_OK, "tick OK");

            pump(15);  // let the GPU display-link draw a few frames

            // ── M1.3: GPU capture (GPU-only) ─────────────────────────────
            std::printf("-- M1.3 capture --\n");
            int backend = pulp_embed_active_backend(v);
            size_t need = 0;
            PulpEmbedResult cap = pulp_embed_capture_png(v, nullptr, 0, &need);
            if (backend == PULP_EMBED_BACKEND_GPU) {
                check(cap == PULP_EMBED_OK && need > 0, "GPU capture reports size");
                std::vector<uint8_t> png(need);
                check(pulp_embed_capture_png(v, png.data(), png.size(), &need) == PULP_EMBED_OK,
                      "GPU capture fills buffer");
                bool is_png = png.size() > 8 && png[0] == 0x89 && png[1] == 'P' &&
                              png[2] == 'N' && png[3] == 'G';
                check(is_png, "captured bytes are a PNG");
                // buffer-too-small contract
                size_t need2 = 0;
                check(pulp_embed_capture_png(v, png.data(), 4, &need2) ==
                          PULP_EMBED_ERR_BUFFER_TOO_SMALL,
                      "capture into tiny buffer -> BUFFER_TOO_SMALL");
            } else {
                check(cap == PULP_EMBED_ERR_UNSUPPORTED, "CPU capture -> UNSUPPORTED");
            }

            auto spng = render_to(v, 320, 200, "/tmp/pulp-embed-synthetic.png");
            check(!spng.empty(), "deterministic render_png(synthetic) non-empty");
            check(looks_nonblank(spng, 320, 200), "synthetic render is non-blank");
            [win close];
            pulp_embed_destroy(v);
        }

        // ── M1.4: Figma fixture renders ──────────────────────────────────
        std::printf("-- M1.4 figma fixture --\n");
        PulpEmbedView* fv = create_from(figma, PULP_EMBED_BACKEND_PREF_AUTO, 1000, 600);
        check(fv != nullptr, "create_from_design_json(figma VST Style) succeeds");
        if (fv) {
            NSWindow* fwin = make_offscreen_window(1000, 600);
            check(pulp_embed_attach(fv, (__bridge void*)fwin.contentView) == PULP_EMBED_OK,
                  "figma attach OK");
            pump(10);
            auto fpng = render_to(fv, 1000, 600, "/tmp/pulp-embed-figma.png");
            check(!fpng.empty(), "deterministic render_png(figma) non-empty");
            check(looks_nonblank(fpng, 1000, 600), "figma VST Style render is non-blank");

            // LIVE GPU back-buffer capture of the SAME figma view tree. This
            // exercises render_frame -> paint_scene -> read_current_rgba, the
            // path that was blank before the flush-before-readback fix.
            if (pulp_embed_active_backend(fv) == PULP_EMBED_BACKEND_GPU) {
                size_t lneed = 0;
                if (pulp_embed_capture_png(fv, nullptr, 0, &lneed) == PULP_EMBED_OK && lneed) {
                    std::vector<uint8_t> lpng(lneed);
                    if (pulp_embed_capture_png(fv, lpng.data(), lpng.size(), &lneed) == PULP_EMBED_OK) {
                        FILE* lf = std::fopen("/tmp/live-fixed.png", "wb");
                        if (lf) { std::fwrite(lpng.data(), 1, lpng.size(), lf); std::fclose(lf);
                                  std::printf("    wrote /tmp/live-fixed.png (%zu bytes)\n", lneed); }
                        check(looks_nonblank(lpng, 1000, 600),
                              "LIVE GPU capture(figma) is non-blank");
                    }
                }
            }
            [fwin close];
            pulp_embed_destroy(fv);
        }

        // ── M1.5: teardown stress ────────────────────────────────────────
        std::printf("-- M1.5 100x create/attach/destroy --\n");
        int ok = 0;
        for (int i = 0; i < 100; ++i) {
            PulpEmbedView* lv = create_from(synthetic, PULP_EMBED_BACKEND_PREF_AUTO, 320, 200);
            if (!lv) continue;
            NSWindow* lw = make_offscreen_window(320, 200);
            if (pulp_embed_attach(lv, (__bridge void*)lw.contentView) == PULP_EMBED_OK) ++ok;
            [lw close];
            pulp_embed_destroy(lv);
        }
        check(ok == 100, "100/100 create+attach+destroy cycles clean");

        // ── M1.6: CPU backend runs (GPU is the goal; CPU must still work) ──
        std::printf("-- M1.6 CPU backend --\n");
        PulpEmbedView* cv = create_from(figma, PULP_EMBED_BACKEND_PREF_CPU, 1000, 600);
        check(cv != nullptr, "create(CPU pref) succeeds");
        if (cv) {
            check(pulp_embed_active_backend(cv) == PULP_EMBED_BACKEND_CPU,
                  "active backend is CPU when CPU requested");
            NSWindow* cwin = make_offscreen_window(1000, 600);
            check(pulp_embed_attach(cv, (__bridge void*)cwin.contentView) == PULP_EMBED_OK,
                  "CPU attach -> OK");
            check(pulp_embed_resize(cv, 1000, 600, 1.0f) == PULP_EMBED_OK, "CPU resize OK");
            check(pulp_embed_tick(cv) == PULP_EMBED_OK, "CPU tick OK");
            // CPU CoreGraphics host has no back-buffer readback: capture is
            // UNSUPPORTED, but the deterministic Skia render still works.
            size_t cneed = 0;
            check(pulp_embed_capture_png(cv, nullptr, 0, &cneed) == PULP_EMBED_ERR_UNSUPPORTED,
                  "CPU live capture -> UNSUPPORTED (no readback)");
            auto cpng = render_to(cv, 1000, 600, "/tmp/pulp-embed-cpu-render.png");
            check(looks_nonblank(cpng, 1000, 600), "CPU deterministic render is non-blank");
            [cwin close];
            pulp_embed_destroy(cv);
        }

        // ── M1.7: high-fidelity scripted-UI bundle path ──────────────────
        // create_from_ui_bundle renders the importer's `--emit js` output
        // through the SAME scripted-UI pipeline the importer's own --validate
        // render uses, so the embed reproduces the importer render (rasterized
        // 3D shapes, skeuo knobs, light glass panels) instead of the flattened
        // native-widget fallback. The fixture bundle uses bundle-RELATIVE asset
        // paths to prove the loader's relative-path resolution.
        std::printf("-- M1.7 hi-fi scripted bundle --\n");
        const std::string bundle = fx + "/figma-vst-style/bundle";
        {
            PulpEmbedDesc bd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
            PulpEmbedView* bv = nullptr;
            PulpEmbedResult br = pulp_embed_create_from_ui_bundle(&bd, bundle.c_str(), &bv);
            if (br != PULP_EMBED_OK) {
                char buf[512];
                pulp_embed_last_create_error(buf, sizeof(buf));
                std::printf("    create_from_ui_bundle failed: result=%d err=%s\n", br, buf);
            }
            check(br == PULP_EMBED_OK && bv != nullptr, "create_from_ui_bundle succeeds");
            if (bv) {
                NSWindow* bwin = make_offscreen_window(1000, 600);
                check(pulp_embed_attach(bv, (__bridge void*)bwin.contentView) == PULP_EMBED_OK,
                      "bundle attach OK");
                pump(15);
                // Deterministic render: a faithful scripted render of this
                // asset-rich design carries far more entropy than the sparse
                // native fallback (M1.4 lands ~0.07 B/px). The scripted render
                // with all rasterized images is well above 0.10 B/px.
                auto bpng = render_to(bv, 1000, 600, "/tmp/embed-fidelity.png");
                check(!bpng.empty(), "bundle render_png non-empty");
                check(looks_nonblank(bpng, 1000, 600), "bundle render is non-blank");
                const double bpp = bpng.empty() ? 0.0
                    : static_cast<double>(bpng.size()) / (1000.0 * 600.0);
                std::printf("    bundle render entropy: %.3f B/px\n", bpp);
                check(bpp > 0.10,
                      "bundle render carries rasterized content (>0.10 B/px)");
                [bwin close];
                pulp_embed_destroy(bv);
            }
        }

        // ── M1.8: interactive parameter bridge (ABI v2) ──────────────────
        // Prove BIDIRECTIONAL host<->view param flow on the hi-fi bundle:
        //   (1) enumerate params (count>0, keys match the design's controls),
        //   (2) a simulated knob drag fires host begin/set/end with a changed
        //       value (UI -> host),
        //   (3) pulp_embed_param_changed pushes a host value into the widget
        //       and the widget's value updates (host -> view),
        //   (4) the host-driven push does NOT echo back to host.set_param.
        std::printf("-- M1.8 parameter bridge (UI<->host) --\n");
        {
            HostParamLog log;
            PulpEmbedDesc pd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
            pd.host_ctx = &log;
            pd.host.set_param = host_set_param;
            pd.host.get_param = host_get_param;
            pd.host.begin_gesture = host_begin_gesture;
            pd.host.end_gesture = host_end_gesture;

            PulpEmbedView* pv = nullptr;
            PulpEmbedResult pr =
                pulp_embed_create_from_ui_bundle(&pd, bundle.c_str(), &pv);
            if (pr != PULP_EMBED_OK) {
                char buf[512];
                pulp_embed_last_create_error(buf, sizeof(buf));
                std::printf("    create(with host cb) failed: result=%d err=%s\n", pr, buf);
            }
            check(pr == PULP_EMBED_OK && pv != nullptr,
                  "create_from_ui_bundle with host callbacks succeeds");

            if (pv) {
                // (1) enumeration
                int n = pulp_embed_param_count(pv);
                std::printf("    param_count = %d\n", n);
                check(n > 0, "param_count > 0 (controls discovered)");

                // Keys must be non-empty and match the createKnob ids in ui.js.
                bool found_known_knob = false;
                bool all_keys_nonempty = (n > 0);
                int found_idx = -1;
                for (int i = 0; i < n; ++i) {
                    char key[128] = {0};
                    size_t kl = pulp_embed_param_key(pv, i, key, sizeof key);
                    if (kl == 0) all_keys_nonempty = false;
                    if (std::string(key) == "Knob_Small47") {
                        found_known_knob = true;
                        found_idx = i;
                    }
                }
                check(all_keys_nonempty, "every param key is non-empty");
                check(found_known_knob,
                      "a known design control key (Knob_Small47) is enumerated");

                if (found_idx >= 0) {
                    char widget_id[128] = {0};
                    pulp_embed_param_widget_id(pv, found_idx, widget_id, sizeof widget_id);
                    check(std::string(widget_id) == "Knob_Small47",
                          "param widget_id matches the design control id");

                    double before = pulp_embed_param_value(pv, found_idx);
                    check(before >= 0.0 && before <= 1.0,
                          "param value is a valid normalized number");

                    // (2) UI -> host: simulate a drag to a value clearly
                    // different from the seed, then assert the host saw a
                    // begin / set(changed) / end sequence.
                    double target = (before < 0.5) ? 0.85 : 0.15;
                    int begin0 = log.begin_count, set0 = log.set_count, end0 = log.end_count;
                    check(pulp_embed_simulate_param_drag(pv, found_idx, target) == PULP_EMBED_OK,
                          "simulate_param_drag OK");
                    check(log.begin_count == begin0 + 1, "host begin_gesture fired once");
                    check(log.end_count == end0 + 1, "host end_gesture fired once");
                    check(log.set_count > set0, "host set_param fired (>=1)");
                    check(log.last_begin_key == "Knob_Small47" &&
                          log.last_end_key == "Knob_Small47" &&
                          log.last_set_key == "Knob_Small47",
                          "host callbacks carried the correct param key");
                    double after = pulp_embed_param_value(pv, found_idx);
                    std::printf("    drag %.3f -> %.3f (host set=%.3f)\n",
                                before, after, log.last_set_value);
                    check(std::abs(after - before) > 0.05,
                          "param value changed after drag (UI->host moved it)");
                    check(std::abs(log.last_set_value - after) < 1e-3,
                          "host set_param value matches the new param value");

                    // (3) host -> view: push a host value and assert the
                    // embedded widget picked it up.
                    double pushed = (after < 0.5) ? 0.90 : 0.10;
                    int set_before_push = log.set_count;
                    check(pulp_embed_param_changed(pv, "Knob_Small47", pushed) == PULP_EMBED_OK,
                          "param_changed(host->view) OK");
                    double now = pulp_embed_param_value(pv, found_idx);
                    std::printf("    host pushed %.3f -> widget value %.3f\n", pushed, now);
                    check(std::abs(now - pushed) < 1e-3,
                          "widget value reflects the host-pushed value");

                    // (4) the host push must NOT re-enter host.set_param.
                    check(log.set_count == set_before_push,
                          "host->view push did NOT echo back to host.set_param (no loop)");

                    // Re-render and confirm the host-driven value is visible:
                    // the deterministic Skia render reflects the widget's value
                    // (non-blank, content-bearing).
                    auto rpng = render_to(pv, 1000, 600, "/tmp/embed-param-bridge.png");
                    check(looks_nonblank(rpng, 1000, 600),
                          "render after host param push is non-blank");

                    // Visual proof the host->view push actually MOVES the knob:
                    // push the opposite extreme and assert the rendered bytes
                    // differ (the rotated indicator changes the PNG). Render is
                    // deterministic, so any difference is the param change.
                    double pushed2 = (pushed > 0.5) ? 0.05 : 0.95;
                    pulp_embed_param_changed(pv, "Knob_Small47", pushed2);
                    auto rpng2 = render_to(pv, 1000, 600, "/tmp/embed-param-bridge-2.png");
                    check(!rpng2.empty() && rpng2 != rpng,
                          "render visibly changes when the host moves the knob");

                    // Unknown-key push is a tolerated no-op.
                    check(pulp_embed_param_changed(pv, "no_such_param", 0.5) == PULP_EMBED_OK,
                          "param_changed(unknown key) is a tolerated no-op");
                }

                pulp_embed_destroy(pv);
            }
        }

        // ── M1.9: resolve_resource host callback (ABI v3) ────────────────
        // Serve ONE asset's bytes via the host callback and assert the same
        // pixels render as the on-disk path; then serve DIFFERENT bytes for
        // that asset and assert the render changes (the host path is really
        // driving the asset load, not disk).
        std::printf("-- M1.9 resolve_resource (host asset bytes) --\n");
        {
            const std::string ui_path = bundle + "/ui.js";
            std::vector<uint8_t> ui_bytes = read_file_bytes(ui_path);
            const std::string ui_js(
                reinterpret_cast<const char*>(ui_bytes.data()), ui_bytes.size());
            const std::string asset_id = first_image_asset_id(ui_js);
            check(!asset_id.empty(), "found an image asset id in ui.js");
            // The on-disk bytes for that asset (id is bundle-relative).
            const std::string asset_disk = bundle + "/" + asset_id;
            std::vector<uint8_t> real_bytes = read_file_bytes(asset_disk);
            check(!real_bytes.empty(), "read the asset's on-disk bytes");

            // Baseline: pure disk render (no resource callback).
            int dw = 0, dh = 0, ds = 0;
            std::vector<uint8_t> disk_px;
            {
                PulpEmbedDesc bd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
                PulpEmbedView* dv = nullptr;
                check(pulp_embed_create_from_ui_bundle(&bd, bundle.c_str(), &dv) == PULP_EMBED_OK,
                      "disk-baseline create OK");
                if (dv) {
                    disk_px = pull_rgba(dv, 1000, 600, 1.0f, &dw, &dh, &ds);
                    check(!disk_px.empty(), "disk-baseline RGBA frame non-empty");
                    pulp_embed_destroy(dv);
                }
            }

            // Host serves the SAME bytes the disk has -> render must match.
            if (!asset_id.empty() && !real_bytes.empty() && !disk_px.empty()) {
                ResourceServer rs;
                rs.serve_id = asset_id;
                rs.serve_bytes = real_bytes;
                PulpEmbedDesc hd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
                hd.host_ctx = &rs;
                hd.host.resolve_resource = host_resolve_resource;
                PulpEmbedView* hv = nullptr;
                check(pulp_embed_create_from_ui_bundle(&hd, bundle.c_str(), &hv) == PULP_EMBED_OK,
                      "create with resolve_resource OK");
                if (hv) {
                    check(!rs.asked.empty(), "shim consulted resolve_resource (>=1 id)");
                    bool offered = false;
                    for (auto& a : rs.asked) if (a == asset_id) offered = true;
                    check(offered, "shim offered the known asset id to the host");

                    int hw = 0, hh = 0, hs = 0;
                    std::vector<uint8_t> host_px = pull_rgba(hv, 1000, 600, 1.0f, &hw, &hh, &hs);
                    check(!host_px.empty(), "host-served RGBA frame non-empty");
                    double same_diff = rgba_diff_fraction(disk_px, host_px);
                    std::printf("    host(same-bytes) vs disk diff = %.5f\n", same_diff);
                    check(same_diff < 0.001,
                          "host-served-same-bytes render matches the disk render");
                    pulp_embed_destroy(hv);
                }

                // Negative control: serve a DIFFERENT image (a tiny solid PNG)
                // for that asset and assert the render visibly differs.
                // Reuse a different asset's bytes from the bundle as the "wrong"
                // image so we know it decodes.
                std::string other_id;
                {
                    size_t p2 = ui_js.find("setImageSource(", ui_js.find("setImageSource(") + 1);
                    if (p2 != std::string::npos) {
                        std::string rest = ui_js.substr(p2);
                        other_id = first_image_asset_id(rest);
                    }
                }
                std::vector<uint8_t> other_bytes =
                    other_id.empty() ? std::vector<uint8_t>() : read_file_bytes(bundle + "/" + other_id);
                if (!other_bytes.empty() && other_id != asset_id) {
                    ResourceServer rs2;
                    rs2.serve_id = asset_id;        // same slot…
                    rs2.serve_bytes = other_bytes;  // …different image
                    PulpEmbedDesc nd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
                    nd.host_ctx = &rs2;
                    nd.host.resolve_resource = host_resolve_resource;
                    PulpEmbedView* nv = nullptr;
                    if (pulp_embed_create_from_ui_bundle(&nd, bundle.c_str(), &nv) == PULP_EMBED_OK && nv) {
                        int nw = 0, nh = 0, ns = 0;
                        std::vector<uint8_t> npx = pull_rgba(nv, 1000, 600, 1.0f, &nw, &nh, &ns);
                        double diff = rgba_diff_fraction(disk_px, npx);
                        std::printf("    host(other-bytes) vs disk diff = %.5f\n", diff);
                        check(diff > 0.0005,
                              "host serving DIFFERENT bytes changes the render (callback drives load)");
                        pulp_embed_destroy(nv);
                    }
                }
            }

            // DesignIR path: resolve_resource is wired there too (assets key on
            // the manifest local_path). Prove the shim consults the host for the
            // native-tree create path, not just the scripted bundle.
            {
                ResourceServer rs;
                rs.serve_id = "__none__";  // serve nothing -> pure disk fallback
                PulpEmbedDesc dd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
                dd.host_ctx = &rs;
                dd.host.resolve_resource = host_resolve_resource;
                PulpEmbedView* dv2 = nullptr;
                if (pulp_embed_create_from_design_json(&dd, figma.c_str(), &dv2) == PULP_EMBED_OK && dv2) {
                    check(!rs.asked.empty(),
                          "DesignIR path consults resolve_resource for its assets");
                    pulp_embed_destroy(dv2);
                }
            }
        }

        // ── M1.10: offscreen / texture render mode (ABI v3) ──────────────
        // Create offscreen from the figma bundle (no parent window), pull a
        // CPU-readable RGBA frame, assert it's non-blank, and assert it matches
        // the WINDOWED render of the same bundle (deterministic renderer).
        std::printf("-- M1.10 offscreen render mode --\n");
        {
            PulpEmbedDesc od = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
            PulpEmbedView* ov = nullptr;
            PulpEmbedResult orr =
                pulp_embed_create_offscreen(&od, bundle.c_str(), /*from_bundle=*/1, &ov);
            if (orr != PULP_EMBED_OK) {
                char buf[512];
                pulp_embed_last_create_error(buf, sizeof(buf));
                std::printf("    create_offscreen failed: result=%d err=%s\n", orr, buf);
            }
            check(orr == PULP_EMBED_OK && ov != nullptr, "create_offscreen(bundle) succeeds");

            if (ov) {
                // No live host: offscreen reports CPU and refuses attach.
                check(pulp_embed_active_backend(ov) == PULP_EMBED_BACKEND_CPU,
                      "offscreen view reports CPU backend (no live host)");
                check(pulp_embed_attach(ov, nullptr) == PULP_EMBED_ERR_INVALID_ARG,
                      "offscreen attach is rejected (no host)");

                // Sizing query first.
                int qw = 0, qh = 0, qs = 0;
                check(pulp_embed_render_frame_rgba(ov, 1000, 600, 1.0f, nullptr, 0,
                                                   &qw, &qh, &qs) == PULP_EMBED_OK,
                      "render_frame_rgba sizing query OK");
                check(qw == 1000 && qh == 600 && qs == 1000 * 4,
                      "offscreen frame dims + stride correct (RGBA8, scale 1)");

                // Buffer-too-small contract.
                uint8_t tiny[16];
                int tw = 0, th = 0, ts = 0;
                check(pulp_embed_render_frame_rgba(ov, 1000, 600, 1.0f, tiny, sizeof tiny,
                                                   &tw, &th, &ts) == PULP_EMBED_ERR_BUFFER_TOO_SMALL,
                      "render_frame_rgba into tiny buffer -> BUFFER_TOO_SMALL");

                int ow = 0, oh = 0, os = 0;
                std::vector<uint8_t> off_px = pull_rgba(ov, 1000, 600, 1.0f, &ow, &oh, &os);
                check(!off_px.empty(), "offscreen RGBA frame non-empty");
                // Non-blank: a real frame has many distinct pixels vs a flat fill.
                {
                    size_t distinct = 0;
                    if (off_px.size() >= 4) {
                        const uint8_t* p0 = off_px.data();
                        for (size_t i = 4; i < off_px.size(); i += 4)
                            if (off_px[i] != p0[0] || off_px[i+1] != p0[1] ||
                                off_px[i+2] != p0[2]) { ++distinct; }
                    }
                    check(distinct > 1000, "offscreen frame is non-blank (varied pixels)");
                }

                // Matches the WINDOWED render of the same bundle.
                std::vector<uint8_t> win_px;
                {
                    PulpEmbedDesc wd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
                    PulpEmbedView* wv = nullptr;
                    if (pulp_embed_create_from_ui_bundle(&wd, bundle.c_str(), &wv) == PULP_EMBED_OK && wv) {
                        NSWindow* wwin = make_offscreen_window(1000, 600);
                        pulp_embed_attach(wv, (__bridge void*)wwin.contentView);
                        pump(10);
                        int wpw = 0, wph = 0, wps = 0;
                        win_px = pull_rgba(wv, 1000, 600, 1.0f, &wpw, &wph, &wps);
                        [wwin close];
                        pulp_embed_destroy(wv);
                    }
                }
                check(!win_px.empty(), "windowed reference RGBA frame non-empty");
                if (!win_px.empty() && !off_px.empty()) {
                    double diff = rgba_diff_fraction(win_px, off_px);
                    std::printf("    offscreen vs windowed diff = %.5f\n", diff);
                    check(diff < 0.001,
                          "offscreen frame matches the windowed render (pixel diff small)");
                }

                pulp_embed_destroy(ov);
            }
        }

        // ── M1.11: resize / scale / DPI stress ───────────────────────────
        // Attach the hi-fi figma bundle to a live window, then drive MANY
        // resize cycles across a wide size range, several non-design aspect
        // ratios, and DPI scale changes (1.0, 2.0). Each cycle ticks + renders
        // deterministically and at the live size, asserting: no crash, a valid
        // result code, and a non-blank render at every size. Loops 50+ cycles
        // so a leak or a per-resize surface bug surfaces as a crash/blank.
        std::printf("-- M1.11 resize / scale / DPI stress --\n");
        {
            PulpEmbedDesc rd = make_desc(1000, 600, PULP_EMBED_BACKEND_PREF_AUTO);
            PulpEmbedView* rv = nullptr;
            PulpEmbedResult rr =
                pulp_embed_create_from_ui_bundle(&rd, bundle.c_str(), &rv);
            check(rr == PULP_EMBED_OK && rv != nullptr,
                  "resize-stress create_from_ui_bundle succeeds");
            if (rv) {
                NSWindow* rwin = make_offscreen_window(1000, 600);
                check(pulp_embed_attach(rv, (__bridge void*)rwin.contentView) == PULP_EMBED_OK,
                      "resize-stress attach OK");

                // A spread of sizes: design size, small floor, large ceiling,
                // and several deliberately NON-design aspect ratios (ultra-wide,
                // tall, square) that the design-viewport letterbox must absorb
                // without crashing or blanking.
                struct Sz { int w, h; };
                const Sz sizes[] = {
                    {1000, 600},  // design
                    {200, 120},   // small floor
                    {1600, 960},  // large, design aspect
                    {1600, 200},  // ultra-wide (non-design aspect)
                    {300, 900},   // tall (non-design aspect)
                    {640, 640},   // square (non-design aspect)
                    {1280, 720},  // 16:9
                    {800, 600},   // 4:3
                };
                const float scales[] = {1.0f, 2.0f};
                const int n_sizes = int(sizeof(sizes) / sizeof(sizes[0]));

                int cycles = 0, resize_ok = 0, nonblank = 0, render_ok = 0;
                int blank_at_size = -1;
                // 8 sizes * 2 scales = 16 per pass; >=4 passes -> 64 cycles.
                for (int pass = 0; pass < 4; ++pass) {
                    for (int s = 0; s < n_sizes; ++s) {
                        for (float scale : scales) {
                            ++cycles;
                            const int w = sizes[s].w, h = sizes[s].h;
                            if (pulp_embed_resize(rv, w, h, scale) == PULP_EMBED_OK)
                                ++resize_ok;
                            // size_hints must still answer post-resize.
                            PulpEmbedSizeHints sh{};
                            pulp_embed_size_hints(rv, &sh);
                            pulp_embed_tick(rv);
                            pump(1);  // service the display-link once

                            // Deterministic render at the resized logical size
                            // (honors scale for pixel density). Assert non-blank.
                            size_t need = 0;
                            const int pw = int(w * scale), ph = int(h * scale);
                            if (pulp_embed_render_png(rv, w, h, scale, nullptr, 0, &need)
                                    == PULP_EMBED_OK && need) {
                                std::vector<uint8_t> png(need);
                                if (pulp_embed_render_png(rv, w, h, scale, png.data(),
                                                          png.size(), &need) == PULP_EMBED_OK) {
                                    ++render_ok;
                                    if (looks_nonblank(png, pw, ph)) ++nonblank;
                                    else if (blank_at_size < 0) blank_at_size = s;
                                }
                            }
                        }
                    }
                }
                std::printf("    cycles=%d resize_ok=%d render_ok=%d nonblank=%d\n",
                            cycles, resize_ok, render_ok, nonblank);
                check(cycles >= 50, "ran 50+ resize cycles");
                check(resize_ok == cycles, "every resize returned OK");
                check(render_ok == cycles, "every resized render produced a PNG");
                check(nonblank == cycles, "every resized render is non-blank");
                if (blank_at_size >= 0)
                    std::printf("    first blank at size index %d (%dx%d)\n",
                                blank_at_size, sizes[blank_at_size].w,
                                sizes[blank_at_size].h);

                // Invalid scale must be rejected, not silently dropped.
                check(pulp_embed_resize(rv, 800, 600, 0.0f) == PULP_EMBED_ERR_INVALID_ARG,
                      "resize(scale=0) -> INVALID_ARG");
                check(pulp_embed_resize(rv, 800, 600, -1.0f) == PULP_EMBED_ERR_INVALID_ARG,
                      "resize(scale<0) -> INVALID_ARG");
                check(pulp_embed_resize(rv, 800, 600, std::nanf("")) == PULP_EMBED_ERR_INVALID_ARG,
                      "resize(scale=NaN) -> INVALID_ARG");
                // Degenerate dimensions still rejected.
                check(pulp_embed_resize(rv, 0, 600, 1.0f) == PULP_EMBED_ERR_INVALID_ARG,
                      "resize(width=0) -> INVALID_ARG");

                // Settle back to design size and prove the view still renders.
                check(pulp_embed_resize(rv, 1000, 600, 1.0f) == PULP_EMBED_OK,
                      "resize back to design size OK");
                auto settled = render_to(rv, 1000, 600, "/tmp/embed-resize-settled.png");
                check(looks_nonblank(settled, 1000, 600),
                      "render after resize-stress is non-blank");

                [rwin close];
                pulp_embed_destroy(rv);
            }
        }

        std::printf("== %d failure(s) ==\n", g_failures);
        return g_failures == 0 ? 0 : 1;
    }
}
