# Requires an elevated Windows test builder, the Qt fixture executable and a
# real release MSI. Administrative extraction exercises the Windows Installer
# service without installing, removing or launching the Client.
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$QtFixture,
    [string]$Helper = (Join-Path $PSScriptRoot '../../app/backend/fernweh-install-update.ps1')
)
$ErrorActionPreference = 'Stop'
$directory = Join-Path ([IO.Path]::GetTempPath()) ('fernweh-msi-service-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($directory)
$stagedPackage = $null
try {
    $stagedPackage = & $QtFixture $Package
    if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $stagedPackage)) { throw 'Qt fixture failed.' }
    $beforeLog = Join-Path $directory 'before.log'
    $extract = Join-Path $directory 'extracted'
    $before = Start-Process "$env:SystemRoot\System32\msiexec.exe" -ArgumentList (
        '/a "' + $stagedPackage + '" /qn /norestart TARGETDIR="' + $extract + '" /L*V "' + $beforeLog + '"'
    ) -Wait -PassThru
    if ($before.ExitCode -ne 110) { throw "Owner-only Qt fixture returned $($before.ExitCode), expected 110." }
    if ((Get-Content -Raw $beforeLog) -notmatch '1309 2: 5') { throw 'Missing service access-denied evidence.' }
    $helperCopy = Join-Path (Split-Path -Parent $stagedPackage) 'install-update.ps1'
    Copy-Item -LiteralPath $Helper -Destination $helperCopy
    $serviceResult = @{ ExitCode = $null; Log = $null }
    function Get-Process { param($Id, $ErrorAction) return $null }
    function Start-Process {
        param($FilePath, $ArgumentList, $Verb, [switch]$Wait, [switch]$PassThru)
        if ($FilePath -ne "$env:SystemRoot\System32\msiexec.exe" -or $Verb -ne 'RunAs' -or -not $Wait -or -not $PassThru) {
            throw 'Unexpected installer launch contract.'
        }
        # Keep the production helper's exact path/log arguments and real MSI
        # process. Change only install into extraction to protect installed apps.
        $ArgumentList = $ArgumentList -replace '^/i ', '/a '
        $ArgumentList = $ArgumentList -replace '/passive', '/qn'
        $ArgumentList += ' TARGETDIR="' + $extract + '"'
        if ($ArgumentList -notmatch '/L\*V "([^"]+)"') { throw 'Missing persistent installer log.' }
        $serviceResult.Log = $Matches[1]
        $process = Microsoft.PowerShell.Management\Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -Wait -PassThru
        $serviceResult.ExitCode = $process.ExitCode
        return $process
    }
    $engine = New-Object -ComObject WindowsInstaller.Installer
    $database = $engine.OpenDatabase((Resolve-Path $Package).Path, 0)
    $view = $database.OpenView('SELECT `Value` FROM `Property` WHERE `Property` = ''ProductVersion''')
    [void]$view.Execute()
    $record = $view.Fetch()
    $version = $record.StringData(1)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
    [void]$view.Close()
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine)
    & $helperCopy -Installer $stagedPackage.Replace('\', '/') `
        -ExpectedHash (Get-FileHash -LiteralPath $stagedPackage -Algorithm SHA256).Hash.ToLowerInvariant() `
        -ExpectedSize (Get-Item -LiteralPath $stagedPackage).Length -ExpectedVersion $version `
        -ParentProcessId 0 -Executable (Join-Path $directory 'absent.exe')
    if ($serviceResult.ExitCode -ne 0) { throw "Repaired helper returned $($serviceResult.ExitCode)" }
    if (-not (Test-Path -LiteralPath (Join-Path $extract 'Program Files\BDE fernweh\plank-client.exe')) -and
        -not (Get-ChildItem $extract -Filter plank-client.exe -Recurse)) { throw 'Client was not extracted.' }
    if (Test-Path -LiteralPath $stagedPackage) { throw 'Helper failed to clean up its MSI.' }
    if (-not (Test-Path -LiteralPath $serviceResult.Log)) { throw 'Helper discarded its installer log.' }
    Write-Output 'PASS: real Qt owner-only directory reproduces MSI 110/access denied; production helper repairs service access and MSI extraction succeeds.'
} finally {
    if ($stagedPackage) {
        $stagedDirectory = Split-Path -Parent $stagedPackage
        if (Test-Path -LiteralPath $stagedDirectory) { Remove-Item -LiteralPath $stagedDirectory -Recurse -Force }
    }
    Remove-Item -LiteralPath $directory -Recurse -Force
}
