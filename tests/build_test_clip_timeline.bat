@echo off
setlocal
call "%~dp0_env.bat" || exit /b 1
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Zi /I src\host ^
   /Fe:tests\test_clip_timeline.exe /Fo:tests\obj\ ^
   tests\test_clip_timeline.cpp
