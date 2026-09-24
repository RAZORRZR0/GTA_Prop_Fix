@echo off
setlocal enabledelayedexpansion

echo ============================================================
echo Building GTA_Prop_Fix v0.0.1 (x64 ASI Plugin)
echo ============================================================

cd /d "%~dp0"

:: 1. Initialize MSVC x64 Environment if not already loaded
if not defined DevEnvDir (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
    ) else (
        echo [ERROR] Could not find Visual Studio 64-bit developer tools!
        echo Please run build.bat from a Visual Studio x64 Developer Command Prompt.
        pause
        exit /b 1
    )
)

:: 2. Ensure output directories exist
if not exist "bin" mkdir "bin"
if not exist "obj" mkdir "obj"

:: 3. Compile and Link
echo Compiling source files...
cl.exe /O2 /W3 /MD /std:c++20 /D_CRT_SECURE_NO_WARNINGS /Iminhook /Fo:obj\ ^
    src\dllmain.cpp ^
    minhook\buffer.c ^
    minhook\hook.c ^
    minhook\trampoline.c ^
    minhook\hde\hde64.c ^
    /link /DLL /OUT:bin\GTA_Prop_Fix.asi user32.lib

if %ERRORLEVEL% equ 0 (
    echo.
    echo ============================================================
    echo BUILD SUCCESS!
    echo Output: bin\GTA_Prop_Fix.asi
    echo ============================================================
    copy /y bin\GTA_Prop_Fix.asi GTA_Prop_Fix.asi >nul
) else (
    echo.
    echo [ERROR] Build failed with exit code %ERRORLEVEL%
    pause
    exit /b %ERRORLEVEL%
)
