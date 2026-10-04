$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
$output = Join-Path $repo 'build/host-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compiler = (Get-Command clang -ErrorAction Stop).Source
$includes = @('-I', "$repo/src", '-I', "$repo/src/driver/lcd", '-I', "$repo/src/third_party/hagl/include", '-I', "$PSScriptRoot/mocks")

function Run-Test($name, $sources, $extra = @()) {
    $exe = Join-Path $output "$name.exe"
    & $compiler '-std=c11' '-O1' '-Wall' '-Wextra' '-Werror' @includes @extra @sources '-o' $exe
    if ($LASTEXITCODE -ne 0) { throw "$name 编译失败" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "$name 测试失败" }
}

$mathSources = @("$PSScriptRoot/test_vec_math.c", "$repo/src/lib/tools/vec_math.c")
Run-Test 'vec_math' $mathSources
$cmsis = "$repo/src/third_party/CMSIS-DSP"
$cmsisSources = $mathSources + @(
    "$cmsis/Source/BasicMathFunctions/arm_add_f32.c",
    "$cmsis/Source/BasicMathFunctions/arm_sub_f32.c",
    "$cmsis/Source/BasicMathFunctions/arm_scale_f32.c",
    "$cmsis/Source/BasicMathFunctions/arm_dot_prod_f32.c",
    "$cmsis/Source/FastMathFunctions/arm_sin_f32.c",
    "$cmsis/Source/FastMathFunctions/arm_cos_f32.c",
    "$cmsis/Source/CommonTables/arm_common_tables.c"
)
Run-Test 'vec_math_cmsis' $cmsisSources @('-DUSE_CMSIS_DSP', '-DDISABLEFLOAT16', '-I', "$cmsis/Include", '-I', "$cmsis/PrivateInclude")
Run-Test 'lcd_hal' @("$PSScriptRoot/test_lcd_hal.c")
foreach ($name in @('pwm', 'adc', 'spi')) {
    $test = "$PSScriptRoot/test_$name.c"
    Run-Test $name @($test, "$PSScriptRoot/mocks/mock_sdk.c")
}
Write-Host '全部固件主机回归测试通过。'
