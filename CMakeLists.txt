cmake_minimum_required(VERSION 3.10)
project(fstack_dpdk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2 -g)

set(PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(INCLUDE_DIR "${PROJECT_DIR}/include")
set(FF_PATH "/home/dev/HMH--8007/F-stack" CACHE PATH "F-Stack root (libfstack.a)")
set(FF_DPDK_PC "${FF_PATH}/dpdk-install/lib/x86_64-linux-gnu/pkgconfig")

# ---------- 依赖 ----------

find_library(LIBURING_LIB uring REQUIRED)
find_path(LIBURING_INCLUDE liburing.h REQUIRED)

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${FF_DPDK_PC}:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(DPDK REQUIRED libdpdk)
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs libdpdk
    OUTPUT_VARIABLE DPDK_STATIC_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT EXISTS "${FF_PATH}/lib/libfstack.a")
    message(FATAL_ERROR "libfstack.a not found at ${FF_PATH}/lib/libfstack.a")
endif()

# F-Stack 构建默认启用 IPv6；与 example/Makefile 对齐
add_compile_definitions(INET6)

# ---------- io_uring 写盘库 ----------

add_library(udp_writer STATIC io-uring/udp_writer.c)
target_include_directories(udp_writer PUBLIC
    ${INCLUDE_DIR}
    ${LIBURING_INCLUDE}
)
target_link_libraries(udp_writer PUBLIC ${LIBURING_LIB})

# ---------- F-Stack UDP 收包主程序 ----------

add_executable(fstack_udp_recv src/fstack_udp_recv.c)
target_include_directories(fstack_udp_recv PRIVATE
    ${INCLUDE_DIR}
    ${PROJECT_DIR}/io-uring
    ${LIBURING_INCLUDE}
    ${FF_PATH}/lib
    ${DPDK_INCLUDE_DIRS}
)
target_compile_options(fstack_udp_recv PRIVATE ${DPDK_CFLAGS_OTHER})
target_link_libraries(fstack_udp_recv PRIVATE
    udp_writer
    -Wl,--whole-archive ${FF_PATH}/lib/libfstack.a -Wl,--no-whole-archive
    -Wl,--whole-archive ${DPDK_STATIC_LIBS} -Wl,--no-whole-archive
    pthread rt m dl numa crypto z
)

install(TARGETS fstack_udp_recv RUNTIME DESTINATION bin)

# ---------- sender 工具包 ----------
# 程序A gen_udp_pcap（IPv6 pcap 生成器）；程序B run_pktgen_sender.sh 为脚本，无需编译。
add_subdirectory(sender)
