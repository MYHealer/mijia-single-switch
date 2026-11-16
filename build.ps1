# pressure-mijia 构建脚本
# 用法: powershell -File build.ps1 [build|flash|monitor|all|menuconfig|fullclean] -Port COMx
# 依赖: dot-source E:\ESP\esp_env.ps1 (ESP-IDF 5.5.4)

param(
    [string]$Action = "build",
    [string]$Port = "COM4"
)

$ErrorActionPreference = "Stop"

# 引入 IDF 环境
. E:\ESP\esp_env.ps1

$ProjectDir = "E:\ESP\c3supermini\pressure-mijia"
$IdfPy      = "E:\ESP\.espressif\v5.5.4\esp-idf\tools\idf.py"
$Python     = "E:\ESP\.espressif\tools\python_env\idf5.5_py3.14_env\Scripts\python.exe"

Set-Location $ProjectDir

switch ($Action) {
    "build"      { & $Python $IdfPy build }
    "flash"      { & $Python $IdfPy -p $Port flash }
    "monitor"    { & $Python $IdfPy -p $Port monitor }
    "flashmon"   { & $Python $IdfPy -p $Port flash monitor }
    "all"        {
        & $Python $IdfPy build
        if ($LASTEXITCODE -eq 0) { & $Python $IdfPy -p $Port flash monitor }
    }
    "menuconfig" { & $Python $IdfPy menuconfig }
    "fullclean"  { & $Python $IdfPy fullclean }
    default {
        Write-Host "用法: build.ps1 [build|flash|monitor|all|menuconfig|fullclean] -Port COMx"
    }
}