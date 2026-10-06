@echo off
setlocal
set "TOOLS=E:\SkyCraftKCD2Tools"
set "OUT=%TOOLS%\surface-test"
if not exist "%OUT%" mkdir "%OUT%"
call "%TOOLS%\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  /I "%~dp0stubs" "%~dp0surface_contract.cpp" "%~dp0..\host\skycraft_surface.cpp" ^
  /Fo"%OUT%\\" /Fe"%OUT%\surface_contract.exe" || exit /b 1
"%OUT%\surface_contract.exe"
exit /b %ERRORLEVEL%
