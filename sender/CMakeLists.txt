# sender/CMakeLists.txt
#
# sender 工具包：程序A gen_udp_pcap（IPv6 pcap 生成器）。
# 程序B run_pktgen_sender.sh 为 shell 脚本，无需编译。
# 程序C tcp_stream_send：内核 TCP 打流客户端。
# 由根 CMakeLists.txt 通过 add_subdirectory(sender) 引入。

add_executable(gen_udp_pcap gen_udp_pcap.c)
target_include_directories(gen_udp_pcap PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(gen_udp_pcap PRIVATE -Wall -Wextra)

add_executable(tcp_stream_send tcp_stream_send.c)
target_include_directories(tcp_stream_send PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_compile_options(tcp_stream_send PRIVATE -Wall -Wextra)

install(TARGETS gen_udp_pcap tcp_stream_send RUNTIME DESTINATION bin)
