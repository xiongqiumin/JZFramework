# ui_mcp_test.ps1 — 通过 MCP (HTTP :9601) 驱动/核对 test_widget
# 用法: 先启动 test_widget.exe, 再运行本脚本
$ErrorActionPreference = 'Stop'
$uri = 'http://127.0.0.1:9601/mcp'
$meta = @{ 'io.modelcontextprotocol/protocolVersion' = '2026-07-28' }
$script:nextId = 1

function Invoke-Mcp {
    param([string]$method, [object]$params)
    $script:nextId++
    $body = @{
        jsonrpc = '2.0'
        id      = $script:nextId
        method  = $method
        params  = $params
        _meta   = $script:meta
    } | ConvertTo-Json -Depth 8
    $resp = Invoke-RestMethod -Method Post -Uri $uri -ContentType 'application/json' -Body $body
    return $resp.result.content[0].text
}

Write-Host '=== 1. tools/list ==='
$bodyList = @{ jsonrpc = '2.0'; id = 0; method = 'tools/list'; params = @{}; _meta = $meta } | ConvertTo-Json -Depth 8
$resp = Invoke-RestMethod -Method Post -Uri $uri -ContentType 'application/json' -Body $bodyList
Write-Host ($resp.result.tools.name -join ', ')

Write-Host '=== 2. get_ui_state (语义树前 40 行) ==='
$tree = Invoke-Mcp 'tools/call' @{ name = 'get_ui_state'; arguments = @{} }
($tree -split "`n" | Select-Object -First 40) -join "`n"

Write-Host '=== 3. ui_invoke: click button (Label 页 Update) ==='
Invoke-Mcp 'tools/call' @{ name = 'ui_invoke'; arguments = @{ target = 'button'; action = 'click' } }

Write-Host '=== 4. select list[2] 切到 Slider 页 ==='
Invoke-Mcp 'tools/call' @{ name = 'ui_invoke'; arguments = @{ target = 'list'; action = 'select'; value = 2 } }

Write-Host '=== 5. ui_invoke: setValue slider = 80 ==='
Invoke-Mcp 'tools/call' @{ name = 'ui_invoke'; arguments = @{ target = 'slider'; action = 'setValue'; value = 80 } }

Write-Host '=== 6. get_ui_state 核对 slider range ==='
$tree = Invoke-Mcp 'tools/call' @{ name = 'get_ui_state'; arguments = @{} }
($tree -split "`n" | Select-String -Pattern 'slider|range|value|min|max' | Select-Object -First 10) -join "`n"

Write-Host '=== 7. ui_screenshot ==='
Invoke-Mcp 'tools/call' @{ name = 'ui_screenshot'; arguments = @{} }

Write-Host 'done.'
