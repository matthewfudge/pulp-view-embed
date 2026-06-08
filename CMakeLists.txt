# pulp-view-embed — flat C ABI for embedding a Pulp-imported frontend in a
# foreign C++ host. Consumes an INSTALLED Pulp SDK via find_package(Pulp CONFIG).
#
# Configure against the SDK installed from the seam branch:
#   cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install
#
# Distributable shared library (foreign host links a stable C ABI, no Pulp
# build, no Pulp C++ symbols):
#   cmake -S . -B build-shared -DCMAKE_BUILD_TYPE=Release \
#         -DPULP_VIEW_EMBED_SHARED=ON -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install
#   cmake --build build-shared -j
#   cmake --install build-shared --prefix /path/to/dist
# The install lays out include/ + lib/libpulp_view_embed.dylib +
# lib/libwgpu_native.dylib + lib/cmake/pulp_view_embed/pulp_view_embedConfig.cmake
# so a host can `find_package(pulp_view_embed CONFIG REQUIRED)`.
cmake_minimum_required(VERSION 3.24)
project(pulp_view_embed VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

find_package(Pulp CONFIG REQUIRED)

option(PULP_VIEW_EMBED_SHARED "Build the embedding ABI as a shared library" OFF)
if(PULP_VIEW_EMBED_SHARED)
    add_library(pulp_view_embed SHARED src/pulp_view_embed.cpp)
else()
    add_library(pulp_view_embed STATIC src/pulp_view_embed.cpp)
endif()
add_library(pulp_view_embed::pulp_view_embed ALIAS pulp_view_embed)

set_target_properties(pulp_view_embed PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR})

target_include_directories(pulp_view_embed PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

# Pulp::view carries the materializer + PluginViewHost; the rest are the
# subsystems the shim names directly. Skia comes transitively via Pulp::render.
target_link_libraries(pulp_view_embed PRIVATE
    Pulp::format Pulp::view Pulp::state Pulp::canvas Pulp::render)

if(APPLE)
    target_link_libraries(pulp_view_embed PRIVATE "-framework AppKit")
endif()

# ── Distribution hardening for the shared library ────────────────────────────
# Pin the exported symbol surface to exactly the pulp_embed_* C ABI and make the
# dylib relocatable (so a foreign host finds its sibling deps via @loader_path,
# not a baked absolute SDK path).
if(PULP_VIEW_EMBED_SHARED AND APPLE)
    set(_embed_exp "${CMAKE_CURRENT_SOURCE_DIR}/src/pulp_view_embed.exp")
    target_link_options(pulp_view_embed PRIVATE
        "-Wl,-exported_symbols_list,${_embed_exp}")
    set_property(TARGET pulp_view_embed APPEND
        PROPERTY LINK_DEPENDS "${_embed_exp}")
    set_target_properties(pulp_view_embed PROPERTIES
        # @rpath install_name so the consumer's own rpath resolves us.
        INSTALL_NAME_DIR "@rpath"
        MACOSX_RPATH ON
        # Find the bundled libwgpu_native.dylib next to us at runtime.
        BUILD_WITH_INSTALL_RPATH OFF
        INSTALL_RPATH "@loader_path")
endif()

# ── Install / export (find_package(pulp_view_embed CONFIG)) ──────────────────
# Only the shared-library build produces a self-contained, host-linkable
# artifact; the static build re-exposes Pulp's archives and is for in-tree use.
if(PULP_VIEW_EMBED_SHARED)
    install(TARGETS pulp_view_embed
        EXPORT pulp_view_embedTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(FILES include/pulp_view_embed.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # Bundle the one non-system runtime dependency (the WebGPU backend dylib)
    # next to ours so the dist is relocatable. Resolved from the Pulp SDK that
    # find_package(Pulp) located.
    if(APPLE)
        get_target_property(_pulp_sdk_dir Pulp::render INTERFACE_INCLUDE_DIRECTORIES)
        find_file(PULP_EMBED_WGPU_DYLIB
            NAMES libwgpu_native.dylib
            HINTS ${PULP_SDK_LIBRARY_DIR} ${PULP_SDK_DIR}/lib
            NO_DEFAULT_PATH)
        if(PULP_EMBED_WGPU_DYLIB)
            install(FILES "${PULP_EMBED_WGPU_DYLIB}"
                DESTINATION ${CMAKE_INSTALL_LIBDIR})
        else()
            message(WARNING
                "libwgpu_native.dylib not found in the Pulp SDK; the installed "
                "dylib will fail to load its WebGPU backend at runtime. Set "
                "CMAKE_PREFIX_PATH to a Pulp SDK that ships it.")
        endif()
    endif()

    install(EXPORT pulp_view_embedTargets
        FILE pulp_view_embedTargets.cmake
        NAMESPACE pulp_view_embed::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pulp_view_embed)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pulp_view_embedConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/pulp_view_embedConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pulp_view_embed)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/pulp_view_embedConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/pulp_view_embedConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/pulp_view_embedConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/pulp_view_embed)
endif()

# ── shim self-tests + harnesses ──────────────────────────────────────────────
# Only when pulp-view-embed is built DIRECTLY (top-level). When an adapter repo
# (pulp-embed-juce / -iplug2) pulls it in via add_subdirectory, these targets +
# ctests must NOT register into the adapter's suite — the adapter tests the
# adapter; the shim tests the shim. (Otherwise the adapter's ctest inherits
# embed-smoke / embed-validate-faithful and reports them "Not Run".)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)

# ── smoke harness (M1.1–M1.10) ───────────────────────────────────────────────
# macOS-only AppKit host: create from a DesignIR fixture, attach to a real
# NSView, resize, tick, capture, and stress create/destroy. Registered as a
# ctest so `ctest -R embed-smoke` runs the gates.
if(APPLE)
    enable_testing()
    add_executable(pulp-embed-smoke-mac examples/macos-nsview-smoke/smoke.mm)
    target_link_libraries(pulp-embed-smoke-mac PRIVATE
        pulp_view_embed "-framework AppKit")
    target_compile_definitions(pulp-embed-smoke-mac PRIVATE
        PULP_EMBED_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_test(NAME embed-smoke COMMAND pulp-embed-smoke-mac)

    # Standalone fidelity harness (not a ctest gate): render an importer JS
    # bundle through the high-fidelity scripted-UI path to a PNG for pixel
    # comparison against the importer's own --validate render.
    add_executable(pulp-embed-bundle-render examples/macos-nsview-smoke/bundle_render.mm)
    target_link_libraries(pulp-embed-bundle-render PRIVATE
        pulp_view_embed "-framework AppKit")

    # Sibling harness for the native DesignIR path — renders a DesignIR JSON
    # (including the faithful-vector lane: faithful_svg nodes → DesignFrameView
    # with native SVG-patch knobs + native overlay controls) to a PNG.
    add_executable(pulp-embed-design-render examples/macos-nsview-smoke/design_render.mm)
    target_link_libraries(pulp-embed-design-render PRIVATE
        pulp_view_embed "-framework AppKit")
endif()

# ── embed-bundle preflight (`pulp embed validate`) ───────────────────────────
# Build-time validation of a design bundle AT THE SEAM a foreign host consumes
# it (parse + materialize + non-blank render + asset resolution + param-bridge
# report) — complementary to the plugin-binary validators (auval/pluginval/
# clap-validator), which understand nothing about a Pulp design. Pure C++ /
# deterministic Skia render, so it runs headless and is portable to Win/Linux.
enable_testing()
add_executable(pulp-embed-validate tools/pulp_embed_validate.cpp)
target_link_libraries(pulp-embed-validate PRIVATE pulp_view_embed)
if(APPLE)
    target_link_libraries(pulp-embed-validate PRIVATE "-framework AppKit")
endif()
# Self-test: the faithful "VST Style" fixture must pass the preflight clean.
add_test(NAME embed-validate-faithful
    COMMAND pulp-embed-validate
        "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/figma-vst-style-faithful/design.ir.json"
        --design-w 1000 --design-h 600)

endif()  # top-level only (CMAKE_SOURCE_DIR == CMAKE_CURRENT_SOURCE_DIR)
