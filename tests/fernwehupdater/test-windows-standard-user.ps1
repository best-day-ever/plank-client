# Run under a real non-elevated Windows PowerShell 5.1 token. The package gate
# supplies a Qt fixture, preserving QTemporaryDir's actual owner-only DACL.
param(
    [Parameter(Mandatory=$true)][string]$Package,
    [Parameter(Mandatory=$true)][string]$QtFixture,
    [Parameter(Mandatory=$true)][string]$Helper,
    [Parameter(Mandatory=$true)][string]$ResultFile
)
$ErrorActionPreference = 'Stop'
$result = [ordered]@{ passed = $false; elevated = $true; cases = @() }
$stagedPackage = $null
try {
    $principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    $result.elevated = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if ($result.elevated) { throw 'An elevated token cannot qualify the standard-user updater path.' }
    if ($PSVersionTable.PSVersion.Major -ne 5) { throw 'Exercise the Windows PowerShell version launched by the real updater.' }
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

    foreach ($unmappedGroup in @($false, $true)) {
        $stagedPackage = & $QtFixture $Package
        if ($LASTEXITCODE -or -not (Test-Path -LiteralPath $stagedPackage)) { throw 'Qt download fixture failed.' }
        $paths = @((Split-Path -Parent $stagedPackage), $stagedPackage)
        $identities = @{}
        foreach ($path in $paths) {
            $acl = Get-Acl -LiteralPath $path
            if ($unmappedGroup) {
                # Windows permits this raw group SID without an account-name
                # mapping. Never create a test account or change a real profile.
                $acl.SetGroup([Security.Principal.SecurityIdentifier]::new('S-1-5-1234-5678'))
                if ([IO.Directory]::Exists($path)) { [IO.Directory]::SetAccessControl($path, $acl) }
                else { [IO.File]::SetAccessControl($path, $acl) }
                $acl = Get-Acl -LiteralPath $path
                $legacyFailed = $false
                try { Set-Acl -LiteralPath $path -AclObject $acl }
                catch [Security.Principal.IdentityNotMappedException] { $legacyFailed = $true }
                if (-not $legacyFailed) { throw 'The unmapped-SID fixture did not reproduce the previous Set-Acl failure.' }
            }
            $identities[$path] = @(
                $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value,
                $acl.GetGroup([Security.Principal.SecurityIdentifier]).Value,
                $acl.AreAccessRulesProtected
            )
        }
        $helperCopy = Join-Path (Split-Path -Parent $stagedPackage) 'install-update.ps1'
        Copy-Item -LiteralPath $Helper -Destination $helperCopy
        $boundary = @{ reached = 0 }
        function Start-Process {
            param($FilePath, $ArgumentList, $Verb, [switch]$Wait, [switch]$PassThru)
            if ($FilePath -ne "$env:SystemRoot\System32\msiexec.exe" -or
                $Verb -ne 'RunAs' -or -not $Wait -or -not $PassThru) {
                throw 'Unexpected installer elevation contract.'
            }
            foreach ($path in $paths) {
                $acl = Get-Acl -LiteralPath $path
                if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -cne $identities[$path][0] -or
                    $acl.GetGroup([Security.Principal.SecurityIdentifier]).Value -cne $identities[$path][1] -or
                    $acl.AreAccessRulesProtected -ne $identities[$path][2]) {
                    throw 'Updating service access changed ownership, group or inheritance protection.'
                }
                $rules = $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])
                foreach ($sid in @('S-1-5-18', 'S-1-5-32-544')) {
                    $grants = @($rules | Where-Object { $_.IdentityReference.Value -eq $sid -and $_.AccessControlType -eq 'Allow' })
                    if (-not $grants.Count) { throw 'Missing installer-service access.' }
                    foreach ($grant in $grants) {
                        if (($grant.FileSystemRights -band [Security.AccessControl.FileSystemRights]::ReadAndExecute) -ne
                            [Security.AccessControl.FileSystemRights]::ReadAndExecute -or
                            ($grant.FileSystemRights -band [Security.AccessControl.FileSystemRights]::Write) -ne 0) {
                            throw 'Service access must grant read/execute without write.'
                        }
                    }
                }
                $unexpected = @($rules | Where-Object {
                    $_.IdentityReference.Value -notin @($identities[$path][0], 'S-1-5-18', 'S-1-5-32-544')
                })
                if ($unexpected.Count) { throw 'The update broadened access to an unrelated identity.' }
            }
            $boundary.reached++
            # Only process launch is replaced. The separate MSI service gate
            # exercises real elevated extraction; no test installs on artists.
            return [pscustomobject]@{ ExitCode = 0 }
        }
        & $helperCopy -Installer $stagedPackage.Replace('\', '/') `
            -ExpectedHash (Get-FileHash -LiteralPath $stagedPackage -Algorithm SHA256).Hash.ToLowerInvariant() `
            -ExpectedSize (Get-Item -LiteralPath $stagedPackage).Length -ExpectedVersion $version `
            -ParentProcessId 2147483647 -Executable (Join-Path (Split-Path -Parent $stagedPackage) 'absent.exe')
        if ($boundary.reached -ne 1) { throw 'The production helper did not reach Windows Installer.' }
        if ((Test-Path -LiteralPath $stagedPackage) -or (Test-Path -LiteralPath $helperCopy)) {
            throw 'The production helper failed to clean up its temporary files.'
        }
        $result.cases += @{ unmapped_group = $unmappedGroup; installer_boundary_reached = $true; owner_group_preserved = $true }
        $stagedPackage = $null
    }
    $result.passed = $true
} catch {
    $result.error = $_.Exception.Message
    $result.error_type = $_.Exception.GetType().FullName
} finally {
    if ($stagedPackage) { Remove-Item -LiteralPath (Split-Path -Parent $stagedPackage) -Recurse -ErrorAction SilentlyContinue }
    $temporaryResult = $ResultFile + '.tmp'
    $result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $temporaryResult
    Move-Item -LiteralPath $temporaryResult -Destination $ResultFile
}
if (-not $result.passed) { exit 1 }
