# CMakeList.txt : CMake project for octoping, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

project ("octoping")

# Add source to this project's executable.
add_executable (octoping "lib/octoping.c")

# TODO: Add tests and install targets if needed.
