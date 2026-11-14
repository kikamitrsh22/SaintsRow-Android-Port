@echo off
rem Builds libsaintsrow.so (NDK) and packages it into an APK (Gradle).
rem Output: android\app\build\outputs\apk\debug\app-debug.apk
setlocal
cd /d "%~dp0"
if not defined ANDROID_SDK set "ANDROID_SDK=%LOCALAPPDATA%\Android\Sdk"
set "ANDROID_HOME=%ANDROID_SDK%"
if not defined JAVA_HOME for /d %%J in ("C:\Program Files\Microsoft\jdk-17*") do set "JAVA_HOME=%%J"

call "%~dp0build_android.bat" --target saintsrow || exit /b 1

set "STRIP=%ANDROID_SDK%\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
if not exist android\app\src\main\jniLibs\arm64-v8a mkdir android\app\src\main\jniLibs\arm64-v8a
"%STRIP%" --strip-unneeded -o android\app\src\main\jniLibs\arm64-v8a\libsaintsrow.so build-android\libsaintsrow.so || exit /b 1

if not exist android\local.properties (
    echo sdk.dir=%ANDROID_SDK:\=\\%> android\local.properties
)
cd /d "%~dp0android"
call "%~dp0android\gradlew.bat" --no-daemon assembleDebug %* || exit /b 1
echo APK: %~dp0android\app\build\outputs\apk\debug\app-debug.apk
