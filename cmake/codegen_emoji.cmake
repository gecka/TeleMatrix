# Builds desktop-app's `codegen_emoji` as a host tool and runs it to produce the emoji
# data tables (`emoji.h` / `emoji.cpp`) that src/ui/emoji/ compiles against.
#
# Upstream builds this via lib/codegen/codegen/emoji/CMakeLists.txt, which depends on
# cmake_helpers' init_target/nice_target_sources and links all of desktop-app::lib_base.
# None of that is in this build, and none of it is needed: the emoji + common codegen
# sources reach outside Qt for exactly one header, base/qt/qt_string_view.h, which is
# self-contained. So we compile the sources directly and put lib/lib_base on the include
# path for that single header.
#
# The tool has a second, opt-in `--images` mode that renders the sprite atlases from an
# emoji font or image set. We never invoke it — the atlases are vendored prebuilt under
# resources/emoji/ (see the README there). It is why Qt6::Gui is linked all the same:
# main.cpp constructs a QGuiApplication when that flag is present. Without the flag it
# builds a plain QCoreApplication, so this runs headless in CI.

set(TM_CODEGEN_DIR "${CMAKE_SOURCE_DIR}/lib/codegen")

add_executable(codegen_emoji
    "${TM_CODEGEN_DIR}/codegen/common/basic_tokenized_file.cpp"
    "${TM_CODEGEN_DIR}/codegen/common/checked_utf8_string.cpp"
    "${TM_CODEGEN_DIR}/codegen/common/clean_file.cpp"
    "${TM_CODEGEN_DIR}/codegen/common/cpp_file.cpp"
    "${TM_CODEGEN_DIR}/codegen/common/logging.cpp"
    "${TM_CODEGEN_DIR}/codegen/emoji/data.cpp"
    "${TM_CODEGEN_DIR}/codegen/emoji/data_old.cpp"
    "${TM_CODEGEN_DIR}/codegen/emoji/data_read.cpp"
    "${TM_CODEGEN_DIR}/codegen/emoji/generator.cpp"
    "${TM_CODEGEN_DIR}/codegen/emoji/main.cpp"
    "${TM_CODEGEN_DIR}/codegen/emoji/options.cpp"
    "${TM_CODEGEN_DIR}/codegen/emoji/replaces.cpp"
)

target_include_directories(codegen_emoji SYSTEM PRIVATE
    "${TM_CODEGEN_DIR}"
    "${CMAKE_SOURCE_DIR}/lib/lib_base"
)

target_link_libraries(codegen_emoji PRIVATE Qt6::Core Qt6::Gui)

# Vendored third-party code: don't hold it to this project's warning settings.
if (MSVC)
    target_compile_options(codegen_emoji PRIVATE /w)
else ()
    target_compile_options(codegen_emoji PRIVATE -w)
endif ()

# A universal macOS build would otherwise build the tool universal too; it only ever runs
# on the host, so keep it single-arch and skip the lipo work.
if (APPLE)
    set_target_properties(codegen_emoji PROPERTIES OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif ()

# ---- Run it -----------------------------------------------------------------------------
# Mirrors lib/lib_ui/cmake/generate_emoji.cmake: the *timestamp* is the dependency-graph
# node and the C++ files are byproducts. Generator::generate() writes the timestamp last
# and only on total success, so a half-written generation can never look up to date.
set(TM_EMOJI_GEN_DIR "${CMAKE_BINARY_DIR}/generated/emoji")
file(MAKE_DIRECTORY "${TM_EMOJI_GEN_DIR}")

set(TM_EMOJI_GEN_TIMESTAMP "${TM_EMOJI_GEN_DIR}/emoji.timestamp")
set(TM_EMOJI_GEN_SOURCES
    "${TM_EMOJI_GEN_DIR}/emoji.cpp"
    "${TM_EMOJI_GEN_DIR}/emoji.h"
)

# emoji.txt pins the emoji order, which must match the vendored atlases cell-for-cell.
# resources/emoji/README.md records the correspondence — re-check it if this input moves.
set(TM_EMOJI_DATA "${CMAKE_SOURCE_DIR}/lib/lib_ui/emoji.txt")
set(TM_EMOJI_REPLACES "${CMAKE_SOURCE_DIR}/lib/lib_ui/emoji_suggestions/emoji_autocomplete.json")

# emoji_suggestions_data.{cpp,h} are also emitted (the tool always writes all four) but
# nothing consumes them: `:shortcode:` autocomplete is not wired up.
add_custom_command(
    OUTPUT "${TM_EMOJI_GEN_TIMESTAMP}"
    BYPRODUCTS ${TM_EMOJI_GEN_SOURCES}
        "${TM_EMOJI_GEN_DIR}/emoji_suggestions_data.cpp"
        "${TM_EMOJI_GEN_DIR}/emoji_suggestions_data.h"
    COMMAND codegen_emoji "-o${TM_EMOJI_GEN_DIR}" "${TM_EMOJI_DATA}" "${TM_EMOJI_REPLACES}"
    DEPENDS codegen_emoji "${TM_EMOJI_DATA}" "${TM_EMOJI_REPLACES}"
    COMMENT "Generating emoji data tables"
    VERBATIM
)
add_custom_target(telematrix_emoji_codegen DEPENDS "${TM_EMOJI_GEN_TIMESTAMP}")
