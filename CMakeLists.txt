cmake_minimum_required(VERSION 3.10)
project(zfslite CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find FUSE (Optional)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(FUSE QUIET fuse)
endif()

# Fallback for WinFsp on Windows if pkg-config fails
if(NOT FUSE_FOUND)
    find_path(FUSE_INCLUDE_DIRS fuse.h PATHS "C:/Program Files (x86)/WinFsp/inc/fuse" "C:/Program Files/WinFsp/inc/fuse")
    find_library(FUSE_LIBRARIES NAMES winfsp-x64 winfsp PATHS "C:/Program Files (x86)/WinFsp/lib" "C:/Program Files/WinFsp/lib")
    if(FUSE_INCLUDE_DIRS AND FUSE_LIBRARIES)
        set(FUSE_FOUND TRUE)
    endif()
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/src)

# Core Source files
set(CORE_SOURCES
    src/vdev.cpp
    src/allocator.cpp
    src/zpool.cpp
    src/zfs.cpp
    src/sha256.cpp
)

# If FUSE is found, build the FUSE wrapper
if(FUSE_FOUND)
    add_definitions(-D_FILE_OFFSET_BITS=64)
    include_directories(${FUSE_INCLUDE_DIRS})
    add_executable(zfslite src/fuse_layer.cpp ${CORE_SOURCES})
    target_link_libraries(zfslite ${FUSE_LIBRARIES})
    message(STATUS "FUSE library found. Building zfslite FUSE wrapper.")
else()
    message(WARNING "FUSE library not found! Skipping build of the zfslite FUSE wrapper. Tests will still be built.")
endif()

# Test Executable
add_executable(test_vdev tests/test_vdev.cpp src/vdev.cpp)

# Test Allocator
add_executable(test_allocator tests/test_allocator.cpp src/allocator.cpp src/vdev.cpp)

# Evaluation Script
add_executable(eval_zfs tests/eval.cpp ${CORE_SOURCES})
