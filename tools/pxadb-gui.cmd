@echo off
setlocal
set "PYTHONPATH=%~dp0pxadb-gui;%~dp0pxadb;%PYTHONPATH%"
set "PYTHONUTF8=1"
if defined PYTHON (
  "%PYTHON%" -X utf8 -m pxadb_gui %*
) else (
  python -X utf8 -m pxadb_gui %*
)
exit /b %errorlevel%
