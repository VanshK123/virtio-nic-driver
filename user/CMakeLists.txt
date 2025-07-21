cmake_minimum_required(VERSION 3.10)
project(virtio_nic_tools C)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/cli
    ${CMAKE_CURRENT_SOURCE_DIR}/loader
    ${CMAKE_CURRENT_SOURCE_DIR}/qos_agent
    ${CMAKE_CURRENT_SOURCE_DIR}/telemetry_exporter
)

add_executable(virtio-nic-loader
    cli/main.c
    loader/loader.c
)

target_link_libraries(virtio-nic-loader
    microhttpd
    mnl
    prometheus
)

add_executable(qos-agent
    qos_agent/qos_agent.c
)

target_link_libraries(qos-agent
    microhttpd
    mnl
    prometheus
)

add_executable(telemetry-exporter
    telemetry_exporter/exporter.c
)

target_link_libraries(telemetry-exporter
    microhttpd
    mnl
    prometheus
)
