# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# ESP-IDF 5.5.x rejects toolchains with the same GCC version but a different
# package date stamp. Allow the project to build with a warning instead.
set(ENV{IDF_MAINTAINER} "1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(alarm_system)
