@echo off
setlocal
set "TOOLS=E:\SkyCraftKCD2Tools"
set "OUT=%TOOLS%\projection-test"
if not exist "%OUT%" mkdir "%OUT%"
call "%TOOLS%\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /EHsc /std:c++20 /MT "%~dp0projection_contract.cpp" /Fo"%OUT%\\" /Fe"%OUT%\projection_contract.exe" || exit /b 1
"%OUT%\projection_contract.exe"
exit /b %ERRORLEVEL%
