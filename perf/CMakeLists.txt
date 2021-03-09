set(CMAKE_CXX_STANDARD 11)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(AUTHOR_WARNING "Benchmark available only in release build")
    return()
endif()

set(BENCHMARK_DOWNLOAD_DEPENDENCIES ON)
add_subdirectory(benchmark)

include_directories("${PROJECT_SOURCE_DIR}/src")

add_executable(xtm.perftest xtm.cc)
target_link_libraries(xtm.perftest xtm benchmark::benchmark)
