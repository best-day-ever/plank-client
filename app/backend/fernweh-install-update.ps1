param(
    [Parameter(Mandatory=$true)][string]$Installer,
    [Parameter(Mandatory=$true)][string]$ExpectedHash,
    [Parameter(Mandatory=$true)][long]$ExpectedSize,
    [Parameter(Mandatory=$true)][string]$ExpectedVersion,
    [Parameter(Mandatory=$true)][int]$ParentProcessId,
    [Parameter(Mandatory=$true)][string]$Executable,
    [switch]$ValidateOnly
)
$ErrorActionPreference = 'Stop'
try {
    # The client only starts this helper once all streams have ended. Wait for
    # its normal process shutdown before asking Windows Installer to replace it.
    $parent = Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue
    if ($parent) { $parent.WaitForExit() }
    if ((Get-Item -LiteralPath $Installer).Length -ne $ExpectedSize -or
        (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ExpectedHash) {
        throw 'The downloaded update failed its integrity check. Please check for updates again.'
    }
    $engine = New-Object -ComObject WindowsInstaller.Installer
    $database = $engine.OpenDatabase($Installer, 0)
    function Read-MsiProperty([string]$name) {
        $view = $database.OpenView('SELECT `Value` FROM `Property` WHERE `Property` = ''' + $name + '''')
        $view.Execute()
        $record = $view.Fetch()
        if (-not $record) { throw 'The downloaded update has incomplete package information.' }
        $value = $record.StringData(1)
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
        $view.Close()
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($view)
        return $value
    }
    if ((Read-MsiProperty 'ProductName') -cne 'BDE fernweh' -or
        (Read-MsiProperty 'UpgradeCode') -ine '{F598C7E9-ECA2-432F-B0C0-41F9E1201F8F}' -or
        [version](Read-MsiProperty 'ProductVersion') -ne [version]$ExpectedVersion) {
        throw 'The downloaded package is not the requested BDE fernweh update.'
    }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine)
    $database = $null
    $engine = $null
    if ($ValidateOnly) { exit 0 }
    # ALPHA releases may be unsigned. Trust comes from the pinned broker HTTPS
    # connection, the exact SHA-256/size and the package identity above. Standard
    # Windows elevation remains in place; no silent privilege bypass is used.
    $arguments = '/i "' + $Installer + '" /passive /norestart REBOOT=ReallySuppress'
    $process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    if ($process.ExitCode -notin @(0, 1641, 3010)) {
        if ($process.ExitCode -eq 1602) { throw 'The update was cancelled. Your current installation and settings are still available.' }
        throw "Windows Installer could not finish the update (code $($process.ExitCode)). Please try again."
    }
} catch {
    if ($ValidateOnly) { Write-Output $_.Exception.Message; exit 1 }
    Add-Type -AssemblyName System.Windows.Forms
    [void][System.Windows.Forms.MessageBox]::Show(
        $_.Exception.Message, 'BDE fernweh update', 'OK', 'Error')
} finally {
    if ($database) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($database) }
    if ($engine) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($engine) }
    if (-not $ValidateOnly) {
    # Only task-owned files are removed; settings and credentials are untouched.
    Remove-Item -LiteralPath $Installer -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $PSCommandPath -ErrorAction SilentlyContinue
    $directory = Split-Path -Parent $Installer
    if (-not (Get-ChildItem -LiteralPath $directory -Force)) {
        Remove-Item -LiteralPath $directory -ErrorAction SilentlyContinue
    }
    if (Test-Path -LiteralPath $Executable) { Start-Process -FilePath $Executable }
    }
}
