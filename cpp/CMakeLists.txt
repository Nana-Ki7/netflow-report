cmake_minimum_required(VERSION 3.16)
project(netflow_chart CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 没有第三方依赖，所以这个 CMakeLists 短得可怜
add_executable(netflow-chart main.cpp)

if(MSVC)
    target_compile_options(netflow-chart PRIVATE /W4)
else()
    target_compile_options(netflow-chart PRIVATE -Wall -Wextra)
endif()

# 顺手加一个把示例数据画出来的 target：cmake --build . --target demo-chart
add_custom_target(
    demo-chart
    COMMAND netflow-chart ${CMAKE_CURRENT_SOURCE_DIR}/data/sample_monthly.csv ${CMAKE_CURRENT_BINARY_DIR}/chart.html
    DEPENDS netflow-chart
    COMMENT "用示例数据生成 chart.html"
)

message(STATUS "构建完成后运行：./netflow-chart data/你的数据.csv")
