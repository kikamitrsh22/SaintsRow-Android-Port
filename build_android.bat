@echo off
rem Builds libsaintsrow for Android arm64-v8a with the NDK (game + runtime + Xendroid Xenia GPU).
setlocal
if not defined ANDROID_SDK set "ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk"
if not defined ANDROID_NDK set "ANDROID_NDK=%ANDROID_SDK%\ndk\29.0.14206865"
set "CMAKE_BIN=%ANDROID_SDK%\cmake\3.31.6\bin"
set "PATH=%CMAKE_BIN%;%PATH%"
if not defined VULKAN_SDK set "VULKAN_SDK=C:\VulkanSDK\1.4.363.0"
cd /d "%~dp0"
if not exist build-android\CMakeCache.txt (
    cmake -S android\native -B build-android -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
        -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK%\build\cmake\android.toolchain.cmake" ^
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static || exit /b 1
)
cmake --build build-android %* || exit /b 1
