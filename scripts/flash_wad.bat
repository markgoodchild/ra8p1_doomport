@echo off
rem ---------------------------------------------------------------------------
rem Program a Doom IWAD into the EK-RA8P1 external octal flash (0x90000000).
rem
rem   scripts\flash_wad.bat [path\to\iwad]     (default: doom\wad\doom1.wad)
rem
rem Only needs to be run once (or when changing IWAD): the application image
rem does not contain the WAD, so normal debug downloads stay fast.
rem Set JLINK_EXE to override the J-Link Commander location.
rem ---------------------------------------------------------------------------
setlocal EnableDelayedExpansion

set "ROOT=%~dp0.."
set "WAD=%~1"
if "%WAD%"=="" set "WAD=%ROOT%\doom\wad\doom1.wad"

if not exist "%WAD%" (
    echo ERROR: WAD file not found: %WAD%
    exit /b 1
)

rem Locate J-Link Commander (newest installed version wins).
if "%JLINK_EXE%"=="" (
    for /d %%D in ("%ProgramFiles%\SEGGER\JLink*") do (
        if exist "%%D\JLink.exe" set "JLINK_EXE=%%D\JLink.exe"
    )
)
if "%JLINK_EXE%"=="" (
    echo ERROR: J-Link Commander not found. Set JLINK_EXE to JLink.exe.
    exit /b 1
)

rem J-Link's loadbin needs a .bin extension.
set "TMPDIR=%TEMP%\ra8p1_doom_wad"
if not exist "%TMPDIR%" mkdir "%TMPDIR%"
copy /y "%WAD%" "%TMPDIR%\iwad.bin" >nul

set "CMDS=%TMPDIR%\flash_wad.jlink"
(
    echo connect
    echo r
    echo loadbin "%TMPDIR%\iwad.bin", 0x90000000
    echo verifybin "%TMPDIR%\iwad.bin", 0x90000000
    echo r
    echo g
    echo exit
) > "%CMDS%"

echo Programming "%WAD%" into OSPI flash at 0x90000000 (about 1 minute)...
"%JLINK_EXE%" -NoGui 1 -ExitOnError 1 -device R7KA8P1KF_CPU0 -if SWD -speed 4000 -autoconnect 1 ^
    -JLinkScriptFile "%~dp0ra8p1_ospi_reset.JLinkScript" -CommandFile "%CMDS%" > "%TMPDIR%\flash_wad.log" 2>&1
set "RESULT=%ERRORLEVEL%"

findstr /c:"Verify successful" "%TMPDIR%\flash_wad.log" >nul
if errorlevel 1 (
    type "%TMPDIR%\flash_wad.log"
    echo.
    echo ERROR: programming or verification failed ^(J-Link exit code %RESULT%^).
    exit /b 1
)

echo Done - IWAD programmed and verified.
exit /b 0
