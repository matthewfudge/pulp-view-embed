// pulp_view_embed.hpp — optional header-only C++ conveniences over the flat C
// ABI in pulp_view_embed.h. Adapters (JUCE / iPlug2 / bespoke) may use these
// instead of re-implementing the same enumeration loops; nothing here is
// required to use the C ABI. Depends ONLY on pulp_view_embed.h + the standard
// library — it names no framework type, so it composes with any host.
#ifndef PULP_VIEW_EMBED_HPP
#define PULP_VIEW_EMBED_HPP

#include "pulp_view_embed.h"

#include <string>
#include <vector>

namespace pulp::embed {

// One design control's parameter description (ABI v5 metadata), read from
// pulp_embed_param_info. Framework-neutral (std::string); a host that prefers
// its own string type converts at the boundary. `key` is the bind key (the
// design control's pulpParamKey, else its widget id). `is_discrete` +
// `option_count` choose a stepped/choice vs continuous host param;
// `default_norm` is the imported default [0,1]. `name`/`unit` are populated only
// when the importer carries them (else empty — fall back to `key`).
struct ParamDesc {
    std::string key;
    std::string widget_kind;   // "knob"/"fader"/"toggle"/"dropdown"/"tab_group"/"stepper"
    bool        is_discrete = false;
    int         option_count = 0;
    double      default_norm = 0.0;
    std::string name;          // "" until imported
    std::string unit;          // "" until imported
};

namespace detail {
inline std::string copy_key(PulpEmbedView* v, int32_t i) {
    char buf[256] = {0};
    pulp_embed_param_key(v, i, buf, sizeof buf);
    return buf;
}
}  // namespace detail

// Descriptors for every bindable control on a live view, in stable ABI order.
inline std::vector<ParamDesc> param_descs(PulpEmbedView* view) {
    std::vector<ParamDesc> out;
    if (view == nullptr) return out;
    const int32_t n = pulp_embed_param_count(view);
    for (int32_t i = 0; i < n; ++i) {
        PulpEmbedParamInfo pi{};
        if (pulp_embed_param_info(view, i, &pi) != PULP_EMBED_OK) continue;
        ParamDesc d;
        d.key = detail::copy_key(view, i);
        d.widget_kind = pi.widget_kind;
        d.is_discrete = pi.is_discrete != 0;
        d.option_count = pi.option_count;
        d.default_norm = pi.default_norm;
        if (pi.has_meta) { d.name = pi.name; d.unit = pi.unit; }
        out.push_back(std::move(d));
    }
    return out;
}

// Greenfield entry point: read a design's parameter descriptors WITHOUT an
// editor/window, so a plugin can declare its params at construction time. Builds
// an offscreen view (pulp_embed_create_offscreen), enumerates, tears it down.
// `source` is a bundle dir (containing ui.js) or a DesignIR JSON file —
// auto-detected by the caller via `from_bundle`. Empty vector on failure.
inline std::vector<ParamDesc> read_design_params(const std::string& source,
                                                 bool from_bundle,
                                                 int logical_width,
                                                 int logical_height) {
    PulpEmbedDesc d{};
    d.struct_size = sizeof(PulpEmbedDesc);
    d.abi_version = PULP_VIEW_EMBED_ABI_VERSION;
    d.logical_width = logical_width;
    d.logical_height = logical_height;
    d.scale_factor = 1.0f;
    d.backend_pref = PULP_EMBED_BACKEND_PREF_AUTO;
    d.design_width = logical_width;
    d.design_height = logical_height;
    PulpEmbedView* v = nullptr;
    if (pulp_embed_create_offscreen(&d, source.c_str(), from_bundle ? 1 : 0, &v) != PULP_EMBED_OK
        || v == nullptr)
        return {};
    auto out = param_descs(v);
    pulp_embed_destroy(v);
    return out;
}

}  // namespace pulp::embed

#endif  // PULP_VIEW_EMBED_HPP
