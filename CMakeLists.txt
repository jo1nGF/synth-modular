cmake_minimum_required(VERSION 3.16)
project(synth_modular CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Núcleo só com headers: reutilizável no projeto Android (Gradle + CMake).
add_library(modular_core INTERFACE)
target_include_directories(modular_core INTERFACE src)

add_executable(demo src/main_desktop.cpp)
target_link_libraries(demo PRIVATE modular_core)
target_compile_options(demo PRIVATE -O2 -Wall -Wextra)

add_executable(test_stages tests/test_stages.cpp)
target_link_libraries(test_stages PRIVATE modular_core)
target_compile_options(test_stages PRIVATE -O2 -Wall -Wextra)
add_executable(test_patch tests/test_patch.cpp)
target_link_libraries(test_patch PRIVATE modular_core)
target_compile_options(test_patch PRIVATE -O2 -Wall -Wextra)

enable_testing()
add_test(NAME stages COMMAND test_stages)
add_test(NAME patch COMMAND test_patch)

# --- Opcional: adaptador do Plaits (precisa do clone pichenettes/eurorack) ---
# cmake -DMI_EURORACK_DIR=/caminho/para/eurorack -DWITH_MI_PLAITS=ON ..
option(WITH_MI_PLAITS "Compila o adaptador do Plaits" OFF)
set(MI_EURORACK_DIR "" CACHE PATH "Pasta do clone pichenettes/eurorack")
if(WITH_MI_PLAITS)
  target_include_directories(modular_core INTERFACE adapters ${MI_EURORACK_DIR})
  target_compile_definitions(modular_core INTERFACE WITH_MI_PLAITS)
  # Os .cc do Plaits (dsp/, resources.cc) e do stmlib precisam entrar aqui.
  # Veja o README: a lista exata depende do seu clone.
endif()
