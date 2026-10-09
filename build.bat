@echo off
REM Genesis Plus GX - AmigaOS Build Script for Windows CMD
REM Requires bebbo's amiga-gcc Windows installer
REM Download: http://franke.ms/download/setup-amiga-gcc.exe

echo.
echo Genesis Plus GX - AmigaOS Build Script
echo =======================================
echo.

REM Check for m68k-amigaos-gcc
where m68k-amigaos-gcc >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    echo ERROR: 'm68k-amigaos-gcc' not found in PATH
    echo.
    echo Please install bebbo's amiga-gcc for Windows:
    echo   http://franke.ms/download/setup-amiga-gcc.exe
    echo.
    echo After installation, you may need to:
    echo   1. Restart your command prompt
    echo   2. Or manually add the bin folder to PATH
    echo.
    pause
    exit /b 1
)

REM Show compiler version
echo Compiler found:
m68k-amigaos-gcc --version 2>&1 | findstr /n "^" | findstr "^1:"
echo.

REM Parse arguments
set CPU=68040
set LIBC=nix20
set TARGET=all
set USE_ZIP_FLAG=USE_ZIP=1

:parse_args
if "%1"=="" goto build
if "%1"=="68030" set CPU=68030 & shift & goto parse_args
if "%1"=="68040" set CPU=68040 & shift & goto parse_args
if "%1"=="68060" set CPU=68060 & shift & goto parse_args
if "%1"=="68080" set CPU=68080 & shift & goto parse_args
if "%1"=="pi3"   set TARGET=pi3 & shift & goto parse_args
if "%1"=="clean" set TARGET=clean & shift & goto parse_args
if "%1"=="debug" set TARGET=debug & shift & goto parse_args
if "%1"=="info"  set TARGET=info & shift & goto parse_args
if "%1"=="clib2"    set LIBC=clib2 & shift & goto parse_args
if "%1"=="launcher" set TARGET=launcher & shift & goto parse_args
if "%1"=="zip"   set USE_ZIP_FLAG=USE_ZIP=1 & shift & goto parse_args
if "%1"=="help"  goto help
if "%1"=="-h"    goto help
if "%1"=="/?"    goto help
shift
goto parse_args

:help
echo Usage: build.bat [options]
echo.
echo CPU options:
echo   68040    - Build for 68040 with FPU (default)
echo   68030    - Build for 68030
echo   68060    - Build for 68060 with FPU
echo   68080    - Build for Vampire/Apollo 68080
echo.
echo Platform presets:
echo   pi3      - Optimised build for Pi 3 PiStorm (all ASM enabled)
echo              Note: Pi 3 may not achieve full speed with sound
echo.
echo Library options:
echo   clib2    - Use clib2 instead of libnix
echo.
echo Commands:
echo   clean    - Remove build files
echo   launcher - Build MUI game launcher + config GUI
echo   debug    - Build with debug symbols
echo   info     - Show build configuration
echo   zip      - Enable ZIP ROM support (requires amiga/miniz.h + miniz.c)
echo   help     - Show this help
echo.
echo Examples:
echo   build.bat              - Build for 68040 with libnix
echo   build.bat pi3          - Pi 3 PiStorm optimised build
echo   build.bat 68060        - Build for 68060
echo   build.bat 68060 clib2  - Build for 68060 with clib2
echo   build.bat launcher     - Build MUI game launcher
echo   build.bat clean        - Clean build files
echo.
pause
exit /b 0

:build
if "%TARGET%"=="launcher" (
    echo Building MUI game launcher...
    make launcher CPU=%CPU% LIBC=%LIBC% SHELL=sh.exe
    if %ERRORLEVEL% NEQ 0 goto build_failed
    echo.
    echo ========================================
    echo LAUNCHER BUILD SUCCESSFUL!
    echo ========================================
    echo Output: GenesisPlusGX-Launcher
    echo.
    goto done
)

if "%TARGET%"=="clean" (
    echo Cleaning build files...
    make clean SHELL=sh.exe
    goto done
)

if "%TARGET%"=="info" (
    make info CPU=%CPU% LIBC=%LIBC% USE_ASM_AUDIO=1 USE_ASM_VIDEO=1 USE_ASM_BLIP=1 SHELL=sh.exe
    goto done
)

if "%TARGET%"=="pi3" (
    echo Building Pi 3 PiStorm optimised binary...
    echo CPU: 68030 with all ASM optimisations
    echo.
    make pi3 CPU=68030 LIBC=%LIBC% SHELL=sh.exe
    if %ERRORLEVEL% NEQ 0 goto build_failed
    echo.
    echo ========================================
    echo Pi 3 BUILD SUCCESSFUL!
    echo ========================================
    echo Output: GenesisPlusGX
    echo.
    echo NOTE: Pi 3 may not achieve full speed with sound enabled
    echo.
    goto done
)

echo Building for CPU: %CPU%
echo C Library: %LIBC%
echo ASM optimizations: audio, video, blip
echo.

make %TARGET% CPU=%CPU% LIBC=%LIBC% USE_ASM_AUDIO=1 USE_ASM_VIDEO=1 USE_ASM_BLIP=1 %USE_ZIP_FLAG% SHELL=sh.exe

:build_failed
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ========================================
    echo BUILD FAILED!
    echo ========================================
    echo.
    echo If you see missing header errors, check that:
    echo   - amiga-gcc is properly installed
    echo   - PATH includes the amiga-gcc bin directory
    echo.
    pause
    exit /b 1
)

echo.
echo ========================================
echo BUILD SUCCESSFUL!
echo ========================================
echo Output: GenesisPlusGX
echo.
if exist GenesisPlusGX dir GenesisPlusGX

:done
echo.
pause
exit /b 0
