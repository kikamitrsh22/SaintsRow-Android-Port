@echo off
rem Builds the Xenia libraries the runtime links against (GPU, UI, base).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
if not defined VULKAN_SDK set "VULKAN_SDK=C:\VulkanSDK\1.4.363.0"
cd /d "%~dp0"
if not exist build-xenia\CMakeCache.txt (
    cmake -S tools\xenia -B build-xenia -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
        -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded || exit /b 1
)
cmake --build build-xenia --target xenia-gpu-vulkan xenia-ui-vulkan xenia-gpu xenia-ui xenia-base %* || exit /b 1
