# extract_credentials.ps1 — 从 PC 端 .runtime/xiaomi_session.json 提取米家凭据
# 输出: main/credentials_auto.h（不提交仓库，.gitignore 忽略）
#
# 用法: powershell -File scripts/extract_credentials.ps1
#   可选:  -SessionPath <路径>  指定 session json（默认如下）
#
# 提取自 tokens.mijia 分支：service_token / ssecurity
# 顶层：user_id / c_user_id

param(
    [string]$SessionPath = "E:\逆向工程\小米互通互联服务\02-工具脚本\.runtime\xiaomi_session.json"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $SessionPath)) {
    Write-Error "session file not found: $SessionPath"
    exit 1
}

$sess = Get-Content $SessionPath -Raw -Encoding UTF8 | ConvertFrom-Json
$mijia = $sess.tokens.mijia
if (-not $mijia) {
    Write-Error "no tokens.mijia in session"
    exit 1
}

$svcToken = $mijia.service_token
$ssec     = $mijia.ssecurity
$userId   = $sess.user_id
$cUser    = $sess.c_user_id

if (-not $svcToken -or -not $ssec) {
    Write-Error "missing service_token or ssecurity"
    exit 1
}

$content = @"
/*
 * credentials_auto.h — 自动生成，请勿手改！由 scripts/extract_credentials.ps1 生成。
 * 内容来自 .runtime/xiaomi_session.json（tokens.mijia 分支）。
 * 已自动加入 .gitignore，避免凭据入库。
 */
#pragma once

#define PMJ_MIOT_SERVICE_TOKEN "$svcToken"
#define PMJ_MIOT_SSECURITY     "$ssec"
#define PMJ_MIOT_USER_ID       "$userId"
#define PMJ_MIOT_C_USER_ID     "$cUser"

/* 双击时间戳用于检测过期（可选） */
#define PMJ_MIOT_EXTRACTED_AT  "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
"@

$outPath = Join-Path $PSScriptRoot "..\main\credentials_auto.h"
Set-Content -Path $outPath -Value $content -Encoding UTF8

Write-Host "OK: $outPath"
Write-Host "  service_token len = $($svcToken.Length)"
Write-Host "  ssecurity     len = $($ssec.Length)"
Write-Host "  userId        = $userId"
Write-Host "  c_user_id     = $cUser"
Write-Host "NOTE: credentials expire; on expiry refresh then re-run this script"