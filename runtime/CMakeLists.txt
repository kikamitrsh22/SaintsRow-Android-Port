cmake_minimum_required(VERSION 3.20)
project(SaintsRowRuntime C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(PORT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(PPC_DIR ${PORT_ROOT}/ppc CACHE PATH "XenonRecomp output directory")
set(XENON_DIR ${PORT_ROOT}/tools/XenonRecomp CACHE PATH "XenonRecomp checkout")
set(XENON_THIRDPARTY ${XENON_DIR}/thirdparty)

if(NOT EXISTS ${PPC_DIR}/ppc_recomp_shared.h)
    message(FATAL_ERROR "Run XenonRecomp first: ${PPC_DIR} has no recompiled code")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    set(ARCH_FLAGS -march=x86-64-v3)
elseif(ANDROID OR CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    # Snapdragon 8xx and newer: ARMv8.2 with dot product and half-precision float.
    set(ARCH_FLAGS -march=armv8.2-a+fp16+dotprod)
endif()

# ---------------------------------------------------------------------------
# Recompiled game code
# ---------------------------------------------------------------------------
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${PPC_DIR}/ppc_recomp.*.cpp ${PPC_DIR}/ppc_func_mapping.cpp)
add_library(ppc STATIC ${PPC_SOURCES})
target_include_directories(ppc PUBLIC ${PPC_DIR} ${XENON_THIRDPARTY}/simde)
target_compile_options(ppc PRIVATE ${ARCH_FLAGS} -Wno-everything -fno-strict-aliasing)
if(MSVC)
    # Keep recompiled code fast even in Debug builds; nobody steps through it.
    target_compile_options(ppc PRIVATE /O2 /Ob2 /EHsc /FI${CMAKE_CURRENT_SOURCE_DIR}/src/ppc_indirect.h)
else()
    # Exceptions + unwind tables: ExTerminateThread unwinds through recompiled frames.
    target_compile_options(ppc PRIVATE -O2 -fexceptions -funwind-tables -ffp-contract=off
        -include ${CMAKE_CURRENT_SOURCE_DIR}/src/ppc_indirect.h)
endif()

# ---------------------------------------------------------------------------
# XEX loader from XenonUtils (no disassembler needed)
# ---------------------------------------------------------------------------
add_library(xexloader STATIC
    ${XENON_DIR}/XenonUtils/xex.cpp
    ${XENON_DIR}/XenonUtils/image.cpp
    ${XENON_DIR}/XenonUtils/xex_patcher.cpp
    ${XENON_DIR}/XenonUtils/memory_mapped_file.cpp
    ${XENON_THIRDPARTY}/libmspack/libmspack/mspack/lzxd.c
    ${XENON_THIRDPARTY}/tiny-AES-c/aes.c
)
target_include_directories(xexloader
    PUBLIC ${XENON_DIR}/XenonUtils ${XENON_THIRDPARTY}/simde
    PRIVATE ${XENON_THIRDPARTY}/libmspack/libmspack/mspack ${XENON_THIRDPARTY}/tiny-AES-c ${XENON_THIRDPARTY}/TinySHA1)
target_compile_definitions(xexloader PRIVATE NOMINMAX)
target_compile_options(xexloader PRIVATE -Wno-everything)

# ---------------------------------------------------------------------------
# Runtime core: kernel/XAM HLE, loader, VFS, memory front-end (platform independent)
# ---------------------------------------------------------------------------
file(GLOB_RECURSE CORE_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
list(FILTER CORE_SOURCES EXCLUDE REGEX "/src/xglue/|/src/main\.cpp$|/src/android/")
add_library(runtime_core STATIC ${CORE_SOURCES})
target_include_directories(runtime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(runtime_core PUBLIC ppc xexloader)
target_compile_options(runtime_core PRIVATE ${ARCH_FLAGS})
if(MSVC)
    target_compile_definitions(runtime_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(runtime_core PRIVATE /EHsc)
else()
    target_compile_options(runtime_core PRIVATE -fexceptions -Wno-unused-parameter)
endif()

if(ANDROID)
    # Android targets (Xenia GPU bridge + JNI entry) are added in a later stage.
    return()
endif()

# ---------------------------------------------------------------------------
# Xenia bridge: guest memory (xe::Memory + MMIO handler) and the Vulkan GPU.
# The Xenia libraries themselves are built by build_xenia.bat (MSVC) into build-xenia.
# ---------------------------------------------------------------------------
set(XENIA_DIR ${PORT_ROOT}/tools/xenia CACHE PATH "xenia-canary checkout")
set(XENIA_BUILD ${PORT_ROOT}/build-xenia CACHE PATH "build_xenia.bat output")
set(XENIA_LIB_DIR ${XENIA_BUILD}/obj/Windows)

add_library(xglue STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xglue/xglue_memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xglue/xglue_gpu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/xglue/xglue_win.cpp
    ${XENIA_DIR}/src/xenia/memory.cc
    ${XENIA_DIR}/src/xenia/cpu/mmio_handler.cc
)
target_include_directories(xglue PRIVATE
    ${XENIA_DIR} ${XENIA_DIR}/src ${XENIA_BUILD} ${XENIA_DIR}/third_party
    ${XENIA_DIR}/third_party/fmt/include ${XENIA_DIR}/third_party/Vulkan-Headers/include
    ${XENIA_DIR}/third_party/glslang)
target_compile_definitions(xglue PRIVATE
    UNICODE _UNICODE WIN32 _WIN64=1 _AMD64=1 NDEBUG USE_CPP17 VULKAN_HPP_NO_TO_STRING
    XE_OPTION_PROFILING=0 XE_OPTION_PROFILING_UI=0 _CRT_NONSTDC_NO_DEPRECATE _CRT_SECURE_NO_WARNINGS
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_USE_STB_SPRINTF IMGUI_USE_WCHAR32)
target_compile_options(xglue PRIVATE /EHsc /utf-8 -Wno-everything)
target_link_libraries(xglue PUBLIC
    ${XENIA_LIB_DIR}/xenia-gpu-vulkan.lib ${XENIA_LIB_DIR}/xenia-gpu.lib
    ${XENIA_LIB_DIR}/xenia-ui-vulkan.lib ${XENIA_LIB_DIR}/xenia-ui.lib
    ${XENIA_LIB_DIR}/xenia-base.lib ${XENIA_LIB_DIR}/glslang-spirv.lib ${XENIA_LIB_DIR}/dxbc.lib
    ${XENIA_LIB_DIR}/snappy.lib ${XENIA_LIB_DIR}/xxhash.lib ${XENIA_LIB_DIR}/fmt.lib
    ntdll wsock32 ws2_32 xinput comctl32 shcore shlwapi dxguid bcrypt dwmapi dxgi)

# ---------------------------------------------------------------------------
# Runtime
# ---------------------------------------------------------------------------
# kernel_stubs.cpp only carries linker directives, so it must be linked directly, not via a library.
add_executable(SaintsRow ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/xglue/kernel_stubs.cpp)
target_link_libraries(SaintsRow PRIVATE runtime_core xglue)
target_compile_options(SaintsRow PRIVATE ${ARCH_FLAGS})
target_compile_definitions(SaintsRow PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(SaintsRow PRIVATE /EHsc -Wno-unused-parameter -Wno-c++98-compat)
    target_link_options(SaintsRow PRIVATE /STACK:8388608)
endif()
