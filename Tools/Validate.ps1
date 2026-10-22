param([switch]$CoreOnly, [switch]$GPU, [string]$WorldDirectory = "", [string]$BuildDirectory = "Build")
$ErrorActionPreference = 'Stop'
$engineRoot = Split-Path $PSScriptRoot -Parent
Push-Location $engineRoot
try {
    New-Item -ItemType Directory -Force Evidence | Out-Null
    $configureArguments = @('-S', '.', '-B', $BuildDirectory, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Debug', '-DBUILD_TESTING=ON')
    $configureArguments += "-DBELLWETHER_BUILD_GRAPHICS=$(if ($CoreOnly) {'OFF'} else {'ON'})"
    $configureArguments += "-DBELLWETHER_GPU_TESTS=$(if ($GPU -and !$CoreOnly) {'ON'} else {'OFF'})"
    if (!$CoreOnly -and (Get-Command py -ErrorAction SilentlyContinue)) {
        $enginePython = & py -3 -c 'import sys; print(sys.executable)'
        if ($LASTEXITCODE -eq 0) { $configureArguments += "-DPython3_EXECUTABLE=$enginePython" }
    }
    & cmake @configureArguments *> Evidence/Configure.log
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed; see Evidence/Configure.log' }
    & cmake --build $BuildDirectory --parallel 6 *> Evidence/Build.log
    if ($LASTEXITCODE -ne 0) { throw 'Build failed; see Evidence/Build.log' }
    & ctest --test-dir $BuildDirectory --output-on-failure *> Evidence/Tests.log
    if ($LASTEXITCODE -ne 0) { throw 'CTest failed; see Evidence/Tests.log' }
    if ($GPU -and $WorldDirectory) {
        & (Join-Path $BuildDirectory 'BellwetherRuntime.exe') --gpu-test --readback Evidence/Readback.ppm --world $WorldDirectory --capture Evidence/World.ppm *> Evidence/GPU.log
        if ($LASTEXITCODE -ne 0) { throw 'World rendering regression failed; see Evidence/GPU.log' }
    }
    Get-Content Evidence/Tests.log
} finally { Pop-Location }
