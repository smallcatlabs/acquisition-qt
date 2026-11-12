$ErrorActionPreference = 'Stop'

$inputDirectory = 'C:\acq-test\input'
$resultsDirectory = 'C:\acq-test\results'
$installDirectory = 'C:\AcquisitionTest'
$dataDirectory = 'C:\AcquisitionTestData'
$setupLog = Join-Path $resultsDirectory 'setup.log'
$uninstallLog = Join-Path $resultsDirectory 'uninstall.log'
$reportPath = Join-Path $resultsDirectory 'report.json'
$transcriptPath = Join-Path $resultsDirectory 'harness.log'
$testConfiguration = Get-Content `
    -Raw -LiteralPath (Join-Path $inputDirectory 'test-config.json') |
    ConvertFrom-Json
$expectedOutcome = $testConfiguration.expected_outcome

$report = [ordered]@{
    started_utc = (Get-Date).ToUniversalTime().ToString('o')
    completed_utc = $null
    passed = $false
    phase = 'initializing'
    installer = $null
    initial_runtime = $null
    final_runtime = $null
    installer_exit_code = $null
    expected_outcome = $expectedOutcome
    test_install_elevated = $expectedOutcome -ne 'skip'
    app_started = $false
    app_stayed_running = $false
    uninstaller_exit_code = $null
    stale_runtime_files = @()
    leftover_install_files = @()
    error = $null
}

function Get-VCRuntimeState {
    $registryPaths = @(
        'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64',
        'HKLM:\SOFTWARE\WOW6432Node\Microsoft\VisualStudio\14.0\VC\Runtimes\x64'
    )

    foreach ($registryPath in $registryPaths) {
        if (Test-Path -LiteralPath $registryPath) {
            $runtime = Get-ItemProperty -LiteralPath $registryPath
            return [ordered]@{
                registry_path = $registryPath
                installed = $runtime.Installed
                version = $runtime.Version
                major = $runtime.Major
                minor = $runtime.Minor
                build = $runtime.Bld
                revision = $runtime.Rbld
            }
        }
    }

    return $null
}

function Save-Report {
    $report.completed_utc = (Get-Date).ToUniversalTime().ToString('o')
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $reportPath -Encoding UTF8
}

New-Item -ItemType Directory -Force -Path $resultsDirectory | Out-Null
Start-Transcript -LiteralPath $transcriptPath -Force

try {
    $report.initial_runtime = Get-VCRuntimeState

    $report.phase = 'locating installer'
    $installers = @(Get-ChildItem -LiteralPath $inputDirectory -Filter 'acquisition_setup_*.exe')
    if ($installers.Count -ne 1) {
        throw "Expected exactly one Acquisition installer, found $($installers.Count)."
    }
    $installer = $installers[0]
    $report.installer = $installer.Name

    if ($expectedOutcome -eq 'skip') {
        $report.phase = 'seeding registered runtime'
        $seedSetupLog = Join-Path $resultsDirectory 'seed-setup.log'
        $seedUninstallLog = Join-Path $resultsDirectory 'seed-uninstall.log'
        $seedArguments =
            '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER ' +
            "/DIR=`"$installDirectory`" /LOG=`"$seedSetupLog`""
        $seedProcess = Start-Process -FilePath $installer.FullName `
            -ArgumentList $seedArguments -Verb RunAs -Wait -PassThru
        if ($seedProcess.ExitCode -ne 0) {
            throw "Runtime-seeding install exited with code $($seedProcess.ExitCode)."
        }

        $seedUninstaller = Join-Path $installDirectory 'unins000.exe'
        $seedUninstallArguments =
            "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$seedUninstallLog`""
        $seedUninstallProcess = Start-Process -FilePath $seedUninstaller `
            -ArgumentList $seedUninstallArguments -Wait -PassThru
        if ($seedUninstallProcess.ExitCode -ne 0) {
            throw "Runtime-seeding uninstall exited with code $($seedUninstallProcess.ExitCode)."
        }

        $report.initial_runtime = Get-VCRuntimeState
        if ($null -eq $report.initial_runtime -or
            $report.initial_runtime.installed -ne 1) {
            throw 'The runtime-seeding install did not register the x64 runtime.'
        }
    }

    $report.phase = 'seeding stale runtime files'
    New-Item -ItemType Directory -Force -Path $installDirectory | Out-Null
    @('msvcp140.dll', 'vcruntime140_1.dll', 'concrt140.dll') |
        ForEach-Object {
            New-Item -ItemType File -Force `
                -Path (Join-Path $installDirectory $_) | Out-Null
        }

    $report.phase = 'installing'
    $installerArguments =
        '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /CURRENTUSER ' +
        "/DIR=`"$installDirectory`" /LOG=`"$setupLog`""
    if ($expectedOutcome -eq 'skip') {
        $installerProcess = Start-Process -FilePath $installer.FullName `
            -ArgumentList $installerArguments -Wait -PassThru
    }
    else {
        $installerProcess = Start-Process -FilePath $installer.FullName `
            -ArgumentList $installerArguments -Verb RunAs -Wait -PassThru
    }
    $report.installer_exit_code = $installerProcess.ExitCode
    if ($expectedOutcome -eq 'failure') {
        if ($installerProcess.ExitCode -eq 0) {
            throw 'The synthetic prerequisite failure unexpectedly succeeded.'
        }
        if (Test-Path -LiteralPath (Join-Path $installDirectory 'acquisition.exe')) {
            throw 'Acquisition files were installed after the prerequisite failed.'
        }
        $report.phase = 'complete'
        $report.passed = $true
        return
    }
    elseif ($installerProcess.ExitCode -ne 0) {
        throw "Installer exited with code $($installerProcess.ExitCode)."
    }

    $setupLogText = Get-Content -Raw -LiteralPath $setupLog
    $restartReported = $setupLogText -match 'Need to restart Windows\? Yes'
    if ($expectedOutcome -eq 'restart' -and -not $restartReported) {
        throw 'Setup did not propagate the synthetic restart-required result.'
    }
    if ($expectedOutcome -eq 'success' -and $restartReported) {
        throw 'Setup unexpectedly reported that Windows needs to restart.'
    }
    if ($expectedOutcome -eq 'skip' -and
        $setupLogText -notmatch 'is already installed; bundled version .* is not required') {
        throw 'Setup did not report skipping the equal or newer registered runtime.'
    }
    if ($expectedOutcome -eq 'skip' -and
        $setupLogText -notmatch 'Administrative install mode: No') {
        throw 'The runtime-skip case did not retain per-user install mode.'
    }

    $report.phase = 'verifying installation'
    $applicationPath = Join-Path $installDirectory 'acquisition.exe'
    if (-not (Test-Path -LiteralPath $applicationPath)) {
        throw 'The installer did not create acquisition.exe.'
    }

    $report.stale_runtime_files = @(
        Get-ChildItem -LiteralPath $installDirectory -File |
            Where-Object Name -Match '^(msvcp140.*|vcruntime140.*|concrt140|vc_redist\.x64)\.exe$|^(msvcp140.*|vcruntime140.*|concrt140)\.dll$' |
            ForEach-Object Name
    )
    if ($report.stale_runtime_files.Count -ne 0) {
        throw "Unexpected app-local runtime files: $($report.stale_runtime_files -join ', ')"
    }

    $report.final_runtime = Get-VCRuntimeState
    if ($expectedOutcome -in @('success', 'skip')) {
        if ($null -eq $report.final_runtime -or $report.final_runtime.installed -ne 1) {
            throw 'The x64 Visual C++ Runtime is not registered after installation.'
        }

        $report.phase = 'launching application'
        New-Item -ItemType Directory -Force -Path $dataDirectory | Out-Null
        $application = Start-Process -FilePath $applicationPath `
            -ArgumentList "--data-dir `"$dataDirectory`"" -PassThru
        $report.app_started = $true
        Start-Sleep -Seconds 8
        $application.Refresh()
        $report.app_stayed_running = -not $application.HasExited
        if (-not $report.app_stayed_running) {
            throw "Acquisition exited early with code $($application.ExitCode)."
        }
        Stop-Process -Id $application.Id -Force
        $application.WaitForExit()
    }

    $report.phase = 'uninstalling'
    $uninstallerPath = Join-Path $installDirectory 'unins000.exe'
    if (-not (Test-Path -LiteralPath $uninstallerPath)) {
        throw 'The generated uninstaller was not found.'
    }
    $uninstallerArguments =
        "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=`"$uninstallLog`""
    $uninstallerProcess = Start-Process -FilePath $uninstallerPath `
        -ArgumentList $uninstallerArguments -Wait -PassThru
    $report.uninstaller_exit_code = $uninstallerProcess.ExitCode
    if ($uninstallerProcess.ExitCode -ne 0) {
        throw "Uninstaller exited with code $($uninstallerProcess.ExitCode)."
    }

    Start-Sleep -Seconds 2
    if (Test-Path -LiteralPath $installDirectory) {
        $report.leftover_install_files = @(
            Get-ChildItem -LiteralPath $installDirectory -Recurse -Force |
                ForEach-Object FullName
        )
    }
    if ($report.leftover_install_files.Count -ne 0) {
        throw 'The uninstaller left files in the installation directory.'
    }

    $report.phase = 'complete'
    $report.passed = $true
}
catch {
    $report.error = $_.Exception.ToString()
    Write-Error $_.Exception.ToString()
}
finally {
    Save-Report
    Stop-Transcript
    shutdown.exe /s /t 5
}
