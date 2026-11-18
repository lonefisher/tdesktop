$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script = Join-Path $PSScriptRoot 'pin-msvc-14-44.ps1'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('pin-msvc-14-44-' + [guid]::NewGuid().ToString('n'))
New-Item -ItemType Directory -Path $root | Out-Null

function New-FakeVisualStudio {
    param(
        [string] $Name,
        [string[]] $Toolsets,
        [switch] $NoVcvars
    )
    $install = Join-Path $root $Name
    foreach ($toolset in $Toolsets) {
        New-Item -ItemType Directory -Force -Path (Join-Path $install 'VC' 'Tools' 'MSVC' $toolset) | Out-Null
    }
    if (-not $NoVcvars) {
        $vcvars = Join-Path $install 'VC' 'Auxiliary' 'Build' 'vcvars64.bat'
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $vcvars) | Out-Null
        Set-Content -LiteralPath $vcvars -Value '@echo off' -Encoding ascii
    }
    return $install
}

function Assert-Selected {
    param(
        [string] $Label,
        [string[]] $Installations,
        [string] $Expected
    )
    $product = Join-Path $root ("product-" + $Label)
    New-Item -ItemType Directory -Path $product | Out-Null
    & $script -ProductRoot $product -Installations $Installations
    $written = Get-Content -Raw -LiteralPath (Join-Path $product 'tools.local.json') | ConvertFrom-Json
    if ($written.visualStudio -ne $Expected) {
        throw "$Label selected $($written.visualStudio), expected $Expected."
    }
}

try {
    $buildTools = New-FakeVisualStudio 'BuildTools2022' @('14.44.35207')
    $visualStudio2026 = New-FakeVisualStudio 'VS2026' @('14.50.35717')
    Assert-Selected 'newer-studio-without-14.44' @($visualStudio2026, $buildTools) $buildTools

    $newerPatch = New-FakeVisualStudio 'VS2026-with-14.44' @('14.50.35717', '14.44.35211')
    Assert-Selected 'highest-14.44-anywhere' @($buildTools, $newerPatch) $newerPatch

    $broken = New-FakeVisualStudio 'NoVcvars' @('14.44.99999') -NoVcvars
    Assert-Selected 'skip-install-without-vcvars' @($broken, $buildTools) $buildTools

    $kept = Join-Path $root 'product-keep'
    New-Item -ItemType Directory -Path $kept | Out-Null
    @{ cmake = 'C:\Tools\cmake.exe'; extra = 'drop-me' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $kept 'tools.local.json') -Encoding utf8NoBOM
    & $script -ProductRoot $kept -Installations @($buildTools)
    $keptJson = Get-Content -Raw -LiteralPath (Join-Path $kept 'tools.local.json') | ConvertFrom-Json
    if ($keptJson.visualStudio -ne $buildTools) { throw 'Override did not pin visualStudio.' }
    if ($keptJson.cmake -ne 'C:\Tools\cmake.exe') { throw 'cmake override was dropped.' }
    if ($keptJson.PSObject.Properties['extra']) { throw 'Unknown override was kept.' }

    $missing = Join-Path $root 'product-missing'
    New-Item -ItemType Directory -Path $missing | Out-Null
    $failed = $false
    try {
        & $script -ProductRoot $missing -Installations @($visualStudio2026)
    } catch {
        $failed = $true
        if ("$_" -notmatch '14\.44') { throw }
    }
    if (-not $failed) { throw 'A Visual Studio install without MSVC 14.44 was accepted.' }
    Write-Output 'pin-msvc-14-44 ok'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
