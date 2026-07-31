cmake_minimum_required(VERSION 3.20)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0")
project(TeleMatrix LANGUAGES CXX)

# Single-source application version. Consumed by the macOS bundle props, CPack,
# and a compile definition the C++ reads for the about page / QApplication.
# The release date feeds the AppStream metainfo (GNOME Software / KDE Discover).
#
# Don't hand-edit these: `scripts/release.sh <version>` bumps both, commits, tags and
# pushes. release.yml refuses to publish a tag that disagrees with TELEMATRIX_VERSION.
set(TELEMATRIX_VERSION "0.9.3")
set(TELEMATRIX_RELEASE_DATE "2026-07-29")

# Objective-C++ only exists on Apple platforms; the .mm sources are added
# conditionally further down.
if (APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Multimedia Concurrent Network Svg Test)
find_package(Qt6 REQUIRED COMPONENTS LinguistTools)

# Video streaming needs Qt's FFmpeg multimedia backend: the AVFoundation-only
# `darwin` backend can't play the loopback http stream from media_stream/, so the
# app silently falls back to downloading whole files before playing. Homebrew's
# qtmultimedia ships only libdarwinmediaplugin.dylib, and nothing catches that
# until runtime — fail here rather than produce a quietly degraded build.
#
# QT6_INSTALL_PREFIX/QT6_INSTALL_PLUGINS come from Qt6's QtInstallPaths.cmake,
# which both layouts provide (Homebrew says "share/qt/plugins", the official
# installer says "plugins"). Plain EXISTS, not find_program/find_file: a cached
# NOTFOUND would survive installing the plugin and re-configuring.
# Linux is covered post-build by cmake/package_appimage.sh.
if (APPLE)
    set(TELEMATRIX_QT_FFMPEG_PLUGIN
        "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/multimedia/libffmpegmediaplugin.dylib")
    if (NOT EXISTS "${TELEMATRIX_QT_FFMPEG_PLUGIN}")
        message(FATAL_ERROR
            "Qt at ${QT6_INSTALL_PREFIX} has no FFmpeg multimedia backend.\n"
            "  expected: ${TELEMATRIX_QT_FFMPEG_PLUGIN}\n"
            "Homebrew's qt does not ship it, so video would fall back to a full "
            "download instead of streaming. Install official Qt ${Qt6_VERSION} "
            "(online installer or aqtinstall) and re-configure against it:\n"
            "  rm -rf build\n"
            "  cmake -S . -B build -DCMAKE_PREFIX_PATH=\"$ENV{HOME}/Qt/${Qt6_VERSION}/macos\"")
    endif()
endif()

# On macOS libav comes from Qt, not Homebrew. Official Qt ships universal
# (arm64+x86_64) FFmpeg 7.1 dylibs alongside the multimedia backend checked
# above, and the .app carries them either way — the playback plugin needs them.
# Homebrew's libav was arm64-only (which is what blocked universal builds), a
# SECOND FFmpeg in the bundle (~37 MB once its transitive deps are counted), and
# GPL where Qt's build is LGPL.
#
# Qt ships the dylibs but no headers and no .pc files. The headers are vendored
# (third_party/ffmpeg-7.1, see its README); the .pc files are generated here into
# the build tree so no absolute path is ever committed. Both consumers — the C++
# link below and ffmpeg-sys-next's build script — read them via PKG_CONFIG_PATH.
if (APPLE)
    set(TM_QT_LIB_DIR "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}")
    set(TM_FFMPEG_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/ffmpeg-7.1/include")
    set(TM_FFMPEG_PKGCONFIG_DIR "${CMAKE_BINARY_DIR}/ffmpeg-pkgconfig")

    # The vendored headers, the ffmpeg-next pin in rust/Cargo.toml and these
    # sonames are one unit: ffmpeg-next's major tracks FFmpeg's, and FFmpeg's
    # major is what names the dylibs. A Qt upgrade that moves to FFmpeg 8 renames
    # all five at once. Catch that here — the alternative is an opaque linker
    # error, or worse, 7.1 headers silently describing an 8.x ABI.
    #
    # swresample is not linked (nothing calls it directly) but must be present:
    # it is a runtime dependency of avcodec and the app will not launch without it.
    set(TM_QT_FFMPEG_LINK_SONAMES libavcodec.61 libavformat.61 libavutil.59 libswscale.8)
    set(TM_QT_FFMPEG_SONAMES ${TM_QT_FFMPEG_LINK_SONAMES} libswresample.5)
    foreach (_tm_soname IN LISTS TM_QT_FFMPEG_SONAMES)
        if (NOT EXISTS "${TM_QT_LIB_DIR}/${_tm_soname}.dylib")
            message(FATAL_ERROR
                "Qt at ${QT6_INSTALL_PREFIX} does not ship the FFmpeg libraries this build expects.\n"
                "  expected: ${TM_QT_LIB_DIR}/${_tm_soname}.dylib\n"
                "Qt's bundled FFmpeg has most likely changed major version. Re-pin all three "
                "together (see third_party/ffmpeg-7.1/README.md):\n"
                "  1. re-vendor third_party/ffmpeg-<ver>/include at Qt's FFmpeg version\n"
                "  2. bump ffmpeg-next in rust/Cargo.toml to the matching major\n"
                "  3. update these sonames and the versions in cmake/ffmpeg-pc/*.pc.in")
        endif()
    endforeach()

    # @ONLY: the templates use pkg-config's own ${libdir}/${includedir} syntax,
    # which CMake would otherwise try to expand.
    foreach (_tm_pc libavutil libswresample libswscale libavcodec libavformat)
        configure_file(
            "${CMAKE_SOURCE_DIR}/cmake/ffmpeg-pc/${_tm_pc}.pc.in"
            "${TM_FFMPEG_PKGCONFIG_DIR}/${_tm_pc}.pc"
            @ONLY)
    endforeach()
endif()

# Same idea, every platform: the emoji sprite atlases (resources/emoji/*.webp) are WebP,
# and WebP is not in qtbase — it comes from the qtimageformats add-on. Without the plugin
# QImage(path, "WEBP") returns null and every emoji renders blank, which is precisely the
# bug the sprite port exists to fix. Catch it at configure time rather than let a user
# discover it by staring at empty squares. tests/tst_emoji_atlas.cpp is the runtime gate.
if (WIN32)
    set(TELEMATRIX_QT_WEBP_PLUGIN "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/imageformats/qwebp.dll")
elseif (APPLE)
    set(TELEMATRIX_QT_WEBP_PLUGIN "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/imageformats/libqwebp.dylib")
else ()
    set(TELEMATRIX_QT_WEBP_PLUGIN "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}/imageformats/libqwebp.so")
endif ()
if (NOT EXISTS "${TELEMATRIX_QT_WEBP_PLUGIN}")
    message(FATAL_ERROR
        "Qt at ${QT6_INSTALL_PREFIX} has no WebP image plugin.\n"
        "  expected: ${TELEMATRIX_QT_WEBP_PLUGIN}\n"
        "It ships in the qtimageformats module, which qtbase does not include. The emoji "
        "sprite atlases cannot be decoded without it. Install it and re-configure:\n"
        "  aqtinstall:   aqt install-qt <host> desktop ${Qt6_VERSION} <arch> -m qtimageformats -O ~/Qt\n"
        "  Debian/Ubuntu: apt install qt6-imageformats-plugins\n"
        "  Fedora/RHEL:   dnf install qt6-qtimageformats")
endif ()

# Emoji data tables, generated from lib/lib_ui/emoji.txt by desktop-app's codegen_emoji.
include("${CMAKE_SOURCE_DIR}/cmake/codegen_emoji.cmake")

# Rust static library.
# Keep this in sync with C++ by rebuilding when Rust sources change.
# MSVC emits telematrix_protocol.lib; Unix toolchains emit libtelematrix_protocol.a.
#
# Match the cargo profile to CMAKE_BUILD_TYPE so a Debug C++ build links a debug
# (unoptimized, debuggable) Rust staticlib and every other build type links the
# optimized release one. Debug uses the `dev-ffi` profile (a debuggable dev build
# with panic=abort — see rust/Cargo.toml); Release, RelWithDebInfo, MinSizeRel
# and an empty/unspecified type all use --release. Both abort on panic, so a
# panic never unwinds across the FFI boundary regardless of build type, and the
# debug staticlib carries no unwind tables (avoids the __eh_frame > 16 MB ld
# warning). `cargo test` keeps using the plain unwinding `dev` profile.
set(CARGO_BUILD_COMMAND cargo build)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND CARGO_BUILD_COMMAND --profile dev-ffi)
    set(CARGO_TARGET_SUBDIR "dev-ffi")
else()
    list(APPEND CARGO_BUILD_COMMAND --release)
    set(CARGO_TARGET_SUBDIR "release")
endif()
if (WIN32)
    set(RUST_LIB_PATH "${CMAKE_SOURCE_DIR}/rust/target/${CARGO_TARGET_SUBDIR}/telematrix_protocol.lib")
else()
    set(RUST_LIB_PATH "${CMAKE_SOURCE_DIR}/rust/target/${CARGO_TARGET_SUBDIR}/libtelematrix_protocol.a")
endif()
set(PROTOCOL_BINDINGS_DIR "${CMAKE_BINARY_DIR}/generated/protocol")
set(PROTOCOL_FFI_HEADER "${PROTOCOL_BINDINGS_DIR}/protocol_ffi.h")
file(GLOB_RECURSE TELEMATRIX_RUST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/rust/src/*.rs"
    "${CMAKE_SOURCE_DIR}/rust/Cargo.toml"
    "${CMAKE_SOURCE_DIR}/rust/Cargo.lock"
    "${CMAKE_SOURCE_DIR}/rust/build.rs"
    "${CMAKE_SOURCE_DIR}/rust/cbindgen.toml"
    "${CMAKE_SOURCE_DIR}/rust/cbindgen_runner/src/*.rs"
    "${CMAKE_SOURCE_DIR}/rust/cbindgen_runner/Cargo.toml"
)
# Environment for the cargo build. The macOS deployment target is only
# meaningful (and only set) on Apple.
set(CARGO_BUILD_ENV "TELEMATRIX_BINDINGS_DIR=${PROTOCOL_BINDINGS_DIR}")
if (APPLE)
    list(APPEND CARGO_BUILD_ENV "MACOSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}")
    # ffmpeg-sys-next resolves libav through pkg-config in its build script, so it
    # needs the same Qt-backed .pc files the C++ link uses. Without this it finds
    # (or fails to find) a system FFmpeg of some unrelated version.
    list(APPEND CARGO_BUILD_ENV "PKG_CONFIG_PATH=${TM_FFMPEG_PKGCONFIG_DIR}")
endif()

# Assemble the Rust build step(s). When the C++ build targets multiple macOS
# architectures (CMAKE_OSX_ARCHITECTURES="arm64;x86_64") the single-arch host
# staticlib can't be linked, so build cargo for each arch and `lipo` the results
# into the universal RUST_LIB_PATH. Single-arch / non-mac builds are unchanged.
list(LENGTH CMAKE_OSX_ARCHITECTURES _tm_narch)
if (APPLE AND _tm_narch GREATER 1)
    # The universal archive does NOT go to rust/target/${CARGO_TARGET_SUBDIR}/:
    # that is exactly where a plain host `cargo build` writes its own thin
    # staticlib, so the two would silently overwrite each other and CMake's
    # staleness check — keyed on the path, not its contents — would never notice.
    # The per-target cargo dirs still live under rust/target/, so wiping build/
    # costs a re-lipo (seconds), not a Rust rebuild.
    set(RUST_LIB_PATH "${CMAKE_BINARY_DIR}/rust-universal/libtelematrix_protocol.a")
    set(_tm_rust_steps "")
    set(_tm_lipo_inputs "")
    foreach (_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
        if (_arch STREQUAL "arm64")
            set(_triple "aarch64-apple-darwin")
        elseif (_arch STREQUAL "x86_64")
            set(_triple "x86_64-apple-darwin")
        else()
            message(FATAL_ERROR "Unsupported CMAKE_OSX_ARCHITECTURES value for the Rust staticlib: ${_arch}")
        endif()
        # Per-slice, so they cannot go in the shared CARGO_BUILD_ENV:
        #   PKG_CONFIG_ALLOW_CROSS - pkg-config-rs refuses to run for any target
        #     that isn't the host, and every non-host slice is a cross build. The
        #     .pc files point at universal dylibs, so one set serves both.
        #   BINDGEN_EXTRA_CLANG_ARGS - ffmpeg-sys-next runs bindgen, which would
        #     otherwise parse the headers as the host arch.
        list(APPEND _tm_rust_steps
            COMMAND ${CMAKE_COMMAND} -E env ${CARGO_BUILD_ENV}
                "PKG_CONFIG_ALLOW_CROSS=1"
                "BINDGEN_EXTRA_CLANG_ARGS=--target=${_triple}"
                ${CARGO_BUILD_COMMAND} --target ${_triple})
        list(APPEND _tm_lipo_inputs
            "${CMAKE_SOURCE_DIR}/rust/target/${_triple}/${CARGO_TARGET_SUBDIR}/libtelematrix_protocol.a")
    endforeach()
    list(APPEND _tm_rust_steps
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/rust-universal"
        COMMAND lipo -create ${_tm_lipo_inputs} -output "${RUST_LIB_PATH}")
else()
    set(_tm_rust_steps
        COMMAND ${CMAKE_COMMAND} -E env ${CARGO_BUILD_ENV} ${CARGO_BUILD_COMMAND})
endif()

# Rust staticlib. RUST_LIB_PATH lives in cargo's own target dir, OUTSIDE the
# CMake build tree, so blowing away `build/` costs a C++ recompile rather than a
# full (slow, disk-hungry) Rust rebuild. (The universal build is the exception —
# see above — but only the lipo output is in the build tree, not cargo's own
# per-target dirs, so the same property holds.)
add_custom_command(
    OUTPUT "${RUST_LIB_PATH}"
    ${_tm_rust_steps}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/rust"
    DEPENDS ${TELEMATRIX_RUST_SOURCES}
    COMMENT "Building Rust protocol library"
    VERBATIM
)

# C FFI header — a SEPARATE custom command from the staticlib above. The header
# is a per-build-tree output while RUST_LIB_PATH is not, so a single multi-output
# command keyed (by the Makefile generator) on the shared lib would skip the
# header whenever cargo considered the lib up to date — leaving the tree without
# a header a clean had just deleted, and stale when a new FFI export is added.
# `cargo run` ALWAYS executes the binary, so the header is rebuilt whenever this
# fires.
# DEPENDS on RUST_LIB_PATH only to order it after the lib build (avoid two
# concurrent cargo invocations), not for staleness — the header's own timestamp
# vs. the Rust sources governs regeneration.
add_custom_command(
    OUTPUT "${PROTOCOL_FFI_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PROTOCOL_BINDINGS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E env
        "TELEMATRIX_CRATE_DIR=${CMAKE_SOURCE_DIR}/rust"
        "TELEMATRIX_BINDINGS_DIR=${PROTOCOL_BINDINGS_DIR}"
        cargo run --quiet --manifest-path cbindgen_runner/Cargo.toml
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/rust"
    DEPENDS ${TELEMATRIX_RUST_SOURCES} "${RUST_LIB_PATH}"
    COMMENT "Generating C FFI header"
    VERBATIM
)
add_custom_target(telematrix_protocol_build DEPENDS "${RUST_LIB_PATH}" "${PROTOCOL_FFI_HEADER}")

add_library(telematrix_protocol STATIC IMPORTED GLOBAL)
set_target_properties(telematrix_protocol PROPERTIES IMPORTED_LOCATION "${RUST_LIB_PATH}")
add_dependencies(telematrix_protocol telematrix_protocol_build)

# Theme resources are compiled with an explicit rcc call rather than by AUTORCC.
# AUTORCC emits its .cpp into a nested per-config autogen dir, and the object
# follows it there — but a target consuming an OBJECT library has to name that
# library's objects by path, and the Visual Studio generator writes the flat
# telematrix_core.dir/<Config>/qrc_theme_resources.obj into TeleMatrix's link
# line. The two disagree and the Windows link dies with LNK1181 on a file that
# was built. qt_add_resources produces an ordinary source in the binary dir, so
# its object name is flat and both sides agree on every generator.
qt_add_resources(TELEMATRIX_THEME_QRC src/theme/theme_resources.qrc)

# The emoji sprite atlases (~9 MB) get the same explicit-rcc treatment, and for the
# same reason. Decoding them needs the WebP plugin from the qtimageformats add-on,
# which qtbase does not ship — see resources/emoji/README.md.
qt_add_resources(TELEMATRIX_EMOJI_QRC resources/emoji/emoji.qrc)

# Reusable "core" of pure, runtime-light translation units (no QtWidgets, no
# ProtocolBridge/Rust FFI). Compiled once as an OBJECT library so both the
# TeleMatrix executable and the unit-test runners (tests/) link the SAME objects
# without recompiling. Members are deliberately limited to TUs that depend only
# on Qt6::Core/Gui (+ the header-only protocol_types.h/timeline_item_content.h)
# so the test binaries stay small and headless-friendly.
add_library(telematrix_core OBJECT
    src/protocol/timeline_item_content.h
    src/protocol/timeline_item_content.cpp
    src/protocol/ffi_conversions.h
    src/protocol/ffi_conversions.cpp
    src/dialogs/room_directory_filter.h
    src/dialogs/room_directory_filter.cpp
    src/dialogs/saved_messages.h
    src/dialogs/saved_messages.cpp
    src/protocol/media_retry_policy.h
    src/protocol/media_retry_policy.cpp
    src/core/core_settings.h
    src/core/core_settings.cpp
    src/core/localization.h
    src/core/localization.cpp
    src/app/unread_state_store.h
    src/app/unread_state_store.cpp
    src/app/account_index.h
    src/app/account_index.cpp
    src/app/dialogs_width.h
    src/app/dialogs_width.cpp
    src/history/history_return_stack.h
    src/history/history_return_stack.cpp
    src/history/history_scroll_state.h
    src/history/history_scroll_state.cpp
    src/history/history_selection_state.h
    src/history/history_selection_state.cpp
    src/history/message_index.h
    src/history/message_index.cpp
    src/history/mention_trigger.h
    src/history/mention_trigger.cpp
    src/history/compose_html.h
    src/history/compose_html.cpp
    src/history/jump_load_controller.h
    src/history/jump_load_controller.cpp
    src/history/unread_bar_placement.h
    src/history/unread_bar_placement.cpp
    src/history/bubble_corners_cache.h
    src/history/bubble_corners_cache.cpp
    src/history/image_recompress.h
    src/history/image_recompress.cpp
    src/history/heic_decode.h
    src/history/heic_decode.cpp
    src/history/emoji_data.h
    src/history/emoji_data.cpp
    src/history/emoji_picker_keys.h
    src/history/emoji_picker_keys.cpp
    src/history/history_video_placeholder.h
    src/history/history_video_placeholder.cpp
    src/media/video_container.h
    src/media/video_rebuffer.h
    src/media/video_rebuffer.cpp
    src/media/video_stream_retry.h
    src/media/video_stream_retry.cpp
    src/media/video_frame_scaler.h
    src/media/video_frame_scaler.cpp
    src/media/video_download_overlay_policy.h
    src/media/video_download_overlay_policy.cpp
    src/ui/format_bytes.h
    src/ui/format_bytes.cpp
    src/ui/safe_url.h
    src/ui/safe_url.cpp
    src/ui/recovery_key_format.cpp
    src/ui/qr_code_image.h
    src/ui/qr_code_image.cpp
    # Sprite-sheet emoji, ported from desktop-app/lib_ui. emoji.cpp is generated
    # (cmake/codegen_emoji.cmake) and #includes "ui/emoji_config.h" by that exact path,
    # which is why the port keeps upstream's file layout. In telematrix_core so the
    # emoji tests link the same objects the app does.
    # Pure scale/DPR state, moved here from the executable: emoji_sprites.cpp sizes its
    # cached pixmaps in device pixels, so the tests need it too.
    src/ui/style/runtime_scale.h
    src/ui/style/runtime_scale.cpp
    src/ui/emoji_compat.h
    src/ui/emoji_config.h
    src/ui/emoji_config.cpp
    src/ui/emoji_sprites.h
    src/ui/emoji_sprites.cpp
    # Emoji inside running text: placeholder substitution + a sprite pass over
    # QTextLayout. Style-free (all sizes are parameters) so it stays testable.
    src/ui/text/emoji_text.h
    src/ui/text/emoji_text.cpp
    "${TM_EMOJI_GEN_DIR}/emoji.cpp"
    # emoji.cpp calls InitReplacements() from the generated suggestions table; the
    # hand-written half of that pair is upstream's, and is free of Qt and lib_base.
    "${TM_EMOJI_GEN_DIR}/emoji_suggestions_data.cpp"
    lib/lib_ui/emoji_suggestions/emoji_suggestions.cpp
    src/ui/widgets/block_format_presence.h
    src/ui/widgets/block_format_presence.cpp
    src/theme/theme_parser.h
    src/theme/theme_parser.cpp
    src/theme/theme_registry.h
    src/theme/theme_registry.cpp
    src/theme/chat_background.h
    src/theme/chat_background.cpp
    # Carries the per-theme palettes and doodle patterns, which theme_registry.cpp
    # and chat_background.cpp read. Lives here rather than on TeleMatrix so the
    # unit tests can load them too. Generated above by qt_add_resources, not
    # AUTORCC — see the comment on that call.
    ${TELEMATRIX_THEME_QRC}
    # Emoji sprite atlases, here rather than on TeleMatrix so tst_emoji_atlas can
    # assert they decode — that test is the gate that catches a missing WebP plugin.
    ${TELEMATRIX_EMOJI_QRC}
    # Pure helpers shared by the platform toast managers; cross-platform so they
    # are unit-tested on any host.
    src/window/notifications_win_keys.h
    src/window/notifications_win_keys.cpp
    src/window/notifications_linux_registry.h
    src/window/notifications_linux_registry.cpp
)
target_include_directories(telematrix_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
# Emoji: the generated tables, upstream's emoji_suggestions headers, and GSL (the
# generated emoji.cpp uses gsl::make_span). SYSTEM so vendored headers do not spray
# warnings through our builds.
target_include_directories(telematrix_core SYSTEM PUBLIC
    "${TM_EMOJI_GEN_DIR}"
    "${CMAKE_SOURCE_DIR}/lib/lib_ui/emoji_suggestions"
    "${CMAKE_SOURCE_DIR}/lib/GSL/include")
# The generated sources must exist before anything compiles them.
add_dependencies(telematrix_core telematrix_emoji_codegen)
# Qt6::Svg depends on Gui, not Widgets, so the test runners stay headless.
target_link_libraries(telematrix_core PUBLIC Qt6::Core Qt6::Gui Qt6::Svg)
if(APPLE)
    # Native HEIC/HEIF decode (heic_decode.cpp) via macOS ImageIO. PUBLIC so the
    # app and the unit-test runners that link this object library resolve it too.
    target_link_libraries(telematrix_core PUBLIC
        "-framework ImageIO"
        "-framework CoreGraphics"
        "-framework CoreFoundation")
endif()

add_executable(TeleMatrix
    src/main.cpp
    src/app/app_icon.qrc
    src/protocol/protocol_bridge.h
    src/protocol/protocol_bridge.cpp
    src/protocol/media_cache.h
    src/protocol/media_cache.cpp
    src/storage/storage_local.h
    src/storage/storage_local.cpp
    src/app/app_controller.h
    src/app/app_controller.cpp
    src/app/account.h
    src/app/account.cpp
    src/app/account_domain.h
    src/app/account_domain.cpp
    src/app/tray_icon.h
    src/app/tray_icon.cpp
    src/app/single_instance.h
    src/app/single_instance.cpp
    # Auto-update. Lives under src/core/ but belongs to the executable, not
    # telematrix_core: it needs the generated FFI header and TELEMATRIX_VERSION_STR,
    # neither of which the OBJECT library (and therefore the unit-test runners) has.
    # The logic worth testing — manifest parsing, semver, signature and version
    # binding — is all Rust-side and covered by `cargo test`.
    src/core/update_service.h
    src/core/update_service.cpp
    src/core/updater.h
    src/core/updater.cpp
    src/app/app_main_window.h
    src/app/app_main_window.cpp
    src/app/app_main_widget.h
    src/app/app_main_widget.cpp
    src/app/network_monitor.h
    src/app/network_monitor.cpp
    src/media/media_view_overlay.h
    src/media/media_view_overlay.cpp
    src/dialogs/dialogs_row.h
    src/dialogs/dialogs_row.cpp
    src/dialogs/dialogs_layout.h
    src/dialogs/dialogs_layout.cpp
    src/dialogs/dialogs_filter_sidebar.h
    src/dialogs/dialogs_filter_sidebar.cpp
    src/dialogs/dialogs_main_menu_overlay.h
    src/dialogs/dialogs_main_menu_overlay.cpp
    src/dialogs/dialogs_main_menu_panel.h
    src/dialogs/dialogs_main_menu_panel.cpp
    src/dialogs/dialogs_folders_box.h
    src/dialogs/dialogs_folders_box.cpp
    src/dialogs/dialogs_edit_folder_box.h
    src/dialogs/dialogs_edit_folder_box.cpp
    src/dialogs/dialogs_include_chats_box.h
    src/dialogs/dialogs_include_chats_box.cpp
    src/dialogs/dialogs_leave_space_box.h
    src/dialogs/dialogs_leave_space_box.cpp
    src/dialogs/dialogs_intro_box.h
    src/dialogs/dialogs_intro_box.cpp
    src/dialogs/dialogs_new_chat_box.h
    src/dialogs/dialogs_new_chat_box.cpp
    src/dialogs/dialogs_explore_rooms_box.h
    src/dialogs/dialogs_explore_rooms_box.cpp
    src/dialogs/dialogs_room_info_box.h
    src/dialogs/dialogs_room_info_box.cpp
    src/dialogs/dialogs_room_create_dialog.h
    src/dialogs/dialogs_room_create_dialog.cpp
    src/dialogs/dialogs_member_picker_box.h
    src/dialogs/dialogs_member_picker_box.cpp
    src/dialogs/dialogs_invite_users_box.h
    src/dialogs/dialogs_invite_users_box.cpp
    src/dialogs/dialogs_chat_search_in.h
    src/dialogs/dialogs_chat_search_in.cpp
    src/dialogs/dialogs_inner.h
    src/dialogs/dialogs_inner.cpp
    src/dialogs/dialogs_widget.h
    src/dialogs/dialogs_widget.cpp
    src/dialogs/dialogs_update_bar.h
    src/dialogs/dialogs_update_bar.cpp
    src/dialogs/dialogs_icons.qrc
    src/intro/intro_step.h
    src/intro/intro_step.cpp
    src/intro/intro_start.h
    src/intro/intro_start.cpp
    src/intro/intro_secret_backend.h
    src/intro/intro_secret_backend.cpp
    src/intro/intro_create_password.h
    src/intro/intro_create_password.cpp
    src/intro/intro_vault_unlock.h
    src/intro/intro_vault_unlock.cpp
    src/intro/intro_login.h
    src/intro/intro_login.cpp
    src/intro/intro_register.h
    src/intro/intro_register.cpp
    src/intro/intro_constants.h
    src/intro/intro_forgot_password.h
    src/intro/intro_forgot_password.cpp
    src/intro/intro_verify_choice.h
    src/intro/intro_verify_choice.cpp
    src/intro/intro_verify_emoji.h
    src/intro/intro_verify_emoji.cpp
    src/intro/intro_verify_qr.h
    src/intro/intro_verify_qr.cpp
    src/intro/intro_verify_recovery_key.h
    src/intro/intro_verify_recovery_key.cpp
    src/intro/intro_verify_success.h
    src/intro/intro_verify_success.cpp
    src/intro/intro_setup_encryption.h
    src/intro/intro_setup_encryption.cpp
    src/intro/verification_flow.h
    src/intro/verification_flow.cpp
    src/intro/intro_widget.h
    src/intro/intro_widget.cpp
    src/intro/intro_widgets.h
    src/intro/intro_widgets.cpp
    src/intro/intro_colors.h
    src/intro/intro_colors.cpp
    src/history/history_message.h
    src/history/history_message.cpp
    src/history/history_message_layout.h
    src/history/history_message_layout.cpp
    src/history/media/history_view_audio.h
    src/history/media/history_view_audio.cpp
    src/history/media/history_view_poll.h
    src/history/media/history_view_poll.cpp
    src/history/history_large_emoji.h
    src/history/history_large_emoji.cpp
    src/history/history_list.h
    src/history/history_list.cpp
    src/history/history_down_button.h
    src/history/history_down_button.cpp
    src/history/history_pinned_bar.h
    src/history/history_pinned_bar.cpp
    src/history/history_draft_state.h
    src/history/history_draft_state.cpp
    src/history/history_typing_state.h
    src/history/history_typing_state.cpp
    src/history/history_pinned_state.h
    src/history/history_pinned_state.cpp
    src/history/history_media_request_state.h
    src/history/history_media_request_state.cpp
    src/history/history_pending_local_media.h
    src/history/history_pending_local_media.cpp
    src/history/history_read_receipt_state.h
    src/history/history_read_receipt_state.cpp
    src/history/history_audio_playback_state.h
    src/history/history_audio_playback_state.cpp
    src/history/history_video_thumbnail_probe_state.h
    src/history/history_video_thumbnail_probe_state.cpp
    src/history/history_inline_video.h
    src/history/history_inline_video.cpp
    src/history/history_input.h
    src/history/history_input.cpp
    src/history/history_voice_recorder.h
    src/history/history_voice_recorder.cpp
    src/history/mention_autocomplete.h
    src/history/mention_autocomplete.cpp
    src/history/history_confirm_dialog.h
    src/history/history_confirm_dialog.cpp
    src/history/history_forward_dialog.h
    src/history/history_forward_dialog.cpp
    src/history/history_emoji_picker.h
    src/history/history_emoji_picker.cpp
    src/history/history_popup_menu_style.h
    src/history/history_popup_menu_style.cpp
    src/history/history_attach_popup.h
    src/history/history_attach_popup.cpp
    src/history/history_prepared_upload.h
    src/history/history_prepared_upload.cpp
    src/history/history_send_files_dialog.h
    src/history/history_send_files_dialog.cpp
    src/history/room_settings_widget.h
    src/history/room_settings_widget.cpp
    src/history/history_menu_icons.qrc
    src/history/user_profile_popup.h
    src/history/user_profile_popup.cpp
    src/history/trust_shield.h
    src/history/trust_shield.cpp
    lib/lib_ui/fonts/fonts.qrc
    src/history/history_widget.h
    src/history/history_widget.cpp
    src/window/notifications_manager.h
    src/window/notifications_manager.cpp
    src/window/notifications.qrc
    # Stub UI widgets (headers for AUTOMOC + .cpp for vtable anchoring)
    src/ui/toast_widget.h
    src/ui/toast_widget.cpp
    src/ui/internal_choice_dialog.h
    src/ui/internal_choice_dialog.cpp
    src/ui/layers/layer_stack_widget.h
    src/ui/layers/layer_stack_widget.cpp
    src/settings/settings_icons.qrc
    src/settings/settings_widget.h
    src/settings/settings_widget.cpp
    src/settings/settings_page.h
    src/settings/settings_page.cpp
    src/settings/settings_common_widgets.h
    src/settings/settings_common_widgets.cpp
    src/settings/notifications_settings_page.h
    src/settings/notifications_settings_page.cpp
    src/settings/encryption/encryption_settings_page.h
    src/settings/encryption/encryption_settings_page.cpp
    src/settings/encryption/encryption_action_row.h
    src/settings/encryption/encryption_action_row.cpp
    src/settings/sessions/sessions_settings_page.h
    src/settings/sessions/sessions_settings_page.cpp
    src/settings/sessions/current_session_card.h
    src/settings/sessions/current_session_card.cpp
    src/settings/sessions/session_row.h
    src/settings/sessions/session_row.cpp
    src/settings/sessions/session_loading_overlay.h
    src/settings/sessions/session_loading_overlay.cpp
    src/settings/sessions/session_rename_dialog.h
    src/settings/sessions/session_rename_dialog.cpp
    src/settings/sessions/session_confirm_dialog.h
    src/settings/sessions/session_confirm_dialog.cpp
    src/settings/appearance/appearance_settings_page.h
    src/settings/appearance/appearance_settings_page.cpp
    src/settings/appearance/interface_scale_control.h
    src/settings/appearance/interface_scale_control.cpp
    src/settings/appearance/theme_selector_panel.h
    src/settings/appearance/theme_selector_panel.cpp
    src/settings/preferences/preferences_settings_page.h
    src/settings/preferences/preferences_settings_page.cpp
    src/settings/help/help_about_settings_page.h
    src/settings/help/help_about_settings_page.cpp
    src/settings/account/account_settings_page.h
    src/settings/account/account_settings_page.cpp
    src/settings/account/account_profile_cover.h
    src/settings/account/account_profile_cover.cpp
    src/settings/account/account_edit_name_dialog.h
    src/settings/account/account_edit_name_dialog.cpp
    src/settings/account/account_change_password_dialog.h
    src/settings/account/account_change_password_dialog.cpp
    src/settings/dialogs/interactive_auth_dialog.h
    src/settings/dialogs/interactive_auth_dialog.cpp
    src/settings/dialogs/recovery_key_dialog.h
    src/settings/dialogs/recovery_key_dialog.cpp
    src/settings/dialogs/reset_identity_dialog.h
    src/settings/dialogs/reset_identity_dialog.cpp
    src/settings/dialogs/settings_passphrase_dialog.h
    src/settings/dialogs/settings_passphrase_dialog.cpp
    src/settings/dialogs/verify_user_dialog.h
    src/settings/dialogs/verify_user_dialog.cpp
    src/settings/dialogs/verify_session_popup.h
    src/settings/dialogs/verify_session_popup.cpp
    src/ui/rp_widget.h
    src/ui/rp_widget.cpp
    src/ui/widgets/input_fields.h
    src/ui/widgets/input_fields.cpp
    src/ui/widgets/scroll_area.h
    src/ui/widgets/scroll_area.cpp
    src/ui/widgets/labels.h
    src/ui/widgets/labels.cpp
    src/ui/widgets/buttons.h
    src/ui/widgets/buttons.cpp
    src/ui/widgets/close_button.h
    src/ui/widgets/close_button.cpp
    src/ui/widgets/formatted_text_edit.h
    src/ui/widgets/formatted_text_edit.cpp
    src/ui/widgets/emoji_objects.h
    src/ui/widgets/emoji_objects.cpp
    src/ui/widgets/emoji_input_field.h
    src/ui/widgets/emoji_input_field.cpp
    src/ui/widgets/connecting_widget.h
    src/ui/widgets/connecting_widget.cpp
    src/ui/text/quote_paint.h
    src/ui/text/quote_paint.cpp
    src/ui/style/runtime_font.h
    src/ui/style/runtime_font.cpp
    src/ui/style/icon_provider.h
    src/ui/style/icon_provider.cpp
    src/ui/input_submit_settings.h
    src/ui/input_submit_settings.cpp
    src/ui/platform/ui_utility_mac.h
    src/ui/platform/confirm_quit_mac.h
    src/ui/platform/reveal_in_folder.h
    src/theme/chat_background_style.h
    src/theme/theme_manager.h
    src/theme/theme_manager.cpp
    src/theme/system_theme_watcher.h
    src/theme/system_theme_watcher.cpp
)

# Translation files
set(TS_FILES
    translations/telematrix_en.ts
    translations/telematrix_es.ts
)

# Generate .qm files from .ts files during build and embed via auto-generated .qrc
qt_add_translations(TeleMatrix
    TS_FILES ${TS_FILES}
    RESOURCE_PREFIX /translations
)

add_dependencies(TeleMatrix telematrix_protocol_build)

# Platform-specific implementations of the native seams (notifications, cursor /
# colour-space tricks, quit confirmation). On Apple these are Objective-C++; on
# every other platform they are plain-C++ stubs so the symbols still resolve.
if (APPLE)
    target_sources(TeleMatrix PRIVATE
        src/window/notifications_manager_mac.mm
        src/window/notification_platform_mac.mm
        src/ui/platform/ui_utility_mac.mm
        src/ui/platform/confirm_quit_mac.mm
        src/ui/platform/reveal_in_folder_mac.mm
    )
else()
    target_sources(TeleMatrix PRIVATE
        src/ui/platform/ui_utility_stub.cpp
        src/ui/platform/confirm_quit_stub.cpp
    )
endif()

# Embed the application icon into the Windows executable.
if (WIN32)
    target_sources(TeleMatrix PRIVATE
        app_icon.rc
        # Native WinRT toast notifications + the AUMID/Start-Menu-shortcut
        # bootstrap they require.
        src/window/notifications_manager_win.cpp
        src/window/notification_platform_win.cpp
        src/app/platform/win/app_user_model_id.cpp
        # "Show in Folder": SHOpenFolderAndSelectItems, explorer /select fallback.
        src/ui/platform/reveal_in_folder_win.cpp
    )
    # GUI subsystem (no console window); EntryPointPrivate supplies WinMain.
    set_target_properties(TeleMatrix PROPERTIES WIN32_EXECUTABLE ON)
    if (TARGET Qt6::EntryPointPrivate)
        target_link_libraries(TeleMatrix PRIVATE Qt6::EntryPointPrivate)
    endif()
endif()

# Native freedesktop.org D-Bus toast notifications on Linux/BSD.
if (UNIX AND NOT APPLE)
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_sources(TeleMatrix PRIVATE
        src/window/notifications_manager_linux.cpp
        src/window/notification_platform_linux.cpp
        # "Show in Folder": org.freedesktop.FileManager1, portal fallback.
        src/ui/platform/reveal_in_folder_linux.cpp
    )
    target_link_libraries(TeleMatrix PRIVATE Qt6::DBus)
endif()

# Where the updater fetches its release manifest. Kept as a cache variable so a
# staging repo can be pointed at (-DTELEMATRIX_UPDATE_MANIFEST_URL=...) without
# touching code. `releases/latest/download/...` is the stable, CDN-served,
# rate-limit-free URL, and it only resolves anonymously on a PUBLIC repo.
set(TELEMATRIX_UPDATE_MANIFEST_URL
    "https://github.com/gecka/telematrix/releases/latest/download/latest.json"
    CACHE STRING "URL of the auto-update manifest")

# Application version available to C++.
target_compile_definitions(TeleMatrix PRIVATE
    TELEMATRIX_VERSION_STR="${TELEMATRIX_VERSION}"
    TELEMATRIX_UPDATE_MANIFEST_URL="${TELEMATRIX_UPDATE_MANIFEST_URL}"
)

target_include_directories(TeleMatrix PRIVATE
    ${PROTOCOL_BINDINGS_DIR}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(TeleMatrix PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::Network
    telematrix_protocol
    telematrix_core
)

# libav* (FFmpeg) is pulled into the Rust staticlib by ffmpeg-next/ffmpeg-sys-next
# (video-thumbnail extraction, rust/src/video_thumbnail_service.rs). A Rust staticlib
# does NOT propagate these to the final C++ link, so link them explicitly. FFmpeg
# ships pkg-config .pc files on every platform — ensure ffmpeg is visible to
# pkg-config (Linux: libav*-dev; Windows: vcpkg/MSYS2 + PKG_CONFIG_PATH). CI must
# likewise expose ffmpeg's pkgconfig dir to the cargo build (the cargo step
# inherits the environment).
#
# macOS needs no system FFmpeg at all, and deliberately does NOT go through
# pkg_check_modules: that caches each resolved library as an absolute FILEPATH
# (pkgcfg_lib_FFMPEG_*), and find_library never re-searches a cache entry once
# set. An existing build tree configured against a system FFmpeg would therefore
# keep linking it forever after this switch — silently, since the two majors
# agree on most symbols and differ only on a few (`avcodec_close` is gone in 8).
# The dylibs are asserted to exist near the top of this file; link those paths.
# The generated .pc files are still needed, but only by ffmpeg-sys-next.
find_package(PkgConfig REQUIRED)
if (APPLE)
    add_library(tm_ffmpeg INTERFACE)
    target_include_directories(tm_ffmpeg INTERFACE "${TM_FFMPEG_INCLUDE_DIR}")
    foreach (_tm_soname IN LISTS TM_QT_FFMPEG_LINK_SONAMES)
        target_link_libraries(tm_ffmpeg INTERFACE "${TM_QT_LIB_DIR}/${_tm_soname}.dylib")
    endforeach()
    target_link_libraries(TeleMatrix PRIVATE tm_ffmpeg)
else()
    pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale)
    target_link_libraries(TeleMatrix PRIVATE PkgConfig::FFMPEG)
endif()

# Windows ships Qt's libav, not the one pkg-config found: vcpkg supplies the headers
# and import libraries, windeployqt deploys the DLLs (cmake/packaging.cmake). That
# only works while both are the same FFmpeg major — an import library records the
# DLL name it needs (avcodec-61.dll), so a vcpkg that drifts to another major asks
# the loader for a file Qt does not ship, or worse links against headers whose ABI
# the shipped DLL does not implement. Assert it here: the alternative is a build
# that fails deep inside bindgen, or an installer that dies at startup.
if (WIN32)
    set(_tm_qt_bin_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    foreach (_tm_lib IN ITEMS avcodec avformat avutil swscale)
        string(REGEX MATCH "^[0-9]+" _tm_major "${FFMPEG_lib${_tm_lib}_VERSION}")
        if (NOT EXISTS "${_tm_qt_bin_dir}/${_tm_lib}-${_tm_major}.dll")
            message(FATAL_ERROR
                "The FFmpeg pkg-config found (lib${_tm_lib} ${FFMPEG_lib${_tm_lib}_VERSION}) is not "
                "the one Qt ships, so the app would link an import library for a DLL that is "
                "never installed beside it.\n"
                "  expected: ${_tm_qt_bin_dir}/${_tm_lib}-${_tm_major}.dll\n"
                "PKG_CONFIG_PATH most likely points at a vcpkg whose ffmpeg port has moved to a "
                "new major. Re-pin it together with the ffmpeg-next version in rust/Cargo.toml "
                "and the vendored headers (see third_party/ffmpeg-7.1/README.md); CI pins the "
                "vcpkg commit in .github/workflows/_reusable-build.yml.")
        endif()
    endforeach()
endif()

if (APPLE)
    target_link_libraries(TeleMatrix PRIVATE
        "-framework Security"
        "-framework CoreFoundation"
        "-framework CoreGraphics"
        "-framework Cocoa"
        "-framework UserNotifications"
    )
elseif (WIN32)
    # Native system libraries pulled in by the Rust staticlib (matrix-sdk,
    # tokio, rustls, keyring/Credential-Manager). A Rust staticlib linked into
    # an MSVC executable does not propagate these automatically; the
    # authoritative list comes from:
    #   cargo rustc --release -- --print native-static-libs
    # Refine this list if the link step reports unresolved externals.
    # gdi32 is needed by the statically-vendored OpenSSL (SQLCipher) on Windows.
    target_link_libraries(TeleMatrix PRIVATE
        ws2_32 userenv advapi32 bcrypt crypt32 ncrypt secur32 ntdll
        kernel32 user32 shell32 ole32 oleaut32 gdi32
        # C++/WinRT toast notifications (notifications_manager_win.cpp) plus the
        # Start-Menu shortcut property store (app_user_model_id.cpp). Needs the
        # Windows 10 SDK (cppwinrt projection headers).
        windowsapp shlwapi propsys
    )
elseif (UNIX AND NOT APPLE)
    # Native system libraries pulled in by the Rust staticlib on Linux that a
    # staticlib does NOT propagate to the final C++ link:
    #   - dbus-1 : keyring "sync-secret-service" backend (libdbus)
    #   - crypto : rusqlite "bundled-sqlcipher" links system OpenSSL libcrypto
    # TLS uses rustls (no native-tls), so libssl is NOT needed.
    # Authoritative list: cargo rustc --release -- --print native-static-libs
    # Refine this if the link step reports further unresolved externals.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(TeleMatrix PRIVATE PkgConfig::DBUS OpenSSL::Crypto)
endif()

if (APPLE)
    # CFBundleShortVersionString / CFBundleVersion must be numeric dot-separated
    # ("1.2.0"); a prerelease suffix ("1.2.0-beta.1") is rejected by Apple's
    # validation. Strip it for the bundle keys only — the About page still shows the
    # full TELEMATRIX_VERSION_STR, and the .dmg filename keeps the full string.
    string(REGEX REPLACE "-.*$" "" TELEMATRIX_BUNDLE_VERSION "${TELEMATRIX_VERSION}")

    set_target_properties(TeleMatrix PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_ICON_FILE "telematrix.icns"
        MACOSX_BUNDLE_BUNDLE_NAME "TeleMatrix"
        MACOSX_BUNDLE_GUI_IDENTIFIER "dev.telematrix.TeleMatrix"
        MACOSX_BUNDLE_BUNDLE_VERSION "${TELEMATRIX_BUNDLE_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${TELEMATRIX_BUNDLE_VERSION}"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/cmake/TeleMatrixInfo.plist.in"
    )
    set_source_files_properties(telematrix.icns PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(TeleMatrix PRIVATE telematrix.icns)

    # Put the icon in the bundle BEFORE anything signs it. MACOSX_PACKAGE_LOCATION
    # above generates its copy as a *sibling* prerequisite of the target's build
    # rule, and make runs it AFTER the link rule that carries every POST_BUILD
    # command — so on a clean build the icon landed in Contents/Resources once
    # codesign had already sealed the bundle:
    #
    #   TeleMatrix.app: a sealed resource is missing or invalid
    #   file added: .../Contents/Resources/telematrix.icns
    #
    # The resource seal lives in the MAIN EXECUTABLE's signature, so that reads as
    # an invalid signature on Contents/MacOS/TeleMatrix — which is exactly what
    # notarization rejected. Incremental builds hid it completely: the icon was
    # already in place from the previous build, so only clean builds (i.e. CI) were
    # ever affected. This POST_BUILD registers first, so it runs first; CMake's own
    # copy still follows and is a harmless no-op on identical bytes.
    add_custom_command(TARGET TeleMatrix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_CONTENT_DIR:TeleMatrix>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/telematrix.icns"
            "$<TARGET_BUNDLE_CONTENT_DIR:TeleMatrix>/Resources/telematrix.icns"
        COMMENT "Staging bundle icon (before deploy + signing)"
        VERBATIM
    )

    find_program(MACDEPLOYQT_EXECUTABLE macdeployqt HINTS "${Qt6_DIR}/../../../bin")
    find_program(CREATEDMG_EXECUTABLE create-dmg)
    set(TELEMATRIX_CODESIGN_IDENTITY "-" CACHE STRING
        "macOS code-signing identity. Use a stable local identity to avoid repeated Keychain trust prompts in dev builds. Set to a 'Developer ID Application' identity to sign the app + .dmg for distribution.")
    set(TELEMATRIX_NOTARIZE_PROFILE "" CACHE STRING
        "xcrun notarytool keychain profile name. When set (and a real Developer ID identity is used), package_dmg notarizes and staples the .dmg. Create one with: xcrun notarytool store-credentials.")

    if (MACDEPLOYQT_EXECUTABLE)
        # Deploy only — NO -sign-for-notarization. macdeployqt rewrites load
        # commands with install_name_tool and then signs, but when a codesign call
        # fails (Apple's timestamp authority rate-limits, and -sign-for-notarization
        # implies -timestamp) it prints "ERROR: Codesign signing error" and exits 0.
        # That shipped a binary modified after it was signed, which notarization
        # rejects. cmake/codesign_bundle.sh below owns signing, and fails loudly.
        add_custom_command(TARGET TeleMatrix POST_BUILD
            COMMAND "${MACDEPLOYQT_EXECUTABLE}"
                "$<TARGET_BUNDLE_DIR:TeleMatrix>"
                -always-overwrite -no-strip
            COMMENT "Deploying Qt runtime into TeleMatrix.app bundle"
            VERBATIM
        )
    endif()

    find_program(CODESIGN_EXECUTABLE codesign)
    if (CODESIGN_EXECUTABLE)
        # One signing pass owns the bundle: nested code inside-out, the bundle last,
        # transient timestamp-authority failures retried, and a hard
        # `codesign --verify --deep --strict` before the build is allowed to
        # succeed. The script never falls back to ad-hoc when a real identity was
        # asked for: macOS pins a keychain item's ACL to the designated requirement
        # of the binary that CREATED it. A Developer ID requirement is stable, so
        # rebuilds and shipped updates keep access; an ad-hoc requirement is a bare
        # `cdhash H"..."` that changes every build, so each rebuild is denied
        # (errSecAuthFailed) — and an ad-hoc build that writes the item locks the
        # released app out of it. keychain.rs gives such builds their own service
        # name, but an ad-hoc bundle still cannot share this one's session.
        add_custom_command(TARGET TeleMatrix POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E env
                "APP_BUNDLE=$<TARGET_BUNDLE_DIR:TeleMatrix>"
                "SIGN_IDENTITY=${TELEMATRIX_CODESIGN_IDENTITY}"
                "BUNDLE_ID=dev.telematrix.TeleMatrix"
                "CODESIGN=${CODESIGN_EXECUTABLE}"
                bash "${CMAKE_SOURCE_DIR}/cmake/codesign_bundle.sh"
            COMMENT "Signing TeleMatrix.app (one pass, inside-out)"
            VERBATIM
        )
    endif()

    if (MACDEPLOYQT_EXECUTABLE AND CREATEDMG_EXECUTABLE)
        # DMG window + icon geometry, in logical points: app icon (left) -> arrow
        # -> Applications (right). The window height is a *bounds*, so the 32pt
        # title bar comes out of it and only ~368pt of the background shows.
        # tools/dmg/generate_background.py draws the art around these numbers --
        # change them there too, or the arrow stops pointing at the icons.
        set(DMG_WINDOW_WIDTH 600)
        set(DMG_WINDOW_HEIGHT 400)
        set(DMG_ICON_SIZE 100)
        set(DMG_TEXT_SIZE 12)
        set(DMG_APP_ICON_X 150)
        set(DMG_APP_ICON_Y 175)
        set(DMG_APPS_DROP_X 450)
        set(DMG_APPS_DROP_Y 175)
        set(DMG_BACKGROUND_SOURCE "${CMAKE_SOURCE_DIR}/cmake/dmg_background.png")

        if (NOT EXISTS "${DMG_BACKGROUND_SOURCE}")
            message(FATAL_ERROR "DMG background image not found: ${DMG_BACKGROUND_SOURCE}")
        endif()

        # The staging / Qt-deploy / create-dmg / file-icon work lives in a script
        # so the shell + AppleScript isn't buried in CMake escaping. Inputs are
        # passed through the environment.
        # Versioned like every other platform's package (deb/rpm/AppImage/NSIS all
        # embed the version), so release assets from different tags don't collide.
        # Arch suffix follows the actual slices built, falling back to the host
        # arch when unset. Both slices together is spelled "universal" rather than
        # "arm64-x86_64" — it is what users expect to see on a macOS download, and
        # what the release page and updater manifest advertise.
        if (CMAKE_OSX_ARCHITECTURES)
            if ("arm64" IN_LIST CMAKE_OSX_ARCHITECTURES AND "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES)
                set(TELEMATRIX_DMG_ARCH "universal")
            else()
                string(REPLACE ";" "-" TELEMATRIX_DMG_ARCH "${CMAKE_OSX_ARCHITECTURES}")
            endif()
        else()
            set(TELEMATRIX_DMG_ARCH "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()

        add_custom_target(package_dmg
            COMMAND ${CMAKE_COMMAND} -E env
                "APP_BUNDLE=$<TARGET_BUNDLE_DIR:TeleMatrix>"
                "OUTPUT_DMG=${CMAKE_BINARY_DIR}/TeleMatrix-${TELEMATRIX_VERSION}-${TELEMATRIX_DMG_ARCH}.dmg"
                "STAGING_DIR=${CMAKE_BINARY_DIR}/dmg_staging"
                "ICNS=${CMAKE_SOURCE_DIR}/telematrix.icns"
                "BACKGROUND=${DMG_BACKGROUND_SOURCE}"
                "CREATEDMG=${CREATEDMG_EXECUTABLE}"
                "VOLNAME=TeleMatrix Installer"
                "WINDOW_W=${DMG_WINDOW_WIDTH}"
                "WINDOW_H=${DMG_WINDOW_HEIGHT}"
                "ICON_SIZE=${DMG_ICON_SIZE}"
                "TEXT_SIZE=${DMG_TEXT_SIZE}"
                "APP_ICON_X=${DMG_APP_ICON_X}"
                "APP_ICON_Y=${DMG_APP_ICON_Y}"
                "APPS_X=${DMG_APPS_DROP_X}"
                "APPS_Y=${DMG_APPS_DROP_Y}"
                "SIGN_IDENTITY=${TELEMATRIX_CODESIGN_IDENTITY}"
                "NOTARIZE_PROFILE=${TELEMATRIX_NOTARIZE_PROFILE}"
                bash "${CMAKE_SOURCE_DIR}/cmake/package_dmg.sh"
            # Deliberately NOT `DEPENDS TeleMatrix`. The release flow notarizes and
            # staples TeleMatrix.app *before* packaging, and the TeleMatrix target
            # relinks on every invocation (the Rust staticlib custom command
            # refreshes its output), which re-fired the deploy+sign POST_BUILD and
            # silently destroyed the ticket that had just been stapled on. Build the
            # app first; package_dmg.sh fails with a readable error if you didn't.
            COMMENT "Building stylized DMG (create-dmg + custom file icon)"
            VERBATIM
        )
    endif()
endif()

# Install rules + CPack packaging for Windows (NSIS) and Linux (DEB/RPM).
# macOS keeps its bespoke create-dmg flow above; packaging.cmake is a no-op there.
include("${CMAKE_SOURCE_DIR}/cmake/packaging.cmake")

# C++ unit tests (Qt Test). The runners link the telematrix_core OBJECT library
# (the same objects the app uses) so they exercise real production code. Run with
# `ctest` (headless: set QT_QPA_PLATFORM=offscreen). Toggle off for pure package
# builds with -DTELEMATRIX_BUILD_TESTS=OFF.
option(TELEMATRIX_BUILD_TESTS "Build the C++ unit-test suite" ON)
if (TELEMATRIX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
