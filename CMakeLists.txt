cmake_minimum_required(VERSION 3.10)
project(smuxcpp17 LANGUAGES CXX VERSION 1.0)

# 设置C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 编译选项
if(MSVC)
    add_compile_options(/W4 /WX)
else()
    # add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# 查找Google Test
find_package(GTest REQUIRED)

# 创建smux库
add_library(smux STATIC
    src/smux.cpp
)

# 设置库包含目录
target_include_directories(smux PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

# 创建测试可执行文件
add_executable(smux_test
    test/smux_test.cpp
)

# 链接测试可执行文件
target_link_libraries(smux_test PRIVATE
    smux
    GTest::GTest
    GTest::Main
)

# 启用测试
enable_testing()
add_test(NAME smux_test COMMAND smux_test)

# 安装规则
install(TARGETS smux
    EXPORT smuxTargets
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)

install(FILES src/smux.h DESTINATION include/smux)

install(EXPORT smuxTargets
    FILE smuxTargets.cmake
    DESTINATION lib/cmake/smux
)

# 添加示例程序编译配置
add_executable(client_example examples/client_example.cpp)
target_link_libraries(client_example PRIVATE smux)

add_executable(server_example examples/server_example.cpp)
target_link_libraries(server_example PRIVATE smux)
