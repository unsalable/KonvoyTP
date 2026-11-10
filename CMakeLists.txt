# ======================================================================
#  KonvoyTP -- ETS2 Konvoy Isinlanma eklentisi (SPF-Framework plugin)
# ======================================================================
cmake_minimum_required(VERSION 3.20)

project(KonvoyTP VERSION 1.3.0 LANGUAGES CXX)
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
target_link_libraries(${PLUGIN_NAME} PRIVATE winhttp bcrypt)

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

# ======================================================================
#  KonvoyTP uygulamasi (kur / guncelle / kaldir): KonvoyTP_Kurulum.exe
#  Mod, metinleri ve SPF Framework uygulamanin icine gomulur.
# ======================================================================
set(APP_VERSION_OVERRIDE "" CACHE STRING "Sadece test: uygulamanin kendini eski surum sanmasi icin")
set(APP_VERSION "${PROJECT_VERSION}")
if(APP_VERSION_OVERRIDE)
  set(APP_VERSION "${APP_VERSION_OVERRIDE}")
endif()

add_executable(ktp_pack tools/pack.cpp)
target_compile_definitions(ktp_pack PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(ktp_pack PRIVATE cabinet)

set(SPF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SPF-Framework-1.2.7")
set(APP_PAYLOAD "${CMAKE_BINARY_DIR}/payload.bin")
set(PACK_ARGS
  "KonvoyTP/KonvoyTP.dll=$<TARGET_FILE:${PLUGIN_NAME}>"
  "KonvoyTP/localization/tr.json=${CMAKE_CURRENT_SOURCE_DIR}/localization/tr.json"
  "KonvoyTP/localization/en.json=${CMAKE_CURRENT_SOURCE_DIR}/localization/en.json")
set(PACK_DEPS ${PLUGIN_NAME} ktp_pack
  "${CMAKE_CURRENT_SOURCE_DIR}/localization/tr.json" "${CMAKE_CURRENT_SOURCE_DIR}/localization/en.json")
if(EXISTS "${SPF_DIR}/spf-framework.dll")
  list(APPEND PACK_ARGS
    "SPF/spf-framework.dll=${SPF_DIR}/spf-framework.dll"
    "SPF/LICENSE-SPF-Framework.txt=${CMAKE_CURRENT_SOURCE_DIR}/SPF_API/LICENSE-SPF-Framework.txt"
    "SPF/spfAssets/localization/=@${SPF_DIR}/spfAssets/localization")
  list(APPEND PACK_DEPS "${SPF_DIR}/spf-framework.dll")
else()
  message(WARNING "SPF-Framework 1.2.7 yok (paketle.ps1 indirir): uygulama SPF olmadan derleniyor.")
endif()
add_custom_command(OUTPUT "${APP_PAYLOAD}"
  COMMAND ktp_pack "${APP_PAYLOAD}" ${PACK_ARGS}
  DEPENDS ${PACK_DEPS}
  COMMENT "Uygulama paketi hazirlaniyor"
  VERBATIM)
add_custom_target(ktp_payload DEPENDS "${APP_PAYLOAD}")

set(APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/app/KonvoyTP.ico")
set(APP_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/app/app.manifest")
configure_file(app/app.rc.in "${CMAKE_BINARY_DIR}/app.rc" @ONLY)
set_source_files_properties("${CMAKE_BINARY_DIR}/app.rc" PROPERTIES OBJECT_DEPENDS "${APP_PAYLOAD};${APP_ICON};${APP_MANIFEST}")
add_executable(KonvoyTPApp WIN32 app/KonvoyTPApp.cpp "${CMAKE_BINARY_DIR}/app.rc")
add_dependencies(KonvoyTPApp ktp_payload)
target_compile_definitions(KonvoyTPApp PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS
  APP_VERSION="${APP_VERSION}" SPF_VERSION="1.2.7")
target_link_libraries(KonvoyTPApp PRIVATE d2d1 dwrite dwmapi cabinet shell32 ole32 uuid version advapi32 user32 winhttp bcrypt)
target_link_options(KonvoyTPApp PRIVATE /MANIFEST:NO)
set_target_properties(KonvoyTPApp PROPERTIES OUTPUT_NAME "KonvoyTP_Kurulum")
foreach(cfg "" _DEBUG _RELEASE _RELWITHDEBINFO _MINSIZEREL)
  set_target_properties(KonvoyTPApp PROPERTIES RUNTIME_OUTPUT_DIRECTORY${cfg} "${CMAKE_BINARY_DIR}/out/app")
endforeach()
