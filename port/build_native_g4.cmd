@echo off
setlocal
set SCRIPT_DIR=%~dp0
set OUT_DIR=%SCRIPT_DIR%build-native-g4
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

where link.exe >nul 2>nul
if errorlevel 1 (
    if exist "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else if exist "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
    )
)

if defined CLANG_BIN (
    set "CLANG=%CLANG_BIN%\clang.exe"
    set "CLANGXX=%CLANG_BIN%\clang++.exe"
) else if defined EMSDK (
    set "CLANG=%EMSDK%\upstream\bin\clang.exe"
    set "CLANGXX=%EMSDK%\upstream\bin\clang++.exe"
) else (
    set "CLANG=clang.exe"
    set "CLANGXX=clang++.exe"
)

if exist "%SCRIPT_DIR%src\sh2_aot_twcup98.cpp" (
    set AOT_UNIT=sh2_aot_twcup98
) else (
    set AOT_UNIT=sh2_aot_stub
)

for %%F in (sh2_core sh2_hle_sgl %AOT_UNIT% stv_315_5881 stv_peripherals cpn8_io stv_vdp stv_scsp twc98_copa_mod twc98_coop_mod stv_machine twc98_net_session) do (
    "%CLANGXX%" --target=x86_64-pc-windows-msvc -std=c++17 -O3 -fno-exceptions -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -I "%SCRIPT_DIR%src" -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%src\%%F.cpp" -o - > "%OUT_DIR%\%%F.o"
    if errorlevel 1 exit /b 1
)

"%CLANG%" --target=x86_64-pc-windows-msvc -O3 -w -D_CRT_SECURE_NO_WARNINGS -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%third_party\musashi\m68kcpu.c" -o - > "%OUT_DIR%\m68kcpu.o"
if errorlevel 1 exit /b 1
"%CLANG%" --target=x86_64-pc-windows-msvc -O3 -w -D_CRT_SECURE_NO_WARNINGS -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%third_party\musashi\m68kops.c" -o - > "%OUT_DIR%\m68kops.o"
if errorlevel 1 exit /b 1
"%CLANG%" --target=x86_64-pc-windows-msvc -O3 -w -D_CRT_SECURE_NO_WARNINGS -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%third_party\musashi\softfloat\softfloat.c" -o - > "%OUT_DIR%\softfloat.o"
if errorlevel 1 exit /b 1

for %%T in (verify_g3_subtasks1_3 verify_g3_save_bench verify_h6_rollback_bench verify_h6_net_sim) do (
    "%CLANGXX%" --target=x86_64-pc-windows-msvc -std=c++17 -O2 -fno-exceptions -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -I "%SCRIPT_DIR%src" -c "%SCRIPT_DIR%tests\%%T.cpp" -o - > "%OUT_DIR%\%%T.o"
    if errorlevel 1 exit /b 1
)

set CORE_OBJS="%OUT_DIR%\sh2_core.o" "%OUT_DIR%\sh2_hle_sgl.o" "%OUT_DIR%\%AOT_UNIT%.o" "%OUT_DIR%\stv_315_5881.o" "%OUT_DIR%\stv_peripherals.o" "%OUT_DIR%\cpn8_io.o" "%OUT_DIR%\stv_vdp.o" "%OUT_DIR%\stv_scsp.o" "%OUT_DIR%\twc98_copa_mod.o" "%OUT_DIR%\twc98_coop_mod.o" "%OUT_DIR%\stv_machine.o" "%OUT_DIR%\twc98_net_session.o" "%OUT_DIR%\m68kcpu.o" "%OUT_DIR%\m68kops.o" "%OUT_DIR%\softfloat.o"

link.exe "-out:%OUT_DIR%\verify_g3_subtasks1_3.exe" -defaultlib:libcmt -defaultlib:oldnames -nologo %CORE_OBJS% "%OUT_DIR%\verify_g3_subtasks1_3.o"
if errorlevel 1 exit /b 1

link.exe "-out:%OUT_DIR%\verify_g3_save_bench.exe" -defaultlib:libcmt -defaultlib:oldnames -nologo %CORE_OBJS% "%OUT_DIR%\verify_g3_save_bench.o"
if errorlevel 1 exit /b 1

link.exe "-out:%OUT_DIR%\verify_h6_rollback_bench.exe" -defaultlib:libcmt -defaultlib:oldnames -nologo %CORE_OBJS% "%OUT_DIR%\verify_h6_rollback_bench.o"
if errorlevel 1 exit /b 1

link.exe "-out:%OUT_DIR%\verify_h6_net_sim.exe" -defaultlib:libcmt -defaultlib:oldnames -nologo %CORE_OBJS% "%OUT_DIR%\verify_h6_net_sim.o"
if errorlevel 1 exit /b 1

"%CLANGXX%" --target=x86_64-pc-windows-msvc -std=c++17 -O3 -fno-exceptions -Wall -Wextra -Werror -D_CRT_SECURE_NO_WARNINGS -I "%SCRIPT_DIR%src" -c "%SCRIPT_DIR%hosts\pc\twc98_pc.cpp" -o - > "%OUT_DIR%\twc98_pc.o"
if errorlevel 1 exit /b 1

link.exe "-out:%OUT_DIR%\twc98_pc.exe" -defaultlib:libcmt -defaultlib:oldnames -nologo %CORE_OBJS% "%OUT_DIR%\twc98_pc.o"
if errorlevel 1 exit /b 1

echo BUILD_NATIVE_G4_OK (%AOT_UNIT%)
