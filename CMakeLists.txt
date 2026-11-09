# ======================================================================
#  KonvoyTP -- ETS2 Konvoy Isinlanma eklentisi (SPF-Framework plugin)
# ======================================================================
cmake_minimum_required(VERSION 3.20)

project(KonvoyTP VERSION 1.1.0 LANGUAGES CXX)
if(NOT PROJECT_VERSION_TWEAK)
  set(PROJECT_VERSION_TWEAK 0)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime (/MT) so friends do not need VC++ redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(MSVC)
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  add_compile_options(/utf-8 /EHa /W3)
endif()

set(PLUGIN_NAME "KonvoyTP")
set(RC_COMPANY "KonvoyTP")

add_library(${PLUGIN_NAME} SHARED src/KonvoyTP.cpp)

target_include_directories(${PLUGIN_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/SPF_API"
)

target_compile_definitions(${PLUGIN_NAME} PRIVATE
  _AMD64_
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  _CRT_SECURE_NO_WARNINGS
  PLUGIN_NAME="${PLUGIN_NAME}"
  PLUGIN_VERSION="${PROJECT_VERSION}"
)

# Version resource
set(RC_DESCRIPTION "KonvoyTP - ETS2 konvoy isinlanma eklentisi (SPF)")
set(RC_INTERNAL_NAME "${PLUGIN_NAME}.dll")
set(RC_FILENAME "${PLUGIN_NAME}.dll")
set(RC_PRODUCT "${PLUGIN_NAME}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in" "${CMAKE_CURRENT_BINARY_DIR}/version.rc" @ONLY)
target_sources(${PLUGIN_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/version.rc")

set_target_properties(${PLUGIN_NAME} PROPERTIES
  PREFIX ""
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${PLUGIN_NAME}"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/out/${PLUGIN_NAME}"
)
foreach(cfg Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER ${cfg} CFGU)
  set_target_properties(${PLUGIN_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${CFGU} "${CMAKE_BINARY_DIR}/out/${PLUGIN_NAME}"
    LIBRARY_OUTPUT_DIRECTORY_${CFGU} "${CMAKE_BINARY_DIR}/out/${PLUGIN_NAME}")
endforeach()
