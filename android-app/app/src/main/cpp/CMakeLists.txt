cmake_minimum_required(VERSION 3.22.1)
project(synthmodular CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Oboe vem do Maven como pacote prefab (buildFeatures.prefab = true no build.gradle.kts).
find_package(oboe REQUIRED CONFIG)

add_library(synthmodular SHARED native-lib.cpp)

# Os headers do motor ficam em engine/ (cópia de ../src do projeto principal).
target_include_directories(synthmodular PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/engine)
target_compile_options(synthmodular PRIVATE -O2 -Wall)

target_link_libraries(synthmodular oboe::oboe log)
