@echo off
:: Builds and runs the offline test. Usage: test\run_test.bat [path\to\SanAndreas.exe]
setlocal
cd /d "%~dp0\.."
if not defined DevEnvDir (
    if exist "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)
if not exist "obj\test" mkdir "obj\test"
cl.exe /nologo /O2 /W3 /MD /EHsc /std:c++20 /Iminhook /Fo:obj\test\ /Fe:obj\test\offline_test.exe ^
    test\offline_test.cpp minhook\buffer.c minhook\hook.c minhook\trampoline.c minhook\hde\hde64.c || exit /b 1
obj\test\offline_test.exe %1
