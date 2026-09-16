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

# Add executable. Default name is the project name, version 0.1

add_executable(CNC_Power_Supply
        main.c
        src/app/freertos_hooks.c
        )

pico_set_program_name(CNC_Power_Supply "CNC_Power_Supply")
pico_set_program_version(CNC_Power_Supply "0.1")

# Generate PIO header
pico_generate_pio_header(CNC_Power_Supply ${CMAKE_CURRENT_LIST_DIR}/src/pio/blink.pio)

# Modify the below lines to enable/disable output over UART/USB
pico_enable_stdio_uart(CNC_Power_Supply 0)
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

