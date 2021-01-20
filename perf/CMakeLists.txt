cmake_minimum_required(VERSION 2.8)

set(perf_test_dispatch_invoke_sources
    perf_test_dispatch_invoke.c
)

set(perf_test_send_recv_sources
    perf_test_send_recv.c
)

if(DEFINED XTM_EMBEDDED)
    include_directories(${LUAJIT_INCLUDE_DIRS})
    add_library(ppq SHARED ppq.c)
    target_link_libraries(ppq xtm_api_static pthread)
endif()

add_executable(perf_test_dispatch_invoke ${perf_test_dispatch_invoke_sources})
target_link_libraries(perf_test_dispatch_invoke xtm_api_static pthread)
add_executable(perf_test_send_recv ${perf_test_send_recv_sources})
target_link_libraries(perf_test_send_recv xtm_api_static pthread)
