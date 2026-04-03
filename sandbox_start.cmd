@echo off
echo [sandbox] Running brokit tests...
echo ============================================
cd /d C:\brokit
build\tests\Release\brokit_test.exe tests\js 2>&1
echo.
echo ============================================
echo [sandbox] brokit_test exited with code %ERRORLEVEL%
echo.
echo If it crashed, check the output above for errors.
pause
