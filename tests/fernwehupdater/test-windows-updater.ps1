# Run with Windows PowerShell 5.1. Generates unsigned MSI database fixtures;
# Validation-only mode preserves fixtures. The install-path tests replace only
# process launch with a native argv probe: no elevation or installation occurs.
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
function Verify-InstallerLaunch([string]$name, [string]$pathStyle) {
    $package = Create-Package $name 'BDE fernweh' $upgrade '1.1.40'
    $inputPath = $package
    if ($pathStyle -eq 'forward') { $inputPath = $package.Replace('\', '/') }
    if ($pathStyle -eq 'mixed') { $inputPath = $package.Replace('\' + $name, '/' + $name) }
    $helperCopy = Join-Path $directory 'install-update.ps1'
    Copy-Item -LiteralPath $Helper -Destination $helperCopy
    $launches = [Collections.Generic.List[object]]::new()
    # Exercise the complete production helper, including package validation and
    # cleanup. Feed its actual ArgumentList through Windows process creation.
    function Get-Process { param($Id, $ErrorAction) return $null }
    function Start-Process {
        param($FilePath, $ArgumentList, $Verb, [switch]$Wait, [switch]$PassThru)
        $launches.Add([pscustomobject]@{
            FilePath = $FilePath; Verb = $Verb; Wait = $Wait.IsPresent; PassThru = $PassThru.IsPresent
        })
        if ($FilePath -eq "$env:SystemRoot\System32\msiexec.exe") {
            # Assert access at the service boundary, before the helper cleans up.
            foreach ($item in @((Split-Path -Parent $package), $package)) {
                $rules = (Get-Acl -LiteralPath $item).GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])
                foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
                    $read = @($rules | Where-Object {
                        $_.IdentityReference.Value -eq $sid -and
                        $_.AccessControlType -eq 'Allow' -and
                        ($_.FileSystemRights -band [Security.AccessControl.FileSystemRights]::ReadAndExecute) -eq
                            [Security.AccessControl.FileSystemRights]::ReadAndExecute
                    })
                    if (-not $read.Count) { throw "Installer service cannot read $item as $sid" }
                }
            }
            return Microsoft.PowerShell.Management\Start-Process -FilePath $probe -ArgumentList $ArgumentList -Wait -PassThru
        }
    }
    & $helperCopy -Installer $inputPath -ExpectedHash (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant() `
        -ExpectedSize (Get-Item -LiteralPath $package).Length -ExpectedVersion '1.1.040' -ParentProcessId 0 -Executable $probe
    $received = @([IO.File]::ReadAllLines($env:FERNWEH_ARGV_PROBE) | ForEach-Object {
        [Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($_))
    })
    $expected = @('/i', $package, '/passive', '/norestart', 'REBOOT=ReallySuppress', '/L*V', $received[-1])
    if ((Split-Path -Leaf $received[-1]) -notmatch '^install-[a-f0-9]{32}\.log$' -or
        (Split-Path -Parent $received[-1]) -cne (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'BDE fernweh\updater')) {
        throw 'Installer diagnostic log is not in the persistent per-user update directory.'
    }
    if ($received.Count -ne $expected.Count) { throw "Installer received $($received.Count) arguments, expected $($expected.Count)." }
    for ($index = 0; $index -lt $expected.Count; $index++) {
        if ($received[$index] -cne $expected[$index]) { throw "Installer argument $index changed: [$($received[$index])] instead of [$($expected[$index])]." }
    }
    if ($launches.Count -ne 2 -or $launches[0].Verb -ne 'RunAs' -or
        -not $launches[0].Wait -or -not $launches[0].PassThru -or
        $launches[1].FilePath -ne $probe) { throw 'Installer elevation/wait or application relaunch contract changed.' }
    if ((Test-Path -LiteralPath $package) -or (Test-Path -LiteralPath $helperCopy)) { throw 'Installer left its temporary files behind.' }
}
$previousProbeOutput = $env:FERNWEH_ARGV_PROBE
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
    $probe = Join-Path $directory 'argv-probe.exe'
    $env:FERNWEH_ARGV_PROBE = Join-Path $directory 'argv.txt'
    Add-Type -OutputAssembly $probe -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.IO;
using System.Text;
public static class ArgvProbe {
    public static void Main(string[] args) {
        File.WriteAllLines(Environment.GetEnvironmentVariable("FERNWEH_ARGV_PROBE"),
            Array.ConvertAll(args, value => Convert.ToBase64String(Encoding.UTF8.GetBytes(value))));
    }
}
'@
    foreach ($pathStyle in @('native', 'forward', 'mixed')) {
        Verify-InstallerLaunch 'update.msi' $pathStyle
        Verify-InstallerLaunch 'update with spaces.msi' $pathStyle
        Verify-InstallerLaunch ("artist's update & " + [char]0x00fc + '.msi') $pathStyle
    }
    Write-Output 'PASS: native installer argv, elevation request, wait, relaunch and cleanup (9 paths).'
} finally {
    $env:FERNWEH_ARGV_PROBE = $previousProbeOutput
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine)
    Get-ChildItem -LiteralPath $directory -File | Remove-Item
    Remove-Item -LiteralPath $directory
}
