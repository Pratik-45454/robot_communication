# The following lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Set the path to ESP-IDF (assuming it's set in the environment)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project name (change to something suitable for your project)
project(robot_gatt_tcp_server)

# Optional: Set extra compile options or definitions if needed
# add_compile_options(-Wall -Wextra)

# The main component registration (assuming all source files are in the 'main' directory or root)
# If files are in root, you can adjust paths accordingly