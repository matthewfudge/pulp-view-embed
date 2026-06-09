// linux-x11-smoke — prove the LIVE X11 PluginViewHost path on Linux.
//
// The preflight (pulp-embed-validate) exercises the deterministic *headless* Skia
// render (render_png). This goes further: it creates a real X11 parent window,
// embeds a Pulp-imported design as a child via the flat C ABI (mode A —
// pulp_embed_attach → X11PluginViewHost::attach_to_parent → XReparentWindow),
// pumps a few frames, and captures the LIVE back buffer — proving the X11 host
// actually attaches into a foreign X11 window and renders, not just the headless
// path. Needs a running X server ($DISPLAY); under CI run it via Xvfb:
//   xvfb-run -a ./pulp-embed-linux-x11-smoke <design.ir.json> [w] [h]
//
// Exit 0 = attached + non-blank live capture; 1 = failure; 2 = no display/usage.

#include "pulp_view_embed.h"

#include <X11/Xlib.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
void pump_x(Display* dpy) {
    while (XPending(dpy)) { XEvent e; XNextEvent(dpy, &e); }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <design.ir.json> [w] [h]\n", argv[0]);
        return 2;
    }
    const std::string ir = argv[1];
    const int W = argc > 2 ? std::atoi(argv[2]) : 800;
    const int H = argc > 3 ? std::atoi(argv[3]) : 480;

    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::fprintf(stderr, "no X server ($DISPLAY); run under xvfb-run\n");
        return 2;
    }
    Window root = DefaultRootWindow(dpy);
    // A real top-level parent window (the "host editor window") to reparent into.
    Window parent = XCreateSimpleWindow(dpy, root, 0, 0,
                                        static_cast<unsigned>(W), static_cast<unsigned>(H),
                                        0, 0, 0x00101418);
    XStoreName(dpy, parent, "pulp-embed-x11-smoke");
    XMapWindow(dpy, parent);
    XFlush(dpy);
    pump_x(dpy);

    PulpEmbedDesc d{};
    d.struct_size = sizeof(PulpEmbedDesc);
    d.abi_version = PULP_VIEW_EMBED_ABI_VERSION;
    d.logical_width = W;
    d.logical_height = H;
    d.scale_factor = 1.0f;
    d.backend_pref = PULP_EMBED_BACKEND_PREF_AUTO;
    d.design_width = W;
    d.design_height = H;

    PulpEmbedView* v = nullptr;
    PulpEmbedResult r = pulp_embed_create_from_design_json(&d, ir.c_str(), &v);
    if (r != PULP_EMBED_OK || v == nullptr) {
        char buf[1024] = {0};
        pulp_embed_last_create_error(buf, sizeof buf);
        std::fprintf(stderr, "create failed: result=%d err=%s\n", r, buf);
        return 1;
    }
    check(true, "create_from_design_json on Linux/X11");
    std::printf("  [info] active backend: %d (1=GPU 2=CPU)\n", pulp_embed_active_backend(v));

    // Mode A: attach Pulp's child X11 window into our parent (XReparentWindow).
    const auto parent_handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(parent));
    check(pulp_embed_attach(v, parent_handle) == PULP_EMBED_OK,
          "pulp_embed_attach into a real X11 parent window (live X11PluginViewHost)");

    // The child's native handle is a valid X11 Window id.
    check(pulp_embed_native_handle(v) != nullptr, "native_handle() returns an X11 Window");

    // Pump a few frames so the host renders into the attached child.
    for (int i = 0; i < 30; ++i) {
        pulp_embed_tick(v);
        pump_x(dpy);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // Capture the LIVE back buffer (the X11 host's raster capture always works).
    size_t need = 0;
    std::vector<uint8_t> png;
    PulpEmbedResult cr = pulp_embed_capture_png(v, nullptr, 0, &need);
    if (cr == PULP_EMBED_OK && need > 0) {
        png.resize(need);
        cr = pulp_embed_capture_png(v, png.data(), png.size(), &need);
    }
    const bool nonblank =
        cr == PULP_EMBED_OK && static_cast<double>(png.size()) / (double(W) * double(H)) > 0.02;
    check(nonblank, "live X11 back-buffer capture is non-blank");
    if (!png.empty()) {
        if (FILE* f = std::fopen("/tmp/x11-smoke-capture.png", "wb")) {
            std::fwrite(png.data(), 1, png.size(), f);
            std::fclose(f);
            std::printf("  [info] wrote /tmp/x11-smoke-capture.png (%zu bytes)\n", png.size());
        }
    }

    // Detach balances the attach; destroy frees everything.
    pulp_embed_detach(v);
    pulp_embed_destroy(v);
    XDestroyWindow(dpy, parent);
    XCloseDisplay(dpy);

    std::printf("%s\n", g_fail == 0 ? "pulp-embed-linux-x11-smoke OK"
                                    : "pulp-embed-linux-x11-smoke FAILED");
    return g_fail == 0 ? 0 : 1;
}
