add_executable(xtm.test xtm.c unit.c)
target_link_libraries(xtm.test xtm pthread)

include_directories("${PROJECT_SOURCE_DIR}")

add_test(xtm ${CMAKE_CURRENT_BUILD_DIR}/xtm.test)

if(DEFINED XTM_EMBEDDED)
    return()
endif()

if(POLICY CMP0037)
    cmake_policy(SET CMP0037 OLD) # don't blame "test" target name
endif(POLICY CMP0037)

add_custom_target(xtm_test
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
    COMMAND ctest
    DEPENDS xtm.test
)
