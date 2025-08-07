message(STATUS "Fetching Google Test.")
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        main 
  SOURCE_DIR     ${FETCHCONTENT_BASE_DIR}/googletest
  BINARY_DIR     ${FETCHCONTENT_BASE_DIR}/googletest-build
)

FetchContent_MakeAvailable(googletest)

FetchContent_GetProperties(googletest)
if (googletest_POPULATED)
    message(STATUS "Google Test is populated successfully.")
else()
    message(FATAL_ERROR "Google Test population failed.")
    FetchContent_Populate(googletest)
endif()

message(STATUS "Google Test source dir: ${googletest_SOURCE_DIR}")
message(STATUS "Google Test binary dir: ${googletest_BINARY_DIR}")

include_directories(
    ${googletest_SOURCE_DIR}/googletest/include
    ${googletest_SOURCE_DIR}/googlemock/include
)

function(add_lcfs_test test_name)
    add_executable(${test_name} ${ARGN}) 
    target_link_libraries(${test_name} PRIVATE gtest gtest_main lc_fs)
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

add_lcfs_test(image_format_test
    image_format_test.cpp
)

add_lcfs_test(block_device_test
    block_device_test.cpp
)

add_lcfs_test(thread_pool_test
    thread_pool_test.cpp
)

# add_lcfs_test(block_manager_test
#     block_manager_test.cpp
# )

# add_lcfs_test(inode_manager_test
#     inode_manager_test.cpp
# )

add_lcfs_test(block_buffer_test
    block_buffer_test.cpp
)


# add_lcfs_test(inode_buffer_test
#     inode_buffer_test.cpp
# )