// pulp_view_embed.cpp — implementation of the flat C embedding ABI.
//
// Wraps pulp::format::ViewBridge + pulp::view::PluginViewHost over a DesignIR
// materialized into a native view tree. The only Pulp surface that crosses the
// public boundary is opaque (PulpEmbedView*); everything else is POD/C.
//
// Lifetime/teardown ordering is the load-bearing detail: the PluginViewHost
// holds a reference to the root View (owned by ViewBridge) and runs a
// display-link/idle loop. So destroy() must, in order: clear host callbacks,
// destroy the host (stops the loop, drops the View&), then close the bridge
// (destroys the View), then drop processor/store.

#include "pulp_view_embed.h"

#include <pulp/format/processor.hpp>
#include <pulp/format/view_bridge.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/view.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace {

// Thread-local detail for creation failures (no handle exists yet).
thread_local std::string g_create_error;

// Minimal inert processor: it exists only to satisfy ViewBridge's
// (Processor&, StateStore&) contract and to hand the materialized DesignIR
// view tree to the bridge via create_view(). No audio ever runs.
class EmbedProcessor final : public pulp::format::Processor {
public:
    EmbedProcessor(pulp::view::DesignIR ir, pulp::format::ViewSize size)
        : ir_(std::move(ir)), size_(size) {}

    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "PulpEmbed";
        d.manufacturer = "Pulp";
        d.bundle_id = "dev.pulp.embed";
        d.version = "0.1.0";
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}

    pulp::format::ViewSize view_size() const override { return size_; }

    std::unique_ptr<pulp::view::View> create_view() override {
        pulp::view::NativeMaterializeOptions opts;
        return pulp::view::build_native_view_tree(ir_, ir_.asset_manifest, opts);
    }

private:
    pulp::view::DesignIR ir_;
    pulp::format::ViewSize size_;
};

}  // namespace

// The opaque handle. Field order matters for teardown — see destroy().
struct PulpEmbedView {
    std::unique_ptr<EmbedProcessor> processor;
    std::unique_ptr<pulp::state::StateStore> store;
    std::unique_ptr<pulp::format::ViewBridge> bridge;
    std::unique_ptr<pulp::view::PluginViewHost> host;
    PulpEmbedBackend backend = PULP_EMBED_BACKEND_UNKNOWN;
    pulp::format::ViewSize size_hints{};
    bool opened = false;     // notify_attached() has fired
    std::string last_error;
};

namespace {

PulpEmbedResult set_err(PulpEmbedView* v, PulpEmbedResult r, std::string msg) {
    if (v) v->last_error = std::move(msg);
    return r;
}

// Validate + normalize the descriptor. Returns PULP_EMBED_OK or an error.
PulpEmbedResult check_desc(const PulpEmbedDesc* desc) {
    if (!desc) return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->abi_version != PULP_VIEW_EMBED_ABI_VERSION) return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->struct_size < sizeof(uint32_t) * 2) return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->logical_width <= 0 || desc->logical_height <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    return PULP_EMBED_OK;
}

// Rewrite relative asset/font local_paths to absolute against base_dir so the
// materializer can load rasterized images (e.g. the figma export's assets/*.png)
// regardless of the process CWD. DesignIR JSON stores local_path relative to the
// IR file; without this, ImageViews fail to load and the design renders without
// its bitmap content. No-op when base_dir is empty or the path is already absolute.
void resolve_asset_paths(pulp::view::DesignIR& ir, const std::string& base_dir) {
    if (base_dir.empty()) return;
    namespace fs = std::filesystem;
    const fs::path base(base_dir);
    for (auto& asset : ir.asset_manifest.assets) {
        if (asset.local_path && !asset.local_path->empty()) {
            fs::path p(*asset.local_path);
            if (p.is_relative()) asset.local_path = (base / p).lexically_normal().string();
        }
    }
    // Bundled fonts reference their file through the asset manifest (asset_id ->
    // IRAssetRef.local_path, resolved above); resolved_path, when set, is already
    // absolute. So no separate font-path rewrite is needed here.
}

// Shared create path over an already-loaded JSON string. asset_base_dir is the
// directory relative asset paths resolve against (the IR file's dir, or
// desc->asset_base_path for the in-memory variant).
PulpEmbedResult create_from_json(const PulpEmbedDesc* desc,
                                 const std::string& json,
                                 const std::string& asset_base_dir,
                                 PulpEmbedView** out_view) {
    if (out_view) *out_view = nullptr;
    g_create_error.clear();
    if (auto r = check_desc(desc); r != PULP_EMBED_OK) {
        g_create_error = "invalid descriptor";
        return r;
    }
    if (!out_view) return PULP_EMBED_ERR_INVALID_ARG;

    pulp::view::DesignIR ir;
    try {
        ir = pulp::view::parse_design_ir_json(json);
    } catch (const std::exception& e) {
        g_create_error = std::string("DesignIR parse failed: ") + e.what();
        return PULP_EMBED_ERR_PARSE;
    }
    resolve_asset_paths(ir, asset_base_dir);

    auto v = std::make_unique<PulpEmbedView>();

    const auto w = static_cast<uint32_t>(desc->logical_width);
    const auto h = static_cast<uint32_t>(desc->logical_height);
    v->size_hints = pulp::format::view_size_from_design(w, h);

    v->processor = std::make_unique<EmbedProcessor>(std::move(ir), v->size_hints);
    v->store = std::make_unique<pulp::state::StateStore>();
    v->bridge = std::make_unique<pulp::format::ViewBridge>(*v->processor, *v->store);

    std::string err;
    if (!v->bridge->open(&err)) {
        g_create_error = "view open failed: " + err;
        return PULP_EMBED_ERR_VIEW_OPEN;
    }
    if (!v->bridge->view()) {
        g_create_error = "materialized view tree is empty";
        return PULP_EMBED_ERR_MATERIALIZE;
    }

    // A freshly materialized DesignIR tree has no laid-out bounds; give the root
    // the logical size and run Yoga so the first frame paints (the host renders
    // the tree but does not itself lay out a guest view).
    if (auto* rv = v->bridge->view()) {
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        rv->layout_children();
    }

    pulp::view::PluginViewHost::Options opts;
    opts.size = {w, h};
    opts.use_gpu = (desc->backend_pref != PULP_EMBED_BACKEND_PREF_CPU);
    v->host = pulp::view::PluginViewHost::create(*v->bridge->view(), opts);
    if (!v->host) {
        g_create_error = "no PluginViewHost (missing platform factory?)";
        return PULP_EMBED_ERR_HOST_CREATE;
    }
    v->backend = v->host->is_gpu_backed() ? PULP_EMBED_BACKEND_GPU : PULP_EMBED_BACKEND_CPU;

    if (desc->design_width > 0 && desc->design_height > 0) {
        v->host->set_design_viewport(static_cast<float>(desc->design_width),
                                     static_cast<float>(desc->design_height));
    }

    *out_view = v.release();
    return PULP_EMBED_OK;
}

}  // namespace

extern "C" {

uint32_t pulp_embed_abi_version(void) { return PULP_VIEW_EMBED_ABI_VERSION; }

PulpEmbedResult pulp_embed_create_from_design_json(const PulpEmbedDesc* desc,
                                                   const char* path,
                                                   PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!path) { g_create_error = "null path"; return PULP_EMBED_ERR_INVALID_ARG; }
        std::ifstream f(path, std::ios::binary);
        if (!f) { g_create_error = std::string("cannot open ") + path; return PULP_EMBED_ERR_PARSE; }
        std::ostringstream ss;
        ss << f.rdbuf();
        // Relative asset paths in the IR resolve against the IR file's directory.
        const std::string base_dir = std::filesystem::path(path).parent_path().string();
        return create_from_json(desc, ss.str(), base_dir, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_create_from_design_json_str(const PulpEmbedDesc* desc,
                                                       const char* json,
                                                       size_t json_len,
                                                       PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!json) { g_create_error = "null json"; return PULP_EMBED_ERR_INVALID_ARG; }
        const std::string base_dir =
            (desc && desc->asset_base_path) ? std::string(desc->asset_base_path) : std::string();
        return create_from_json(desc, std::string(json, json_len), base_dir, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

size_t pulp_embed_last_create_error(char* buf, size_t cap) {
    const auto& s = g_create_error;
    if (buf && cap) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        for (size_t i = 0; i < n; ++i) buf[i] = s[i];
        buf[n] = '\0';
    }
    return s.size();
}

void* pulp_embed_native_handle(PulpEmbedView* v) {
    if (!v || !v->host) return nullptr;
    try { return v->host->native_handle(); } catch (...) { return nullptr; }
}

PulpEmbedResult pulp_embed_notify_attached(PulpEmbedView* v) {
    if (!v || !v->host || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        if (!v->host->is_attached())
            return set_err(v, PULP_EMBED_ERR_ATTACH, "child not parented; cannot notify_attached");
        if (!v->opened) { v->bridge->notify_attached(); v->opened = true; }
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "notify_attached threw");
    }
}

PulpEmbedResult pulp_embed_attach(PulpEmbedView* v, void* parent) {
    if (!v || !v->host || !v->bridge) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        if (v->host->is_attached()) return PULP_EMBED_OK;  // idempotent
        if (!v->host->try_attach_to_parent(parent))
            return set_err(v, PULP_EMBED_ERR_ATTACH, "attach_to_parent did not take");
        if (!v->opened) { v->bridge->notify_attached(); v->opened = true; }
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "unknown exception");
    }
}

PulpEmbedResult pulp_embed_detach(PulpEmbedView* v) {
    if (!v || !v->host) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        v->host->detach();
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "detach threw");
    }
}

PulpEmbedResult pulp_embed_resize(PulpEmbedView* v, int32_t w, int32_t h, float /*scale*/) {
    if (!v || !v->host || !v->bridge || w <= 0 || h <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        v->host->set_size(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        if (auto* rv = v->bridge->view()) {
            rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
            rv->layout_children();
        }
        v->bridge->resize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "resize threw");
    }
}

PulpEmbedResult pulp_embed_tick(PulpEmbedView* v) {
    if (!v || !v->host) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        v->host->repaint();
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "tick threw");
    }
}

PulpEmbedResult pulp_embed_repaint(PulpEmbedView* v) {
    if (!v || !v->host) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        v->host->repaint();
        return PULP_EMBED_OK;
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "repaint threw");
    }
}

PulpEmbedResult pulp_embed_size_hints(PulpEmbedView* v, PulpEmbedSizeHints* out) {
    if (!v || !out) return PULP_EMBED_ERR_INVALID_ARG;
    const auto& s = v->size_hints;
    out->preferred_width = static_cast<int32_t>(s.preferred_width);
    out->preferred_height = static_cast<int32_t>(s.preferred_height);
    out->min_width = static_cast<int32_t>(s.min_width);
    out->min_height = static_cast<int32_t>(s.min_height);
    out->max_width = static_cast<int32_t>(s.max_width);
    out->max_height = static_cast<int32_t>(s.max_height);
    out->aspect_ratio = static_cast<float>(s.aspect_ratio);
    out->resizable = 1;
    return PULP_EMBED_OK;
}

int32_t pulp_embed_active_backend(PulpEmbedView* v) {
    return v ? static_cast<int32_t>(v->backend) : PULP_EMBED_BACKEND_UNKNOWN;
}

PulpEmbedResult pulp_embed_capture_png(PulpEmbedView* v, uint8_t* out,
                                       size_t cap, size_t* out_len) {
    if (!v || !v->host || !out_len) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        std::vector<uint8_t> png = v->host->capture_back_buffer_png();
        *out_len = png.size();
        if (png.empty())
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED, "no back-buffer capture (CPU host?)");
        if (!out) return PULP_EMBED_OK;  // sizing query
        if (cap < png.size()) return PULP_EMBED_ERR_BUFFER_TOO_SMALL;
        for (size_t i = 0; i < png.size(); ++i) out[i] = png[i];
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "capture threw");
    }
}

PulpEmbedResult pulp_embed_render_png(PulpEmbedView* v, int32_t w, int32_t h,
                                      float scale, uint8_t* out, size_t cap,
                                      size_t* out_len) {
    if (!v || !v->bridge || !out_len || w <= 0 || h <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* rv = v->bridge->view();
        if (!rv) return set_err(v, PULP_EMBED_ERR_MATERIALIZE, "no view");
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        rv->layout_children();
        std::vector<uint8_t> png = pulp::view::render_to_png(
            *rv, static_cast<uint32_t>(w), static_cast<uint32_t>(h),
            scale > 0.0f ? scale : 1.0f, pulp::view::ScreenshotBackend::skia);
        *out_len = png.size();
        if (png.empty())
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED, "render_to_png produced no bytes");
        if (!out) return PULP_EMBED_OK;  // sizing query
        if (cap < png.size()) return PULP_EMBED_ERR_BUFFER_TOO_SMALL;
        for (size_t i = 0; i < png.size(); ++i) out[i] = png[i];
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "render_png threw");
    }
}

size_t pulp_embed_last_error(PulpEmbedView* v, char* buf, size_t cap) {
    if (!v) return 0;
    const auto& s = v->last_error;
    if (buf && cap) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        for (size_t i = 0; i < n; ++i) buf[i] = s[i];
        buf[n] = '\0';
    }
    return s.size();
}

void pulp_embed_destroy(PulpEmbedView* v) {
    if (!v) return;
    try {
        // Teardown order: stop the host's loop/callbacks and drop it (it holds
        // a View& into the bridge-owned tree), THEN close the bridge (destroys
        // the view + fires on_view_closed iff opened), THEN drop proc/store.
        if (v->host) {
            v->host->set_idle_callback(nullptr);
            v->host->set_resize_callback(nullptr);
            v->host->detach();
            v->host.reset();
        }
        if (v->bridge) { v->bridge->close(); v->bridge.reset(); }
        v->processor.reset();
        v->store.reset();
    } catch (...) {
        // swallow — destroy must not throw across the C boundary
    }
    delete v;
}

}  // extern "C"
