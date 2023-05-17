# CMakeList.txt : CMake project for wifiaway, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

project ("wifiaway")

# Add source to this project's executable.
add_executable (wifiaway "lib/wifiaway.c")

# TODO: Add tests and install targets if needed.
