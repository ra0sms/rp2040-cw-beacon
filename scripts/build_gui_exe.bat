@echo off
rem Build the standalone Windows GUI (gui\beacon_gui.exe) with PyInstaller.
rem Run on Windows from any directory: scripts\build_gui_exe.bat
rem Requires Python 3 in PATH.
setlocal
set ROOT=%~dp0..

python -m pip install --upgrade -r "%ROOT%\requirements.txt" pyinstaller || exit /b 1
python -m PyInstaller --onefile --windowed --name beacon_gui ^
    --distpath "%ROOT%\gui" --workpath "%TEMP%\beacon_gui_build" --specpath "%TEMP%\beacon_gui_build" ^
    "%ROOT%\gui\beacon_gui.py" || exit /b 1

echo ==^> Done: %ROOT%\gui\beacon_gui.exe
