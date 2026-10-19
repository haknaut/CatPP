@echo off
setlocal EnableExtensions
cd /d "%~dp0"

rem ============================================================
rem  CatPP build script
rem
rem    build.bat         compile main.cpp -> CatPP.exe
rem    build.bat res     rebuild app.o only (windres from app.rc)
rem
rem  Compiler path is read from gpp_path.txt, then
rem  CatPP_gpp_path.txt, then defaults to C:\msys64\mingw64\bin
rem ============================================================

set "MODE=%~1"
set "SRC=main.cpp"
set "OUT=CatPP.exe"

if /i "%MODE%"=="res" goto :res
if not "%MODE%"=="" (
  echo Usage: build.bat [res]
  exit /b 1
)

call :find_gpp || exit /b 1

if not exist "%SRC%" (
  echo [build] source not found: %SRC%
  exit /b 1
)

if not exist "app.o" (
  echo [build] app.o missing, rebuilding resources...
  call :res || exit /b 1
)

echo [build] compiling %SRC% -^> %OUT% ...
"%GPP%" -std=c++17 -O2 -municode -mwindows %SRC% app.o -lgdiplus -lwinmm -ldwmapi -lole32 -luuid -o "%OUT%.new"
if errorlevel 1 (
  echo [build] compile FAILED
  if exist "%OUT%.new" del /q "%OUT%.new" >nul 2>&1
  exit /b 1
)

rem A running exe cannot be overwritten, but it CAN be renamed -
rem so move the old one aside, then put the fresh build in place.
move /y "%OUT%.new" "%OUT%" >nul 2>&1
if errorlevel 1 (
  echo [build] %OUT% is locked, probably still running - renaming old exe aside...
  if exist "%OUT%.old" del /f /q "%OUT%.old" >nul 2>&1
  ren "%OUT%" "%OUT%.old" >nul 2>&1
  if errorlevel 1 (
    echo [build] could not replace %OUT%. Close CatPP and run this script again.
    del /q "%OUT%.new" >nul 2>&1
    exit /b 1
  )
  move /y "%OUT%.new" "%OUT%" >nul 2>&1
  if errorlevel 1 (
    echo [build] could not move new exe into place.
    exit /b 1
  )
)

echo [build] OK -^> %OUT%
exit /b 0

:res
call :find_gpp || exit /b 1
for %%W in ("%GPP%") do set "WINDRES=%%~dpWwindres.exe"
if not exist "%WINDRES%" (
  echo [build] windres not found next to g++: %WINDRES%
  exit /b 1
)
echo [build] windres app.rc -^> app.o ...
"%WINDRES%" app.rc -O coff -o app.o
if errorlevel 1 (
  echo [build] windres FAILED
  exit /b 1
)
echo [build] OK -^> app.o
exit /b 0

:find_gpp
set "GPP="
if exist "gpp_path.txt" for /f "usebackq delims=" %%F in ("gpp_path.txt") do if not defined GPP set "GPP=%%F"
if not defined GPP if exist "CatPP_gpp_path.txt" for /f "usebackq delims=" %%F in ("CatPP_gpp_path.txt") do if not defined GPP set "GPP=%%F"
if not defined GPP set "GPP=C:\msys64\mingw64\bin\g++.exe"
if not exist "%GPP%" (
  echo [build] g++ not found: "%GPP%"
  echo [build] put the full path to g++.exe in gpp_path.txt, or install MSYS2 MinGW64.
  exit /b 1
)
echo [build] compiler: %GPP%
exit /b 0
