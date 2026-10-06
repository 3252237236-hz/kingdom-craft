@echo off
setlocal
set "SKY_TOOLS=E:\SkyCraftKCD2Tools"
set "SKY_OUT=%SKY_TOOLS%\shapes-test"
if not exist "%SKY_OUT%" mkdir "%SKY_OUT%"
call "%SKY_TOOLS%\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  "%~dp0collision_shapes_contract.cpp" /Fo"%SKY_OUT%\\" /Fe"%SKY_OUT%\collision_shapes_contract.exe" || exit /b 1
"%SKY_OUT%\collision_shapes_contract.exe"
exit /b %ERRORLEVEL%
