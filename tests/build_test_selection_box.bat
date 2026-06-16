@echo off
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /Fe:tests\test_selection_box.exe /Fo:tests\obj\ tests\test_selection_box.cpp
set ERR=%errorlevel%
popd
exit /b %ERR%
