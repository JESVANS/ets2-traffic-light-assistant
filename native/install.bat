@echo off
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "ESC="
for /f "delims=" %%E in ('echo prompt $E^| cmd') do set "ESC=%%E"
set "C_RST="
set "C_CYAN="
set "C_INFO="
set "C_OK="
set "C_WARN="
set "C_ERR="
set "C_DIM="
if not defined NO_COLOR if defined ESC (
    set "C_RST=!ESC![0m"
    set "C_CYAN=!ESC![96m"
    set "C_INFO=!ESC![36m"
    set "C_OK=!ESC![92m"
    set "C_WARN=!ESC![93m"
    set "C_ERR=!ESC![91m"
    set "C_DIM=!ESC![90m"
)

set "FLAG_REBUILD=0"
set "FLAG_RESET_INI=0"
set "FLAG_UNINSTALL=0"
set "FLAG_SILENT=0"
set "CUSTOM_DIR="
set "BAD_ARG="
set "EXIT_CODE=0"
set "SHOULD_PAUSE=0"

:: Detect Explorer double-click launch: cmd /c ""...\install.bat" " with no CLI args
if "%~1"=="" (
    echo !CMDCMDLINE! | findstr /I /C:"/c \"\"" >nul
    if not errorlevel 1 set "SHOULD_PAUSE=1"
)

:parse_args
if "%~1"=="" goto :args_done
set "ARG=%~1"
if /I "!ARG!"=="--help" goto :show_help
if /I "!ARG!"=="-h" goto :show_help
if /I "!ARG!"=="/?" goto :show_help
if /I "!ARG!"=="--rebuild" set "FLAG_REBUILD=1" & shift & goto :parse_args
if /I "!ARG!"=="-b" set "FLAG_REBUILD=1" & shift & goto :parse_args
if /I "!ARG!"=="--reset-ini" set "FLAG_RESET_INI=1" & shift & goto :parse_args
if /I "!ARG!"=="-r" set "FLAG_RESET_INI=1" & shift & goto :parse_args
if /I "!ARG!"=="--uninstall" set "FLAG_UNINSTALL=1" & shift & goto :parse_args
if /I "!ARG!"=="-u" set "FLAG_UNINSTALL=1" & shift & goto :parse_args
if /I "!ARG!"=="--silent" set "FLAG_SILENT=1" & shift & goto :parse_args
if /I "!ARG!"=="-y" set "FLAG_SILENT=1" & shift & goto :parse_args

if "!ARG:~0,1!"=="-" (
    if not defined BAD_ARG set "BAD_ARG=!ARG!"
    shift
    goto :parse_args
)
if not defined CUSTOM_DIR set "CUSTOM_DIR=!ARG!"
shift
goto :parse_args

:show_help
echo !C_CYAN!Usage:!C_RST! install.bat [options] [game_or_plugins_dir]
echo.
echo !C_CYAN!Options:!C_RST!
echo   !C_INFO![game_or_plugins_dir]!C_RST!  !C_DIM!ETS2 root folder (with bin\win_x64\eurotrucks2.exe) or plugins folder!C_RST!
echo   !C_INFO!--rebuild, -b!C_RST!          !C_DIM!Force rebuild ets2_tl_hud.dll before installing!C_RST!
echo   !C_INFO!--reset-ini, -r!C_RST!        !C_DIM!Overwrite target ets2_tl_hud.ini (backs up old file to .ini.bak)!C_RST!
echo   !C_INFO!--uninstall, -u!C_RST!        !C_DIM!Remove ets2_tl_hud.dll from ETS2 plugins directory!C_RST!
echo   !C_INFO!--silent, -y!C_RST!           !C_DIM!Non-interactive mode (no prompts, no pause)!C_RST!
echo   !C_INFO!--help, -h!C_RST!             !C_DIM!Show this help message!C_RST!
exit /b 0

:args_done
if "!FLAG_SILENT!"=="1" set "SHOULD_PAUSE=0"

if defined BAD_ARG (
    echo !C_ERR![ERROR]!C_RST! Unknown option: !BAD_ARG!
    echo !C_DIM!Run "install.bat --help" for usage.!C_RST!
    set "EXIT_CODE=1"
    goto :finish
)

echo !C_CYAN!============================================================!C_RST!
echo !C_CYAN!  ETS2 Traffic Light Windshield HUD - Installer!C_RST!
echo !C_CYAN!============================================================!C_RST!

set "GAME_DIR="
set "PLUGINS_DIR="

if defined CUSTOM_DIR goto :resolve_custom

echo !C_INFO![INFO]!C_RST! Searching for Euro Truck Simulator 2 installation...
for /f "usebackq delims=" %%D in (`powershell -NoProfile -Command "$c=@(); foreach($k in 'HKCU:\Software\Valve\Steam','HKLM:\SOFTWARE\WOW6432Node\Valve\Steam','HKLM:\SOFTWARE\Valve\Steam'){ $p=Get-ItemProperty $k -EA 0; if($p.SteamPath){$c+=$p.SteamPath}; if($p.InstallPath){$c+=$p.InstallPath} }; $libs=@(); $q=[char]34; foreach($s in $c){ $libs+=$s; $vdf=Join-Path $s 'steamapps\libraryfolders.vdf'; if(Test-Path $vdf){ foreach($m in [regex]::Matches((Get-Content $vdf -Raw), ($q+'path'+$q+'\s+'+$q+'([^'+$q+']+)'+$q))){ $libs+=$m.Groups[1].Value.Replace('\\','\') } } }; foreach($d in [IO.DriveInfo]::GetDrives()){ if($d.DriveType -eq 'Fixed'){ $r=$d.RootDirectory.FullName; $libs+=(Join-Path $r 'Steam'),(Join-Path $r 'Program Files (x86)\Steam'),(Join-Path $r 'Program Files\Steam'),(Join-Path $r 'SteamLibrary'),(Join-Path $r 'Games') } }; foreach($l in $libs){ $g=Join-Path $l 'steamapps\common\Euro Truck Simulator 2'; if(Test-Path (Join-Path $g 'bin\win_x64\eurotrucks2.exe')){ Write-Output $g; exit 0 }; $g2=Join-Path $l 'Euro Truck Simulator 2'; if(Test-Path (Join-Path $g2 'bin\win_x64\eurotrucks2.exe')){ Write-Output $g2; exit 0 } }"`) do (
    if not defined GAME_DIR set "GAME_DIR=%%D"
)

if defined GAME_DIR (
    set "PLUGINS_DIR=!GAME_DIR!\bin\win_x64\plugins"
    echo !C_OK![OK]!C_RST! Found ETS2: !GAME_DIR!
    goto :check_running
)

echo !C_WARN![WARN]!C_RST! Could not auto-detect Euro Truck Simulator 2.
if "!FLAG_SILENT!"=="1" (
    echo !C_ERR![ERROR]!C_RST! Please pass ETS2 path as an argument in silent mode.
    set "EXIT_CODE=1"
    goto :finish
)

:prompt_dir
echo !C_INFO![INFO]!C_RST! Please enter or drag-and-drop your ETS2 folder (or plugins folder):
set /p "CUSTOM_DIR=> "
if not defined CUSTOM_DIR (
    echo !C_ERR![ERROR]!C_RST! No path entered.
    set "EXIT_CODE=1"
    goto :finish
)
:: Strip surrounding quotes
set "CUSTOM_DIR=!CUSTOM_DIR:"=!"

:resolve_custom
:: Remove trailing backslash if any
if "!CUSTOM_DIR:~-1!"=="\" set "CUSTOM_DIR=!CUSTOM_DIR:~0,-1!"
if exist "!CUSTOM_DIR!\bin\win_x64\eurotrucks2.exe" (
    set "GAME_DIR=!CUSTOM_DIR!"
    set "PLUGINS_DIR=!CUSTOM_DIR!\bin\win_x64\plugins"
    echo !C_OK![OK]!C_RST! Target ETS2: !GAME_DIR!
    goto :check_running
)
if exist "!CUSTOM_DIR!\..\eurotrucks2.exe" (
    set "PLUGINS_DIR=!CUSTOM_DIR!"
    echo !C_OK![OK]!C_RST! Target plugins dir: !PLUGINS_DIR!
    goto :check_running
)
for %%F in ("!CUSTOM_DIR!") do set "LEAF=%%~nxF"
if /I "!LEAF!"=="plugins" (
    set "PLUGINS_DIR=!CUSTOM_DIR!"
    echo !C_OK![OK]!C_RST! Target plugins dir: !PLUGINS_DIR!
    goto :check_running
)
echo !C_ERR![ERROR]!C_RST! Invalid ETS2 directory: "!CUSTOM_DIR!"
echo !C_DIM!        Expected "bin\win_x64\eurotrucks2.exe" or a valid "plugins" folder.!C_RST!
set "EXIT_CODE=1"
goto :finish

:check_running
tasklist /FI "IMAGENAME eq eurotrucks2.exe" 2>nul | find /I "eurotrucks2.exe" >nul
if not errorlevel 1 (
    echo !C_ERR![ERROR]!C_RST! Euro Truck Simulator 2 is currently running.
    echo !C_DIM!        Please close the game before installing or uninstalling plugins.!C_RST!
    set "EXIT_CODE=1"
    goto :finish
)

if "!FLAG_UNINSTALL!"=="1" goto :do_uninstall

:: Ensure plugins directory exists (fresh ETS2 installs may not have it)
if exist "!PLUGINS_DIR!" goto :plugins_dir_ok
echo !C_INFO![INFO]!C_RST! Creating plugins directory: !PLUGINS_DIR!
mkdir "!PLUGINS_DIR!" >nul 2>&1
if errorlevel 1 (
    echo !C_ERR![ERROR]!C_RST! Failed to create plugins directory: !PLUGINS_DIR!
    echo !C_DIM!        If the game is in Program Files, right-click install.bat and "Run as administrator".!C_RST!
    set "EXIT_CODE=1"
    goto :finish
)

:plugins_dir_ok
if "!FLAG_REBUILD!"=="1" goto :do_build
if exist "dist\ets2_tl_hud.dll" goto :do_copy_dll
echo !C_INFO![INFO]!C_RST! dist\ets2_tl_hud.dll not found, building first...

:do_build
call build.bat
if errorlevel 1 (
    echo !C_ERR![ERROR]!C_RST! Build failed.
    set "EXIT_CODE=1"
    goto :finish
)

:do_copy_dll
copy /Y "dist\ets2_tl_hud.dll" "!PLUGINS_DIR!\ets2_tl_hud.dll" >nul
if errorlevel 1 (
    echo !C_ERR![ERROR]!C_RST! Failed to copy DLL to "!PLUGINS_DIR!\ets2_tl_hud.dll".
    echo !C_DIM!        Ensure ETS2 is closed, or run as administrator if under Program Files.!C_RST!
    set "EXIT_CODE=1"
    goto :finish
)
echo !C_OK![OK]!C_RST! Installed DLL: !PLUGINS_DIR!\ets2_tl_hud.dll

:: Handle INI config
if not exist "!PLUGINS_DIR!\ets2_tl_hud.ini" goto :copy_default_ini
if "!FLAG_RESET_INI!"=="1" goto :reset_ini
echo !C_INFO![INFO]!C_RST! Kept existing config: !PLUGINS_DIR!\ets2_tl_hud.ini
goto :check_deps

:reset_ini
copy /Y "!PLUGINS_DIR!\ets2_tl_hud.ini" "!PLUGINS_DIR!\ets2_tl_hud.ini.bak" >nul
if not errorlevel 1 echo !C_INFO![INFO]!C_RST! Backed up old config to: !PLUGINS_DIR!\ets2_tl_hud.ini.bak
copy /Y "ets2_tl_hud.ini" "!PLUGINS_DIR!\ets2_tl_hud.ini" >nul
if errorlevel 1 (
    echo !C_WARN![WARN]!C_RST! Failed to overwrite config file.
) else (
    echo !C_OK![OK]!C_RST! Reset config to default: !PLUGINS_DIR!\ets2_tl_hud.ini
)
goto :check_deps

:copy_default_ini
copy /Y "ets2_tl_hud.ini" "!PLUGINS_DIR!\ets2_tl_hud.ini" >nul
if errorlevel 1 (
    echo !C_WARN![WARN]!C_RST! Failed to copy default config file.
) else (
    echo !C_OK![OK]!C_RST! Copied default config: !PLUGINS_DIR!\ets2_tl_hud.ini
)

:check_deps
if exist "!PLUGINS_DIR!\ets2la_plugin.dll" (
    echo !C_OK![OK]!C_RST! Dependency found: ets2la_plugin.dll
) else (
    echo !C_WARN![WARN]!C_RST! Missing dependency: !PLUGINS_DIR!\ets2la_plugin.dll
    echo !C_DIM!       Traffic light ^& windshield 3D camera data require ets2la_plugin.dll ^(matching game version^).!C_RST!
    echo !C_DIM!       Download or install via ETS2LA / https://gitlab.com/ETS2LA/ets2la_plugin!C_RST!
)

echo.
echo !C_OK![DONE]!C_RST! Installation complete! Launch ETS2 and confirm the SDK plugin prompt.
goto :finish

:do_uninstall
if not exist "!PLUGINS_DIR!\ets2_tl_hud.dll" (
    echo !C_INFO![INFO]!C_RST! ets2_tl_hud.dll is not installed in: !PLUGINS_DIR!
) else (
    del /F /Q "!PLUGINS_DIR!\ets2_tl_hud.dll" >nul 2>&1
    if errorlevel 1 (
        echo !C_ERR![ERROR]!C_RST! Failed to delete "!PLUGINS_DIR!\ets2_tl_hud.dll".
        echo !C_DIM!        Close ETS2 first or run as administrator.!C_RST!
        set "EXIT_CODE=1"
        goto :finish
    )
    echo !C_OK![OK]!C_RST! Removed DLL: !PLUGINS_DIR!\ets2_tl_hud.dll
)

if exist "!PLUGINS_DIR!\ets2_tl_hud.ini" (
    if "!FLAG_SILENT!"=="1" (
        echo !C_INFO![INFO]!C_RST! Kept config file: !PLUGINS_DIR!\ets2_tl_hud.ini
    ) else (
        set "DEL_INI="
        set /p "DEL_INI=!C_WARN![PROMPT]!C_RST! Also remove config file ets2_tl_hud.ini? [y/N]: "
        if /I "!DEL_INI!"=="y" (
            del /F /Q "!PLUGINS_DIR!\ets2_tl_hud.ini" >nul 2>&1
            echo !C_OK![OK]!C_RST! Removed config: !PLUGINS_DIR!\ets2_tl_hud.ini
        ) else (
            echo !C_INFO![INFO]!C_RST! Kept config: !PLUGINS_DIR!\ets2_tl_hud.ini
        )
    )
)
echo !C_OK![DONE]!C_RST! Uninstall complete.

:finish
if "!SHOULD_PAUSE!"=="1" (
    echo.
    pause
)
exit /b !EXIT_CODE!
