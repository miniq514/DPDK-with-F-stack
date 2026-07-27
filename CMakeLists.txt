cmake_minimum_required(VERSION 3.10)
project(fstack_dpdk C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2 -g)

set(PROJECT_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(INCLUDE_DIR "${PROJECT_DIR}/include")

# F-Stack 根目录：优先 -DFF_PATH / 环境变量 FF_PATH，再尝试常见路径
if(DEFINED ENV{FF_PATH} AND NOT "$ENV{FF_PATH}" STREQUAL "")
    set(_ff_default "$ENV{FF_PATH}")
else()
    set(_ff_default "/home/dev/HMH--8007/F-stack")
endif()
set(FF_PATH "${_ff_default}" CACHE PATH "F-Stack root (libfstack.a + dpdk-install)")

if(NOT EXISTS "${FF_PATH}/lib/libfstack.a")
    set(_ff_candidates
        "$ENV{FF_PATH}"
        "${FF_PATH}"
        "/home/cics/HMH/F-stack"
        "/home/dev/HMH--8004/F-stack"
        "/home/dev/HMH--8007/F-stack"
    )
    foreach(_cand IN LISTS _ff_candidates)
        if(_cand AND EXISTS "${_cand}/lib/libfstack.a")
            set(FF_PATH "${_cand}" CACHE PATH "F-Stack root (libfstack.a + dpdk-install)" FORCE)
            message(STATUS "Found F-Stack at ${FF_PATH}")
            break()
        endif()
    endforeach()
endif()

set(FF_DPDK_PC "${FF_PATH}/dpdk-install/lib/x86_64-linux-gnu/pkgconfig")

# ---------- 依赖 ----------

find_library(LIBURING_LIB uring REQUIRED)
find_path(LIBURING_INCLUDE liburing.h REQUIRED)

find_package(PkgConfig REQUIRED)
set(_system_pc "/usr/lib/x86_64-linux-gnu/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "${FF_DPDK_PC}:${_system_pc}:$ENV{PKG_CONFIG_PATH}")
pkg_check_modules(DPDK REQUIRED libdpdk)
execute_process(
    COMMAND ${PKG_CONFIG_EXECUTABLE} --static --libs libdpdk
    OUTPUT_VARIABLE DPDK_STATIC_LIBS
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT EXISTS "${FF_PATH}/lib/libfstack.a")
    message(FATAL_ERROR
        "libfstack.a not found.\n"
        "  Tried FF_PATH=${FF_PATH}\n"
        "  This project links against F-Stack (libfstack.a + dpdk-install).\n"
        "  Fix options:\n"
        "    1) Copy/build F-Stack on this machine, then:\n"
        "         cmake -S . -B build -DFF_PATH=/path/to/F-stack\n"
        "    2) Or export FF_PATH=/path/to/F-stack before cmake\n"
        "  Expected layout: \$FF_PATH/lib/libfstack.a and \$FF_PATH/dpdk-install/...")
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
target_compile_options(fstack_udp_recv PRIVATE ${DPDK_CFLAGS_OTHER} -DFSTACK_ZC_RECV)
target_link_libraries(fstack_udp_recv PRIVATE
    udp_writer
    -Wl,--whole-archive ${FF_PATH}/lib/libfstack.a -Wl,--no-whole-archive
    -Wl,--whole-archive ${DPDK_STATIC_LIBS} -Wl,--no-whole-archive
    pthread rt m dl numa crypto z
)

install(TARGETS fstack_udp_recv RUNTIME DESTINATION bin)

# ---------- F-Stack TCP 收包主程序 ----------

add_executable(fstack_tcp_recv src/fstack_tcp_recv.c)
target_include_directories(fstack_tcp_recv PRIVATE
    ${INCLUDE_DIR}
    ${PROJECT_DIR}/io-uring
    ${LIBURING_INCLUDE}
    ${FF_PATH}/lib
    ${DPDK_INCLUDE_DIRS}
)
target_compile_options(fstack_tcp_recv PRIVATE ${DPDK_CFLAGS_OTHER})
target_link_libraries(fstack_tcp_recv PRIVATE
    udp_writer
    -Wl,--whole-archive ${FF_PATH}/lib/libfstack.a -Wl,--no-whole-archive
    -Wl,--whole-archive ${DPDK_STATIC_LIBS} -Wl,--no-whole-archive
    pthread rt m dl numa crypto z
)

install(TARGETS fstack_tcp_recv RUNTIME DESTINATION bin)

# ---------- F-Stack TCP 发端主程序 ----------

add_executable(fstack_tcp_send src/fstack_tcp_send.c)
target_include_directories(fstack_tcp_send PRIVATE
    ${INCLUDE_DIR}
    ${PROJECT_DIR}/sender/include
    ${FF_PATH}/lib
    ${DPDK_INCLUDE_DIRS}
)
target_compile_options(fstack_tcp_send PRIVATE ${DPDK_CFLAGS_OTHER})
target_link_libraries(fstack_tcp_send PRIVATE
    -Wl,--whole-archive ${FF_PATH}/lib/libfstack.a -Wl,--no-whole-archive
    -Wl,--whole-archive ${DPDK_STATIC_LIBS} -Wl,--no-whole-archive
    pthread rt m dl numa crypto z
)

install(TARGETS fstack_tcp_send RUNTIME DESTINATION bin)

# ---------- sender 工具包 ----------
# 程序A gen_udp_pcap（IPv6 pcap 生成器）；程序B run_pktgen_sender.sh 为脚本，无需编译。
# 程序C tcp_stream_send：内核 TCP 打流（V2.0）。
add_subdirectory(sender)
