# Android native build: libsaintsrow.so = recompiled game + runtime + Xendroid's Xenia GPU (Vulkan).
# Xenia setup mirrors Xendroid+'s emulator-core/src/main/cpp/CMakeLists.txt (GPL/BSD sources live in
# tools/xendroid, patched for host mode - see patches/xendroid-recomp-host-mode.patch).
cmake_minimum_required(VERSION 3.30)
project(saintsrow LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

get_filename_component(PORT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(XENDROID_CPP "${PORT_ROOT}/tools/xendroid/emulator-core/src/main/cpp" CACHE PATH "Xendroid native sources")
set(XENDROID_ROOT "${PORT_ROOT}/tools/xendroid")

add_compile_options(-march=armv8-a -fno-omit-frame-pointer -funwind-tables)
add_link_options(-Wl,-z,max-page-size=16384)

# ---------------------------------------------------------------------------
# Shader bytecode (SPIR-V) - generated at configure time like Xendroid does
# ---------------------------------------------------------------------------
set(_xe_vk_hint_dirs)
if(DEFINED ENV{VULKAN_SDK})
    list(APPEND _xe_vk_hint_dirs "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
endif()
foreach(_tool glslangValidator spirv-opt spirv-dis)
    string(MAKE_C_IDENTIFIER "XE_TOOL_${_tool}" _tool_var)
    find_program(${_tool_var} NAMES ${_tool} HINTS ${_xe_vk_hint_dirs} REQUIRED)
endforeach()
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(_xe_shader_root "${XENDROID_CPP}/xenia/src/xenia")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${XENDROID_CPP}/xenia/tools/build/gen_android_spirv.py"
            "${_xe_shader_root}/gpu/shaders" "${_xe_shader_root}/ui/shaders"
    RESULT_VARIABLE _xe_spirv_result
    OUTPUT_VARIABLE _xe_spirv_out)
message(STATUS "SPIR-V shader bytecode: ${_xe_spirv_out}")
if(NOT _xe_spirv_result EQUAL 0)
    message(FATAL_ERROR "SPIR-V shader bytecode generation failed (exit ${_xe_spirv_result}).")
endif()

# ---------------------------------------------------------------------------
# Xenia (Xendroid fork) and the extras its Vulkan UI needs
# ---------------------------------------------------------------------------
set(ZEROFG_VULKAN_HEADERS_DIR "${XENDROID_CPP}/xenia/third_party/Vulkan-Headers/include")
set(ZEROFG_PREBUILT_SHADERS ON CACHE STRING "Use ZeroFG's qualified shipped SPIR-V")
add_subdirectory(${XENDROID_CPP}/zerofg zerofg EXCLUDE_FROM_ALL)
add_subdirectory(${XENDROID_CPP}/xenia xenia EXCLUDE_FROM_ALL)

target_sources(xenia-ui-vulkan PRIVATE "${XENDROID_CPP}/third_party/winfg/src/framegen.cpp")
target_include_directories(xenia-ui-vulkan PRIVATE "${XENDROID_CPP}")
add_subdirectory(${XENDROID_CPP}/third_party/lsfg-dxbc lsfg-dxbc)
add_subdirectory(${XENDROID_CPP}/third_party/lsfg lsfg)
target_link_libraries(xenia-ui-vulkan PRIVATE xendroid-lsfg)

set(XENDROID_COLOR_SPV "${CMAKE_CURRENT_BINARY_DIR}/xendroid_color_filter.spv")
set(XENDROID_COLOR_HEADER "${CMAKE_CURRENT_BINARY_DIR}/xendroid_color_filter.inc")
add_custom_command(OUTPUT "${XENDROID_COLOR_HEADER}"
    COMMAND "${XE_TOOL_glslangValidator}" -V --target-env vulkan1.0
        "${XENDROID_CPP}/xenia/src/xenia/ui/vulkan/shaders/xendroid_color_filter.comp" -o "${XENDROID_COLOR_SPV}"
    COMMAND "${Python3_EXECUTABLE}" "${XENDROID_ROOT}/tools/spirv-to-header.py"
        "${XENDROID_COLOR_SPV}" "${XENDROID_COLOR_HEADER}" xendroid_color_filter_spv
    DEPENDS "${XENDROID_CPP}/xenia/src/xenia/ui/vulkan/shaders/xendroid_color_filter.comp")
add_custom_target(xendroid-color-shader DEPENDS "${XENDROID_COLOR_HEADER}")
add_dependencies(xenia-ui-vulkan xendroid-color-shader)
target_include_directories(xenia-ui-vulkan PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")

add_subdirectory(${XENDROID_CPP}/xenia/third_party/libadrenotools libadrenotools EXCLUDE_FROM_ALL)

# Convenience target for stage 2: everything the GPU bridge links against.
add_custom_target(xenia_gpu_libs DEPENDS xenia-gpu-vulkan xenia-gpu xenia-ui-vulkan xenia-ui xenia-base)

# ---------------------------------------------------------------------------
# Recompiled game + runtime core (platform independent)
# ---------------------------------------------------------------------------
add_subdirectory(${PORT_ROOT}/runtime runtime)

# ---------------------------------------------------------------------------
# libsaintsrow.so
# ---------------------------------------------------------------------------
set(RT_SRC ${PORT_ROOT}/runtime/src)
add_library(saintsrow SHARED
    ${RT_SRC}/android/android_main.cpp
    ${RT_SRC}/android/kernel_stubs_elf.cpp
    ${RT_SRC}/xglue/xglue_memory.cpp
    ${RT_SRC}/xglue/xglue_gpu.cpp
    ${RT_SRC}/xglue/xglue_android.cpp
    ${XENDROID_CPP}/xenia/src/xenia/memory.cc
    ${XENDROID_CPP}/xenia/src/xenia/cpu/mmio_handler.cc
    ${XENDROID_CPP}/xenia/src/xenia/ui/surface_android.cc
)
target_include_directories(saintsrow PRIVATE
    ${XENDROID_CPP}/xenia ${XENDROID_CPP}/xenia/src ${XENDROID_CPP}/xenia/third_party
    ${XENDROID_CPP}/xenia/third_party/fmt/include ${XENDROID_CPP}/xenia/third_party/Vulkan-Headers/include
    ${XENDROID_CPP}/xenia/third_party/glslang ${CMAKE_BINARY_DIR}/xenia)
target_compile_options(saintsrow PRIVATE -fexceptions -Wno-everything)
target_link_options(saintsrow PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections)
target_link_libraries(saintsrow PRIVATE
    runtime_core
    xenia-gpu-vulkan xenia-gpu xenia-ui-vulkan xenia-ui xenia-base
    glslang-spirv dxbc snappy xxhash fmt adrenotools
    android log vulkan dl z)
