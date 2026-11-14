@echo off
rem Builds the Saints Row runtime (Windows, clang-cl + lld-link + Ninja).
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "%~dp0"
if not exist build\CMakeCache.txt (
    cmake -S runtime -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_LINKER=lld-link || exit /b 1
)
cmake --build build %* || exit /b 1
