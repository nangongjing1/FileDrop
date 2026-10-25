@echo off
setlocal
rem 本机若安装了 w64devkit，则优先使用其中的 g++ / make
for %%D in ("%USERPROFILE%\w64devkit\w64devkit\bin" "%USERPROFILE%\w64devkit\bin" "C:\w64devkit\bin" "C:\Users\48068\w64devkit\w64devkit\bin") do (
  if exist "%%~D\g++.exe" (
    set "PATH=%%~D;%PATH%"
    goto :toolchain_ready
  )
)
:toolchain_ready

if not exist "%~dp0src\gen_web_assets.cpp" (
  where python >nul 2>nul
  if errorlevel 1 (
    echo 未找到 Python，无法生成内嵌页面资源。
    exit /b 1
  )
  python "%~dp0tools\embed_web.py" "%~dp0web" "%~dp0src\gen_web_assets.cpp"
  if errorlevel 1 exit /b 1
)

pushd "%~dp0"
make %*
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
