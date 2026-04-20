## Copyright 2026 Patrick3399. Licensed under MPL 2.0: www.mozilla.org/MPL/2.0/
## Based on NX Metadata SDK sample plugin CMakeLists; retained MPL 2.0 header per
## NX SDK file-level copyleft.

cmake_minimum_required(VERSION 3.14)
project(p3_vision_nx)

# ---------------------------------------------------------------------------
# Metadata SDK location
#
# Must be version 6.0.1.39873 (matches NX VMS 6.1.1.42624). Downloaded from
# Nx's public update server as a zip, unpacked to a directory of your choice,
# then pass via:   cmake -DmetadataSdkDir=<unpacked-zip-path> ...
# See SDK_PIN.txt for checksum + download URL.
# ---------------------------------------------------------------------------

set(metadataSdkDir "" CACHE PATH "Path to unpacked VMS Metadata SDK zip.")
if(metadataSdkDir STREQUAL "")
    set(metadataSdkDir ${CMAKE_CURRENT_LIST_DIR}/../..) #< Assume building samples inside the SDK.
    if(NOT EXISTS ${metadataSdkDir}/src/nx/sdk OR NOT EXISTS ${metadataSdkDir}/nx_kit/src/nx/kit)
        message(FATAL_ERROR
            "Define metadataSdkDir cache variable to point to the unzipped SDK. "
            "See SDK_PIN.txt for the required version and checksum.")
    endif()
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(UNIX)
    # In Linux, for the plugin .so library, set `rpath` to "$ORIGIN" and do not set `runpath`, thus
    # enabling the lookup of the dependencies in the plugin dir first.
    string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--disable-new-dtags")
endif()
set(CMAKE_SKIP_BUILD_RPATH ON)
set(CMAKE_BUILD_WITH_INSTALL_RPATH ON)
set(CMAKE_INSTALL_RPATH "$ORIGIN")

# Use all CPU cores by MSVC.
if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /MP")
    # Do not create separate .pdb files for object files (workaround for mspdbsrv.exe bug, needed
    # for using "/MP" flag).
    add_compile_options("/Z7")
endif()

if(WIN32)
    set(API_EXPORT_MACRO "__declspec(dllexport)")
else()
    set(API_EXPORT_MACRO "__attribute__((visibility(\"default\")))")
endif()

#--------------------------------------------------------------------------------------------------
# Define nx_kit lib, static.

set(nxKitLibraryType "STATIC" CACHE STRING "" FORCE)
set(nxKitWithTests "NO" CACHE STRING "" FORCE)

add_subdirectory(${metadataSdkDir}/nx_kit ${CMAKE_CURRENT_BINARY_DIR}/nx_kit)

#--------------------------------------------------------------------------------------------------
# Define nx_sdk lib, static, depends on nx_kit.

set(SDK_SRC_DIR ${metadataSdkDir}/src)
file(GLOB_RECURSE SDK_SRC CONFIGURE_DEPENDS ${SDK_SRC_DIR}/*)

add_library(nx_sdk STATIC ${SDK_SRC})
target_include_directories(nx_sdk PUBLIC ${SDK_SRC_DIR})
target_link_libraries(nx_sdk PRIVATE nx_kit)

target_compile_definitions(nx_sdk PRIVATE NX_PLUGIN_API=${API_EXPORT_MACRO}) #< for nxLibContext()

#--------------------------------------------------------------------------------------------------
# Define p3_vision_nx lib, dynamic, depends on nx_kit and nx_sdk.
# Produces libp3_vision_nx.so (Linux) / p3_vision_nx.dll (Windows).

set(P3_VISION_NX_SRC_DIR ${CMAKE_CURRENT_LIST_DIR}/src)
file(GLOB_RECURSE P3_VISION_NX_SRC CONFIGURE_DEPENDS
    ${P3_VISION_NX_SRC_DIR}/*)

add_library(p3_vision_nx SHARED ${P3_VISION_NX_SRC})
target_include_directories(p3_vision_nx PRIVATE ${P3_VISION_NX_SRC_DIR})
target_link_libraries(p3_vision_nx PRIVATE nx_kit nx_sdk)

target_compile_definitions(p3_vision_nx
    PRIVATE NX_PLUGIN_API=${API_EXPORT_MACRO}
)
