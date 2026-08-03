@echo off
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /I"%DXSDK_INC%" /Fe:tests\test_manip_readout.exe /Fo:tests\obj\ tests\test_manip_readout.cpp /link /LIBPATH:"%DXSDK_LIB%" d3dx9.lib
set ERR=%errorlevel%
popd
exit /b %ERR%
