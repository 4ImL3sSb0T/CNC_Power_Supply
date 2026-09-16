# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.1)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.1)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(CNC_Power_Supply C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# ====================================================================================
# FreeRTOS：官方内核 + 社区支持移植（两个平级子模块）
#
#   src/third_party/FreeRTOS-Kernel                          FreeRTOS/FreeRTOS-Kernel (V11.3.1)
#   src/third_party/FreeRTOS-Kernel-Community-Supported-Ports FreeRTOS/...-Community-Supported-Ports
#
# 不用内核根目录那个通用 CMakeLists.txt：它只认内核自带的 port，需要手工指定
# FREERTOS_PORT，且不认识 Pico SDK 的 multicore / spinlock / clk_sys 依赖。
#
# 也不用社区移植自带的 FreeRTOS_Kernel_import.cmake：它按 FREERTOS_KERNEL_PATH
# 去内核树里找 portable/ThirdParty/Community-Supported-Ports/GCC/...，而我们把
# 社区移植放在平级目录，所以直接 add_subdirectory 那个 RP2350_ARM_NTZ 移植目录
# （社区 README 里说明的替代用法）。它的 CMakeLists.txt 会读我们设好的
# FREERTOS_KERNEL_PATH，不去猜内核位置。
#
# 换目标平台时改最后一行的移植目录：
#   rp2040        -> .../Community-Supported-Ports/GCC/RP2040
#   rp2350-riscv  -> .../Community-Supported-Ports/GCC/RP2350_RISC-V
#   pico2 (默认)   -> .../Community-Supported-Ports/GCC/RP2350_ARM_NTZ
#
# 必须在 pico_sdk_init() 之后（PICO_PLATFORM 此时才确定，移植的 CMakeLists 也
# 需要 _pico_sdk_inclusion_marker 已经存在才会直接展开 library.cmake）。
# ====================================================================================
set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/src/third_party/FreeRTOS-Kernel)
set(FREERTOS_CONFIG_FILE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/src/config)  # FreeRTOSConfig.h 所在目录

add_subdirectory(
        ${CMAKE_CURRENT_LIST_DIR}/src/third_party/FreeRTOS-Kernel-Community-Supported-Ports/GCC/RP2350_ARM_NTZ
        FREERTOS_KERNEL)

# ====================================================================================
# CMSIS-DSP：FIR/IIR/FFT/矩阵/统计等信号处理内核
#
# 用官方自带的 CMake（Source/CMakeLists.txt 会建出 CMSISDSP target），三个输入：
#   CMSISCORE       它按 ${CMSISCORE}/Include 去找 CMSIS Core 头（cmsis_compiler.h 等），
#                   直接复用 Pico SDK 自带的 stub（CMSIS Core 6.1），不再单独拉 CMSIS_5
#   FASTBUILD=ON    改用各功能目录下的聚合源文件（BasicMathFunctions.c 这种，内部
#                   #include 了目录里全部 arm_*_f32.c/_q15.c…），只编译 27 个翻译
#                   单元，而不是 660 个源文件
#   DISABLEFLOAT16  f16 内核在 M33 上本来就不会启用（__ARM_FP16_FORMAT_IEEE 未定义，
#                   见 Include/arm_math_types_f16.h:52-62），关掉纯粹为了少编译
# ====================================================================================
set(CMSISCORE "${PICO_SDK_PATH}/src/rp2_common/cmsis/stub/CMSIS/Core")
if (NOT EXISTS "${CMSISCORE}/Include/cmsis_compiler.h")
    message(FATAL_ERROR "CMSIS Core 头不在 ${CMSISCORE}，SDK 目录结构可能变了")
endif()
set(FASTBUILD ON CACHE BOOL "CMSIS-DSP: 用聚合源文件编译，缩短编译时间")
set(DISABLEFLOAT16 ON CACHE BOOL "CMSIS-DSP: 不编译 f16 内核")
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/src/third_party/CMSIS-DSP/Source CMSIS-DSP)
# --gc-sections 要靠 -ffunction-sections 才能丢掉未调用的函数。pico 把这两个选项挂在
# pico_standard_link 上按 INTERFACE 传播，只会到达链接了它的 target；CMSISDSP 是独立
# 静态库、够不着，必须自己补一份 —— 否则 FASTBUILD 聚合出来的巨型 .o 会整块进固件
# （实测 TransformFunctions.c：带选项 170 个 .text.* 段，不带 0 个）。
target_compile_options(CMSISDSP PRIVATE -ffunction-sections -fdata-sections)

# ====================================================================================
# 应用源码：自动扫描 src/ 下的 .c / .cpp，新增源文件后不需要再改这个文件
#
# CONFIGURE_DEPENDS：构建时重新比对 glob 结果，目录里增删源文件会自动触发 cmake
# 重新配置（Ninja / Makefile 生成器支持良好，本工程用的就是 Ninja）。
#
# 必须排除 src/third_party：那里是 FreeRTOS 内核与社区移植子模块，包含数千个 .c
# 文件，由它们各自的 CMakeLists.txt 负责编译，不能被这里的 glob 吞掉。
#
# src/pio/*.pio 不由这里处理，见下面的 pico_generate_pio_header。
# ====================================================================================
file(GLOB_RECURSE APP_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_LIST_DIR}/src/*.c
        ${CMAKE_CURRENT_LIST_DIR}/src/*.cpp)
list(FILTER APP_SOURCES EXCLUDE REGEX "/third_party/")

# Add executable. Default name is the project name, version 0.1

add_executable(CNC_Power_Supply
        main.c
        ${APP_SOURCES}
        )

pico_set_program_name(CNC_Power_Supply "CNC_Power_Supply")
pico_set_program_version(CNC_Power_Supply "0.1")

# Generate PIO header
pico_generate_pio_header(CNC_Power_Supply ${CMAKE_CURRENT_LIST_DIR}/src/pio/blink.pio)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(CNC_Power_Supply 1)
pico_enable_stdio_usb(CNC_Power_Supply 1)

# Add the standard library to the build
target_link_libraries(CNC_Power_Supply
        pico_stdlib)

# Add the standard include files to the build
target_include_directories(CNC_Power_Supply PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${CMAKE_CURRENT_LIST_DIR}/src
)

# Add any user requested libraries
target_link_libraries(CNC_Power_Supply
        # FreeRTOS-Kernel: 内核 + RP2350 移植（INTERFACE 库，由移植的 library.cmake 提供）
        # FreeRTOS-Kernel-Heap4: heap_4.c（带合并的首次适配分配器），换 heap_1/2/3/5 改这里即可
        # 注：移植还提供 FreeRTOS-Kernel-Static，作用是往编译命令里塞
        #   configSUPPORT_STATIC_ALLOCATION / configKERNEL_PROVIDED_STATIC_MEMORY 两个 -D。
        #   我们改为在 FreeRTOSConfig.h 里统一定义这些宏，就不链它了——配置集中在一处，
        #   也不会和头文件里的 #define 撞成重定义。
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        CMSISDSP
        hardware_spi
        hardware_i2c
        hardware_dma
        hardware_pio
        hardware_interp
        hardware_timer
        hardware_watchdog
        hardware_clocks
        )

pico_add_extra_outputs(CNC_Power_Supply)

