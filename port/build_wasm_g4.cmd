@echo off
setlocal
set SCRIPT_DIR=%~dp0
set OUT_DIR=%SCRIPT_DIR%build-wasm-g4
if not exist "%OUT_DIR%" mkdir "%OUT_DIR%"

if not defined EMSDK (
    echo ERROR: Set EMSDK to your Emscripten SDK directory, e.g. C:\emsdk
    exit /b 1
)

set "CLANG=%EMSDK%\upstream\bin\clang.exe"
set "CLANGXX=%EMSDK%\upstream\bin\clang++.exe"
set "WASMLD=%EMSDK%\upstream\bin\wasm-ld.exe"
set "SYSROOT=%EMSDK%\upstream\emscripten\cache\sysroot"

set TEMP=%OUT_DIR%
set TMP=%OUT_DIR%
set TMPDIR=%OUT_DIR%

if exist "%SCRIPT_DIR%src\sh2_aot_twcup98.cpp" (
    set AOT_UNIT=sh2_aot_twcup98
) else (
    set AOT_UNIT=sh2_aot_stub
)

for %%F in (sh2_core sh2_hle_sgl %AOT_UNIT% stv_315_5881 stv_peripherals cpn8_io stv_vdp stv_scsp twc98_copa_mod twc98_coop_mod stv_machine twc98_net_session wasm_port) do (
    "%CLANGXX%" --target=wasm32-unknown-emscripten "--sysroot=%SYSROOT%" -std=c++17 -O3 -flto -msimd128 -mbulk-memory -fno-exceptions -fno-rtti -Wall -Wextra -Werror -DTWC98_NO_STL -I "%SCRIPT_DIR%src" -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%src\%%F.cpp" -o - > "%OUT_DIR%\%%F.o"
    if errorlevel 1 exit /b 1
)

"%CLANG%" --target=wasm32-unknown-emscripten "--sysroot=%SYSROOT%" -O3 -flto -msimd128 -mbulk-memory -w -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%third_party\musashi\m68kcpu.c" -o - > "%OUT_DIR%\m68kcpu.o"
if errorlevel 1 exit /b 1
"%CLANG%" --target=wasm32-unknown-emscripten "--sysroot=%SYSROOT%" -O3 -flto -msimd128 -mbulk-memory -w -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%third_party\musashi\m68kops.c" -o - > "%OUT_DIR%\m68kops.o"
if errorlevel 1 exit /b 1
"%CLANG%" --target=wasm32-unknown-emscripten "--sysroot=%SYSROOT%" -O3 -flto -msimd128 -mbulk-memory -w -I "%SCRIPT_DIR%third_party\musashi" -c "%SCRIPT_DIR%third_party\musashi\softfloat\softfloat.c" -o - > "%OUT_DIR%\softfloat.o"
if errorlevel 1 exit /b 1

"%WASMLD%" --no-entry --export-dynamic --lto-O3 --initial-memory=134217728 -o - "%OUT_DIR%\sh2_core.o" "%OUT_DIR%\sh2_hle_sgl.o" "%OUT_DIR%\%AOT_UNIT%.o" "%OUT_DIR%\stv_315_5881.o" "%OUT_DIR%\stv_peripherals.o" "%OUT_DIR%\cpn8_io.o" "%OUT_DIR%\stv_vdp.o" "%OUT_DIR%\stv_scsp.o" "%OUT_DIR%\twc98_copa_mod.o" "%OUT_DIR%\twc98_coop_mod.o" "%OUT_DIR%\stv_machine.o" "%OUT_DIR%\twc98_net_session.o" "%OUT_DIR%\m68kcpu.o" "%OUT_DIR%\m68kops.o" "%OUT_DIR%\softfloat.o" "%OUT_DIR%\wasm_port.o" "%SYSROOT%\lib\wasm32-emscripten\libc.a" "%SYSROOT%\lib\wasm32-emscripten\libclang_rt.builtins.a" > "%OUT_DIR%\twc98_g4.wasm"
if errorlevel 1 exit /b 1

echo BUILD_WASM_G4_OK (%AOT_UNIT%)
