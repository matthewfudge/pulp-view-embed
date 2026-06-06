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

#include <cstdio>
#include <cstdint>
#include <string>
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

        std::printf("== %d failure(s) ==\n", g_failures);
        return g_failures == 0 ? 0 : 1;
    }
}
