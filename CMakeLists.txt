# ESP-IDF 项目入口文件。
# 每次 CMake 配置时都会 include 项目级的 project.cmake（构建系统的核心）。
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 工程名：会决定最终固件名（la-wave-demo.bin）和 build 目录里的 elf 名字
project(la-wave-demo)
