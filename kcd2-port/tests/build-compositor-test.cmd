@echo off
setlocal
set "TOOLS=E:\SkyCraftKCD2Tools"
set "OUT=%TOOLS%\candidates\world-combat-repair-20261007\shader-test"
if not exist "%OUT%" mkdir "%OUT%"
call "%TOOLS%\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /MT /DNOMINMAX /DWIN32_LEAN_AND_MEAN /I "%TOOLS%\reshade-sdk" "%~dp0runtime\compositor_smoke.cpp" /Fo"%OUT%\\" /Fe"%OUT%\shader-test.exe" /link user32.lib d3d11.lib dxgi.lib || exit /b 1
exit /b %ERRORLEVEL%
