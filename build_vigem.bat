@echo off
setlocal
cd /d "%~dp0"

echo [vigem] Locating Visual Studio...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novswhere

for /f "delims=" %%i in ('"%VSWHERE%" -latest -property installationPath') do set "VS=%%i"
if "%VS%"=="" goto novswhere

call "%VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto nomsvc

cd /d "%~dp0ViGEmClient"
echo [vigem] Compiling ViGEmClient.cpp -^> ViGEmClient.dll ...
cl /nologo /LD /O2 /EHsc /utf-8 /DVIGEM_DYNAMIC /DVIGEM_EXPORTS /I include /I src src\ViGEmClient.cpp /Fe:ViGEmClient.dll /link setupapi.lib
if errorlevel 1 goto fail

cd /d "%~dp0"
if not exist build mkdir build
copy /Y "%~dp0ViGEmClient\ViGEmClient.dll" "%~dp0build\" >nul
echo [vigem] DONE - copied to build\ViGEmClient.dll
goto end

:novswhere
echo [ERROR] vswhere not found.
echo        Looked at: %VSWHERE%
goto end

:nomsvc
echo [ERROR] vcvars64.bat failed.
echo        VS path: %VS%
goto end

:fail
echo [ERROR] compilation failed.

:end
endlocal
