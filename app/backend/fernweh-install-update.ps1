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
$installerLog = $null
try {
    # Qt supplies forward-slash paths. MSI database inspection accepts them,
    # but msiexec installation returns 1619 unless separators are native.
    $Installer = [IO.Path]::GetFullPath($Installer.Replace('/', '\'))
    # The client only starts this helper once all streams have ended. Wait for
    # its normal process shutdown before asking Windows Installer to replace it.
    if (-not $ValidateOnly) {
        $parent = Get-Process -Id $ParentProcessId -ErrorAction SilentlyContinue
        if ($parent) { $parent.WaitForExit() }
    }
    if ((Get-Item -LiteralPath $Installer).Length -ne $ExpectedSize -or
        (Get-FileHash -LiteralPath $Installer -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ExpectedHash) {
        throw 'The downloaded update failed its integrity check. Please check for updates again.'
    }
    $engine = New-Object -ComObject WindowsInstaller.Installer
    $database = $engine.OpenDatabase($Installer, 0)
    function Read-MsiProperty([string]$name) {
        $view = $database.OpenView('SELECT `Value` FROM `Property` WHERE `Property` = ''' + $name + '''')
        [void]$view.Execute()
        $record = $view.Fetch()
        if (-not $record) { throw 'The downloaded update has incomplete package information.' }
        $value = $record.StringData(1)
        [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($record)
        [void]$view.Close()
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
    # QTemporaryDir deliberately creates an owner-only Windows DACL. MSI runs
    # in the SYSTEM service, and UAC can use a different administrator account.
    # Both need to traverse the directory and read the MSI, even though our
    # current-user integrity and database checks above succeeded. Grant only
    # read/execute to these well-known SIDs; never grant Everyone or write access.
    foreach ($path in @((Split-Path -Parent $Installer), $Installer)) {
        # Windows PowerShell's Set-Acl writes the entire security descriptor.
        # Its non-elevated fallback translates owner/group SIDs into account
        # names and can fail before UAC, including with an unmapped group SID.
        # Read and persist only the DACL through .NET: ownership, group and
        # auditing remain untouched and no account-name lookup is needed.
        $directory = [IO.Directory]::Exists($path)
        $section = [Security.AccessControl.AccessControlSections]::Access
        if ($directory) {
            $acl = [IO.Directory]::GetAccessControl($path, $section)
        } else {
            $acl = [IO.File]::GetAccessControl($path, $section)
        }
        foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
            $identity = [Security.Principal.SecurityIdentifier]::new($sid)
            $rule = [Security.AccessControl.FileSystemAccessRule]::new(
                $identity, [Security.AccessControl.FileSystemRights]::ReadAndExecute,
                [Security.AccessControl.AccessControlType]::Allow)
            [void]$acl.AddAccessRule($rule)
        }
        if ($directory) {
            [IO.Directory]::SetAccessControl($path, $acl)
        } else {
            [IO.File]::SetAccessControl($path, $acl)
        }
    }
    $logDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'BDE fernweh\updater'
    [void][IO.Directory]::CreateDirectory($logDirectory)
    $installerLog = Join-Path $logDirectory ('install-' + [guid]::NewGuid().ToString('N') + '.log')
    # ALPHA releases may be unsigned. Trust comes from the pinned broker HTTPS
    # connection, the exact SHA-256/size and the package identity above. Standard
    # Windows elevation remains in place; no silent privilege bypass is used.
    $arguments = '/i "' + $Installer + '" /passive /norestart REBOOT=ReallySuppress /L*V "' + $installerLog + '"'
    $process = Start-Process -FilePath "$env:SystemRoot\System32\msiexec.exe" -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    if ($process.ExitCode -notin @(0, 1641, 3010)) {
        if ($process.ExitCode -eq 1602) { throw 'The update was cancelled. Your current installation and settings are still available.' }
        throw "Windows Installer could not finish the update (code $($process.ExitCode)). Please try again."
    }
} catch {
    if ($ValidateOnly) { Write-Output $_.Exception.Message; exit 1 }
    # Pre-elevation failures have no Windows Installer log yet. Keep their
    # exception and stage location locally so a dialog is not the only evidence.
    $failure = $_
    $failureLog = $null
    try {
        $failureDirectory = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'BDE fernweh\updater'
        [void][IO.Directory]::CreateDirectory($failureDirectory)
        $failureLog = Join-Path $failureDirectory ('helper-' + [guid]::NewGuid().ToString('N') + '.log')
        [IO.File]::WriteAllText($failureLog,
            $failure.Exception.ToString() + "`r`n" + $failure.ScriptStackTrace,
            [Text.UTF8Encoding]::new($false))
    } catch { $failureLog = $null }
    Add-Type -AssemblyName System.Windows.Forms
    $message = $failure.Exception.Message
    if ($failureLog) { $message += "`r`n`r`nUpdater log: $failureLog" }
    if ($installerLog -and (Test-Path -LiteralPath $installerLog)) {
        $message += "`r`n`r`nInstaller log: $installerLog"
    }
    [void][System.Windows.Forms.MessageBox]::Show(
        $message, 'BDE fernweh update', 'OK', 'Error')
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
