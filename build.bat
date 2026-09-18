@echo off
setlocal
set PATH=C:\w64devkit\bin;%PATH%
g++ -std=c++17 -municode -mwindows -O2 -static ascii_gui.cpp -o ascii_gui.exe ^
    -lcomdlg32 -lshell32 -lole32 -lwinmm -luuid -ladvapi32
if %errorlevel%==0 (
  echo Built ascii_gui.exe
) else (
  echo Build failed
)
endlocal
