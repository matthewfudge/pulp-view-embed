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
#include <pulp/state/listener_token.hpp>
#include <pulp/state/parameter.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widgets.hpp>

#include <chrono>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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

    // Faithful-vector binding keys for every value-bearing element, in the same
    // index order make_faithful_svg_frame builds them into the DesignFrameView,
    // so element index i here maps to DesignFrameView element i. Key is the
    // source node id (the importer's "binding key"); falls back to a stable
    // synthetic key when the source carried none. Knobs are continuous; dropdown/
    // tab_group/stepper are normalized-index choice params (DesignFrameView's
    // uniform element_value/set_element_value handle both). text_field is text,
    // not a normalized param, so it's skipped (still in-view interactive).
    std::vector<std::pair<int, std::string>> faithful_element_keys() const {
        std::vector<std::pair<int, std::string>> out;
        const pulp::view::IRNode* frame = find_faithful_node(ir_.root);
        if (!frame) return out;
        using K = pulp::view::InteractiveElementKind;
        for (int i = 0; i < static_cast<int>(frame->interactive_elements.size()); ++i) {
            const auto& e = frame->interactive_elements[static_cast<size_t>(i)];
            if (e.kind == K::text_field) continue;  // text is not a normalized param
            std::string key = e.source_node_id.value_or("");
            if (key.empty()) {
                const char* p = e.kind == K::knob ? "knob"
                              : e.kind == K::dropdown ? "dropdown"
                              : e.kind == K::tab_group ? "tabs" : "stepper";
                key = std::string(p) + ":" + std::to_string(i);
            }
            out.emplace_back(i, std::move(key));
        }
        return out;
    }

private:
    static const pulp::view::IRNode* find_faithful_node(const pulp::view::IRNode& n) {
        if (n.render_mode == pulp::view::NodeRenderMode::faithful_svg) return &n;
        for (const auto& c : n.children)
            if (const auto* f = find_faithful_node(c)) return f;
        return nullptr;
    }

    pulp::view::DesignIR ir_;
    pulp::format::ViewSize size_;
};

// High-fidelity processor: renders an importer JS bundle (`ui.js`) through the
// SAME scripted-UI pipeline (ScriptedUiSession + WidgetBridge) the Pulp
// importer's own --validate render and real GPU-scripted plugins use. The
// scripted path drives the native widget bridge (createCol/createImage/
// createKnob + setImageSource) and composites the rasterized assets, so the
// embed reproduces the importer render instead of the flattened native-widget
// fallback that build_native_view_tree produces.
//
// Ownership/lifetime: this processor owns the root View and the
// ScriptedUiSession (which holds `View&`). create_view() hands the root to the
// ViewBridge by transferring the unique_ptr — the View object is unmoved, so
// the session's reference stays valid. active_scripted_ui() lets ViewBridge
// (and the shim's GPU-surface handoff) reach the session.
class EmbedScriptedProcessor final : public pulp::format::Processor {
public:
    EmbedScriptedProcessor(std::filesystem::path script_path,
                           std::filesystem::path bundle_dir,
                           pulp::format::ViewSize size)
        : script_path_(std::move(script_path)),
          bundle_dir_(std::move(bundle_dir)),
          size_(size) {}

    ~EmbedScriptedProcessor() override {
        // Drop the resolved-script temp file if we wrote one.
        if (!effective_script_.empty() && effective_script_ != script_path_) {
            std::error_code ec;
            std::filesystem::remove(effective_script_, ec);
        }
    }

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

    // Optional id -> absolute-staged-path overrides for host-served assets
    // (resolve_resource). The JS path-resolver preamble checks this map FIRST
    // (by the path as written in ui.js) and falls back to the bundle dir, so a
    // host-served `assets/x.png` overrides the on-disk one. Set before
    // load_or_throw().
    void set_asset_overrides(std::vector<std::pair<std::string, std::string>> ov) {
        asset_overrides_ = std::move(ov);
    }

    // Build the root + scripted session and load the bundle. Throws on load
    // failure so the create path reports a precise error.
    void load_or_throw() {
        root_ = std::make_unique<pulp::view::View>();
        root_->set_theme(pulp::view::Theme::dark());
        root_->flex().direction = pulp::view::FlexDirection::column;
        // Tag the root so the host auto-selects the GPU PluginViewHost — the
        // scripted UI paints through the Skia/Dawn pipeline. Mirrors
        // pulp::format::build_editor_ui().
        root_->set_requires_gpu_host(true);
        // Give the root the design bounds before the script runs so any
        // position:absolute + inset:0 chain resolves against a real size
        // (pulp #1899). The shim re-applies bounds before each render too.
        root_->set_bounds({0.0f, 0.0f,
                           static_cast<float>(size_.preferred_width),
                           static_cast<float>(size_.preferred_height)});

        // Portable bundles may reference assets by path relative to the bundle
        // dir (e.g. `assets/foo.png`). setImageSource / setKnobSpriteStrip load
        // a path verbatim (absolute, or relative to CWD), so without help a
        // relative bundle would only render its images when run from the bundle
        // dir. Prepend a tiny JS shim that resolves relative asset paths against
        // the bundle dir before the original setters run. Absolute paths (the
        // CLI's default `--emit js` output) pass through untouched, so this is a
        // no-op for those. The combined script is written next to ui.js as a
        // temp file and removed in the destructor.
        // Dev hot-reload (opt-in, off by default = production-safe): set
        // PULP_EMBED_HOT_RELOAD=1 to live-reload the bundle while a host editor
        // is open. It reuses Pulp's ScriptedUiSession HotReloader (background
        // choc::file::Watcher → UI-thread poll_reload() with widget-value
        // preservation), which the embed already pumps via the host idle tick
        // (wire_scripted_session_to_host). The watcher watches the script that
        // is loaded, so in dev mode we load the ORIGINAL ui.js directly (the
        // file the developer edits) rather than the temp asset-resolver wrapper
        // — dev bundles should therefore use the importer's default absolute
        // asset paths (a portabilized relative bundle resolves assets via the
        // wrapper, which is the production path). See the README dev-loop guide.
        const bool dev_hot_reload = std::getenv("PULP_EMBED_HOT_RELOAD") != nullptr;
        effective_script_ = dev_hot_reload ? script_path_ : build_effective_script();

        pulp::view::ScriptedUiOptions options;
        options.script_path = effective_script_;
        options.enable_hot_reload = dev_hot_reload;
        options.enable_theme_reload = dev_hot_reload;
        session_ = std::make_unique<pulp::view::ScriptedUiSession>(*root_, store_, std::move(options));

        std::string err;
        if (!session_->load(&err)) {
            throw std::runtime_error("scripted UI load failed: " + (err.empty() ? "unknown" : err));
        }
    }

    std::unique_ptr<pulp::view::View> create_view() override {
        // ViewBridge::open() calls this once. Transfer the already-built root;
        // the session keeps its View& valid (the object is not moved).
        return std::move(root_);
    }

    pulp::view::ScriptedUiSession* active_scripted_ui() override { return session_.get(); }
    const pulp::view::ScriptedUiSession* active_scripted_ui() const override { return session_.get(); }

    // Reload the scripted UI in place (pulp_embed_reload_bundle). With an empty
    // new_bundle_dir, reload the current bundle (picks up edits to its ui.js);
    // otherwise switch to new_bundle_dir/ui.js. Regenerates the effective script
    // the same way load_or_throw did (dev = the original ui.js; otherwise the
    // asset-resolver-wrapped temp) and hands it to ScriptedUiSession::reload_from,
    // which rebuilds under the same root + GPU surface, preserves widget state,
    // and probes first (a bad reload keeps the last-good UI -> returns false).
    bool reload(const std::filesystem::path& new_bundle_dir, std::string* error) {
        if (!session_) { if (error) *error = "no scripted session to reload"; return false; }
        if (!new_bundle_dir.empty()) {
            bundle_dir_ = new_bundle_dir;
            script_path_ = bundle_dir_ / "ui.js";
        }
        const bool dev = std::getenv("PULP_EMBED_HOT_RELOAD") != nullptr;
        effective_script_ = dev ? script_path_ : build_effective_script();
        return session_->reload_from(effective_script_, error);
    }

    // Destroy the scripted session (and its WidgetBridge) while the root View
    // it references is still alive. MUST be called before the ViewBridge closes
    // (which destroys the root) — the WidgetBridge destructor touches root_,
    // so destroying it after the View is freed is a use-after-free. The shim's
    // teardown calls this first; see pulp_embed_destroy().
    void release_session() { session_.reset(); }

private:
    // Build the script the session actually loads. When bundle_dir_ is known we
    // wrap ui.js with a path-resolving preamble so relative `assets/...` paths
    // load regardless of CWD, written as a temp file beside ui.js. If no rewrite
    // is needed (no bundle dir, or write fails) we fall back to script_path_.
    std::filesystem::path build_effective_script() {
        namespace fs = std::filesystem;
        if (bundle_dir_.empty()) return script_path_;

        std::ifstream in(script_path_, std::ios::binary);
        if (!in) return script_path_;
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string ui = ss.str();

        // JSON-string-escape helper for preamble literals.
        auto esc_js = [](const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c == '\\' || c == '"') out.push_back('\\');
                out.push_back(c);
            }
            return out;
        };

        // Host-served asset overrides, as a JS object literal keyed by the path
        // as written in ui.js -> the absolute staged path. Checked before the
        // bundle-dir fallback so resolve_resource wins over disk.
        std::string overrides_obj = "{";
        for (size_t i = 0; i < asset_overrides_.size(); ++i) {
            if (i) overrides_obj += ",";
            overrides_obj += "\"" + esc_js(asset_overrides_[i].first) + "\":\"" +
                             esc_js(asset_overrides_[i].second) + "\"";
        }
        overrides_obj += "}";

        // The preamble wraps the two path-taking setters. A path is first looked
        // up in the host-override map (resolve_resource); otherwise a path that
        // is relative (no leading '/' and no 'scheme://') is prefixed with the
        // bundle dir, and everything else is untouched.
        std::string preamble =
            "(function(){\n"
            "  var __pulpEmbedBase = \"" + esc_js(bundle_dir_.string()) + "\";\n"
            "  var __pulpEmbedOverrides = " + overrides_obj + ";\n"
            "  function __pulpEmbedResolve(p){\n"
            "    if (typeof p !== 'string' || p.length === 0) return p;\n"
            "    if (Object.prototype.hasOwnProperty.call(__pulpEmbedOverrides, p))\n"
            "      return __pulpEmbedOverrides[p];\n"
            "    if (p.charAt(0) === '/' || /^[a-zA-Z]+:\\/\\//.test(p)) return p;\n"
            "    return __pulpEmbedBase + '/' + p;\n"
            "  }\n"
            "  if (typeof setImageSource === 'function'){\n"
            "    var __sis = setImageSource;\n"
            "    setImageSource = function(id, p){ return __sis(id, __pulpEmbedResolve(p)); };\n"
            "  }\n"
            "  if (typeof setKnobSpriteStrip === 'function'){\n"
            "    var __sks = setKnobSpriteStrip;\n"
            "    setKnobSpriteStrip = function(id, p, n, o){ return __sks(id, __pulpEmbedResolve(p), n, o); };\n"
            "  }\n"
            "  if (typeof registerFont === 'function'){\n"
            "    var __rf = registerFont;\n"
            "    registerFont = function(fam, p){ return __rf(fam, __pulpEmbedResolve(p)); };\n"
            "  }\n"
            "})();\n";

        const fs::path out = bundle_dir_ / ".pulp-embed-run.js";
        std::ofstream of(out, std::ios::binary | std::ios::trunc);
        if (!of) return script_path_;
        of << preamble << ui;
        of.close();
        return out;
    }

    std::filesystem::path script_path_;
    std::filesystem::path bundle_dir_;
    std::filesystem::path effective_script_;
    std::vector<std::pair<std::string, std::string>> asset_overrides_;
    pulp::format::ViewSize size_;
    pulp::state::StateStore store_;  // session needs a store for bindings
    std::unique_ptr<pulp::view::View> root_;
    std::unique_ptr<pulp::view::ScriptedUiSession> session_;
};

// ── Parameter bridge ──────────────────────────────────────────────────────
//
// One ParamBinding per bindable control discovered in the design. The control
// is addressed across the C ABI by its string `key` (the design's pulpParamKey
// when present, else the widget id). Each binding owns a StateStore parameter
// (id == registration index + 1) whose name == key; the StateStore is the
// single source of truth, and the embed mirrors it both into the live widget
// and out to the host.
enum class ParamWidgetKind { knob, fader, toggle };

struct ParamBinding {
    std::string key;             // ABI identity (pulpParamKey or widget id)
    std::string widget_id;       // widget the param drives
    pulp::state::ParamID param_id = 0;
    ParamWidgetKind kind = ParamWidgetKind::knob;
    pulp::view::View* widget = nullptr;  // borrowed; owned by the view tree
    // Faithful-vector binding (v2): when >= 0, `widget` is the DesignFrameView
    // and the param drives its element at this index (knob OR choice control) via
    // its uniform element_value()/set_element_value() instead of a Knob/Fader/
    // Toggle. UI->host is event-driven through DesignFrameView::on_element_changed
    // (wired in build_param_bridge); set_element_value is silent so host pushes
    // don't echo back.
    int   frame_element_index = -1;
};

}  // namespace

// The opaque handle. Field order matters for teardown — see destroy().
struct PulpEmbedView {
    std::unique_ptr<pulp::format::Processor> processor;
    std::unique_ptr<pulp::state::StateStore> store;
    std::unique_ptr<pulp::format::ViewBridge> bridge;
    std::unique_ptr<pulp::view::PluginViewHost> host;
    PulpEmbedBackend backend = PULP_EMBED_BACKEND_UNKNOWN;
    pulp::format::ViewSize size_hints{};
    bool opened = false;     // notify_attached() has fired
    bool offscreen = false;  // created via pulp_embed_create_offscreen (no host)
    std::string last_error;

    // ── host resource staging (ABI v3) ──
    // Temp dir holding host-served asset bytes (resolve_resource), written so the
    // existing on-disk render path loads them. Removed in destroy(). Empty when
    // the host served nothing.
    std::string staging_dir;

    // ── parameter bridge (ABI v2) ──
    PulpEmbedHostCallbacks host_cb{};         // copied from the desc; may be all-NULL
    void* host_ctx = nullptr;
    std::vector<ParamBinding> params;         // stable, registration-ordered
    std::unordered_map<std::string, size_t> key_to_index;
    pulp::state::ListenerToken param_listener; // forwards store changes to host
    // Guard: true while applying a HOST-driven change so the store listener does
    // not bounce the value back out to host.set_param (feedback-loop break).
    bool applying_host_change = false;

    // Thread that created the view. pulp_embed_reload_bundle (which rebuilds views
    // + touches the GPU surface) must run here; a call from another thread is
    // rejected with PULP_EMBED_ERR_WRONG_THREAD. Captured at construction.
    std::thread::id creator_thread = std::this_thread::get_id();
};

namespace {

PulpEmbedResult set_err(PulpEmbedView* v, PulpEmbedResult r, std::string msg) {
    if (v) v->last_error = std::move(msg);
    return r;
}

// Validate + normalize the descriptor. Returns PULP_EMBED_OK or an error.
//
// abi_version is accepted when it is <= the library's version: a v1 caller hands
// a smaller struct (no host-bridge tail) and a v2 library reads that absent tail
// as all-NULL. A caller from the FUTURE (abi_version greater than ours) is
// rejected — we can't know its layout. struct_size gates how much of the desc
// we may read.
PulpEmbedResult check_desc(const PulpEmbedDesc* desc) {
    if (!desc) return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->abi_version == 0 || desc->abi_version > PULP_VIEW_EMBED_ABI_VERSION)
        return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->struct_size < sizeof(uint32_t) * 2) return PULP_EMBED_ERR_INVALID_ARG;
    if (desc->logical_width <= 0 || desc->logical_height <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    return PULP_EMBED_OK;
}

// Copy the desc's host-callback block into v, gating EACH field on the caller's
// struct_size so older callers stay supported as the block grows:
//   - A v1 caller (struct stops before `host`) leaves host_cb all-NULL.
//   - A v2 caller carries the original five callbacks but not resolve_resource.
//   - A v3 caller carries resolve_resource too.
// We read up to whatever prefix of `host` the caller's struct_size covers.
void capture_host_callbacks(PulpEmbedView* v, const PulpEmbedDesc* desc) {
    v->host_ctx = desc->host_ctx;
    // Reachable bytes of the trailing `host` member, clamped to its real size.
    const size_t host_off = offsetof(PulpEmbedDesc, host);
    if (desc->struct_size <= host_off) return;  // no host block at all
    size_t avail = desc->struct_size - host_off;
    if (avail > sizeof(PulpEmbedHostCallbacks)) avail = sizeof(PulpEmbedHostCallbacks);
    // Copy only the prefix the caller actually provided; the rest stays NULL.
    std::memcpy(&v->host_cb, &desc->host, avail);
}

// Forward declarations — the param-bridge builders are defined further down
// (next to the host/session wiring) but the create paths above reference them.
void build_param_bridge(PulpEmbedView* v);
void poll_host_meters(PulpEmbedView* v);

// ── host resource staging (resolve_resource, ABI v3) ───────────────────────
//
// The host can serve an asset's bytes by id via desc->host.resolve_resource.
// Rather than reach into every image/font/canvas decode site (which load by
// path through SkData::MakeFromFileName, NOT a hookable AssetManager), the shim
// stages served bytes to a temp dir and points the existing on-disk path at
// them. Disk fallback is automatic: when the callback returns NULL we leave the
// original path untouched, and the renderer loads it from the bundle/IR dir.

// Create (once) a unique temp dir for this view's staged resources. Returns the
// path, or empty on failure (callers then skip staging and fall back to disk).
std::string ensure_staging_dir(PulpEmbedView* v) {
    if (!v->staging_dir.empty()) return v->staging_dir;
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) return {};
    // A per-view dir keyed by address + clock keeps concurrent embeds isolated.
    const auto uniq = std::to_string(reinterpret_cast<uintptr_t>(v)) + "-" +
                      std::to_string(static_cast<unsigned long long>(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path dir = base / ("pulp-embed-res-" + uniq);
    fs::create_directories(dir, ec);
    if (ec) return {};
    v->staging_dir = dir.string();
    return v->staging_dir;
}

// Ask the host for `id`'s bytes; if served, write them under the staging dir at
// the SAME relative layout as `id` (so a path-based loader finds them) and
// return the absolute staged path. Returns empty when the host serves nothing
// (NULL) — the caller then keeps the disk path. `id` is the asset's design
// identifier (the path as written in ui.js / the manifest local_path).
std::string stage_host_resource(PulpEmbedView* v, const std::string& id) {
    if (!v || !v->host_cb.resolve_resource || id.empty()) return {};
    size_t len = 0;
    const uint8_t* bytes = v->host_cb.resolve_resource(v->host_ctx, id.c_str(), &len);
    if (!bytes || len == 0) return {};  // disk fallback

    const std::string dir = ensure_staging_dir(v);
    if (dir.empty()) return {};

    namespace fs = std::filesystem;
    // Preserve `id`'s relative shape under the staging root. An absolute id is
    // reduced to its filename so it still lands inside the staging dir.
    fs::path rel(id);
    if (rel.is_absolute()) rel = rel.filename();
    const fs::path out = fs::path(dir) / rel;
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    std::ofstream of(out, std::ios::binary | std::ios::trunc);
    if (!of) return {};
    of.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(len));
    of.close();
    if (!of) return {};
    return out.lexically_normal().string();
}

// Scan a ui.js source for the asset paths it references (the path argument of
// setImageSource / setKnobSpriteStrip) and offer each to the host's
// resolve_resource. For every id the host serves, stage the bytes and record an
// id -> staged-absolute-path override. Returns the override list (empty when the
// host serves nothing or has no resolve_resource). The id offered to the host is
// the path EXACTLY as written in ui.js, matching the resolve_resource contract.
std::vector<std::pair<std::string, std::string>>
stage_bundle_resources(PulpEmbedView* v, const std::string& ui_js) {
    std::vector<std::pair<std::string, std::string>> overrides;
    if (!v || !v->host_cb.resolve_resource) return overrides;

    // Match the SECOND string argument (the path) of setImageSource(id, path)
    // and setKnobSpriteStrip(id, path, ...). Quotes may be ' or ".
    static const std::regex re(
        R"((?:setImageSource|setKnobSpriteStrip)\s*\(\s*['"][^'"]*['"]\s*,\s*['"]([^'"]+)['"])");
    std::unordered_map<std::string, std::string> seen;  // id -> staged (dedup)
    for (std::sregex_iterator it(ui_js.begin(), ui_js.end(), re), end; it != end; ++it) {
        const std::string id = (*it)[1].str();
        if (id.empty() || seen.count(id)) continue;
        std::string staged = stage_host_resource(v, id);
        seen[id] = staged;  // cache even empties so we don't re-offer
        if (!staged.empty()) overrides.emplace_back(id, staged);
    }
    return overrides;
}

// Rewrite relative asset/font local_paths to absolute against base_dir so the
// materializer can load rasterized images (e.g. the figma export's assets/*.png)
// regardless of the process CWD. DesignIR JSON stores local_path relative to the
// IR file; without this, ImageViews fail to load and the design renders without
// its bitmap content. No-op when base_dir is empty or the path is already absolute.
// `v` (may be NULL) carries the optional resolve_resource host callback: for
// each asset whose bytes the host serves (keyed by the asset's local_path as
// written in the IR), the served bytes are staged and local_path is pointed at
// the staged file BEFORE the disk-relative rewrite, so the host wins over disk.
void resolve_asset_paths(PulpEmbedView* v, pulp::view::DesignIR& ir,
                         const std::string& base_dir) {
    namespace fs = std::filesystem;
    const bool have_base = !base_dir.empty();
    const fs::path base(base_dir);
    for (auto& asset : ir.asset_manifest.assets) {
        if (!asset.local_path || asset.local_path->empty()) continue;
        // Host-served bytes take precedence over disk (id == the IR local_path).
        std::string staged = stage_host_resource(v, *asset.local_path);
        if (!staged.empty()) { asset.local_path = staged; continue; }
        // Disk fallback: resolve a relative path against the IR directory.
        fs::path p(*asset.local_path);
        if (have_base && p.is_relative())
            asset.local_path = (base / p).lexically_normal().string();
    }
    // Bundled fonts reference their file through the asset manifest (asset_id ->
    // IRAssetRef.local_path, resolved above); resolved_path, when set, is already
    // absolute. So no separate font-path rewrite is needed here.
}

// Shared create path over an already-loaded JSON string. asset_base_dir is the
// directory relative asset paths resolve against (the IR file's dir, or
// desc->asset_base_path for the in-memory variant). When `offscreen` is true the
// view is built fully but no PluginViewHost is created — the host pulls frames
// via pulp_embed_render_frame_rgba instead of attaching a native child.
PulpEmbedResult create_from_json(const PulpEmbedDesc* desc,
                                 const std::string& json,
                                 const std::string& asset_base_dir,
                                 bool offscreen,
                                 PulpEmbedView** out_view) {
    if (out_view) *out_view = nullptr;
    g_create_error.clear();
    if (auto r = check_desc(desc); r != PULP_EMBED_OK) {
        g_create_error = "invalid descriptor";
        return r;
    }
    if (!out_view) return PULP_EMBED_ERR_INVALID_ARG;

    auto v = std::make_unique<PulpEmbedView>();
    v->offscreen = offscreen;
    // Capture host callbacks up front so resolve_resource can stage assets
    // BEFORE the materializer loads them from disk.
    capture_host_callbacks(v.get(), desc);

    pulp::view::DesignIR ir;
    try {
        ir = pulp::view::parse_design_ir_json(json);
    } catch (const std::exception& e) {
        g_create_error = std::string("DesignIR parse failed: ") + e.what();
        return PULP_EMBED_ERR_PARSE;
    }
    resolve_asset_paths(v.get(), ir, asset_base_dir);

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

    if (!offscreen) {
        pulp::view::PluginViewHost::Options opts;
        opts.size = {w, h};
        opts.use_gpu = (desc->backend_pref != PULP_EMBED_BACKEND_PREF_CPU);
        v->host = pulp::view::PluginViewHost::create(*v->bridge->view(), opts);
        if (!v->host) {
            g_create_error = "no PluginViewHost (missing platform factory?)";
            return PULP_EMBED_ERR_HOST_CREATE;
        }
        v->backend = v->host->is_gpu_backed() ? PULP_EMBED_BACKEND_GPU
                                              : PULP_EMBED_BACKEND_CPU;
    } else {
        // Offscreen: no live host. Frames come from the deterministic renderer.
        v->backend = PULP_EMBED_BACKEND_CPU;
    }

    // Interactive parameter bridge (ABI v2): also available on the native
    // DesignIR tree path (Knob/Fader/Toggle widgets carry their node ids).
    build_param_bridge(v.get());

    if (!offscreen && desc->design_width > 0 && desc->design_height > 0) {
        v->host->set_design_viewport(static_cast<float>(desc->design_width),
                                     static_cast<float>(desc->design_height));
    }

    *out_view = v.release();
    return PULP_EMBED_OK;
}

// Wire the host's live GpuSurface into the scripted-UI session's WidgetBridge
// and install the per-vsync idle pump. This is the load-bearing handoff for
// scripted/GPU content: without the surface, the JS-side navigator.gpu /
// canvas.getContext('webgpu') bridge returns mocks and any JS-rendered/canvas
// output is black (the threejs-bridge "gpu_surface MUST be passed to
// WidgetBridge" gotcha). For a native-widget-bridge design (rasterized images
// + skeuo knobs, no live 3D) the surface is harmless but correct to attach.
// Idempotent: a null surface (CPU host) is a safe detach.
void wire_scripted_session_to_host(PulpEmbedView* v) {
    if (!v || !v->bridge || !v->host) return;
    auto* scripted = v->bridge->scripted_ui();
    if (!scripted) return;
    scripted->attach_gpu_surface(v->host->gpu_surface());
    // Pump the session's poll() once per display-link tick so timers /
    // requestAnimationFrame / async results keep running while embedded.
    v->host->set_idle_callback([scripted]() {
        std::string err;
        scripted->poll(&err);
    });
}

// ── parameter bridge wiring ────────────────────────────────────────────────

// Recursively classify a view as a bindable control. Returns true + writes the
// kind when the view is a Knob / Fader / Toggle, false otherwise.
bool classify_bindable(pulp::view::View* w, ParamWidgetKind* out_kind) {
    if (dynamic_cast<pulp::view::Knob*>(w))   { *out_kind = ParamWidgetKind::knob;   return true; }
    if (dynamic_cast<pulp::view::Fader*>(w))  { *out_kind = ParamWidgetKind::fader;  return true; }
    if (dynamic_cast<pulp::view::Toggle*>(w)) { *out_kind = ParamWidgetKind::toggle; return true; }
    return false;
}

// Read a control's current normalized [0,1] value.
float widget_normalized(const ParamBinding& b) {
    if (b.frame_element_index >= 0) {  // faithful-vector SVG-patch knob
        float v = static_cast<pulp::view::DesignFrameView*>(b.widget)
                      ->element_value(b.frame_element_index);
        return v < 0.0f ? 0.0f : v;    // element_value returns -1 for bad index
    }
    switch (b.kind) {
        case ParamWidgetKind::knob:
            return static_cast<pulp::view::Knob*>(b.widget)->value();
        case ParamWidgetKind::fader:
            return static_cast<pulp::view::Fader*>(b.widget)->value();
        case ParamWidgetKind::toggle:
            return static_cast<pulp::view::Toggle*>(b.widget)->is_on() ? 1.0f : 0.0f;
    }
    return 0.0f;
}

// Apply a normalized [0,1] value to a control (programmatic; does NOT fire the
// widget's on_change — see widgets.cpp set_value / set_on, which only repaint).
void widget_set_normalized(const ParamBinding& b, float v) {
    if (b.frame_element_index >= 0) {  // faithful-vector SVG-patch knob
        static_cast<pulp::view::DesignFrameView*>(b.widget)
            ->set_element_value(b.frame_element_index, v);
        return;
    }
    switch (b.kind) {
        case ParamWidgetKind::knob:
            static_cast<pulp::view::Knob*>(b.widget)->set_value(v); break;
        case ParamWidgetKind::fader:
            static_cast<pulp::view::Fader*>(b.widget)->set_value(v); break;
        case ParamWidgetKind::toggle:
            static_cast<pulp::view::Toggle*>(b.widget)->set_on(v > 0.5f); break;
    }
}

// First DesignFrameView in the tree (the faithful-vector frame), or nullptr.
pulp::view::DesignFrameView* find_design_frame_view(pulp::view::View* v) {
    if (!v) return nullptr;
    if (auto* f = dynamic_cast<pulp::view::DesignFrameView*>(v)) return f;
    for (size_t i = 0; i < v->child_count(); ++i)
        if (auto* f = find_design_frame_view(v->child_at(i))) return f;
    return nullptr;
}

// Walk the view tree depth-first, collecting bindable controls in document
// order so the param index is stable and matches the design's reading order.
void collect_bindable(pulp::view::View* v, std::vector<ParamBinding>& out) {
    if (!v) return;
    ParamWidgetKind kind;
    if (!v->id().empty() && classify_bindable(v, &kind)) {
        ParamBinding b;
        b.widget_id = v->id();
        b.key = v->id();        // default key == widget id (no metadata in ui.js)
        b.kind = kind;
        b.widget = v;
        out.push_back(std::move(b));
    }
    for (size_t i = 0; i < v->child_count(); ++i)
        collect_bindable(v->child_at(i), out);
}

// Install the UI->host hooks on one control. Composes with (does not clobber)
// any on_change the WidgetBridge already installed for JS dispatch.
void wire_widget_to_host(PulpEmbedView* v, const ParamBinding& b) {
    auto* store = v->store.get();
    const pulp::state::ParamID pid = b.param_id;

    if (b.kind == ParamWidgetKind::knob || b.kind == ParamWidgetKind::fader) {
        auto prev_change = (b.kind == ParamWidgetKind::knob)
            ? static_cast<pulp::view::Knob*>(b.widget)->on_change
            : static_cast<pulp::view::Fader*>(b.widget)->on_change;

        auto on_change = [v, store, pid, prev_change](float val) {
            if (prev_change) prev_change(val);  // keep JS dispatch alive
            // UI-driven write: store is the source of truth, and its listener
            // forwards to host.set_param (applying_host_change is false here).
            store->set_normalized(pid, val);
        };
        auto on_begin = [store, pid]() { store->begin_gesture(pid); };
        auto on_end   = [store, pid]() { store->end_gesture(pid); };

        if (b.kind == ParamWidgetKind::knob) {
            auto* k = static_cast<pulp::view::Knob*>(b.widget);
            k->on_change = on_change;
            k->on_gesture_begin = on_begin;
            k->on_gesture_end = on_end;
        } else {
            auto* f = static_cast<pulp::view::Fader*>(b.widget);
            f->on_change = on_change;
            f->on_gesture_begin = on_begin;
            f->on_gesture_end = on_end;
        }
    } else {  // toggle — no gesture phases; click = begin/set/end atomically
        auto* t = static_cast<pulp::view::Toggle*>(b.widget);
        auto prev = t->on_toggle;
        t->on_toggle = [v, store, pid, prev](bool on) {
            if (prev) prev(on);
            store->begin_gesture(pid);
            store->set_normalized(pid, on ? 1.0f : 0.0f);
            store->end_gesture(pid);
        };
    }
}

// Build the parameter registry: discover bindable controls, register one
// StateStore param per control (name == key), seed it from the widget, wire the
// UI->host hooks, and install a single store listener that forwards UI-driven
// value changes to host.set_param. Gesture begin/end forward through
// StateStore::set_gesture_callbacks. Idempotent per view (called once at create).
void build_param_bridge(PulpEmbedView* v) {
    if (!v || !v->store || !v->bridge) return;
    auto* root = v->bridge->view();
    if (!root) return;

    // Reload-safe: a rebuild (pulp_embed_reload_bundle) must reuse the StateStore
    // params that already exist for a key — the store has no remove API, and the
    // host binds by key. Snapshot key->param_id, drop the old widget bindings +
    // listener, then re-collect; existing keys reuse their store param, new keys
    // allocate a fresh id past the current max. (First call: prev is empty, so
    // ids are 1..N exactly as before.)
    std::unordered_map<std::string, pulp::state::ParamID> prev;
    pulp::state::ParamID max_id = 0;
    for (const auto& b : v->params) {
        prev[b.key] = b.param_id;
        if (b.param_id > max_id) max_id = b.param_id;
    }
    v->param_listener.reset();
    v->params.clear();
    v->key_to_index.clear();

    collect_bindable(root, v->params);

    // Faithful-vector lane (v2): DesignFrameView's elements (SVG-patch knobs +
    // native overlay dropdown/tab/stepper) aren't Knob/Fader/Toggle widgets, so
    // collect_bindable misses them. Append one binding per value-bearing element,
    // keyed by the importer's source node id, targeting the frame view by element
    // index (read/written via DesignFrameView's uniform element_value/
    // set_element_value). The frame's on_element_changed / gesture callbacks are
    // wired event-driven at the end of this function. (text_field is text, not a
    // normalized param — skipped, still in-view interactive.)
    if (auto* ep = dynamic_cast<EmbedProcessor*>(v->processor.get())) {
        if (auto* frame = find_design_frame_view(root)) {
            for (auto& [idx, key] : ep->faithful_element_keys()) {
                ParamBinding b;
                b.key = key;
                b.widget_id = key;
                b.kind = ParamWidgetKind::knob;  // value carried via frame element
                b.widget = frame;
                b.frame_element_index = idx;
                v->params.push_back(std::move(b));
            }
        }
    }

    for (size_t i = 0; i < v->params.size(); ++i) {
        auto& b = v->params[i];
        // Reuse the store param for a key that already existed (reload); else
        // allocate a fresh id past the current max and register it once.
        auto reused = prev.find(b.key);
        const bool is_new = (reused == prev.end());
        b.param_id = is_new ? ++max_id : reused->second;
        v->key_to_index[b.key] = i;

        if (is_new) {
            pulp::state::ParamInfo info;
            info.id = b.param_id;
            info.name = b.key;
            info.range = pulp::state::ParamRange{0.0f, 1.0f, 0.0f, 0.0f};
            v->store->add_parameter(info);
        }

        // Seed: prefer the host's current value (automation/preset already set
        // before the editor opened), else the widget's imported default.
        float seed = widget_normalized(b);
        if (v->host_cb.get_param) {
            double hv = v->host_cb.get_param(v->host_ctx, b.key.c_str());
            if (hv >= 0.0 && hv <= 1.0) {
                seed = static_cast<float>(hv);
                widget_set_normalized(b, seed);
            }
        }
        v->store->set_normalized(b.param_id, seed);

        // Frame elements have no per-widget on_change; their UI->host forwarding
        // is wired event-driven via DesignFrameView::on_element_changed at the end
        // of this function. Other widgets wire their on_change here.
        if (b.frame_element_index < 0)
            wire_widget_to_host(v, b);
    }

    // Gesture begin/end forwarding (one set of callbacks for the whole store).
    if (v->host_cb.begin_gesture || v->host_cb.end_gesture) {
        PulpEmbedView* self = v;
        v->store->set_gesture_callbacks(
            [self](pulp::state::ParamID id) {
                if (!self->host_cb.begin_gesture) return;
                if (id == 0 || id > self->params.size()) return;
                self->host_cb.begin_gesture(self->host_ctx,
                                            self->params[id - 1].key.c_str());
            },
            [self](pulp::state::ParamID id) {
                if (!self->host_cb.end_gesture) return;
                if (id == 0 || id > self->params.size()) return;
                self->host_cb.end_gesture(self->host_ctx,
                                          self->params[id - 1].key.c_str());
            });
    }

    // Value-change forwarding: UI writes -> host.set_param. Suppressed while a
    // host-driven change is being applied (pulp_embed_param_changed).
    if (v->host_cb.set_param) {
        PulpEmbedView* self = v;
        v->param_listener = v->store->add_listener(
            [self](pulp::state::ParamID id, float /*denorm*/) {
                if (self->applying_host_change) return;          // break the loop
                if (!self->host_cb.set_param) return;
                if (id == 0 || id > self->params.size()) return;
                const auto& b = self->params[id - 1];
                self->host_cb.set_param(self->host_ctx, b.key.c_str(),
                                        self->store->get_normalized(id));
            },
            pulp::state::ListenerThread::Main);
    }

    // Faithful-vector lane (v2, event-driven): route DesignFrameView's USER
    // changes + gestures through the SAME store -> listener -> host path as every
    // other control — no per-tick poll. set_element_value is silent, so a
    // host-driven push (pulp_embed_param_changed) does NOT echo back through
    // on_element_changed. Covers knobs AND choice controls (dropdown/tab/stepper)
    // uniformly via the frame's element index.
    if (auto* frame = find_design_frame_view(root)) {
        PulpEmbedView* self = v;
        auto pid_for = [self](int idx) -> pulp::state::ParamID {
            for (const auto& b : self->params)
                if (b.frame_element_index == idx) return b.param_id;
            return 0;
        };
        frame->on_element_changed = [self, pid_for](int idx, float val) {
            if (self->applying_host_change) return;             // break the loop
            if (auto pid = pid_for(idx)) self->store->set_normalized(pid, val);
        };
        frame->on_gesture_begin = [self, pid_for](int idx) {
            if (auto pid = pid_for(idx)) self->store->begin_gesture(pid);
        };
        frame->on_gesture_end = [self, pid_for](int idx) {
            if (auto pid = pid_for(idx)) self->store->end_gesture(pid);
        };
    }
}

// Poll host meters once (called from tick). Designs without meter widgets and
// hosts without a read_meters callback make this a no-op. The figma fixture has
// no meters, so this is currently a forwarding stub kept ready for meter-bearing
// designs (see report note).
void poll_host_meters(PulpEmbedView* v) {
    if (!v || !v->host_cb.read_meters) return;
    constexpr int kCap = pulp::view::MeterData::max_channels;
    float levels[kCap] = {};
    int n = v->host_cb.read_meters(v->host_ctx, levels, kCap);
    (void)n;
    // No meter widgets in the current importer JS bundle surface. When the
    // scripted bundle gains createMeter bindings, route `levels` through the
    // session's AudioBridge here.
}

// Shared create path for the high-fidelity scripted-UI bundle. bundle_dir must
// contain ui.js; asset paths inside resolve absolute or relative to bundle_dir.
// When `offscreen` is true no PluginViewHost is created (the host pulls frames).
PulpEmbedResult create_from_bundle(const PulpEmbedDesc* desc,
                                   const std::string& bundle_dir,
                                   bool offscreen,
                                   PulpEmbedView** out_view) {
    if (out_view) *out_view = nullptr;
    g_create_error.clear();
    if (auto r = check_desc(desc); r != PULP_EMBED_OK) {
        g_create_error = "invalid descriptor";
        return r;
    }
    if (!out_view) return PULP_EMBED_ERR_INVALID_ARG;

    namespace fs = std::filesystem;
    const fs::path script = fs::path(bundle_dir) / "ui.js";
    if (!fs::exists(script)) {
        g_create_error = "bundle missing ui.js: " + script.string();
        return PULP_EMBED_ERR_PARSE;
    }

    auto v = std::make_unique<PulpEmbedView>();
    v->offscreen = offscreen;
    // Capture host callbacks up front so resolve_resource can stage bundle
    // assets BEFORE the script runs and the renderer loads them.
    capture_host_callbacks(v.get(), desc);

    const auto w = static_cast<uint32_t>(desc->logical_width);
    const auto h = static_cast<uint32_t>(desc->logical_height);
    v->size_hints = pulp::format::view_size_from_design(w, h);

    auto proc = std::make_unique<EmbedScriptedProcessor>(script, fs::path(bundle_dir), v->size_hints);

    // Resource session (ABI v3): offer each asset path in ui.js to the host's
    // resolve_resource; stage the bytes it serves and install id -> staged-path
    // overrides the JS resolver consults before the bundle-dir fallback.
    if (v->host_cb.resolve_resource) {
        std::ifstream uin(script, std::ios::binary);
        if (uin) {
            std::ostringstream us;
            us << uin.rdbuf();
            proc->set_asset_overrides(stage_bundle_resources(v.get(), us.str()));
        }
    }

    try {
        proc->load_or_throw();
    } catch (const std::exception& e) {
        g_create_error = std::string("bundle load failed: ") + e.what();
        return PULP_EMBED_ERR_VIEW_OPEN;
    }
    v->processor = std::move(proc);
    v->store = std::make_unique<pulp::state::StateStore>();
    v->bridge = std::make_unique<pulp::format::ViewBridge>(*v->processor, *v->store);

    std::string err;
    if (!v->bridge->open(&err)) {
        g_create_error = "view open failed: " + err;
        return PULP_EMBED_ERR_VIEW_OPEN;
    }
    if (!v->bridge->view()) {
        g_create_error = "scripted view tree is empty";
        return PULP_EMBED_ERR_MATERIALIZE;
    }

    if (auto* rv = v->bridge->view()) {
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h)});
        rv->layout_children();
    }

    if (!offscreen) {
        pulp::view::PluginViewHost::Options opts;
        opts.size = {w, h};
        opts.use_gpu = (desc->backend_pref != PULP_EMBED_BACKEND_PREF_CPU);
        v->host = pulp::view::PluginViewHost::create(*v->bridge->view(), opts);
        if (!v->host) {
            g_create_error = "no PluginViewHost (missing platform factory?)";
            return PULP_EMBED_ERR_HOST_CREATE;
        }
        v->backend = v->host->is_gpu_backed() ? PULP_EMBED_BACKEND_GPU
                                              : PULP_EMBED_BACKEND_CPU;

        // Load-bearing for scripted/GPU fidelity — see wire_scripted_session_to_host.
        wire_scripted_session_to_host(v.get());
    } else {
        // Offscreen: no live host / display-link; frames come from the
        // deterministic renderer. The scripted session still loaded + ran, so
        // poll() once so timers/rAF/async asset loads settle before first pull.
        v->backend = PULP_EMBED_BACKEND_CPU;
        if (auto* scripted = v->bridge->scripted_ui()) {
            std::string perr;
            scripted->poll(&perr);
        }
    }

    // Interactive parameter bridge (ABI v2): discover the design's controls,
    // register them in the StateStore, and wire UI<->host param + gesture flow.
    build_param_bridge(v.get());

    if (!offscreen && desc->design_width > 0 && desc->design_height > 0) {
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
        return create_from_json(desc, ss.str(), base_dir, /*offscreen=*/false, out_view);
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
        return create_from_json(desc, std::string(json, json_len), base_dir, /*offscreen=*/false, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_create_from_ui_bundle(const PulpEmbedDesc* desc,
                                                 const char* bundle_dir,
                                                 PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!bundle_dir) { g_create_error = "null bundle_dir"; return PULP_EMBED_ERR_INVALID_ARG; }
        return create_from_bundle(desc, std::string(bundle_dir), /*offscreen=*/false, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_create_offscreen(const PulpEmbedDesc* desc,
                                            const char* source,
                                            int32_t from_bundle,
                                            PulpEmbedView** out_view) {
    try {
        if (out_view) *out_view = nullptr;
        if (!source) { g_create_error = "null source"; return PULP_EMBED_ERR_INVALID_ARG; }
        if (from_bundle) {
            return create_from_bundle(desc, std::string(source), /*offscreen=*/true, out_view);
        }
        std::ifstream f(source, std::ios::binary);
        if (!f) { g_create_error = std::string("cannot open ") + source; return PULP_EMBED_ERR_PARSE; }
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string base_dir = std::filesystem::path(source).parent_path().string();
        return create_from_json(desc, ss.str(), base_dir, /*offscreen=*/true, out_view);
    } catch (const std::exception& e) {
        g_create_error = std::string("internal: ") + e.what();
        return PULP_EMBED_ERR_INTERNAL;
    } catch (...) {
        g_create_error = "internal: unknown exception";
        return PULP_EMBED_ERR_INTERNAL;
    }
}

PulpEmbedResult pulp_embed_render_frame_rgba(PulpEmbedView* v, int32_t width,
                                             int32_t height, float scale,
                                             uint8_t* out, size_t cap, int32_t* w,
                                             int32_t* h, int32_t* stride) {
    if (!v || !v->bridge || width <= 0 || height <= 0)
        return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto* rv = v->bridge->view();
        if (!rv) return set_err(v, PULP_EMBED_ERR_MATERIALIZE, "no view");
        rv->set_bounds({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});
        rv->layout_children();

        uint32_t pw = 0, ph = 0;
        std::vector<uint8_t> rgba = pulp::view::render_to_rgba(
            *rv, static_cast<uint32_t>(width), static_cast<uint32_t>(height),
            scale > 0.0f ? scale : 1.0f, &pw, &ph);
        if (rgba.empty() || pw == 0 || ph == 0)
            return set_err(v, PULP_EMBED_ERR_UNSUPPORTED,
                           "render_to_rgba produced no pixels (no Skia backend?)");

        const int32_t row = static_cast<int32_t>(pw) * 4;
        if (w) *w = static_cast<int32_t>(pw);
        if (h) *h = static_cast<int32_t>(ph);
        if (stride) *stride = row;
        if (!out) return PULP_EMBED_OK;  // sizing query
        if (cap < rgba.size()) return PULP_EMBED_ERR_BUFFER_TOO_SMALL;
        std::memcpy(out, rgba.data(), rgba.size());
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "render_frame_rgba threw");
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

PulpEmbedResult pulp_embed_resize(PulpEmbedView* v, int32_t w, int32_t h, float scale) {
    if (!v || !v->host || !v->bridge || w <= 0 || h <= 0) return PULP_EMBED_ERR_INVALID_ARG;
    // `scale` is validated but advisory for the windowed/native embed path:
    // PluginViewHost has no backing-scale setter — the attached NSWindow's
    // backingScaleFactor drives surface DPI. Only the explicit deterministic
    // capture APIs (pulp_embed_render_png / pulp_embed_render_frame_rgba) honor
    // a caller-supplied scale. We still reject a non-finite/non-positive scale
    // so a host bug surfaces here instead of producing a degenerate surface.
    if (!(scale > 0.0f) || !std::isfinite(scale)) return PULP_EMBED_ERR_INVALID_ARG;
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
        // Drain host param writes queued from the audio thread, then pull the
        // latest meter levels. (Faithful-vector control changes are forwarded
        // event-driven via DesignFrameView::on_element_changed, not polled here.)
        if (v->store) v->store->pump_listeners();
        poll_host_meters(v);
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

PulpEmbedResult pulp_embed_reload_bundle(PulpEmbedView* v, const char* bundle_dir) {
    if (!v) return PULP_EMBED_ERR_INVALID_ARG;
    // Rebuilds views + touches the GPU surface — must run on the creator thread.
    if (std::this_thread::get_id() != v->creator_thread)
        return set_err(v, PULP_EMBED_ERR_WRONG_THREAD,
                       "reload must be called on the thread that created the view");
    auto* sp = dynamic_cast<EmbedScriptedProcessor*>(v->processor.get());
    if (!sp)
        return set_err(v, PULP_EMBED_ERR_UNSUPPORTED,
                       "reload is supported only for the scripted bundle path "
                       "(create_from_ui_bundle)");
    try {
        std::string err;
        const std::filesystem::path dir =
            bundle_dir ? std::filesystem::path(bundle_dir) : std::filesystem::path();
        // probe-first / last-good lives in ScriptedUiSession::reload_from: on
        // failure the running UI is untouched and we report the error.
        if (!sp->reload(dir, &err))
            return set_err(v, PULP_EMBED_ERR_INTERNAL,
                           err.empty() ? "reload failed (last-good UI kept)" : err);
        // The widget tree was rebuilt — rebuild the param bridge (reuses store
        // params by key; resets the old bindings/listener internally).
        build_param_bridge(v);
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "reload threw");
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

// ── parameter bridge (ABI v2) ──────────────────────────────────────────────

namespace {
// Copy `s` into buf (NUL-terminated, truncated to cap); return s.size().
size_t copy_str(const std::string& s, char* buf, size_t cap) {
    if (buf && cap) {
        const size_t n = s.size() < cap - 1 ? s.size() : cap - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = '\0';
    }
    return s.size();
}
}  // namespace

int32_t pulp_embed_param_count(PulpEmbedView* v) {
    return v ? static_cast<int32_t>(v->params.size()) : 0;
}

size_t pulp_embed_param_key(PulpEmbedView* v, int32_t index, char* buf, size_t cap) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->params.size()) {
        if (buf && cap) buf[0] = '\0';
        return 0;
    }
    return copy_str(v->params[static_cast<size_t>(index)].key, buf, cap);
}

size_t pulp_embed_param_widget_id(PulpEmbedView* v, int32_t index, char* buf, size_t cap) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->params.size()) {
        if (buf && cap) buf[0] = '\0';
        return 0;
    }
    return copy_str(v->params[static_cast<size_t>(index)].widget_id, buf, cap);
}

double pulp_embed_param_value(PulpEmbedView* v, int32_t index) {
    if (!v || !v->store || index < 0 ||
        static_cast<size_t>(index) >= v->params.size())
        return -1.0;
    return static_cast<double>(
        v->store->get_normalized(v->params[static_cast<size_t>(index)].param_id));
}

PulpEmbedResult pulp_embed_simulate_param_drag(PulpEmbedView* v, int32_t index, double target) {
    if (!v || index < 0 || static_cast<size_t>(index) >= v->params.size())
        return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto& b = v->params[static_cast<size_t>(index)];
        auto* w = b.widget;
        if (!w) return set_err(v, PULP_EMBED_ERR_INVALID_ARG, "param widget gone");
        const float tgt = static_cast<float>(target < 0.0 ? 0.0 : (target > 1.0 ? 1.0 : target));

        if (b.frame_element_index >= 0) {
            // Faithful-vector lane: `widget` is the DesignFrameView, not a
            // Knob/Fader/Toggle, so the widget-cast paths below don't apply.
            // Drive the SAME begin -> change -> end callbacks a real drag on the
            // frame fires (wired in build_param_bridge), so UI->host forwarding
            // and the visual both move. set_element_value updates the visual
            // silently; on_element_changed drives the store -> host.set_param path.
            auto* frame = static_cast<pulp::view::DesignFrameView*>(w);
            const int el = b.frame_element_index;
            if (frame->on_gesture_begin) frame->on_gesture_begin(el);
            frame->set_element_value(el, tgt);
            if (frame->on_element_changed) frame->on_element_changed(el, tgt);
            if (frame->on_gesture_end) frame->on_gesture_end(el);
        } else if (b.kind == ParamWidgetKind::knob) {
            // Knob drag is delta-based: down records start value at start_y;
            // drag up by (target-cur)*150 px reaches the target (widgets.cpp).
            auto* k = static_cast<pulp::view::Knob*>(w);
            const float cur = k->value();
            const float y0 = 1000.0f;
            k->on_mouse_down({0.0f, y0});                       // fires gesture_begin
            k->on_mouse_drag({0.0f, y0 - (tgt - cur) * 150.0f}); // fires on_change
            k->on_mouse_up({0.0f, y0 - (tgt - cur) * 150.0f});   // fires gesture_end
        } else if (b.kind == ParamWidgetKind::fader) {
            // Fader maps local position to value over its bounds.
            auto* f = static_cast<pulp::view::Fader*>(w);
            const auto lb = f->local_bounds();
            // Vertical default: value = 1 - y/height. Compute the target y.
            const float yh = lb.height > 0 ? lb.height : 150.0f;
            const float y = (1.0f - tgt) * yh;
            f->on_mouse_down({lb.width * 0.5f, y});  // begin + set
            f->on_mouse_drag({lb.width * 0.5f, y});  // change
            f->on_mouse_up({lb.width * 0.5f, y});    // end
        } else {  // toggle
            auto* t = static_cast<pulp::view::Toggle*>(w);
            const bool want = tgt > 0.5f;
            if (t->is_on() != want) t->on_mouse_down({0.0f, 0.0f});  // flips + fires on_toggle
        }
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "simulate_param_drag threw");
    }
}

PulpEmbedResult pulp_embed_param_changed(PulpEmbedView* v, const char* key, double normalized) {
    if (!v || !key || !v->store) return PULP_EMBED_ERR_INVALID_ARG;
    try {
        auto it = v->key_to_index.find(key);
        if (it == v->key_to_index.end()) return PULP_EMBED_OK;  // unknown key: no-op
        auto& b = v->params[it->second];

        const float val = static_cast<float>(
            normalized < 0.0 ? 0.0 : (normalized > 1.0 ? 1.0 : normalized));

        // Suppress the store listener's host-forward so a host-driven change
        // does not echo back to host.set_param (feedback-loop break).
        v->applying_host_change = true;
        v->store->set_normalized(b.param_id, val);
        v->applying_host_change = false;

        // Mirror into the live widget (set_value/set_on and DesignFrameView::
        // set_element_value are silent — no on_change / on_element_changed — so
        // this stays a one-way host->view push) and repaint.
        if (b.widget) widget_set_normalized(b, val);
        if (v->host) v->host->repaint();
        return PULP_EMBED_OK;
    } catch (const std::exception& e) {
        v->applying_host_change = false;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, e.what());
    } catch (...) {
        v->applying_host_change = false;
        return set_err(v, PULP_EMBED_ERR_INTERNAL, "param_changed threw");
    }
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
        // For the high-fidelity scripted path the processor owns the
        // ScriptedUiSession (+ its WidgetBridge), which holds a View& into the
        // bridge-owned root. Destroy that session BEFORE the bridge frees the
        // root, or the WidgetBridge destructor touches freed memory. The
        // DesignIR/native path has no session and this is a no-op.
        if (auto* sp = dynamic_cast<EmbedScriptedProcessor*>(v->processor.get())) {
            sp->release_session();
        }
        if (v->bridge) { v->bridge->close(); v->bridge.reset(); }
        v->processor.reset();
        // Drop the param-bridge subscriptions (which capture `v`) BEFORE the
        // store they target is destroyed. The widgets the param bindings borrow
        // are already gone with the bridge above; null them defensively.
        v->param_listener.reset();
        if (v->store) v->store->set_gesture_callbacks(nullptr, nullptr);
        for (auto& b : v->params) b.widget = nullptr;
        v->store.reset();
        // Remove the host-resource staging dir (resolve_resource), if any. The
        // renderer is gone, so the staged files are no longer referenced.
        if (!v->staging_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(v->staging_dir, ec);
        }
    } catch (...) {
        // swallow — destroy must not throw across the C boundary
    }
    delete v;
}

}  // extern "C"
