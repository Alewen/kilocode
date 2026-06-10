@echo off
chcp 936 >nul
setlocal enabledelayedexpansion

:: ============================================================
:: kiloHistory-Svr.cmd
:: 使用 NSSM 将 Kilo History Server (server.py) 注册为
:: Windows 后台服务，避免占用控制台窗口
::
:: 用法: kiloHistory-Svr.cmd [install|uninstall|start|stop|restart|status]
::
:: 注意: install / uninstall / start / stop / restart
::       均需要「以管理员身份运行」
:: ============================================================

set "SERVICE_NAME=kiloHistory"
set "SCRIPT_DIR=%~dp0"
set "SERVER_SCRIPT=%SCRIPT_DIR%server.py"

:: ----- 有参数 → 直接转到路由 -----
if not "%~1"=="" goto route

:: ----- 无参数 → 显示帮助 -----
echo.
echo ============================================================
echo   Kilo History Server - Windows 服务管理脚本 (NSSM)
echo   注意：执行本脚本前，需要通过 powershell，运行安装 winget install nssm
echo ============================================================
echo.
echo 用法: %~nx0 [命令]
echo.
echo 支持命令:
echo   install    安装并配置服务（开机自启）
echo   uninstall  卸载服务
echo   start      启动服务
echo   stop       停止服务
echo   restart    重启服务
echo   status     查看服务运行状态
echo.
echo 重要: install/start/stop/restart/uninstall
echo       必须「以管理员身份运行」才有效
echo.
pause
exit /b 0

:route
:: ----- 参数路由 -----
if /i "%~1"=="install"   goto do_install
if /i "%~1"=="uninstall" goto do_uninstall
if /i "%~1"=="start"     goto do_start
if /i "%~1"=="stop"      goto do_stop
if /i "%~1"=="restart"   goto do_restart
if /i "%~1"=="status"    goto do_status

echo [错误] 未知参数: %~1
echo 请使用 %~nx0 查看帮助信息。
pause
exit /b 1

:: ============================================================
:: 主逻辑结束 - 以下全为子程序/标签
:: ============================================================
goto :eof

:: ---------- 辅助：检查是否管理员 ----------
:check_admin
net session >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [错误] 请「以管理员身份运行」此脚本。
    echo         右键点击 %~nx0 ^> 「以管理员身份运行」
    exit /b 1
)
exit /b 0

:: ---------- 辅助：获取 Python 路径 ----------
:get_python
set "PYTHON_EXE="
for /f "delims=" %%i in ('where python 2^>nul') do (
    if not defined PYTHON_EXE set "PYTHON_EXE=%%i"
)
if not defined PYTHON_EXE (
    echo [错误] 未在系统 PATH 中找到 python.exe
    exit /b 1
)
exit /b 0

:: ---------- INSTALL ----------
:do_install
call :check_admin
if %ERRORLEVEL% neq 0 (pause & exit /b 1)

call :get_python
if %ERRORLEVEL% neq 0 (pause & exit /b 1)
echo [*] 检测到 Python: %PYTHON_EXE%

if not exist "%SERVER_SCRIPT%" (
    echo [错误] 未找到 server.py，请将此脚本与 server.py 放在同一目录。
    pause
    exit /b 1
)

:: 如果服务已存在，先移除
nssm status "%SERVICE_NAME%" >nul 2>&1
if %ERRORLEVEL% equ 0 (
    echo [*] 检测到旧服务，正在移除...
    nssm stop "%SERVICE_NAME%" >nul 2>&1
    nssm remove "%SERVICE_NAME%" confirm >nul 2>&1
    timeout /t 2 /nobreak >nul
)

echo [*] 正在安装服务...
nssm install "%SERVICE_NAME%" "%PYTHON_EXE%" "%SERVER_SCRIPT%"
if %ERRORLEVEL% neq 0 (
    echo [错误] 服务安装失败。
    pause
    exit /b 1
)

:: 配置服务参数
nssm set "%SERVICE_NAME%" AppDirectory "%~dp0."               >nul
nssm set "%SERVICE_NAME%" Description "Kilo History Server (Python)" >nul
nssm set "%SERVICE_NAME%" Start SERVICE_AUTO_START                   >nul

:: 创建日志目录
if not exist "%SCRIPT_DIR%logs" mkdir "%SCRIPT_DIR%logs"

:: 日志输出
nssm set "%SERVICE_NAME%" AppStdout "%SCRIPT_DIR%logs\kiloHistory-out.log"   >nul
nssm set "%SERVICE_NAME%" AppStderr "%SCRIPT_DIR%logs\kiloHistory-error.log" >nul

:: 设置数据库路径环境变量，绕过 NSSM 服务环境下 expanduser 解析问题
nssm set "%SERVICE_NAME%" AppEnvironmentExtra KILO_DB_PATH="%USERPROFILE%\.local\share\kilo\kilo.db" >nul

:: 禁止控制台窗口弹出
nssm set "%SERVICE_NAME%" AppNoConsole 1 >nul

echo.
echo [OK] 服务 '%SERVICE_NAME%' 安装成功。
echo.
echo 常用命令:
echo   启动:   %~nx0 start
echo   停止:   %~nx0 stop
echo   状态:   %~nx0 status
echo   卸载:   %~nx0 uninstall
echo.
pause
exit /b 0

:: ---------- UNINSTALL ----------
:do_uninstall
call :check_admin
if %ERRORLEVEL% neq 0 (pause & exit /b 1)

nssm status "%SERVICE_NAME%" >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo [提示] 服务 '%SERVICE_NAME%' 不存在。
    pause
    exit /b 0
)
echo [*] 正在卸载服务...
nssm stop "%SERVICE_NAME%" >nul 2>&1
nssm remove "%SERVICE_NAME%" confirm
if %ERRORLEVEL% neq 0 (
    echo [错误] 卸载失败。
    pause
    exit /b 1
)
echo [OK] 服务 '%SERVICE_NAME%' 已卸载。
pause
exit /b 0

:: ---------- START ----------
:do_start
call :check_admin
if %ERRORLEVEL% neq 0 (pause & exit /b 1)

nssm start "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 (
    echo [错误] 服务启动失败。请查看 logs\kiloHistory-error.log 获取详情。
    pause
    exit /b 1
)
echo [OK] 服务启动成功。
pause
exit /b 0

:: ---------- STOP ----------
:do_stop
call :check_admin
if %ERRORLEVEL% neq 0 (pause & exit /b 1)

nssm stop "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 (
    echo [错误] 服务停止失败。
    pause
    exit /b 1
)
echo [OK] 服务已停止。
pause
exit /b 0

:: ---------- RESTART ----------
:do_restart
call :check_admin
if %ERRORLEVEL% neq 0 (pause & exit /b 1)

nssm restart "%SERVICE_NAME%"
if %ERRORLEVEL% neq 0 (
    echo [错误] 服务重启失败。
    pause
    exit /b 1
)
echo [OK] 服务重启成功。
pause
exit /b 0

:: ---------- STATUS ----------
:do_status
echo [*] 服务状态:
nssm status "%SERVICE_NAME%"
pause
exit /b 0
