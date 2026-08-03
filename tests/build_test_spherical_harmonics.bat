@echo off
REM Build the SPH_Calculate_Matrices unit test as a standalone x64 console exe.
REM Links src\SphericalHarmonics.cpp as shipped (classical fallback branch);
REM the test TU additionally re-includes the same source in a namespace with
REM USE_PROPER_SPH defined, so BOTH compile-time branches are exercised in one
REM exe. d3dx9.lib is genuinely exercised at runtime here
REM (D3DXSHEvalDirectionalLight / D3DXSHAdd are CPU-side d3dx9 exports; no
REM device). engine.h is compile-only (Engine::Light); no engine TU is linked.
setlocal
call "%~dp0_env.bat" || exit /b 1
if errorlevel 1 ( echo vcvars failed & exit /b 1 )

set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj

cl /nologo /EHsc /std:c++17 /MDd /Zi ^
   /DUNICODE /D_UNICODE /D_DEBUG /D_WINDOWS /D_CRT_SECURE_NO_WARNINGS ^
   /I "%DXSDK_DIR%Include" /I "src" ^
   /Fe:tests\test_spherical_harmonics.exe /Fo:tests\obj\ ^
   tests\test_spherical_harmonics.cpp ^
   src\SphericalHarmonics.cpp ^
   /link /LIBPATH:"%DXSDK_DIR%Lib\x64" d3dx9.lib shlwapi.lib ole32.lib oleaut32.lib advapi32.lib user32.lib

set ERR=%errorlevel%
popd
exit /b %ERR%
