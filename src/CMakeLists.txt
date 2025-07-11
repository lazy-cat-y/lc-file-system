# src/CMakeLists.txt

# 自动收集 C++ 和 C 文件
file(GLOB_RECURSE FS_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
)

# 生成静态库 simplefs
add_library(lc_fs STATIC ${FS_SOURCES})

# 将 include/ 添加到此目标的 include 路径
target_include_directories(lc_fs PUBLIC ${CMAKE_SOURCE_DIR}/include)

# 如果启用了多线程
if (ENABLE_MULTITHREAD)
    target_link_libraries(lc_fs PUBLIC pthread)
endif()