param(
    [Parameter(Mandatory = $true)]
    [string] $ProductRoot,
    [string[]] $Installations
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Get-Msvc144Installation {
    param([string[]] $Candidates)

    $bestPath = $null
    $bestVersion = $null
    foreach ($candidate in $Candidates) {
        if ($null -eq $candidate) { continue }
        $install = $candidate.Trim()
        if (-not $install) { continue }
        $vcvars = Join-Path $install 'VC' 'Auxiliary' 'Build' 'vcvars64.bat'
        $tools = Join-Path $install 'VC' 'Tools' 'MSVC'
        if (-not (Test-Path -LiteralPath $vcvars) -or -not (Test-Path -LiteralPath $tools)) {
            continue
        }
        foreach ($dir in (Get-ChildItem -LiteralPath $tools -Directory)) {
            if ($dir.Name -notlike '14.44.*') { continue }
            $version = $null
            if (-not [version]::TryParse($dir.Name, [ref]$version)) { continue }
            if ($null -eq $bestVersion -or $version -gt $bestVersion) {
                $bestVersion = $version
                $bestPath = $install
            }
        }
    }
    if (-not $bestPath) {
        throw 'MSVC 14.44 was not found. docs/building-win.md requires -vcvars_ver=14.44 (v144.4). A newer Visual Studio without that toolset cannot be used.'
    }
    return @{ Path = $bestPath; Version = $bestVersion.ToString() }
}

if (-not $Installations -or $Installations.Count -eq 0) {
    $finder = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $finder)) {
        throw 'vswhere.exe is required to locate MSVC 14.44.'
    }
    $found = @(& $finder -products * -prerelease -property installationPath)
    if ($LASTEXITCODE -ne 0) { throw 'vswhere failed while listing Visual Studio installations.' }
    $Installations = @($found | Where-Object { $_ -and $_.Trim() })
    if ($Installations.Count -eq 0) { throw 'vswhere found no Visual Studio installation.' }
}

$selected = Get-Msvc144Installation -Candidates @($Installations)
$localFile = Join-Path $ProductRoot 'tools.local.json'
$payload = [ordered]@{ visualStudio = $selected.Path }
if (Test-Path -LiteralPath $localFile) {
    $existing = Get-Content -Raw -LiteralPath $localFile | ConvertFrom-Json
    foreach ($name in @('cmake', 'ninja', 'python')) {
        $property = $existing.PSObject.Properties[$name]
        if ($property -and "$($property.Value)") {
            $payload[$name] = [string]$property.Value
        }
    }
}
$payload | ConvertTo-Json | Set-Content -LiteralPath $localFile -Encoding utf8NoBOM
Write-Output ("MSVC {0} at {1}" -f $selected.Version, $selected.Path)
