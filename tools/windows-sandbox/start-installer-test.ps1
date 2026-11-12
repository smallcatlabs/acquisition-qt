param(
    [Parameter(Mandatory = $true)]
    [string] $InstallerPath,

    [ValidateSet('success', 'skip', 'restart', 'failure')]
    [string] $ExpectedOutcome = 'success'
)

$ErrorActionPreference = 'Stop'

$repositoryRoot = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$installer = Get-Item -LiteralPath $InstallerPath
$buildDirectory = Join-Path $repositoryRoot 'build'
$inputDirectory = Join-Path $buildDirectory 'sandbox-input'
$resultsDirectory = Join-Path $buildDirectory 'sandbox-results'
$configurationPath = Join-Path $buildDirectory 'acquisition-installer-test.wsb'

New-Item -ItemType Directory -Force -Path $inputDirectory, $resultsDirectory | Out-Null
Get-ChildItem -LiteralPath $inputDirectory -Force | Remove-Item -Recurse -Force
Get-ChildItem -LiteralPath $resultsDirectory -Force | Remove-Item -Recurse -Force
Copy-Item -LiteralPath $installer.FullName -Destination $inputDirectory
@{ expected_outcome = $ExpectedOutcome } |
    ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $inputDirectory 'test-config.json') -Encoding UTF8

function ConvertTo-XmlText([string] $Value) {
    return [System.Security.SecurityElement]::Escape($Value)
}

$inputXml = ConvertTo-XmlText $inputDirectory
$harnessXml = ConvertTo-XmlText $PSScriptRoot
$resultsXml = ConvertTo-XmlText $resultsDirectory

$configuration = @"
<Configuration>
  <VGpu>Disable</VGpu>
  <Networking>Disable</Networking>
  <AudioInput>Disable</AudioInput>
  <VideoInput>Disable</VideoInput>
  <PrinterRedirection>Disable</PrinterRedirection>
  <ClipboardRedirection>Disable</ClipboardRedirection>
  <ProtectedClient>Enable</ProtectedClient>
  <MemoryInMB>4096</MemoryInMB>
  <MappedFolders>
    <MappedFolder>
      <HostFolder>$inputXml</HostFolder>
      <SandboxFolder>C:\acq-test\input</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$harnessXml</HostFolder>
      <SandboxFolder>C:\acq-test\harness</SandboxFolder>
      <ReadOnly>true</ReadOnly>
    </MappedFolder>
    <MappedFolder>
      <HostFolder>$resultsXml</HostFolder>
      <SandboxFolder>C:\acq-test\results</SandboxFolder>
      <ReadOnly>false</ReadOnly>
    </MappedFolder>
  </MappedFolders>
  <LogonCommand>
    <Command>powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File C:\acq-test\harness\run-installer-test.ps1</Command>
  </LogonCommand>
</Configuration>
"@

Set-Content -LiteralPath $configurationPath -Value $configuration -Encoding UTF8
Start-Process -FilePath "$env:SystemRoot\System32\WindowsSandbox.exe" `
    -ArgumentList ('"' + $configurationPath + '"')

Write-Host "Windows Sandbox started. Results will appear in $resultsDirectory"
