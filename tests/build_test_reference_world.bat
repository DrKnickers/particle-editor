@echo off
REM Build the reference-object world-matrix unit test as a standalone x64
REM console exe. Pure-header test (src/ReferenceObjectWorld.h); links only
REM d3dx9.lib (the test_manip_readout / test_gizmo_sizing precedent) -- no engine TU.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /I"%DXSDK_INC%" /Fe:tests\test_reference_world.exe /Fo:tests\obj\ tests\test_reference_world.cpp /link /LIBPATH:"%DXSDK_LIB%" d3dx9.lib
set ERR=%errorlevel%
popd
exit /b %ERR%
