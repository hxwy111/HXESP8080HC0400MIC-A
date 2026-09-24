param(
    [Parameter(Mandatory = $true)]
    [string]$Port,

    [ValidateRange(115200, 921600)]
    [int]$Baud = 460800,

    [switch]$EraseFlash
)

$ErrorActionPreference = "Stop"
$firmware = Join-Path $PSScriptRoot "esp32c6_hosted_2.11.6_full.bin"

if (-not (Test-Path -LiteralPath $firmware)) {
    throw "Firmware file not found: $firmware"
}

$idfPython = Get-ChildItem -Path "C:\Espressif\python_env\idf*_env\Scripts\python.exe" `
    -File -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending |
    Select-Object -First 1

if ($null -eq $idfPython) {
    throw "ESP-IDF Python was not found under C:\Espressif\python_env. Install ESP-IDF or run esptool manually as described in README_CN.md."
}

$esptoolPrefix = @("-m", "esptool")
$baseConnectionArgs = @(
    "--chip", "esp32c6",
    "--port", $Port,
    "--baud", $Baud,
    "--before", "no_reset"
)
$resetAfterArgs = $baseConnectionArgs + @("--after", "watchdog_reset")
$stayInBootloaderArgs = $baseConnectionArgs + @("--after", "no_reset")

Write-Host "Checking ESP32-C6 ROM bootloader on $Port ..."
& $idfPython.FullName @esptoolPrefix @resetAfterArgs chip_id
if ($LASTEXITCODE -ne 0) {
    throw "Cannot communicate with ESP32-C6. Check TX/RX/GND and keep C6_IO9 connected to GND while powering the board."
}

if ($EraseFlash) {
    Write-Host "Erasing the ESP32-C6 flash ..."
    & $idfPython.FullName @esptoolPrefix @resetAfterArgs erase_flash
    if ($LASTEXITCODE -ne 0) {
        throw "ESP32-C6 flash erase failed."
    }
}

Write-Host "Writing ESP-Hosted 2.11.6 to ESP32-C6 ..."
& $idfPython.FullName @esptoolPrefix @resetAfterArgs write_flash `
    --flash_mode dio --flash_freq 80m --flash_size 4MB `
    0x0 $firmware
if ($LASTEXITCODE -ne 0) {
    throw "ESP32-C6 firmware write failed."
}

Write-Host "Verifying ESP32-C6 flash ..."
& $idfPython.FullName @esptoolPrefix @stayInBootloaderArgs verify_flash 0x0 $firmware
if ($LASTEXITCODE -ne 0) {
    throw "ESP32-C6 firmware verification failed."
}

Write-Host "Completed. Disconnect board power, remove C6_IO9 from GND, and power the board again."
