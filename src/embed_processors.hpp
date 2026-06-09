// embed_processors.hpp — the two pulp::format::Processor facades the embed shim
// hands to ViewBridge. PRIVATE to the pulp_view_embed library target (not in the
// install set): split out of pulp_view_embed.cpp purely to separate the
// "what view tree do we build" concern from the "how does the C ABI drive it"
// surface. Both classes are fully self-contained — they name only pulp:: + std
// types, never PulpEmbedView or the shim's free helpers — so this is a pure code
// move with no behavior change.
//
//   EmbedProcessor          — DesignIR materialized into a native view tree.
//   EmbedScriptedProcessor  — an importer JS bundle (ui.js) run through the same
//                             ScriptedUiSession + WidgetBridge pipeline as the
//                             importer's --validate render and real GPU plugins.
#ifndef PULP_VIEW_EMBED_PROCESSORS_HPP
#define PULP_VIEW_EMBED_PROCESSORS_HPP

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/design_ir.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace pulp::embed::shim {

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

    // Per-element metadata for the faithful lane (ABI v5 pulp_embed_param_info),
    // derived from the IRInteractiveElement the materializer already carries — no
    // pulp-core change needed. Index matches faithful_element_keys() / the
    // DesignFrameView element order. Choice controls (dropdown/tab/stepper) are
    // discrete; their default is selected_index normalized over the option span.
    struct FaithfulMeta {
        int element_index = -1;
        const char* kind = "knob";
        bool is_discrete = false;
        int option_count = 0;
        float default_norm = 0.5f;
        std::string label;  // §2.1 design caption (IRInteractiveElement.label); "" if none
    };
    // text_field elements (skipped by faithful_element_keys, which only covers
    // normalized params) — for the ABI v6 string bridge. (index, key) in frame
    // element order; key is the source node id, else a synthetic "text:i".
    std::vector<std::pair<int, std::string>> faithful_text_field_keys() const {
        std::vector<std::pair<int, std::string>> out;
        const pulp::view::IRNode* frame = find_faithful_node(ir_.root);
        if (!frame) return out;
        using K = pulp::view::InteractiveElementKind;
        for (int i = 0; i < static_cast<int>(frame->interactive_elements.size()); ++i) {
            const auto& e = frame->interactive_elements[static_cast<size_t>(i)];
            if (e.kind != K::text_field) continue;
            std::string key = e.source_node_id.value_or("");
            if (key.empty()) key = "text:" + std::to_string(i);
            out.emplace_back(i, std::move(key));
        }
        return out;
    }

    std::vector<FaithfulMeta> faithful_element_metas() const {
        std::vector<FaithfulMeta> out;
        const pulp::view::IRNode* frame = find_faithful_node(ir_.root);
        if (!frame) return out;
        using K = pulp::view::InteractiveElementKind;
        for (int i = 0; i < static_cast<int>(frame->interactive_elements.size()); ++i) {
            const auto& e = frame->interactive_elements[static_cast<size_t>(i)];
            if (e.kind == K::text_field) continue;  // skipped in keys() too
            FaithfulMeta m;
            m.element_index = i;
            switch (e.kind) {
                case K::knob:      m.kind = "knob";      m.is_discrete = false; break;
                case K::dropdown:  m.kind = "dropdown";  m.is_discrete = true;  break;
                case K::tab_group: m.kind = "tab_group"; m.is_discrete = true;  break;
                case K::stepper:   m.kind = "stepper";   m.is_discrete = true;  break;
                default:           m.kind = "knob";      m.is_discrete = false; break;
            }
            if (m.is_discrete) {
                m.option_count = static_cast<int>(e.options.size());
                const int span = m.option_count > 1 ? m.option_count - 1 : 1;
                m.default_norm = static_cast<float>(e.selected_index) / static_cast<float>(span);
            } else {
                m.option_count = 0;
                m.default_norm = e.default_value;
            }
            m.label = e.label;  // §2.1 caption -> generated-param name (empty if unnamed)
            out.push_back(m);
        }
        return out;
    }

    // The parsed DesignIR (asset local_paths already rewritten to absolute by
    // resolve_asset_paths) — for the missing-render-asset query (ABI v7).
    const pulp::view::DesignIR& ir() const { return ir_; }

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

}  // namespace pulp::embed::shim

#endif  // PULP_VIEW_EMBED_PROCESSORS_HPP
