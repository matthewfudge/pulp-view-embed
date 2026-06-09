// pulp-embed-validate — a build-time preflight for an embed design bundle.
//
// Validates a Pulp-imported design AT THE SEAM a foreign host (JUCE / iPlug2)
// consumes it, BEFORE the design is embedded into a plugin and handed to the
// real plugin validators. It is complementary to auval / pluginval /
// clap-validator (and JUCE's pluginval): those validate the assembled plugin
// BINARY and understand nothing about a Pulp design bundle; this validates that
// the design parses, materializes, renders non-blank, resolves its assets, and
// exposes the parameter bridge the host expects.
//
//   pulp-embed-validate <bundle-dir | design.ir.json> [options]
//     --design-w N --design-h N   logical/design size (default 1000x600)
//     --scale F                   render scale for the non-blank check (default 1)
//     --host-keys k1,k2,...       the host's param keys; report bound vs
//                                 visual-only (design keys with no host param)
//                                 and dangling host keys (no design control)
//     --out path.png              write the preflight render (default: none)
//     --golden ref.png            byte-compare the render against a reference
//
// Pure C++ (no window / GPU back-buffer needed): rendering uses the deterministic
// Skia raster path (pulp_embed_render_png), so it runs headless and is portable
// to the Win/Linux hosts. Exit code 0 = all checks pass, 1 = a check failed,
// 2 = usage error.

#include "pulp_view_embed.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const std::string& what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what.c_str());
    if (!ok) ++g_fail;
}
void info(const std::string& what) { std::printf("  [info] %s\n", what.c_str()); }

bool is_dir(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFDIR);
}
bool file_exists(const std::string& p) {
    struct stat st {};
    return ::stat(p.c_str(), &st) == 0 && (st.st_mode & S_IFREG);
}
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// A blank/degenerate PNG compresses to almost nothing; real content does not.
bool looks_nonblank(const std::vector<uint8_t>& png, int w, int h) {
    if (png.size() < 8 || w <= 0 || h <= 0) return false;
    return static_cast<double>(png.size()) / (double(w) * double(h)) > 0.02;
}

std::vector<std::string> split_csv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') { if (!cur.empty()) out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

PulpEmbedView* create(const std::string& src, bool is_bundle, int w, int h) {
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
    const PulpEmbedResult r = is_bundle
        ? pulp_embed_create_from_ui_bundle(&d, src.c_str(), &v)
        : pulp_embed_create_from_design_json(&d, src.c_str(), &v);
    if (r != PULP_EMBED_OK || !v) {
        char buf[1024] = {0};
        pulp_embed_last_create_error(buf, sizeof buf);
        std::fprintf(stderr, "create failed: result=%d err=%s\n", r, buf);
        return nullptr;
    }
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: %s <bundle-dir | design.ir.json> [--design-w N --design-h N]\n"
            "          [--scale F] [--host-keys k1,k2,...] [--out png] [--golden png]\n",
            argv[0]);
        return 2;
    }
    const std::string src = argv[1];
    int W = 1000, H = 600;
    float scale = 1.0f;
    std::string host_keys_csv, out_png, golden_png;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* what) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (a == "--design-w") W = std::atoi(next("--design-w").c_str());
        else if (a == "--design-h") H = std::atoi(next("--design-h").c_str());
        else if (a == "--scale") scale = static_cast<float>(std::atof(next("--scale").c_str()));
        else if (a == "--host-keys") host_keys_csv = next("--host-keys");
        else if (a == "--out") out_png = next("--out");
        else if (a == "--golden") golden_png = next("--golden");
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return 2; }
    }

    const bool is_bundle = is_dir(src) || file_exists(src + "/ui.js");
    std::printf("pulp-embed-validate: %s (%s, %dx%d)\n", src.c_str(),
                is_bundle ? "bundle" : "DesignIR", W, H);

    // ── 1) parse + materialize ──────────────────────────────────────────────
    PulpEmbedView* v = create(src, is_bundle, W, H);
    check(v != nullptr, "design parses + materializes through the embed ABI");
    if (!v) { std::printf("FAILED (could not create view)\n"); return 1; }
    info(std::string("active backend: ") +
         (pulp_embed_active_backend(v) == PULP_EMBED_BACKEND_GPU ? "GPU"
          : pulp_embed_active_backend(v) == PULP_EMBED_BACKEND_CPU ? "CPU" : "UNKNOWN"));

    // ── 2) parameter-bridge report ──────────────────────────────────────────
    const int n = pulp_embed_param_count(v);
    info("bindable controls: " + std::to_string(n));
    std::set<std::string> design_keys;
    int empty_keys = 0, dup_keys = 0;
    for (int i = 0; i < n; ++i) {
        char key[256] = {0};
        pulp_embed_param_key(v, i, key, sizeof key);
        if (key[0] == '\0') ++empty_keys;
        else if (!design_keys.insert(key).second) ++dup_keys;
    }
    check(empty_keys == 0, "every bindable control key is non-empty");
    check(dup_keys == 0, "no duplicate control keys (would collide in the host bridge)");

    // Bound vs visual-only — only meaningful if the host supplies its param keys.
    if (!host_keys_csv.empty()) {
        const auto hk = split_csv(host_keys_csv);
        std::set<std::string> host_keys(hk.begin(), hk.end());
        int bound = 0;
        std::vector<std::string> visual_only, dangling;
        for (const auto& k : design_keys)
            (host_keys.count(k) ? ++bound : (visual_only.push_back(k), 0));
        for (const auto& k : host_keys)
            if (!design_keys.count(k)) dangling.push_back(k);
        info("design controls bound to a host param: " + std::to_string(bound) +
             " / " + std::to_string(design_keys.size()));
        for (const auto& k : visual_only) info("  visual-only (no host param): " + k);
        for (const auto& k : dangling) info("  dangling host key (no design control): " + k);
        check(bound > 0, "at least one design control binds to a host param");
    } else {
        info("(pass --host-keys to report bound vs visual-only controls)");
    }

    // Per-control metadata (ABI v5) — what a host needs to BUILD a correct param
    // from the design (greenfield) or VERIFY its own. Reported, not gated.
    {
        int discrete = 0, continuous = 0;
        for (int i = 0; i < n; ++i) {
            PulpEmbedParamInfo pi{};
            if (pulp_embed_param_info(v, i, &pi) != PULP_EMBED_OK) continue;
            char key[256] = {0};
            pulp_embed_param_key(v, i, key, sizeof key);
            (pi.is_discrete ? ++discrete : ++continuous);
            char line[420];
            std::snprintf(line, sizeof line,
                "  %-22s kind=%-9s %s default=%.3f%s%s", key, pi.widget_kind,
                pi.is_discrete ? "discrete" : "continuous", pi.default_norm,
                pi.option_count ? (std::string(" options=") + std::to_string(pi.option_count)).c_str() : "",
                pi.has_meta ? (std::string(" name=\"") + pi.name + "\" unit=\"" + pi.unit + "\"").c_str() : "");
            info(line);
        }
        info("param metadata: " + std::to_string(continuous) + " continuous, " +
             std::to_string(discrete) + " discrete");
    }

    // ── 3) asset resolution (the placeholder-render trap) ───────────────────
    // Check the assets the RENDER actually consumes, not every manifest entry.
    // DesignIR: faithful frames render `svg_asset_id`; fonts reference an
    //   `asset_id`. The manifest also carries unreferenced FALLBACK rasters (the
    //   non-faithful PNGs) that the faithful render never loads — flagging those
    //   as missing would be a false positive, so they are a warning, not a fail.
    // Bundle: every "assets/..." literal in ui.js must exist under the bundle.
    {
        int missing = 0, checked = 0;
        if (!is_bundle) {
            // Authoritative: the shim already walked the DesignIR asset manifest
            // at create time and recorded exactly the faithful-frame svg refs
            // whose resolved local_path is absent on disk (ABI v7). Querying it
            // avoids re-implementing JSON + manifest parsing here and stays in
            // lockstep with what the render actually consumes — the unreferenced
            // fallback rasters are not flagged, so there is no false positive.
            const int nmissing = pulp_embed_missing_asset_count(v);
            for (int i = 0; i < nmissing; ++i) {
                char path[1024] = {0};
                pulp_embed_missing_asset(v, i, path, sizeof path);
                info(std::string("  MISSING render asset: ") + path);
            }
            missing = nmissing;
            info("render assets missing: " + std::to_string(missing) +
                 " (faithful-frame svg refs, via embed ABI)");
            check(missing == 0, "every render-referenced asset resolves on disk (no placeholder)");
        } else {
            const std::string ui = read_file(src + "/ui.js");
            // Asset refs in the importer's ui.js are quoted string literals (the
            // CLI emits SINGLE quotes, e.g. setImageSource('id','assets/x.png'))
            // ending in a known image/font extension; accept ' and " and check
            // absolute and bundle-relative.
            std::set<std::string> seen;
            static const char* exts[] = {".png", ".jpg", ".jpeg", ".svg", ".webp",
                                         ".ttf", ".otf", ".woff", ".woff2"};
            for (char q : {'\'', '"'}) {
                size_t i = 0;
                while ((i = ui.find(q, i)) != std::string::npos) {
                    size_t j = ui.find(q, i + 1);
                    if (j == std::string::npos) break;
                    std::string lit = ui.substr(i + 1, j - i - 1);
                    i = j + 1;
                    bool asset = false;
                    for (auto* e : exts) if (lit.size() > strlen(e) &&
                        lit.compare(lit.size() - strlen(e), strlen(e), e) == 0) { asset = true; break; }
                    if (!asset || !seen.insert(lit).second) continue;
                    ++checked;
                    const std::string full = (!lit.empty() && lit[0] == '/') ? lit : src + "/" + lit;
                    if (!file_exists(full)) { ++missing; info("  MISSING asset: " + lit); }
                }
            }
            info("bundle assets referenced: " + std::to_string(checked) +
                 ", missing: " + std::to_string(missing));
            check(missing == 0, "every referenced asset resolves on disk (no placeholder render)");
        }
    }

    // ── 4) deterministic render is non-blank ────────────────────────────────
    {
        size_t need = 0;
        std::vector<uint8_t> png;
        PulpEmbedResult r = pulp_embed_render_png(v, W, H, scale, nullptr, 0, &need);
        if (r == PULP_EMBED_OK && need > 0) {
            png.resize(need);
            r = pulp_embed_render_png(v, W, H, scale, png.data(), png.size(), &need);
        }
        check(r == PULP_EMBED_OK && looks_nonblank(png, W, H),
              "deterministic Skia render is non-blank");
        if (!out_png.empty() && !png.empty()) {
            std::ofstream(out_png, std::ios::binary)
                .write(reinterpret_cast<const char*>(png.data()),
                       static_cast<std::streamsize>(png.size()));
            info("wrote render to " + out_png);
        }
        if (!golden_png.empty()) {
            const std::string g = read_file(golden_png);
            // Byte-exact compare (the render is deterministic). A perceptual /
            // masked diff is a future refinement (see the v3 plan §3).
            check(!g.empty() && g.size() == png.size() &&
                      std::memcmp(g.data(), png.data(), png.size()) == 0,
                  "render byte-matches the golden reference");
        }
    }

    pulp_embed_destroy(v);
    std::printf("%s\n", g_fail == 0 ? "pulp-embed-validate: OK"
                                    : "pulp-embed-validate: FAILED");
    return g_fail == 0 ? 0 : 1;
}
