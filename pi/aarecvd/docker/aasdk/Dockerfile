# syntax=docker/dockerfile:1
#
# The aasdk that aarecvd is built on: opencardev/aasdk at commit 9bf6adf (branch newdev), built for Debian 13
# trixie from Debian's own packages. The source is fetched at that exact commit when the image is made and is not
# kept in this repo. Nothing else is downloaded at configure or build time; googletest comes from Debian's
# `googletest` package instead of CMake's FetchContent.
#
# pi/aarecvd/build.sh makes this image when it is missing, and "./build.sh aasdk" makes it again. By hand, from
# this folder:
#   docker build --platform linux/arm64 --target build -t veloaa-aasdk-build:arm64 .
#   docker build --platform linux/amd64 --target build -t veloaa-aasdk-build:amd64 .
# The image has aasdk installed in /opt/aasdk (shared and static libraries, headers, CMake and pkg-config files) and
# runs aasdk's unit tests while it is made. To get those files out onto the host instead:
#   docker build --platform linux/arm64 --target export --output type=local,dest=out-arm64 .
#
# aasdk is GPLv3, as aarecvd is. It compiles a head-unit certificate and key into the library (Cryptor.cpp). They
# come with its source and are not copied into this repo.

ARG DEBIAN_TAG=trixie
FROM debian:${DEBIAN_TAG} AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG JOBS=12
ARG RUN_TESTS=ON
ARG AASDK_REPO=https://github.com/opencardev/aasdk.git
ARG AASDK_COMMIT=9bf6adf933665dee26532201719fac14a047ccf1
# Pin the version so the build is reproducible. aasdk otherwise stamps today's date, which also breaks
# compilation in August and September because Version.hpp gets "MINOR = 08".
ARG AASDK_VERSION=2026.6.11

ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build g++ make pkg-config file binutils \
        libboost-system-dev libboost-log-dev \
        libprotobuf-dev protobuf-compiler \
        libssl-dev libusb-1.0-0-dev \
        libzstd-dev zlib1g-dev \
        googletest git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# aasdk's build normally reads these from git; they are supplied here because .git is removed after the checkout.
ENV GIT_COMMIT_ID=9bf6adf GIT_BRANCH=newdev GIT_DESCRIBE=v4.1.20260427-newdev.5 \
    GIT_COMMIT_TIMESTAMP=1781214115 GIT_DIRTY=0

# Fetch exactly that commit by its full id, so a moved branch or tag cannot change what is built.
RUN set -eux; \
    git init -q /src; \
    git -C /src fetch -q --depth 1 "${AASDK_REPO}" "${AASDK_COMMIT}"; \
    git -C /src checkout -q FETCH_HEAD; \
    test "$(git -C /src rev-parse HEAD)" = "${AASDK_COMMIT}"; \
    rm -rf /src/.git

RUN set -eux; \
    cmake -S /src -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/opt/aasdk \
        -DAASDK_VERSION_OVERRIDE=${AASDK_VERSION} \
        -DSKIP_BUILD_PROTOBUF=ON -DSKIP_BUILD_ABSL=ON \
        -DAASDK_TEST=${RUN_TESTS} \
        -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=/usr/src/googletest \
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
        -DINSTALL_GTEST=OFF; \
    start=$(date +%s); \
    cmake --build /build --parallel ${JOBS}; \
    echo "BUILD_SECONDS=$(( $(date +%s) - start ))" | tee /build/build-time.txt

RUN set -eux; \
    if [ "${RUN_TESTS}" = "ON" ]; then \
        cd /build; \
        if ! ./bin/aasdk_ut --gtest_brief=1 > /build/unit-test.log 2>&1; then tail -40 /build/unit-test.log; exit 1; fi; \
        tail -5 /build/unit-test.log; \
    fi

# Install, then fix up what aasdk's own install leaves out:
#  - static archives: aasdk's CMakeLists hard-codes SHARED on Linux. The objects are -fPIC, so archive them
#    as they are (debug info stripped) rather than patching the source.
#  - aasdk builds Release with "-g -O3": split the debug info into lib/.debug and ship stripped .so files.
#  - aasdk.pc lacks -laap_protobuf and is not relocatable, so add a relocatable aasdk-complete.pc.
RUN set -eux; \
    cmake --install /build; \
    P=/opt/aasdk; \
    ar crs $P/lib/libaasdk.a $(find /build/CMakeFiles/aasdk.dir -name '*.o'); \
    ar crs $P/lib/libaap_protobuf.a $(find /build/protobuf/CMakeFiles/aap_protobuf.dir -name '*.o'); \
    strip --strip-debug $P/lib/libaasdk.a $P/lib/libaap_protobuf.a; \
    mkdir -p $P/lib/.debug; \
    for f in $(find $P/lib -maxdepth 1 -type f -name '*.so.*'); do \
        b=$(basename "$f"); \
        objcopy --only-keep-debug "$f" "$P/lib/.debug/$b.debug"; \
        strip --strip-unneeded "$f"; \
        objcopy --add-gnu-debuglink="$P/lib/.debug/$b.debug" "$f"; \
    done; \
    printf '%s\n' \
      'prefix=${pcfiledir}/../..' \
      'libdir=${prefix}/lib' \
      'includedir=${prefix}/include' \
      'Name: aasdk-complete' \
      'Description: aasdk + aap_protobuf (complete link flags)' \
      "Version: ${AASDK_VERSION}" \
      'Requires: libusb-1.0 protobuf openssl' \
      'Libs: -L${libdir} -laasdk -laap_protobuf' \
      'Cflags: -I${includedir}' > $P/lib/pkgconfig/aasdk-complete.pc; \
    cp /build/build-time.txt $P/; [ -f /build/unit-test.log ] && cp /build/unit-test.log $P/ || true; \
    { echo "arch=$(dpkg --print-architecture)"; echo "aasdk=9bf6adf version=${AASDK_VERSION}"; \
      dpkg-query -W -f='${Package}=${Version}\n' libprotobuf-dev libboost-log-dev libssl-dev libusb-1.0-0-dev g++ cmake; } > $P/BUILD-INFO.txt; \
    ldconfig

# Export layout. Docker's local exporter on Windows cannot create symlinks, so the exported tree has them
# dereferenced into plain copies. The tarballs keep the real symlinks and are what to unpack into /opt in a
# container, or onto the Pi; the runtime tarball has only the .so files.
RUN set -eux; A=$(dpkg --print-architecture); mkdir -p /export; \
    cp -rL /opt/aasdk /export/tree; \
    tar -C /opt -czf /export/aasdk-dev-trixie-$A.tar.gz --exclude=aasdk/lib/.debug aasdk; \
    cd /opt && tar -czf /export/aasdk-runtime-trixie-$A.tar.gz aasdk/lib/*.so.*

ENV PKG_CONFIG_PATH=/opt/aasdk/lib/pkgconfig \
    CMAKE_PREFIX_PATH=/opt/aasdk \
    LD_LIBRARY_PATH=/opt/aasdk/lib
WORKDIR /work

FROM scratch AS export
COPY --from=build /export/tree /
COPY --from=build /export/*.tar.gz /
