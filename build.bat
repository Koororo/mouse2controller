@echo off
setlocal
cd /d "%~dp0"

echo [build] Locating Visual Studio...

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto novswhere

for /f "delims=" %%i in ('"%VSWHERE%" -latest -property installationPath') do set "VS=%%i"
if "%VS%"=="" goto novswhere

call "%VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 goto nomsvc

if not exist build mkdir build
echo [build] Compiling resources (icon)...
rc /nologo /I res /fo build\app.res res\resource.rc
if errorlevel 1 goto fail
echo [build] Compiling src\main.cpp ...
cl /nologo /EHsc /O2 /std:c++17 /utf-8 /W3 /Fo:build\ /Fe:build\mouse2controller.exe ^
  /I"third_party\imgui" /I"third_party\imgui\backends" ^
  src\main.cpp ^
  third_party\imgui\imgui.cpp ^
  third_party\imgui\imgui_draw.cpp ^
  third_party\imgui\imgui_tables.cpp ^
  third_party\imgui\imgui_widgets.cpp ^
  third_party\imgui\backends\imgui_impl_win32.cpp ^
  third_party\imgui\backends\imgui_impl_dx9.cpp ^
  build\app.res ^
  /link /SUBSYSTEM:CONSOLE user32.lib comctl32.lib gdi32.lib d3d9.lib
if errorlevel 1 goto fail
goto ok

:novswhere
echo [ERROR] vswhere not found.
echo        Looked at: %VSWHERE%
goto failend

:nomsvc
echo [ERROR] vcvars64.bat failed.
echo        VS path: %VS%
goto failend

:fail
echo [FAIL] compilation failed.

:failend
exit /b 1

:ok
echo [DONE] build\mouse2controller.exe
echo       Make sure ViGEmBus driver is installed and ViGEmClient.dll sits next to the exe.
endlocal
