# TeleMatrix Linux builder image.
#
# Reproduces the Linux CI toolchain (.github/workflows/build.yml) so `.deb`, `.rpm`, and
# `.AppImage` packages can be built the same way locally, off any host distro. Pins must
# stay in step with CI: Ubuntu 22.04, gcc-12, Qt 6.10.1, rustc 1.96.0.
#
#   # Build the image (once):
#   docker build -t telematrix-linux-builder .
#
#   # Build the packages (source mounted; must include submodules — run
#   # `git submodule update --init --recursive` on the host first). Output (.deb, .rpm,
#   # .AppImage) lands in ./dist on the host:
#   docker run --rm -v "$PWD":/src telematrix-linux-builder
#
#   # Or drop into a shell to build by hand:
#   docker run --rm -it -v "$PWD":/src telematrix-linux-builder bash
#
# The Rust target dir and CMake build dir live under the mounted /src, so rebuilds
# are incremental across runs.

FROM ubuntu:22.04

ARG QT_VERSION=6.10.1
ARG RUST_VERSION=1.96.0
ARG DEBIAN_FRONTEND=noninteractive

# --- System dependencies (mirror the CI "Install system dependencies" step) -------
#  build-essential/g++-12 : project needs gcc >= 12 (22.04 default is 11)
#  ninja-build/cmake      : build driver
#  pkg-config/patchelf    : FFmpeg discovery + Linux deploy RPATH fixups
#  libssl-dev             : rusqlite bundled-sqlcipher
#  libdbus-1-dev          : keyring (Secret Service)
#  nasm                   : aws-lc-rs/ring asm (rustls crypto provider)
#  rpm                    : rpmbuild for the CPack RPM generator
#  libav*-dev             : ffmpeg-next (video thumbnails) — the Rust staticlib and
#                           the app both link libav{codec,format,util,swscale}
#  libgl/xkbcommon/vulkan : Qt runtime/build deps
#  curl/ca-certificates/git : rustup bootstrap + submodule / VCS access
#  python3-pip            : aqtinstall (Qt)
#  file/desktop-file-utils : linuxdeploy (binary type sniffing + .desktop validation)
#  zsync                  : appimagetool (present so it never warns; no update-info used)
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential g++-12 ninja-build cmake pkg-config patchelf \
        libssl-dev libdbus-1-dev nasm rpm \
        libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
        libgl1-mesa-dev libxkbcommon-dev libvulkan-dev \
        curl ca-certificates git python3 python3-pip \
        file desktop-file-utils zsync \
    && rm -rf /var/lib/apt/lists/*

# --- Qt (pinned, via aqtinstall — matches the CI install-qt-action version) --------
ENV QT_ROOT=/opt/qt
RUN pip3 install --no-cache-dir "aqtinstall==3.*" \
    && aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 \
        -m qtmultimedia -O ${QT_ROOT} \
    && rm -rf /root/.cache/pip
ENV QT_PREFIX=${QT_ROOT}/${QT_VERSION}/gcc_64
ENV CMAKE_PREFIX_PATH=${QT_PREFIX}
ENV PATH=${QT_PREFIX}/bin:${PATH}

# --- Rust (pinned to rust-toolchain.toml) -----------------------------------------
ENV RUSTUP_HOME=/opt/rustup \
    CARGO_HOME=/opt/cargo \
    PATH=/opt/cargo/bin:${PATH}
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
        | sh -s -- -y --profile minimal --default-toolchain ${RUST_VERSION} \
    && rustc --version && cargo --version

# gcc-12 for the C/C++ compile (22.04's default `gcc` is 11).
ENV CC=gcc-12 \
    CXX=g++-12

# --- AppImage tooling (linuxdeploy + qt plugin + appimagetool) ---------------------
# Baked into the image (continuous releases) so `docker run` never re-downloads them;
# package_appimage.sh picks them up via the LINUXDEPLOY*/APPIMAGETOOL env vars. FUSE is
# unavailable in containers, so APPIMAGE_EXTRACT_AND_RUN=1 makes the tools self-extract.
RUN mkdir -p /opt/appimage-tools && cd /opt/appimage-tools \
    && curl -fL --retry 3 -o linuxdeploy-x86_64.AppImage \
        https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
    && curl -fL --retry 3 -o linuxdeploy-plugin-qt-x86_64.AppImage \
        https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-x86_64.AppImage \
    && curl -fL --retry 3 -o appimagetool-x86_64.AppImage \
        https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage \
    && chmod +x ./*.AppImage
ENV APPIMAGE_EXTRACT_AND_RUN=1 \
    LINUXDEPLOY=/opt/appimage-tools/linuxdeploy-x86_64.AppImage \
    LINUXDEPLOY_PLUGIN_QT=/opt/appimage-tools/linuxdeploy-plugin-qt-x86_64.AppImage \
    APPIMAGETOOL=/opt/appimage-tools/appimagetool-x86_64.AppImage

WORKDIR /src

# Default: configure (Release), build the app once, and package .deb + .rpm + .AppImage
# into /src/dist. The single TeleMatrix compile is shared across all three formats
# (cpack and package_appimage both depend on the already-built binary). JSON exec form
# (proper signal handling); the shell is invoked explicitly for the `$(nproc)` expansion
# and the multi-command pipeline. Override with e.g. `bash` to build by hand.
CMD ["bash", "-c", "set -eux; \
    cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_COMPILER=gcc-12 \
        -DCMAKE_CXX_COMPILER=g++-12; \
    cmake --build build -j\"$(nproc)\" --target TeleMatrix; \
    ( cd build && cpack -G 'DEB;RPM' ); \
    cmake --build build --target package_appimage; \
    mkdir -p dist; \
    cp build/*.deb build/*.rpm build/*.AppImage dist/; \
    ls -l dist"]
