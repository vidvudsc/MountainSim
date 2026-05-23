cmake_minimum_required(VERSION 3.24)

project(MountainsTerrainLab LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(glm REQUIRED)
find_package(GLEW REQUIRED)
find_package(Vulkan REQUIRED)

FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.5
)
FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

target_compile_definitions(imgui PUBLIC
  IMGUI_IMPL_OPENGL_LOADER_GLEW
  GL_SILENCE_DEPRECATION
)

target_link_libraries(imgui PUBLIC glfw GLEW::GLEW OpenGL::GL)

add_executable(Mountains
  src/main.cpp
)

if(APPLE)
  set_source_files_properties(src/main.cpp PROPERTIES LANGUAGE OBJCXX)
endif()

target_compile_definitions(Mountains PRIVATE
  GL_SILENCE_DEPRECATION
)

target_link_libraries(Mountains PRIVATE
  glfw
  GLEW::GLEW
  OpenGL::GL
  glm::glm
  imgui
)

target_include_directories(Mountains PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb
)

if(APPLE)
  target_link_libraries(Mountains PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreVideo")
endif()

add_executable(VulkanProbe
  src/vulkan_probe.cpp
)

target_compile_definitions(VulkanProbe PRIVATE
  VK_ENABLE_BETA_EXTENSIONS
)

target_link_libraries(VulkanProbe PRIVATE
  glfw
  Vulkan::Vulkan
)
