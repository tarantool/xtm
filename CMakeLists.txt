project(xtm C CXX)
cmake_minimum_required(VERSION 2.8 FATAL_ERROR)

include(CheckFunctionExists)
include(CheckSymbolExists)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

if(NOT DEFINED XTM_EMBEDDED)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -Wall -Wextra")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -O0 -Werror")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -O0 -Werror")
endif()

# Enable GNU glibc extentions.
add_definitions("-D_GNU_SOURCE")

set(CMAKE_REQUIRED_DEFINITIONS "-D_GNU_SOURCE")

check_function_exists(eventfd TARANTOOL_XTM_HAVE_EVENTFD)

set(config_h "${CMAKE_CURRENT_BINARY_DIR}/xtm/include/xtm_config.h")
configure_file(
    "xtm/xtm_config.h.cmake"
    "${config_h}"
    )
message (STATUS "")

include_directories("${CMAKE_CURRENT_BINARY_DIR}/xtm/include")

# Valgrind
include_directories(third_party)

set(lib_headers
    "${config_h}"
    xtm/xtm_api.h
    xtm/xtm_scsp_queue.h)

set(lib_sources
    xtm/xtm_api.cc)

add_library(${PROJECT_NAME} STATIC ${lib_sources})
set_property(TARGET ${PROJECT_NAME} PROPERTY POSITION_INDEPENDENT_CODE ON)

enable_testing()
#add_subdirectory(test)
add_subdirectory(perf)

if(DEFINED XTM_EMBEDDED)
    # Don't build shared library and skip INSTALL() targets if this
    # library is used as submodule in other project.
    return()
endif()

option(ENABLE_VALGRIND "Enable integration with valgrind, a memory analyzing tool" OFF)
if (NOT ENABLE_VALGRIND)
    add_definitions(-DNVALGRIND=1)
endif()

add_library(${PROJECT_NAME}_shared SHARED ${lib_sources})
set_target_properties(${PROJECT_NAME}_shared PROPERTIES VERSION 1.0 SOVERSION 1)
set_target_properties(${PROJECT_NAME}_shared PROPERTIES OUTPUT_NAME ${PROJECT_NAME})

include(GNUInstallDirs)

install(TARGETS ${PROJECT_NAME}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT library)

install(TARGETS ${PROJECT_NAME}_shared
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    COMPONENT library)

install(FILES ${lib_headers} DESTINATION include/${PROJECT_NAME})
install(DIRECTORY third_party DESTINATION include/${PROJECT_NAME} FILES_MATCHING PATTERN "*.h")
