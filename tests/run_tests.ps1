$ErrorActionPreference = 'Stop'
$testRoot = Split-Path -Parent $PSScriptRoot
$testOldPath = $env:PATH
Push-Location $testRoot
try {
    $testGcc = (Get-Command gcc -ErrorAction Stop).Source
    $env:PATH = (Split-Path -Parent $testGcc) + ';' + $env:PATH
    $testOutput = Join-Path $testRoot 'cmake-build-debug/tests'
    New-Item -ItemType Directory -Path $testOutput -Force | Out-Null
    & $testGcc -std=c11 -Wall -Wextra -Werror -ICore/Inc tests/test_bootloader.c Core/Src/bootloader.c -o "$testOutput/test_bootloader.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Bootloader test compilation failed' }
    & "$testOutput/test_bootloader.exe"
    if ($LASTEXITCODE -ne 0) { throw 'Bootloader tests failed' }
    & $testGcc -std=c11 -Wall -Wextra -Werror -ICore/Inc tests/test_w25q16.c Core/Src/w25q16.c -o "$testOutput/test_w25q16.exe"
    if ($LASTEXITCODE -ne 0) { throw 'W25Q16 test compilation failed' }
    & "$testOutput/test_w25q16.exe"
    if ($LASTEXITCODE -ne 0) { throw 'W25Q16 tests failed' }
    & $testGcc -std=c11 -Wall -Wextra -Werror -Itests/fakes -ICore/Inc tests/test_boot_spi.c Core/Src/boot_spi_stm32.c -o "$testOutput/test_boot_spi.exe"
    if ($LASTEXITCODE -ne 0) { throw 'SPI polling test compilation failed' }
    & "$testOutput/test_boot_spi.exe"
    if ($LASTEXITCODE -ne 0) { throw 'SPI polling tests failed' }
    python -B -m unittest discover -s tests -p test_manifest.py -v
    if ($LASTEXITCODE -ne 0) { throw 'Packaging tests failed' }
}
finally {
    $env:PATH = $testOldPath
    Pop-Location
}
