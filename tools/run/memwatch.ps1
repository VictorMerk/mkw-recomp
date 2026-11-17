# Record a Windows process's RAM, commit and GPU-memory counters to correlate multi-race growth with frame logs.
# Run: powershell -NoProfile -File tools/run/memwatch.ps1 -TargetProcessId 1234 -OutPath build/run/memory.tsv
# Read-only observer: stops when that process exits; never changes its settings or terminates it.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$TargetProcessId,
    [string]$OutPath,
    [ValidateRange(0.2,3600)][double]$IntervalSeconds=5,
    [ValidateRange(0,2147483647)][int]$Samples=0,
    [switch]$NoGpuCounters
)
$ErrorActionPreference='Stop'
if($env:OS -ne 'Windows_NT'){throw 'memwatch.ps1 requires Windows; use memwatch.sh on Linux'}
if(-not ('MkwMemwatchNative' -as [type])){
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
public static class MkwMemwatchNative {
    [StructLayout(LayoutKind.Sequential)]
    public struct Status {
        public uint length, load;
        public ulong totalPhysical, availablePhysical, commitLimit, availableCommit;
        public ulong totalVirtual, availableVirtual, availableExtendedVirtual;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref Status status);
    public static Status Read() {
        Status status = new Status();
        status.length = (uint)Marshal.SizeOf(typeof(Status));
        if (!GlobalMemoryStatusEx(ref status)) throw new Win32Exception(Marshal.GetLastWin32Error());
        return status;
    }
}
'@
}
$target=[Diagnostics.Process]::GetProcessById($TargetProcessId)
$started=$target.StartTime.ToUniversalTime()
$processName=$target.ProcessName
if(-not $OutPath){
    $OutPath=Join-Path $PSScriptRoot ("../../build/run/memory-{0}-{1}.tsv" -f $TargetProcessId,(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
$OutPath=[IO.Path]::GetFullPath($OutPath)
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($OutPath)) | Out-Null
# CreateNew prevents an accidental restart from destroying an earlier measurement.
$stream=[IO.File]::Open($OutPath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
$writer=[IO.StreamWriter]::new($stream,[Text.UTF8Encoding]::new($false))
$culture=[Globalization.CultureInfo]::InvariantCulture
$gpuAvailable=-not $NoGpuCounters
$sampleCount=0
try{
    $writer.WriteLine("# memwatch Windows; process=$processName; pid=$TargetProcessId; start_utc=$($started.ToString('o'))")
    $writer.WriteLine("# qpc_frequency_hz=$([Diagnostics.Stopwatch]::Frequency); GPU values are process counters, not a physical-residency measurement")
    $writer.WriteLine("utc`tqpc_ticks`telapsed_seconds`tprivate_bytes`tworking_set_bytes`tvirtual_bytes`tcpu_seconds`thandles`tsystem_available_bytes`tsystem_commit_bytes`tsystem_commit_limit_bytes`tgpu_dedicated_bytes`tgpu_shared_bytes`tgpu_committed_bytes`tsample_ms")
    $writer.Flush()
    while($true){
        $sampleClock=[Diagnostics.Stopwatch]::StartNew()
        try{
            $target.Refresh()
            if($target.HasExited -or $target.StartTime.ToUniversalTime().Ticks -ne $started.Ticks){break}
            $utc=[DateTime]::UtcNow
            $qpc=[Diagnostics.Stopwatch]::GetTimestamp()
            $privateBytes=$target.PrivateMemorySize64
            $workingBytes=$target.WorkingSet64
            $virtualBytes=$target.VirtualMemorySize64
            $cpuSeconds=$target.TotalProcessorTime.TotalSeconds
            $handles=$target.HandleCount
        }catch [InvalidOperationException]{break}catch [ArgumentException]{break}
        $system=[MkwMemwatchNative]::Read()
        $gpuDedicated='';$gpuShared='';$gpuCommitted=''
        if($gpuAvailable){
            try{
                # WMI class/property names are stable across Windows display languages.
                $gpuRows=@(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -ErrorAction Stop |
                    Where-Object { $_.Name -match "^pid_${TargetProcessId}_" })
                [UInt64]$dedicatedTotal=0;[UInt64]$sharedTotal=0;[UInt64]$committedTotal=0
                foreach($gpuRow in $gpuRows){
                    $dedicatedTotal+=[UInt64]$gpuRow.DedicatedUsage
                    $sharedTotal+=[UInt64]$gpuRow.SharedUsage
                    $committedTotal+=[UInt64]$gpuRow.TotalCommitted
                }
                $gpuDedicated=$dedicatedTotal.ToString($culture)
                $gpuShared=$sharedTotal.ToString($culture)
                $gpuCommitted=$committedTotal.ToString($culture)
            }catch{
                # Missing counters remain blank, distinct from a measured zero; avoid retry overhead every sample.
                $gpuAvailable=$false
                $writer.WriteLine('# GPU counters unavailable; GPU columns remain blank')
            }
        }
        $sampleClock.Stop()
        $fields=@(
            $utc.ToString('o'),$qpc.ToString($culture),($utc-$started).TotalSeconds.ToString('F6',$culture),
            $privateBytes.ToString($culture),$workingBytes.ToString($culture),$virtualBytes.ToString($culture),
            $cpuSeconds.ToString('F6',$culture),$handles.ToString($culture),
            $system.availablePhysical.ToString($culture),($system.commitLimit-$system.availableCommit).ToString($culture),
            $system.commitLimit.ToString($culture),$gpuDedicated,$gpuShared,$gpuCommitted,
            $sampleClock.Elapsed.TotalMilliseconds.ToString('F3',$culture)
        )
        $writer.WriteLine(($fields -join "`t"))
        $writer.Flush()
        $sampleCount++
        if($Samples -gt 0 -and $sampleCount -ge $Samples){break}
        if($target.WaitForExit([int]($IntervalSeconds*1000))){break}
    }
}finally{
    $writer.Dispose()
    $target.Dispose()
}
