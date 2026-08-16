cmake_minimum_required(VERSION 3.16)
project(batcan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Release binaries run on ROS2 Humble targets without requiring users to source
# setup.bash before invoking `batcan service install`.
set(CMAKE_BUILD_RPATH "/opt/ros/humble/lib;/opt/ros/humble/lib/aarch64-linux-gnu;/opt/ros/humble/lib/x86_64-linux-gnu")
set(CMAKE_INSTALL_RPATH "/opt/ros/humble/lib;/opt/ros/humble/lib/aarch64-linux-gnu;/opt/ros/humble/lib/x86_64-linux-gnu")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)

find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(sensor_msgs REQUIRED)

file(GLOB BATCAN_MODEL_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/models/*.yml")
if(NOT BATCAN_MODEL_FILES)
  message(FATAL_ERROR "No model configuration files found in models/")
endif()

set(BATCAN_EMBEDDED_MODEL_ENTRIES "")
foreach(BATCAN_MODEL_FILE IN LISTS BATCAN_MODEL_FILES)
  get_filename_component(BATCAN_MODEL_FILENAME "${BATCAN_MODEL_FILE}" NAME)
  string(REGEX REPLACE "\\.yml$" "" BATCAN_MODEL_NAME
         "${BATCAN_MODEL_FILENAME}")
  string(LENGTH "${BATCAN_MODEL_NAME}" BATCAN_MODEL_NAME_LENGTH)
  if(NOT BATCAN_MODEL_NAME MATCHES "^[A-Za-z0-9._-]+$" OR
     BATCAN_MODEL_NAME_LENGTH GREATER 64)
    message(FATAL_ERROR "Invalid model configuration filename: ${BATCAN_MODEL_FILE}")
  endif()
  file(READ "${BATCAN_MODEL_FILE}" BATCAN_MODEL_CONTENT)
  string(SHA256 BATCAN_MODEL_HASH "${BATCAN_MODEL_CONTENT}")
  string(SUBSTRING "${BATCAN_MODEL_HASH}" 0 8 BATCAN_MODEL_DELIMITER)
  string(APPEND BATCAN_EMBEDDED_MODEL_ENTRIES
    "  {\"${BATCAN_MODEL_NAME}\", R\"BATCAN_${BATCAN_MODEL_DELIMITER}(\n${BATCAN_MODEL_CONTENT})BATCAN_${BATCAN_MODEL_DELIMITER}\"},\n")
endforeach()
list(LENGTH BATCAN_MODEL_FILES BATCAN_EMBEDDED_MODEL_COUNT)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/batcan")
configure_file(
  cmake/embedded_models.hpp.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/batcan/embedded_models.hpp"
  @ONLY)

add_library(batcan_core STATIC
  src/config.cpp
  src/models.cpp
  src/protocol.cpp
)
target_include_directories(batcan_core PUBLIC include PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_options(batcan_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(batcan
  src/main.cpp
  src/bridge.cpp
  src/service.cpp
)
set_target_properties(batcan PROPERTIES
  BUILD_RPATH "/opt/ros/humble/lib;/opt/ros/humble/lib/aarch64-linux-gnu;/opt/ros/humble/lib/x86_64-linux-gnu"
  INSTALL_RPATH "/opt/ros/humble/lib;/opt/ros/humble/lib/aarch64-linux-gnu;/opt/ros/humble/lib/x86_64-linux-gnu")
target_include_directories(batcan PRIVATE include)
target_link_libraries(batcan batcan_core)
ament_target_dependencies(batcan rclcpp sensor_msgs)
target_link_options(batcan PRIVATE -Wl,--disable-new-dtags)
target_compile_definitions(batcan PRIVATE
  BATCAN_VERSION="${PROJECT_VERSION}")
target_compile_options(batcan PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(BUILD_TESTING)
  enable_testing()
  add_executable(batcan_tests tests/test_main.cpp)
  target_include_directories(batcan_tests PRIVATE include)
  target_link_libraries(batcan_tests PRIVATE batcan_core)
  target_compile_options(batcan_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME batcan_tests COMMAND batcan_tests)
endif()

install(TARGETS batcan RUNTIME DESTINATION lib/${PROJECT_NAME})
install(FILES config.example.yml DESTINATION share/${PROJECT_NAME})

ament_package()
