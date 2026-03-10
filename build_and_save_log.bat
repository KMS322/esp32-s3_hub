@echo off
cd /d "C:\Users\z\Desktop\esp32_s3\esp32_s3_idf\gatt_server_251105"
idf.py build > build_output.log 2>&1
echo.
echo ========================================
echo Build log saved to: build_output.log
echo ========================================
echo.
echo Checking for errors...
findstr /i "error failed" build_output.log
if %errorlevel% equ 0 (
    echo.
    echo ERRORS FOUND! Check build_output.log
) else (
    echo.
    echo No errors found in build log.
)

