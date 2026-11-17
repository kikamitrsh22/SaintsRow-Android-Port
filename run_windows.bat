@echo off
rem Runs the Windows build (keyboard controls: see README.md).
cd /d "%~dp0build"
"%~dp0build\SaintsRow.exe" "%~dp0..\Saints_Row_1_Modded.8\Saints Row 1 Modded" > "%~dp0work\win_run.log" 2>&1
