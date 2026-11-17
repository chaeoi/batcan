cmake_minimum_required(VERSION 3.16)
set(BATCAN_VERSION "" CACHE STRING "Build version for batcan and ROS package metadata")
if(NOT BATCAN_VERSION)
  execute_process(COMMAND git describe --tags --always --match "[0-9][0-9][0-9][0-9].*.*"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE BATCAN_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT BATCAN_VERSION)
    set(BATCAN_VERSION "dev")
  endif()
endif()

# ROS manifests need a numeric version; development snapshots use 0.0.0.
set(BATCAN_PACKAGE_VERSION "0.0.0")
if(BATCAN_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  set(BATCAN_PACKAGE_VERSION "${BATCAN_VERSION}")
endif()
project(batcan VERSION "${BATCAN_PACKAGE_VERSION}" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Release binaries run on ROS2 Humble targets without requiring users to source
# setup.bash before invoking `batcan service install`.
set(CMAKE_BUILD_RPATH "/opt/ros/humble/lib;/opt/ros/humble/lib/aarch64-linux-gnu;/opt/ros/humble/lib/x86_64-linux-gnu")
set(CMAKE_INSTALL_RPATH "/opt/ros/humble/lib;/opt/ros/humble/lib/aarch64-linux-gnu;/opt/ros/humble/lib/x86_64-linux-gnu")
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)

find_package(ament_cmake REQUIRED)
find_package(diagnostic_msgs REQUIRED)
find_package(rclcpp REQUIRED)
find_package(fastrtps REQUIRED)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/package.xml")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/package.xml" BATCAN_PACKAGE_XML)
string(REGEX REPLACE "<version>[^<]+</version>"
  "<version>${PROJECT_VERSION}</version>"
  BATCAN_PACKAGE_XML "${BATCAN_PACKAGE_XML}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/package.xml"
  "${BATCAN_PACKAGE_XML}")
ament_package_xml(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")

set(BATCAN_MODEL_FILE "${CMAKE_CURRENT_SOURCE_DIR}/models/bms.yml")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${BATCAN_MODEL_FILE}")
if(NOT EXISTS "${BATCAN_MODEL_FILE}")
  message(FATAL_ERROR "Missing BMS configuration: models/bms.yml")
endif()
file(GLOB BATCAN_MODEL_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/models/*.yml")
list(LENGTH BATCAN_MODEL_FILES BATCAN_MODEL_FILE_COUNT)
if(NOT BATCAN_MODEL_FILE_COUNT EQUAL 1)
  message(FATAL_ERROR "models/ must contain exactly one YAML file: bms.yml")
endif()
file(READ "${BATCAN_MODEL_FILE}" BATCAN_MODEL_CONTENT)
string(SHA256 BATCAN_MODEL_HASH "${BATCAN_MODEL_CONTENT}")
string(SUBSTRING "${BATCAN_MODEL_HASH}" 0 8 BATCAN_MODEL_DELIMITER)
set(BATCAN_EMBEDDED_MODEL_ENTRIES
  "  {\"bms\", R\"BATCAN_${BATCAN_MODEL_DELIMITER}(\n${BATCAN_MODEL_CONTENT})BATCAN_${BATCAN_MODEL_DELIMITER}\"},\n")
set(BATCAN_EMBEDDED_MODEL_COUNT 1)
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
target_include_directories(batcan PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(batcan batcan_core fastrtps)
ament_target_dependencies(batcan diagnostic_msgs rclcpp)
target_link_options(batcan PRIVATE -Wl,--disable-new-dtags)
target_compile_definitions(batcan PRIVATE
  BATCAN_VERSION="${BATCAN_VERSION}")
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
# ament installs the source placeholder; install the versioned manifest last.
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/generated/package.xml"
  DESTINATION share/${PROJECT_NAME})
