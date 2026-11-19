# aarecvd - wireless Android Auto receiver for the Veloster head unit, on aasdk (GPLv3).
# Built inside the veloaa-aarecvd-build:<arch> images, which have aasdk in /opt/aasdk and libsystemd. See build.sh.
#
# With AARECVD_STATIC=ON everything but glibc is linked in: aasdk, aap_protobuf, protobuf, OpenSSL, zlib/zstd,
# libsystemd (sd-bus, for --bt-only), libcap, libstdc++ and libgcc. glibc can stay dynamic because the Pi runs the
# same Debian 13 as the build image. libsystemd is LGPL-2.1+, which may be linked into this GPLv3 program.
cmake_minimum_required(VERSION 3.16)
project(aarecvd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)   # Debian's BoostConfig.cmake
endif()

option(AARECVD_STATIC "link everything but glibc statically" OFF)

find_package(Threads REQUIRED)
file(GLOB_RECURSE AARECVD_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
add_executable(aarecvd ${AARECVD_SOURCES})
target_compile_options(aarecvd PRIVATE -Wall -Wextra -Wno-unused-parameter -ffunction-sections -fdata-sections)

if(AARECVD_STATIC)
  find_path(AASDK_INCLUDE_DIR aasdk/Messenger/Cryptor.hpp REQUIRED)
  find_library(AASDK_STATIC_LIB NAMES libaasdk.a REQUIRED)
  find_library(AAP_PROTOBUF_STATIC_LIB NAMES libaap_protobuf.a REQUIRED)
  find_package(Boost REQUIRED)    # headers only, for asio
  target_include_directories(aarecvd PRIVATE ${AASDK_INCLUDE_DIR} ${Boost_INCLUDE_DIRS})
  target_link_libraries(aarecvd PRIVATE
    -Wl,--start-group ${AASDK_STATIC_LIB} ${AAP_PROTOBUF_STATIC_LIB} -Wl,--end-group
    -Wl,-Bstatic -lprotobuf -lssl -lcrypto -lz -lzstd -lsystemd -lcap -Wl,-Bdynamic
    -static-libstdc++ -static-libgcc -Wl,--gc-sections -Wl,--as-needed
    Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(aarecvd PRIVATE -s)
else()
  # aasdkConfig.cmake does not call find_dependency(), but its exported target refers to Boost:: and AAPProto::
  # targets, so find those first.
  find_package(Boost REQUIRED COMPONENTS system log log_setup)
  find_package(aap_protobuf REQUIRED)
  find_package(aasdk REQUIRED)
  find_library(SYSTEMD_LIB NAMES systemd REQUIRED)   # sd-bus / sd-event, for --bt-only
  target_link_libraries(aarecvd PRIVATE AASDK::aasdk ${SYSTEMD_LIB} Threads::Threads)
endif()

install(TARGETS aarecvd RUNTIME DESTINATION bin)
