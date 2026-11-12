# Windows installer sandbox test

This harness runs an Acquisition installer in a disposable Windows Sandbox. It
records the initial and final Visual C++ Runtime state, exercises stale
app-local runtime cleanup, launches Acquisition with isolated data, uninstalls
it, and writes logs plus `report.json` to `build/sandbox-results/`.

The production installer remains a per-user installer. It skips the
redistributable without elevation when the registered x64 runtime is equal to
or newer than the bundled version. If the runtime is missing or older, Windows
requests administrator approval for that machine-wide prerequisite; Acquisition
then continues with the selected per-user installation.

To verify the unelevated skip path, use the `skip` outcome. The harness first
performs an elevated install to register the bundled runtime, removes
Acquisition, and then runs the installer without `RunAs`. The second install
must skip the prerequisite and complete per-user without elevation:

```powershell
tools\windows-sandbox\start-installer-test.ps1 `
  -InstallerPath Output\acquisition_setup_0.18.4.exe `
  -ExpectedOutcome skip
```

Windows Sandbox must be enabled on the host. Build the installer, then run:

```powershell
tools\windows-sandbox\start-installer-test.ps1 `
  -InstallerPath Output\acquisition_setup_0.18.4.exe
```

The launcher generates `build/acquisition-installer-test.wsb` from the current
checkout path, stages exactly the requested installer, disables networking and
host integrations, and launches the sandbox. The harness shuts the sandbox down
after saving its report. All guest state is discarded; only the mapped results
directory persists.

For installers compiled with `VC_REDIST_TEST_EXIT_CODE`, select the matching
expected result:

```powershell
# Synthetic 3010 (restart required)
tools\windows-sandbox\start-installer-test.ps1 `
  -InstallerPath Output\acquisition_setup_0.18.4.exe `
  -ExpectedOutcome restart

# Synthetic prerequisite failure
tools\windows-sandbox\start-installer-test.ps1 `
  -InstallerPath Output\acquisition_setup_0.18.4.exe `
  -ExpectedOutcome failure
```

Compile synthetic installers by passing one of these test-only preprocessor
definitions to Inno Setup:

```powershell
ISCC.exe /DBUILD_DIR=build /DDEPLOY_DIR=build\deploy `
  /DVC_REDIST_TEST_EXIT_CODE=3010 /Obuild\synthetic-restart installer.iss
ISCC.exe /DBUILD_DIR=build /DDEPLOY_DIR=build\deploy `
  /DVC_REDIST_TEST_EXIT_CODE=1603 /Obuild\synthetic-failure installer.iss
```

Use a separate `/O` output directory for each variant. The define is absent
from release packaging, so production installers always execute the embedded
Microsoft redistributable.
