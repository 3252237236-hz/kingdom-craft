@echo off
setlocal
set "TOOLS=E:\SkyCraftKCD2Tools"
set "SDK=%TOOLS%\reshade-sdk"
set "OUT=%TOOLS%\reshade-runtime\SkyCraftKCD2.addon64"
set "OBJ=%TOOLS%\reshade-runtime\"
if not "%~1"=="" (
  set "OUT=%~1\SkyCraftKCD2.addon64"
  set "OBJ=%~1\"
  if not exist "%~1" mkdir "%~1"
)
if not exist "%SDK%\reshade.hpp" exit /b 1
call "%TOOLS%\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /LD /O2 /EHsc /std:c++20 /MT /W3 /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  /I "%SDK%" "%~dp0addon.cpp" "%~dp0compositor.cpp" ^
  /Fo"%OBJ%\" /Fe"%OUT%" ^
  /link user32.lib
exit /b %ERRORLEVEL%
