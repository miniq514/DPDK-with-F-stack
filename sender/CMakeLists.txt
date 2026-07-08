# sender/CMakeLists.txt
#
# sender 工具包：程序A gen_udp_pcap（IPv6 pcap 生成器）。
# 程序B run_pktgen_sender.sh 为 shell 脚本，无需编译。
# 由根 CMakeLists.txt 通过 add_subdirectory(sender) 引入。

add_executable(gen_udp_pcap gen_udp_pcap.c)
target_include_directories(gen_udp_pcap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(gen_udp_pcap PRIVATE -Wall -Wextra)

install(TARGETS gen_udp_pcap RUNTIME DESTINATION bin)
