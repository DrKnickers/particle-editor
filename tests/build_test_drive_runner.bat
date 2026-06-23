@echo off
setlocal
call "C:\Program Files\<path>" >nul
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi /I src\host ^
   /Fe:tests\test_drive_runner.exe /Fo:tests\obj\ ^
   tests\test_drive_runner.cpp src\host\DriveRunner.cpp
