param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
# Some launchers provide both PATH and Path; MSBuild's .NET Framework process
# launcher rejects those duplicate keys. Normalize only the child environment.
function Invoke-BuildTool([string]$Tool, [string[]]$ToolArguments) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = (Get-Command $Tool -ErrorAction Stop).Source
    $start.UseShellExecute = $false
    $start.WorkingDirectory = (Get-Location).Path
    $start.Environment.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $start.Environment[$entry.Key.ToUpperInvariant()] = $entry.Value
    }
    foreach ($argument in $ToolArguments) { $start.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($start)
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "$Tool failed with exit code $($process.ExitCode)." }
    $process.Dispose()
}
Push-Location $PSScriptRoot
try {
    Invoke-BuildTool cmake @('-S', '.', '-B', 'build', '-G', 'Visual Studio 17 2022', '-A', 'x64')
    Invoke-BuildTool cmake @('--build', 'build', '--config', $Configuration, '--parallel', '1')
    Invoke-BuildTool ctest @('--test-dir', 'build', '-C', $Configuration, '--output-on-failure')
    Write-Host "Built: $PSScriptRoot\bin\$Configuration\AutoMortar.exe"
} finally { Pop-Location }
