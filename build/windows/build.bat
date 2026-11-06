@echo off
rem Builds dng2prr on Windows: compiles the C core to source\libprr.dll and runs the self-test.
rem UNTESTED on real Windows so far - see PORTING.md. Please report what happens.
rem
rem Needs ONE of these C compilers:
rem   - Visual Studio Build Tools (run this from "x64 Native Tools Command Prompt for VS")
rem   - MinGW-w64 gcc on PATH (e.g. from MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc)
rem and Python 3 (python.org installer, "Add to PATH" ticked) with numpy:
rem   py -m pip install numpy
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "SRC=%ROOT%\source"

where py >nul 2>nul && (set "PY=py -3") || (set "PY=python")
%PY% -c "import numpy" 2>nul
if errorlevel 1 (
  echo numpy is missing. Run:  %PY% -m pip install numpy
  goto :fail
)

where cl >nul 2>nul
if not errorlevel 1 (
  rem MSVC: /fp:precise is the default (no contraction across statements) - matches the reference build
  cl /nologo /O2 /LD /fp:precise "%SRC%\prr_core.c" "%SRC%\dng_lj92.c" "%SRC%\dng_jpeg.c" /Fe:"%SRC%\libprr.dll" /Fo"%TEMP%\\"
  if errorlevel 1 goto :fail
  del "%SRC%\libprr.lib" "%SRC%\libprr.exp" 2>nul
  goto :built
)
where gcc >nul 2>nul
if not errorlevel 1 (
  gcc -O2 -shared -ffp-contract=off -o "%SRC%\libprr.dll" "%SRC%\prr_core.c" "%SRC%\dng_lj92.c" "%SRC%\dng_jpeg.c"
  if errorlevel 1 goto :fail
  goto :built
)
echo No C compiler found. Install Visual Studio Build Tools (C++ workload) and run this
echo from the "x64 Native Tools Command Prompt", or install MinGW-w64 gcc.
goto :fail

:built
echo Built %SRC%\libprr.dll
%PY% "%ROOT%\tests\selftest.py"
if errorlevel 1 goto :fail
%PY% "%ROOT%\tests\test_lj92.py"
if errorlevel 1 goto :fail
echo.
echo Done. Command line:  %PY% "%SRC%\dng2prr.py" OUT.mov CLIP_FOLDER
echo       Window:        %PY% "%SRC%\dng2prr_gui.py"
echo To make a standalone .exe for sharing, run build_exe.bat next.
pause
exit /b 0

:fail
echo.
echo BUILD FAILED
pause
exit /b 1
