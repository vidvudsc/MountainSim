cmake_minimum_required(VERSION 3.24)

project(MountainsTerrainLab LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

find_package(Vulkan REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.5
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

target_compile_definitions(imgui PUBLIC
  VK_ENABLE_BETA_EXTENSIONS
)

target_link_libraries(imgui PUBLIC glfw Vulkan::Vulkan)

set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

set(VULKAN_SHADERS
  ${SHADER_SOURCE_DIR}/sky.vert
  ${SHADER_SOURCE_DIR}/sky.frag
  ${SHADER_SOURCE_DIR}/terrain.vert
  ${SHADER_SOURCE_DIR}/terrain.frag
)

set(SPIRV_SHADERS)
foreach(SHADER ${VULKAN_SHADERS})
  get_filename_component(SHADER_NAME ${SHADER} NAME)
  set(SPIRV ${SHADER_BINARY_DIR}/${SHADER_NAME}.spv)
  add_custom_command(
    OUTPUT ${SPIRV}
    COMMAND Vulkan::glslc ${SHADER} -o ${SPIRV}
    DEPENDS ${SHADER}
    COMMENT "Compiling ${SHADER_NAME}"
    VERBATIM
  )
  list(APPEND SPIRV_SHADERS ${SPIRV})
endforeach()

add_custom_target(Shaders DEPENDS ${SPIRV_SHADERS})

add_executable(Mountains
  src/main.cpp
)

add_dependencies(Mountains Shaders)

target_compile_definitions(Mountains PRIVATE
  VK_ENABLE_BETA_EXTENSIONS
  SHADER_BINARY_DIR="${SHADER_BINARY_DIR}"
)

target_include_directories(Mountains PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

# The CPU weather solver is the heaviest compute in the app; let the compiler use the
# full M-series instruction set and aggressive optimization for the vectorizable
# advection/microphysics/thermo loops. (No -ffast-math: the solver relies on finite-math
# clamps and must stay well-defined during transient spikes.)
target_compile_options(Mountains PRIVATE -O3 -mcpu=apple-m1)

target_link_libraries(Mountains PRIVATE
  glfw
  Vulkan::Vulkan
  glm::glm
  imgui
)

if(APPLE)
  target_link_libraries(Mountains PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo" "-framework QuartzCore")
endif()
