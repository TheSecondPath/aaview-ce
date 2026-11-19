#!/usr/bin/env bash
# Builds aarecvd inside Docker and, on request, runs the desk tests. The build images are
# veloaa-aarecvd-build:amd64 / :arm64 (docker/Dockerfile.build), layered on the aasdk build images
# veloaa-aasdk-build:<arch> (docker/aasdk/Dockerfile, aasdk at a fixed commit in /opt/aasdk). Both are made here
# when they are missing. Results go to pi/aarecvd/out/, which is not in git.
#
#   ./build.sh                 amd64 (for the desk tests) and the static arm64 binary for the Pi
#   ./build.sh amd64           out/aarecvd-amd64, dynamic, runs inside veloaa-aarecvd-build:amd64
#   ./build.sh arm64           out/aarecvd-arm64, static except for glibc; copy it to the Pi as is
#   ./build.sh test            builds amd64, then runs test/aarecvd_selftest.py (the session) in the amd64 image
#   ./build.sh bttest          builds amd64, then runs test/aarecvd_bt_selftest.py (--bt-only against a pretend
#                              BlueZ on a private D-Bus) in the amd64 image
#   ./build.sh settingstest    builds amd64, then runs test/aarecvd_settings_test.py (veloaa.txt, --show-settings,
#                              --quit-aaview) in the amd64 image
#   ./build.sh shtest          builds amd64, then runs pi's test_aa_ap.sh and test_install.sh with it (they read
#                              the settings through aarecvd) in the amd64 image
#   ./build.sh alltest         all four tests
#   ./build.sh aasdk           make the aasdk images again (amd64 and arm64), then the aarecvd images on top of them
#
# Making a build image the first time needs the network for Debian packages and aasdk's source from GitHub. From
# nothing, amd64 takes about 5 minutes and arm64, under qemu, about an hour and a half. After that an arm64 build
# takes a few minutes.
#
# Works from Git Bash on Windows (path conversion is turned off below) or any Linux host with Docker.
set -euo pipefail
export MSYS_NO_PATHCONV=1
HERE="$(cd "$(dirname "$0")" && pwd)"
PI="$(cd "$HERE/.." && pwd)"            # pi, mounted as /work; the tests read veloaa.txt.example there
mkdir -p "$HERE/out"
JOBS="${JOBS:-8}"

aasdk_image() {   # arch [again]: make veloaa-aasdk-build:<arch> if it is missing, or always with "again"
  local arch=$1 again=${2:-}
  if [ -n "$again" ] || ! docker image inspect "veloaa-aasdk-build:$arch" > /dev/null 2>&1; then
    (cd "$HERE/docker/aasdk" && docker build --platform "linux/$arch" --build-arg "JOBS=$JOBS" --target build \
                                             -t "veloaa-aasdk-build:$arch" .)
  fi
}

image() {   # arch: make veloaa-aarecvd-build:<arch> if it is missing
  local arch=$1
  aasdk_image "$arch"
  docker image inspect "veloaa-aarecvd-build:$arch" > /dev/null 2>&1 ||
    (cd "$HERE" && docker build --platform "linux/$arch" --build-arg "ARCH=$arch" -t "veloaa-aarecvd-build:$arch" \
                                -f docker/Dockerfile.build docker)   # relative paths: Docker on Windows can't read /c/...
}

build() {   # arch static
  local arch=$1 static=$2
  image "$arch"
  docker run --rm --platform "linux/$arch" -v "$PI:/work" "veloaa-aarecvd-build:$arch" bash -c "
    set -e
    cmake -S /work/aarecvd -B /work/aarecvd/out/build-$arch -G Ninja -DCMAKE_BUILD_TYPE=Release \
          -DAARECVD_STATIC=$static > /dev/null
    cmake --build /work/aarecvd/out/build-$arch --parallel $JOBS
    cp /work/aarecvd/out/build-$arch/aarecvd /work/aarecvd/out/aarecvd-$arch
    echo; file /work/aarecvd/out/aarecvd-$arch; ls -l /work/aarecvd/out/aarecvd-$arch
    echo needs:; readelf -d /work/aarecvd/out/aarecvd-$arch | awk '/NEEDED/{print \"   \", \$5}'"
}

run_test() {   # test script
  docker run --rm --platform linux/amd64 -v "$PI:/work" veloaa-aarecvd-build:amd64 \
    python3 "/work/aarecvd/test/$1" /work/aarecvd/out/aarecvd-amd64 ${SELFTEST_ARGS:-}
}

run_shtest() {
  docker run --rm --platform linux/amd64 -v "$PI:/work" -e VELOAA_AARECVD=/work/aarecvd/out/aarecvd-amd64 \
    veloaa-aarecvd-build:amd64 sh -c 'cd /work && sh test_aa_ap.sh && sh test_install.sh'
}

[ $# -eq 0 ] && set -- amd64 arm64
for what in "$@"; do
  case "$what" in
    amd64) build amd64 OFF ;;
    arm64) build arm64 ON ;;
    test) build amd64 OFF; run_test aarecvd_selftest.py ;;
    bttest) build amd64 OFF; run_test aarecvd_bt_selftest.py ;;
    settingstest) build amd64 OFF; run_test aarecvd_settings_test.py ;;
    shtest) build amd64 OFF; run_shtest ;;
    alltest) build amd64 OFF; run_test aarecvd_settings_test.py; run_shtest; run_test aarecvd_selftest.py
             run_test aarecvd_bt_selftest.py ;;
    aasdk) for a in amd64 arm64; do
             aasdk_image "$a" again
             docker image rm "veloaa-aarecvd-build:$a" > /dev/null 2>&1 || true   # rebuilt on top of the new aasdk image
             image "$a"
           done ;;
    *) echo "usage: $0 [amd64] [arm64] [test] [bttest] [settingstest] [shtest] [alltest] [aasdk]" >&2; exit 2 ;;
  esac
done
