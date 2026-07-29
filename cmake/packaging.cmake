# Install rules + CPack packaging for Windows (NSIS) and Linux (DEB/RPM).
#
# macOS is intentionally untouched: it keeps the bespoke create-dmg `package_dmg`
# target defined in the top-level CMakeLists.txt, so we bail out early here.
#
# Qt is bundled on both Windows and Linux (per project decision) via Qt's official
# deployment helper `qt_generate_deploy_app_script`, which gathers the Qt runtime
# libraries, the required plugins, and a qt.conf into the install tree.
#
# NOTE: the Windows and Linux package layouts below can only be fully validated on
# their respective hosts. Treat this as a correct-by-construction scaffold and
# refine on-target (see BUILDING.md / the plan's verification section).

if (APPLE)
    return()
endif()

include(GNUInstallDirs)

# ---- Common CPack metadata ---------------------------------------------------
# Prerelease versions ("1.2.0-beta.1") must be rewritten for DEB/RPM: RPM forbids '-'
# in a Version field outright (rpmbuild fails), and dpkg reads it as the debian_revision
# separator, which would sort the beta ABOVE the final 1.2.0. '~' sorts BEFORE the
# release in both -> "1.2.0~beta.1" < "1.2.0". A plain "1.2.0" is unaffected.
string(REPLACE "-" "~" TELEMATRIX_PACKAGE_VERSION "${TELEMATRIX_VERSION}")

set(CPACK_PACKAGE_NAME "TeleMatrix")
set(CPACK_PACKAGE_VENDOR "TeleMatrix")
set(CPACK_PACKAGE_VERSION "${TELEMATRIX_PACKAGE_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "A Matrix client with a Telegram Desktop-style interface")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://telematrix.app")
set(CPACK_PACKAGE_CONTACT "TeleMatrix <info@gecka.info>")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "TeleMatrix")
set(CPACK_STRIP_FILES ON)

# License shown on the NSIS installer's license page. GPLv3 is plain ASCII, so
# the LICENSE file renders fine as NSIS license data. (RPM carries the license
# via CPACK_RPM_PACKAGE_LICENSE; macOS uses the bespoke create-dmg flow.)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

if (WIN32)
    # ---- Windows: flat single-folder install via windeployqt ----------------
    # qt_generate_deploy_app_script deploys Qt to QT_DEPLOY_BIN_DIR (= "bin" by
    # default; see Qt6CoreDeploySupport.cmake), but we ship the exe at the install
    # root and Windows has no RPATH, so the installed app could not find
    # Qt6Widgets.dll (it landed in <prefix>\bin\ while the exe sat at <prefix>\).
    # windeployqt instead lays the exe + every Qt DLL/plugin it needs down flat in
    # one folder; --compiler-runtime also bundles the MSVC runtime so the app runs
    # on a clean machine without the VC++ redistributable. Mirrors the macOS
    # macdeployqt POST_BUILD step in CMakeLists.txt.
    find_program(WINDEPLOYQT_EXECUTABLE windeployqt
        HINTS "${Qt6_DIR}/../../../bin" "${Qt6_DIR}/../../../../bin")
    if (NOT WINDEPLOYQT_EXECUTABLE)
        message(FATAL_ERROR "windeployqt not found; cannot bundle Qt for the Windows package.")
    endif()

    set(TM_WINDEPLOY_DIR "${CMAKE_BINARY_DIR}/windeploy")
    add_custom_command(TARGET TeleMatrix POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${TM_WINDEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TM_WINDEPLOY_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:TeleMatrix>" "${TM_WINDEPLOY_DIR}/"
        COMMAND "${WINDEPLOYQT_EXECUTABLE}"
            $<IF:$<CONFIG:Debug>,--debug,--release>
            --no-translations --compiler-runtime
            "${TM_WINDEPLOY_DIR}/TeleMatrix.exe"
        COMMENT "Deploying Qt runtime (windeployqt) into ${TM_WINDEPLOY_DIR}"
        VERBATIM)

    # No FFmpeg DLLs are copied here on purpose. windeployqt deploys the libav set
    # Qt ships for its multimedia backend (avcodec-61, avformat-61, avutil-59,
    # swresample-5, swscale-8), and the import libraries vcpkg provides resolve to
    # exactly those names — same major.minor by the pin in
    # .github/workflows/_reusable-build.yml, which CMakeLists.txt asserts. Copying
    # vcpkg's DLLs on top would overwrite Qt's full build with the lean
    # [core,avcodec,avformat,swresample,swscale] one and hand Qt's media plugin a
    # libav it was never tested against. One FFmpeg in the package, like macOS.
    #
    # The CI gate ("Verify Qt is bundled next to the exe") walks every import with
    # dumpbin and fails if libav is not beside the exe, so a windeployqt that ever
    # stops deploying it is caught there rather than by a user.

    # Ship the staged folder verbatim at the install root. The exe is already in
    # it, so no separate install(TARGETS) is needed; the NSIS shortcuts below
    # still point at $INSTDIR\TeleMatrix.exe.
    install(DIRECTORY "${TM_WINDEPLOY_DIR}/" DESTINATION .)

    set(CPACK_GENERATOR "NSIS")
    set(CPACK_NSIS_DISPLAY_NAME "TeleMatrix")
    set(CPACK_NSIS_PACKAGE_NAME "TeleMatrix")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/telematrix.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/telematrix.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "TeleMatrix.exe")
    set(CPACK_NSIS_URL_INFO_ABOUT "${CPACK_PACKAGE_HOMEPAGE_URL}")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
    set(CPACK_NSIS_MODIFY_PATH OFF)

    # Start-menu shortcut (target . => exe sits at the install root) and a
    # matching desktop shortcut created/removed by the installer.
    set(CPACK_PACKAGE_EXECUTABLES "TeleMatrix" "TeleMatrix")
    set(CPACK_NSIS_CREATE_ICONS_EXTRA
        "CreateShortCut '$DESKTOP\\\\TeleMatrix.lnk' '$INSTDIR\\\\TeleMatrix.exe'")
    set(CPACK_NSIS_DELETE_ICONS_EXTRA
        "Delete '$DESKTOP\\\\TeleMatrix.lnk'")

else ()
    # ---- Linux: self-contained /opt prefix, DEB + RPM -----------------------
    # Everything (binary, bundled Qt, plugins, desktop integration files) lives
    # under /opt/TeleMatrix; the maintainer scripts symlink the launcher, the
    # .desktop, the icons and the metainfo into the standard /usr locations.
    set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/TeleMatrix")

    # The executable looks for its bundled Qt libraries next to / one level up
    # from itself.
    set_target_properties(TeleMatrix PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")

    # Official Qt deployment script: bundles Qt libs + plugins + qt.conf at
    # install time, relative to CMAKE_INSTALL_PREFIX (exe -> bin, libs -> lib,
    # found via the $ORIGIN/../lib RPATH above). NO_UNSUPPORTED_PLATFORM_ERROR
    # keeps it from hard-failing where Qt's CMake deployment is best-effort.
    # (Windows uses windeployqt instead — see the if() branch above.)
    qt_generate_deploy_app_script(
        TARGET TeleMatrix
        OUTPUT_SCRIPT TM_QT_DEPLOY_SCRIPT
        NO_TRANSLATIONS
        NO_UNSUPPORTED_PLATFORM_ERROR
    )

    install(TARGETS TeleMatrix RUNTIME DESTINATION bin)
    install(SCRIPT ${TM_QT_DEPLOY_SCRIPT})

    # Desktop integration (installed under the /opt prefix, symlinked into /usr
    # by the maintainer scripts).
    install(FILES
        "${CMAKE_SOURCE_DIR}/resources/linux/dev.telematrix.TeleMatrix.desktop"
        DESTINATION share/applications)

    set(TM_METAINFO "${CMAKE_BINARY_DIR}/dev.telematrix.TeleMatrix.metainfo.xml")
    configure_file(
        "${CMAKE_SOURCE_DIR}/resources/linux/dev.telematrix.TeleMatrix.metainfo.xml.in"
        "${TM_METAINFO}" @ONLY)
    install(FILES "${TM_METAINFO}" DESTINATION share/metainfo)

    foreach (sz IN ITEMS 16 32 48 64 128 256)
        install(FILES
            "${CMAKE_SOURCE_DIR}/resources/icons/hicolor/${sz}x${sz}/apps/dev.telematrix.TeleMatrix.png"
            DESTINATION "share/icons/hicolor/${sz}x${sz}/apps")
    endforeach ()

    # ---- AppImage: `cmake --build build --target package_appimage` ----------
    # Built independently of the DEB/RPM install rules above: package_appimage.sh
    # assembles its own AppDir and lets linuxdeploy-plugin-qt be the SOLE Qt bundler
    # (no double-deploy vs. qt_generate_deploy_app_script), so one configure yields
    # deb + rpm + AppImage with no -DTELEMATRIX_APPIMAGE toggle. X11/xcb only (runs on
    # Wayland via XWayland). See docs/appimage-build-target-plan.md.
    #
    # This is also the only Linux package that can self-update: the app replaces
    # the file at $APPIMAGE in place (src/core/updater.cpp). deb/rpm are owned by
    # the package manager and stay notify-only. No embedded zsync/AppImageUpdate
    # info — updates go through our own signed manifest.
    # qmake feeds linuxdeploy-plugin-qt; the script falls back to PATH if it's missing.
    get_target_property(TM_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    add_custom_target(package_appimage
        COMMAND ${CMAKE_COMMAND} -E env
            "BINARY=$<TARGET_FILE:TeleMatrix>"
            "VERSION=${TELEMATRIX_VERSION}"
            "DESKTOP_FILE=${CMAKE_SOURCE_DIR}/resources/linux/dev.telematrix.TeleMatrix.desktop"
            "ICON_FILE=${CMAKE_SOURCE_DIR}/resources/icons/hicolor/256x256/apps/dev.telematrix.TeleMatrix.png"
            "METAINFO_FILE=${TM_METAINFO}"
            "QMAKE=${TM_QMAKE_EXECUTABLE}"
            "APPDIR=${CMAKE_BINARY_DIR}/AppDir"
            "APPIMAGE_OUTPUT=${CMAKE_BINARY_DIR}/TeleMatrix-${TELEMATRIX_VERSION}-x86_64.AppImage"
            bash "${CMAKE_SOURCE_DIR}/cmake/package_appimage.sh"
        DEPENDS TeleMatrix
        COMMENT "Building AppImage (linuxdeploy + plugin-qt + appimagetool)"
        VERBATIM)

    set(CPACK_GENERATOR "DEB;RPM")
    set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
    set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")

    # DEB ---------------------------------------------------------------------
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${CPACK_PACKAGE_CONTACT}")
    set(CPACK_DEBIAN_PACKAGE_SECTION "net")
    set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
    set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "${CPACK_PACKAGE_HOMEPAGE_URL}")
    # Qt is bundled, so dependencies are just the base X/graphics stack. Tune on
    # target with `dpkg-shlibdeps` if a launch shows missing libraries.
    # libssl3 ships libcrypto.so.3 (needed by rusqlite bundled-sqlcipher; TLS is
    # rustls so libssl itself is not linked); libdbus-1-3 for keyring Secret Service.
    set(CPACK_DEBIAN_PACKAGE_DEPENDS
        "libc6, libstdc++6, libx11-6, libxcb1, libxkbcommon0, libfontconfig1, libfreetype6, libglib2.0-0, libssl3, libdbus-1-3")
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/resources/linux/deb/postinst;${CMAKE_SOURCE_DIR}/resources/linux/deb/postrm")

    # RPM ---------------------------------------------------------------------
    # GPL-3.0 is inherited from Telegram Desktop; adjust if your licensing differs.
    set(CPACK_RPM_PACKAGE_LICENSE "GPLv3+")
    set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
    set(CPACK_RPM_PACKAGE_URL "${CPACK_PACKAGE_HOMEPAGE_URL}")
    # Bundled Qt => don't let rpmbuild auto-require the system Qt sonames.
    set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
    # openssl-libs ships libcrypto (needed by rusqlite bundled-sqlcipher; TLS is
    # rustls so libssl is not linked); dbus-libs: libdbus-1 for keyring Secret Service.
    set(CPACK_RPM_PACKAGE_REQUIRES
        "glibc, libstdc++, libX11, libxcb, libxkbcommon, fontconfig, freetype, glib2, openssl-libs, dbus-libs")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
        "${CMAKE_SOURCE_DIR}/resources/linux/rpm/post.sh")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
        "${CMAKE_SOURCE_DIR}/resources/linux/rpm/postun.sh")
    # /opt and its subdirs are owned by filesystem; avoid clashing ownership.
    set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION "/opt")
endif ()

include(CPack)
