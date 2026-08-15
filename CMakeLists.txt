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

add_library(batcan_core STATIC
  src/config.cpp
  src/protocol.cpp
)
target_include_directories(batcan_core PUBLIC include)
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
