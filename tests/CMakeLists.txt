# SPDX-License-Identifier: GPL-3.0-or-later

# C++ unit tests (Qt Test). Each tst_<area> links the telematrix_core OBJECT
# library so it exercises the SAME production objects the app uses (not a copy).
# Tests are pure-logic and headless: run with `ctest` and QT_QPA_PLATFORM=offscreen.

set(TELEMATRIX_TEST_AREAS
    safe_url
    recovery_key_format
    theme_parser
    theme_registry
    chat_background
    localization
    core_settings
    dialogs_width
    account_index
    unread_state_store
    history_return_stack
    history_selection_state
    history_scroll_state
    message_index
    mention_trigger
    compose_html
    block_format_presence
    jump_load_controller
    unread_bar_placement
    unread_read_progress
    bubble_corners
    trust_shield
    prepared_upload
    timeline_item_content
    emoji_data
    emoji_atlas
    emoji_picker_keys
    empty_userpic
    ffi_conversions
    room_directory_filter
    saved_messages_targets
    qr_code_image
    notifications_win_keys
    notifications_linux_registry
    video_placeholder
    video_download_overlay_policy
    video_rebuffer
    video_stream_retry
    video_frame_scaler
    media_retry_policy
)

# Aggregate target so CI can build every test runner with one `--target` (no need
# to keep a target list in sync between CMake and the workflow).
add_custom_target(telematrix_cpp_tests)

foreach(area ${TELEMATRIX_TEST_AREAS})
    add_executable(tst_${area} tst_${area}.cpp)
    target_link_libraries(tst_${area} PRIVATE telematrix_core Qt6::Test)
    add_test(NAME ${area} COMMAND tst_${area})
    add_dependencies(telematrix_cpp_tests tst_${area})
    # Headless: no real windowing system needed on CI.
    set_tests_properties(${area} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

# Drives a real QSplitter through the startup resize sequence, so it needs the
# widget classes themselves rather than just the pure-logic core.
target_link_libraries(tst_dialogs_width PRIVATE Qt6::Widgets)

# MediaCache lives in the app target (not telematrix_core), and pulls in
# Qt Widgets/Concurrent. Give it a dedicated runner that compiles the real
# production media_cache.cpp directly (exercises the same object, not a copy).
add_executable(tst_media_cache
    tst_media_cache.cpp
    ${CMAKE_SOURCE_DIR}/src/protocol/media_cache.cpp)
target_link_libraries(tst_media_cache
    PRIVATE telematrix_core Qt6::Test Qt6::Widgets Qt6::Concurrent)
add_test(NAME media_cache COMMAND tst_media_cache)
add_dependencies(telematrix_cpp_tests tst_media_cache)
set_tests_properties(media_cache PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
