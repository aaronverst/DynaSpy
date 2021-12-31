# CMakeList.txt : CMake project for DynaSpy, include source and define
# project specific logic here.
#
cmake_minimum_required (VERSION 3.8)

project ("DynaSpy")


# Add source to this project's executable.
add_executable (DynaSpy "DynaSpy.cpp" "DynaSpy.h" "args.hxx")

# TODO: Add tests and install targets if needed.
