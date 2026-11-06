@echo off
rem Packages the GUI + encoder + libprr.dll into a standalone folder app with PyInstaller:
rem   dist\cDNG Raw Convert\cDNG Raw Convert.exe
rem Users need nothing installed. Drop clip folders onto the .exe (or a shortcut to it),
rem or open it and use "Add clip folder".
rem UNTESTED on real Windows so far - see PORTING.md.
rem Run build.bat first (it makes source\libprr.dll and runs the self-test).
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "SRC=%ROOT%\source"
where py >nul 2>nul && (set "PY=py -3") || (set "PY=python")

if not exist "%SRC%\libprr.dll" (
  echo source\libprr.dll not found - run build.bat first.
  goto :fail
)
%PY% -m pip install --upgrade pyinstaller numpy
if errorlevel 1 goto :fail

pushd "%HERE%"
rem --onedir (not --onefile): starts faster and the encoder's worker processes don't
rem unpack a copy each. --windowed: no console window.
%PY% -m PyInstaller --noconfirm --clean --onedir --windowed ^
  --name "cDNG Raw Convert" ^
  --paths "%SRC%" --hidden-import dng2prr ^
  --add-binary "%SRC%\libprr.dll;." ^
  "%SRC%\dng2prr_gui.py"
if errorlevel 1 (popd & goto :fail)
popd

copy /y "%ROOT%\README.md" "%HERE%dist\cDNG Raw Convert\" >nul
copy /y "%ROOT%\LICENSE" "%HERE%dist\cDNG Raw Convert\" >nul
copy /y "%ROOT%\NOTICE.md" "%HERE%dist\cDNG Raw Convert\" >nul
echo.
echo Done: %HERE%dist\cDNG Raw Convert\
echo Zip that whole folder to share it. Test the .exe on a PC without Python before sharing,
echo then run tests\validation on its output (see PORTING.md).
pause
exit /b 0

:fail
echo.
echo PACKAGING FAILED
pause
exit /b 1
