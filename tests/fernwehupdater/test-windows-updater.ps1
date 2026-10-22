# Run with Windows PowerShell 5.1. Generates unsigned MSI database fixtures;
# validation-only mode never elevates, installs, deletes fixtures or launches UI.
param([string]$Helper = (Join-Path $PSScriptRoot '../../app/backend/fernweh-install-update.ps1'))
$ErrorActionPreference = 'Stop'
$directory = Join-Path ([IO.Path]::GetTempPath()) ('fernweh-updater-test-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $directory)
$engine = New-Object -ComObject WindowsInstaller.Installer
function Create-Package([string]$name, [string]$product, [string]$upgrade, [string]$version) {
    $path = Join-Path $directory $name
    $database = $engine.OpenDatabase($path, 3)
    $view = $database.OpenView('CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` CHAR(0) LOCALIZABLE PRIMARY KEY `Property`)')
    [void]$view.Execute()
    [void]$view.Close()
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    foreach ($entry in @(@('ProductName', $product), @('UpgradeCode', $upgrade), @('ProductVersion', $version))) {
        $view = $database.OpenView("INSERT INTO ``Property`` (``Property``, ``Value``) VALUES ('$($entry[0])', '$($entry[1])')")
        [void]$view.Execute()
        [void]$view.Close()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    }
    [void]$database.Commit()
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    return $path
}
function Verify-Package([string]$path, [bool]$valid, [string]$hash = '', [long]$size = 0) {
    if (-not $hash) { $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
    if (-not $size) { $size = (Get-Item -LiteralPath $path).Length }
    $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $Helper,
        '-Installer', $path, '-ExpectedHash', $hash, '-ExpectedSize', $size,
        '-ExpectedVersion', '1.1.040', '-ParentProcessId', '0', '-Executable', 'unused.exe', '-ValidateOnly')
    $result = & "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" @arguments
    if (($LASTEXITCODE -eq 0) -ne $valid) { throw "Unexpected validation result for $(Split-Path -Leaf $path): $result" }
    if (-not (Test-Path -LiteralPath $path)) { throw 'Validation deleted its input.' }
}
try {
    $upgrade = '{F598C7E9-ECA2-432F-B0C0-41F9E1201F8F}'
    $good = Create-Package 'valid unsigned.msi' 'BDE fernweh' $upgrade '1.1.40'
    Verify-Package $good $true
    Verify-Package $good $false ('0' * 64)
    Verify-Package $good $false '' ((Get-Item -LiteralPath $good).Length + 1)
    Verify-Package (Create-Package 'wrong-name.msi' 'Another app' $upgrade '1.1.40') $false
    Verify-Package (Create-Package 'wrong-family.msi' 'BDE fernweh' '{00000000-0000-0000-0000-000000000000}' '1.1.40') $false
    Verify-Package (Create-Package 'wrong-version.msi' 'BDE fernweh' $upgrade '1.1.41') $false
    $invalid = Join-Path $directory 'invalid.msi'
    [IO.File]::WriteAllText($invalid, 'not an MSI')
    Verify-Package $invalid $false
    Write-Output 'PASS: unsigned MSI identity, numeric version, tamper, size and malformed package validation (7 cases).'
} finally {
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine)
    Get-ChildItem -LiteralPath $directory -File | Remove-Item
    Remove-Item -LiteralPath $directory
}
