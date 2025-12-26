param(
    [string]$LogFile = "ci-resource-usage.log",
    [int]$IntervalSeconds = 30
)

function Write-Log {
    param(
        [Parameter(ValueFromPipeline = $true)]
        [string]$Message
    )
    process {
        Add-Content -Path $LogFile -Value $Message
    }
}

"Resource monitor started for Windows, interval ${IntervalSeconds}s, writing to ${LogFile}" | Write-Log

while ($true) {
    $timestamp = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")

    "[$timestamp] ===== Disk =====" | Write-Log
    try {
        $disk = Get-Volume | Select-Object DriveLetter, FileSystem, @{Name="FreeGB";Expression={[math]::Round($_.SizeRemaining/1GB,2)}}, @{Name="SizeGB";Expression={[math]::Round($_.Size/1GB,2)}} | Format-Table -AutoSize | Out-String
        $disk | Write-Log
    } catch {
        "Disk query failed: $($_.Exception.Message)" | Write-Log
    }

    "[$timestamp] ===== Memory =====" | Write-Log
    try {
        $mem = Get-CimInstance Win32_OperatingSystem | Select-Object @{Name="TotalGB";Expression={[math]::Round($_.TotalVisibleMemorySize/1MB,2)}}, @{Name="FreeGB";Expression={[math]::Round($_.FreePhysicalMemory/1MB,2)}} | Format-Table -AutoSize | Out-String
        $mem | Write-Log
    } catch {
        "Memory query failed: $($_.Exception.Message)" | Write-Log
    }

    "" | Write-Log
    Start-Sleep -Seconds $IntervalSeconds
}
