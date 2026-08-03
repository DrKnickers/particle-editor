@echo off
REM Build the module-path grow-until-it-fits regression test as a standalone
REM x64 console exe. The test self-relocates this real executable beyond
REM MAX_PATH so host::ModuleFilePath(), not only its injected helper, is covered.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "src" ^
   /Fe:tests\test_module_path.exe /Fo:tests\obj\ ^
   tests\test_module_path.cpp ^
   /link kernel32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
