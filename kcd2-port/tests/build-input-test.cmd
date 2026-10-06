@echo off
setlocal
set "TOOLS=E:\SkyCraftKCD2Tools"
set "OUT=%TOOLS%\input-test"
if not exist "%OUT%" mkdir "%OUT%"
call "%TOOLS%\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /MT /DWIN32_LEAN_AND_MEAN /DNOMINMAX ^
  "%~dp0input_contract.cpp" /Fo"%OUT%\\" /Fe"%OUT%\input_contract.exe" || exit /b 1
"%OUT%\input_contract.exe"
exit /b %ERRORLEVEL%
