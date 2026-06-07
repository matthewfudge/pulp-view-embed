# Distributing pulp-view-embed

How to ship the embedding SDK so a foreign C++ host can consume it without
building Pulp from source. Three deliverables:

1. A **package manifest** (`pulp-package.json`) — cargo-like, for `pulp add`.
2. A **shippable shared library** (`libpulp_view_embed.dylib`) with a
   `find_package(pulp_view_embed)` config and a symbol surface pinned to the
   C ABI.
3. A **published-SDK tarball** so consumers don't have to build the Pulp SDK
   from the embedding-seam branch.

Everything below is verified on macOS (arm64). Non-Apple platforms are not yet
covered (the offscreen RGBA producer and the GPU host are macOS-only today).

---

## 1. Package manifest (`pulp-package.json`)

`pulp-package.json` is a [Pulp registry v2](https://github.com/danielraffel/pulp/blob/main/tools/packages/registry-schema.json)-shaped
package definition. It validates against the core registry schema, so it is
drop-in for `pulp add` the moment the package is registered in Pulp's
`tools/packages/registry.json`, and the FetchContent form it implies is usable
directly today.

### Consumer flow A — once registered in the Pulp registry

```bash
# Inside a Pulp project:
pulp add pulp-view-embed
# → generates cmake/pulp-packages.cmake (FetchContent), updates packages.lock.json
target_link_libraries(my_target PRIVATE pulp_view_embed)
```

### Consumer flow B — FetchContent directly (today, no registry needed)

```cmake
include(FetchContent)
FetchContent_Declare(pulp_view_embed
    GIT_REPOSITORY https://github.com/danielraffel/pulp-view-embed.git
    GIT_TAG        v0.1.0)
FetchContent_MakeAvailable(pulp_view_embed)
target_link_libraries(my_target PRIVATE pulp_view_embed)
```

> **Caveat (needs core/registry infra).** The source-fetch form still requires
> an installed Pulp SDK on `CMAKE_PREFIX_PATH` — pulp-view-embed's
> `CMakeLists.txt` does `find_package(Pulp CONFIG REQUIRED)` and cannot build
> standalone. Pulp's registry/`pulp add` model also assumes the consumer is a
> *Pulp plugin project*, not a foreign host, and has no "prebuilt binary /
> find_package config" fetch method. So full `pulp add` integration for a
> foreign host needs two things only Pulp core can add: (a) a registry entry,
> and (b) a fetch method that models a prebuilt SDK. Until then, the realistic
> foreign-host path is **flow 2 below** (prebuilt dylib + `find_package`).

---

## 2. Shippable shared library

`-DPULP_VIEW_EMBED_SHARED=ON` builds `libpulp_view_embed.dylib`, which statically
links the entire Pulp C++ runtime (Skia, Dawn, Abseil, …) and **exports only the
26 `pulp_embed_*` C ABI symbols**. A foreign host links it with no Pulp build and
no exposure to Pulp's C++ symbols.

### Build + install the dist

```bash
cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release \
      -DPULP_VIEW_EMBED_SHARED=ON \
      -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install
cmake --build build-shared -j
cmake --install build-shared --prefix /path/to/dist
```

Install layout (relocatable):

```
dist/
├── include/pulp_view_embed.h
└── lib/
    ├── libpulp_view_embed.dylib            # @rpath install_name, @loader_path rpath
    ├── libwgpu_native.dylib                # the one non-system runtime dep, bundled
    └── cmake/pulp_view_embed/
        ├── pulp_view_embedConfig.cmake
        ├── pulp_view_embedConfigVersion.cmake
        └── pulp_view_embedTargets*.cmake
```

### Consumer flow — `find_package(pulp_view_embed)`

```cmake
find_package(pulp_view_embed CONFIG REQUIRED)
target_link_libraries(my_host PRIVATE pulp_view_embed::pulp_view_embed)
```

```bash
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/dist
```

The host needs **neither a Pulp SDK nor `find_package(Pulp)`** — the dylib is
self-contained. `libwgpu_native.dylib` ships beside it and resolves at runtime
via `@loader_path`.

### Symbol-surface / linkage proof

```bash
# Exactly the 26 pulp_embed_* symbols, nothing else:
nm -gU dist/lib/libpulp_view_embed.dylib | wc -l          # 26
nm -gU dist/lib/libpulp_view_embed.dylib | grep -v ' _pulp_embed_'   # (empty)
nm -gU dist/lib/libpulp_view_embed.dylib | c++filt | grep -c 'pulp::' # 0

# Only system frameworks + the two @rpath deps (self + bundled wgpu):
otool -L dist/lib/libpulp_view_embed.dylib | grep -v '/System/\|/usr/lib/'
#   @rpath/libpulp_view_embed.0.dylib
#   @rpath/libwgpu_native.dylib
```

The pin is enforced by `src/pulp_view_embed.exp` (a `-exported_symbols_list`
passed to ld64). **When you add a public function to `pulp_view_embed.h`, add
its `_`-prefixed symbol to that file** or it won't be visible.

### Code-signing & notarization (documented; needs an Apple Developer ID)

The dylib ships **unsigned**. To distribute it signed + notarized you need an
Apple Developer ID. Follow the same commands the Pulp `ship` skill drives
(`pulp ship` wraps `codesign` + `notarytool` + `stapler`):

```bash
# 1. Sign with a hardened runtime + secure timestamp.
codesign --force --timestamp --options runtime \
    --sign "Developer ID Application: YOUR NAME (TEAMID)" \
    dist/lib/libpulp_view_embed.dylib
# Sign the bundled dependency too — every Mach-O in the dist must be signed.
codesign --force --timestamp --options runtime \
    --sign "Developer ID Application: YOUR NAME (TEAMID)" \
    dist/lib/libwgpu_native.dylib

# 2. A bare .dylib cannot be notarized directly — notarytool needs a
#    .zip/.dmg/.pkg. Zip the dist, submit, and wait.
ditto -c -k --keepParent dist /tmp/pulp-view-embed-dist.zip
xcrun notarytool submit /tmp/pulp-view-embed-dist.zip \
    --apple-id "you@example.com" --team-id TEAMID \
    --password "app-specific-password" --wait
#  (App Store Connect API key is preferred over apple-id/password:
#   xcrun notarytool submit … --key AuthKey_XXXX.p8 --key-id KID --issuer ISS)

# 3. Stapling: a loose .dylib has nowhere to staple a ticket. Ship the dylib
#    inside a notarized .dmg/.pkg and staple THAT container, or rely on online
#    Gatekeeper checks. Verify the container, then the lib:
xcrun stapler staple /tmp/pulp-view-embed.dmg          # if shipping a .dmg/.pkg
codesign --verify --strict --verbose=2 dist/lib/libpulp_view_embed.dylib
spctl --assess --type exec --verbose dist/lib/libpulp_view_embed.dylib  # signature check
```

See `.agents/skills/ship/SKILL.md` in the Pulp repo for the full pipeline
(`pulp ship sign` / `pulp ship release` / `pulp ship check`).

---

## 3. Published-SDK tarball

Consumers shouldn't have to build the Pulp SDK from the `explore/foreign-host-embed`
branch. `tools/package-sdk.sh` snapshots an *already-installed* SDK into a
single relocatable tarball.

```bash
# Full SDK (all platforms, ~540 MB):
tools/package-sdk.sh

# macOS-only (drops iOS/visionOS Skia slices, ~80 MB):
tools/package-sdk.sh --mac-only --out dist/pulp-sdk-mac.tar.gz
```

The script verifies relocatability invariants before tarring (no build-machine
absolute paths in `lib/cmake`, every shipped `.dylib` uses an `@rpath`/`@loader_path`
install_name). The installed SDK is already relocatable because `PulpConfig.cmake`
resolves paths via `CMAKE_CURRENT_LIST_DIR` / `PACKAGE_PREFIX_DIR`.

### Consumer flow — untar + point `CMAKE_PREFIX_PATH`

```bash
tar xzf pulp-sdk-mac.tar.gz                  # → pulp-sdk-<version>/
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_VIEW_EMBED_SHARED=ON \
      -DCMAKE_PREFIX_PATH=$PWD/pulp-sdk-<version>
cmake --build build -j
ctest --test-dir build -R embed-smoke --output-on-failure
```

No Pulp checkout, no branch build. The clean-room build of pulp-view-embed
against an untarred tarball produces the same 26-symbol dylib and passes
`embed-smoke`.

---

## What still needs infra not in this repo

- **`pulp add` foreign-host integration** — a registry entry in Pulp core plus a
  registry fetch method that models a prebuilt SDK / `find_package` config
  (today the registry only models git-source FetchContent into a Pulp *plugin*).
  `pulp-package.json` is forward-compatible with the first; the second is a core
  change.
- **Code signing / notarization** — needs an Apple Developer ID; the recipe
  above is verified-by-construction but unrun here (no credentials).
- **Non-macOS** — the shared-lib hardening and tarball recipe are macOS-only so
  far, matching the embedding ABI's current platform support.
