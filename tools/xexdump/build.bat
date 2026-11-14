@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
set "XR=%~dp0..\XenonRecomp"
clang-cl /nologo /std:c++20 /O2 /EHsc /MT -DNOMINMAX -I"%XR%\XenonUtils" -I"%XR%\thirdparty\simde" "%~dp0main.cpp" /Fe"%~dp0xexdump.exe" /Fo"%~dp0main.obj" /link "%XR%\build\XenonUtils\XenonUtils.lib" || exit /b 1
