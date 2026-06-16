@echo off
setlocal
call "C:\Program Files\<path>" >nul
if errorlevel 1 ( echo vcvars failed & exit /b 1 )
set ROOT=%~dp0..
pushd "%ROOT%"
if not exist tests\obj mkdir tests\obj
cl /nologo /EHsc /std:c++17 /MDd /I"C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)\Include" /Fe:tests\test_manip_readout.exe /Fo:tests\obj\ tests\test_manip_readout.cpp /link /LIBPATH:"C:\Program Files (x86)\Microsoft DirectX SDK (June 2010)\Lib\x64" d3dx9.lib
set ERR=%errorlevel%
popd
exit /b %ERR%
