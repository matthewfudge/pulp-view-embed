// bundle_render — render an importer JS bundle through the high-fidelity
// scripted-UI embed path to a PNG. Standalone fidelity harness (not a gate):
//   bundle_render <bundle_dir> <w> <h> <out.png>
//
// Uses pulp_embed_create_from_ui_bundle + the deterministic Skia render path so
// the output is directly comparable to the Pulp importer's own --validate
// render. Attaches to an offscreen NSWindow so the GPU host path is exercised
// too (and the GpuSurface handoff runs), then captures via render_png.

#import <AppKit/AppKit.h>
#include "pulp_view_embed.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void pump(int frames) {
    for (int i = 0; i < frames; ++i) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
             beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    }
}

int main(int argc, const char* argv[]) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <bundle_dir> <w> <h> <out.png>\n", argv[0]);
        return 2;
    }
    const std::string bundle = argv[1];
    const int w = std::atoi(argv[2]);
    const int h = std::atoi(argv[3]);
    const std::string out = argv[4];
    const float scale = (argc >= 6) ? static_cast<float>(std::atof(argv[5])) : 1.0f;

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        PulpEmbedDesc d{};
        d.struct_size = sizeof(PulpEmbedDesc);
        d.abi_version = PULP_VIEW_EMBED_ABI_VERSION;
        d.logical_width = w;
        d.logical_height = h;
        d.scale_factor = 1.0f;
        d.backend_pref = PULP_EMBED_BACKEND_PREF_AUTO;
        d.design_width = w;
        d.design_height = h;

        PulpEmbedView* v = nullptr;
        PulpEmbedResult r = pulp_embed_create_from_ui_bundle(&d, bundle.c_str(), &v);
        if (r != PULP_EMBED_OK || !v) {
            char buf[1024];
            pulp_embed_last_create_error(buf, sizeof(buf));
            std::fprintf(stderr, "create_from_ui_bundle failed: result=%d err=%s\n", r, buf);
            return 1;
        }
        std::printf("active backend: %d\n", pulp_embed_active_backend(v));

        NSRect frame = NSMakeRect(-20000, -20000, w, h);
        NSWindow* win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:NSWindowStyleMaskTitled
                        backing:NSBackingStoreBuffered
                          defer:NO];
        [win setReleasedWhenClosed:NO];
        [win orderFront:nil];
        pulp_embed_attach(v, (__bridge void*)win.contentView);
        pump(20);

        // Reload self-check (ABI v4): PULP_EMBED_TEST_RELOAD=1 reloads the current
        // bundle in place and confirms it round-trips with a stable param count.
        if (std::getenv("PULP_EMBED_TEST_RELOAD")) {
            const int before = pulp_embed_param_count(v);
            PulpEmbedResult rr = pulp_embed_reload_bundle(v, nullptr);
            pump(10);
            const int after = pulp_embed_param_count(v);
            char e[512]; e[0] = 0; pulp_embed_last_error(v, e, sizeof e);
            std::printf("RELOAD result=%d paramCount %d->%d err=%s\n",
                        rr, before, after, e);
        }

        // Deterministic Skia render — directly comparable to importer --validate.
        size_t need = 0;
        if (pulp_embed_render_png(v, w, h, scale, nullptr, 0, &need) != PULP_EMBED_OK || !need) {
            std::fprintf(stderr, "render_png sizing failed\n");
            return 1;
        }
        std::vector<uint8_t> png(need);
        if (pulp_embed_render_png(v, w, h, scale, png.data(), png.size(), &need) != PULP_EMBED_OK) {
            std::fprintf(stderr, "render_png fill failed\n");
            return 1;
        }
        FILE* f = std::fopen(out.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "cannot open %s\n", out.c_str()); return 1; }
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
        std::printf("wrote %s (%zu bytes)\n", out.c_str(), png.size());

        [win close];
        pulp_embed_destroy(v);
    }
    return 0;
}
