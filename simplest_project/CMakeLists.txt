cmake_minimum_required(VERSION 3.5)
project(Mini)

# 创建libA
add_library(libA a.cpp)

# 创建libB，依赖libA
add_library(libB b.cpp)
target_link_libraries(libB PUBLIC libA)

# 创建主程序，依赖libB
add_executable(app main.cpp)
target_link_libraries(app PRIVATE libB)