# STM32F405 Bootloader

本工程适用于 STM32F405RGT6（1 MiB Flash）：**APP 下载，Bootloader 安装并启动**。
Bootloader 不包含串口、网络或其他下载协议。

正常启动流程：复位 → 检查升级请求 → 校验并安装暂存固件（如有）→ 校验运行区 → 跳转 APP。
安装使用复位后的 HSI 16 MHz 时钟，不依赖外部晶振，不调用 `SystemClock_Config()`。
启动流程只有 `HAL_Init()` → `Boot_PrepareApplication()` → 跳转 APP。
没有合法 APP 或跳转失败时关闭中断和 SysTick，在 `Error_Handler()` 中等待调试，
不再初始化 GPIO、USART1；可通过调试器查看 `boot_status`。
`HAL_Init()` 保留 1 ms SysTick，为现有 HAL Flash 操作提供超时计时。

## Flash 分区

| 区域 | 扇区 | 起始地址 | 大小 |
| --- | --- | --- | --- |
| Bootloader | 0 | `0x08000000` | 16 KiB |
| 保留 | 1 | `0x08004000` | 16 KiB |
| 已安装 APP 的描述信息 | 2 | `0x08008000` | 16 KiB |
| 升级请求及描述信息 | 3 | `0x0800C000` | 16 KiB |
| 保留 | 4 | `0x08010000` | 64 KiB |
| APP 运行区 | 5–7 | `0x08020000` | 384 KiB |
| 新固件暂存区 | 8–10 | `0x08080000` | 384 KiB |
| 保留 | 11 | `0x080E0000` | 128 KiB |

分区按 STM32F405 的物理扇区边界划分，参考
[ST RM0090](https://www.st.com/resource/en/reference_manual/dm00031020-stm32f405-and-stm32f407-advanced-armbased-32bit-microcontrollers-stmicroelectronics.pdf)。
地址和协议的唯一 C 定义位于 `Core/Inc/boot_protocol.h`，APP 应共享该头文件。
Bootloader 链接脚本限制为 16 KiB（`0x08000000`–`0x08003FFF`），超过会链接失败。
释放出的 sector 1 暂时保留，已安装信息、升级请求、APP 和暂存区的地址均不变。
Flash 擦写采用 32 位编程，要求板卡电源为 2.7–3.6 V。

## APP 工程需要做的修改

1. APP 链接脚本中的 Flash 区域设置为：

   ```ld
   FLASH (rx) : ORIGIN = 0x08020000, LENGTH = 384K
   ```

2. APP 在 `SystemInit()` 中设置 `SCB->VTOR = 0x08020000`。
   对于本仓库同款 STM32Cube `system_stm32f4xx.c`，可在 **APP 工程**添加：

   ```cmake
   add_compile_definitions(USER_VECT_TAB_ADDRESS VECT_TAB_OFFSET=0x00020000U)
   ```

   不要在 APP 的系统初始化里将向量表重新设置为 `0x08000000`。

3. 使用 APP 运行区链接生成的原始 `.bin` 作为下载内容。即使写入暂存区，
   固件也始终按 `0x08020000` 链接，不能按 `0x08080000` 链接。
   APP 自己执行 HAL、时钟和外设初始化。

4. APP 的下载和升级模块按下一节协议操作；不能覆盖 Bootloader、已安装描述信息或保留区。
   STM32F405 是单 Bank Flash，内部擦写会阻塞取指，下载协议需要安排流控/确认，
   避免擦写期间持续发送导致接收溢出。

## APP 下载和提交升级的顺序

1. 擦除 sector 3，撤销之前的升级请求；保留 sector 2 的已安装信息。
2. 擦除 sectors 8–10，将完整新 `.bin` 下载并写入 `0x08080000`。
3. 对暂存 Flash **读回的数据**计算 CRC，并检查长度、向量表与预期值一致。
4. 向 `0x0800C000` 写入描述信息的前 24 字节，读回校验。
5. 最后单独写入 offset 24 的 `commit` 字段，再读回确认；offset 28 保持擦除值。
6. 锁定 Flash，调用 `NVIC_SystemReset()`。

APP 开始下一次下载时必须先执行第 1 步。Bootloader 不会擦除暂存区和请求区，
完成安装后只把请求的 `consumed` 字段写为 0。
APP 不应自行更改 sector 2；它由 Bootloader 在安装完成后更新。

描述信息为 32 字节、小端、无额外文件头：

| 偏移 | 字段 | 内容 |
| --- | --- | --- |
| 0 | `magic` | `0x424C4657` |
| 4 | `format` | `1` |
| 8 | `image_size` | 原始 `.bin` 字节数，8–393216 |
| 12 | `image_crc` | 固件的 CRC-32/ISO-HDLC |
| 16 | `version` | APP 自定义的 32 位版本号 |
| 20 | `header_crc` | 前 20 字节的 CRC-32/ISO-HDLC |
| 24 | `commit` | `0xB007C0DE`，最后写入 |
| 28 | `consumed` | 新请求为 `0xFFFFFFFF`，安装后为 `0` |

CRC 参数：反射多项式 `0xEDB88320`、初值和末尾异或均为 `0xFFFFFFFF`；
与 Python `zlib.crc32()` 一致，`123456789` 的结果为 `0xCBF43926`。
CRC 只覆盖真实固件长度，不包含尾部 4 字节对齐填充。
Bootloader 已接入 STM32F405 硬件 CRC：直接操作 RCC/CRC 寄存器，每次计算先开启 CRC
时钟并复位 CRC，完整 32 位字通过 Cortex-M4 的 `RBIT` 指令反转位序后送入 `CRC->DR`，
读回后反转输出位序并关闭 CRC 时钟；最后不足 4 字节的部分用软件续算，最后取反。
这样保持与既有 CRC-32/ISO-HDLC、Python `zlib.crc32()` 和描述信息格式一致。
不能把 `HAL_CRC_Calculate()` 的默认原始结果直接写进升级描述信息。
CRC 外设由主循环独占，不能在中断中或并发调用 `Boot_Crc32()`。
版本号用于识别请求，不实施版本大小限制，也不实施防降级策略。

## 安装和断电行为

- 暂存固件的描述信息、CRC、初始 MSP、Thumb 入口和入口范围全部合法后才开始安装。
- 先擦除已安装信息，再擦除 APP 运行区，逐字搬运固件。
- 运行区读回校验成功后写入已安装信息，最后写其 `commit`，然后将请求标记为已处理。
- 擦除/搬运/描述信息写入过程中断电，下次复位仍用完整暂存固件重新安装。
- 已安装信息提交后、请求标记完成前断电，下次启动只补写标记，不重复擦写 APP。
- 下载不完整或暂存 CRC 错误时不擦除原 APP；原 APP 校验通过即可继续启动。
- APP 启动前再次校验其描述信息、CRC 和向量表，没有描述信息的裸 APP 不会被启动。
- 跳转前恢复 HSI、复位外设、停止 SysTick、禁用并清除 NVIC 中断，设置 VTOR、MSP 后进入 APP 的 Reset_Handler。

这里的断电恢复以 Bootloader、已提交请求和暂存固件未损坏为前提。
本实现没有旧版 APP 回滚槽、试运行确认或签名认证；CRC 能检测损坏，不能验证发布者身份。
首次装机需通过 SWD 写入有效 APP 和已安装描述信息，或通过 SWD 写入暂存固件和升级请求。
如果当前 APP 与暂存固件都无效，需要通过 SWD 恢复，因为 Bootloader 没有下载服务。

## 生成描述信息及首次装机

```powershell
python -B tools/make_manifest.py app.bin --version 1 --output app.manifest.bin
```

脚本会拒绝错误的固件大小、MSP 或链接地址。输出文件同时适用于已安装信息和新升级请求。

首次装机有两种方式，写入之后复位：

- 直接运行：Bootloader `.elf` 写入其链接地址，APP `.bin` 写到 `0x08020000`，
  `app.manifest.bin` 写到 `0x08008000`，擦除 sector 3 中的旧请求。
- 验证安装流程：Bootloader `.elf` 写入其链接地址，APP `.bin` 写到 `0x08080000`，
  `app.manifest.bin` **最后**写到 `0x0800C000`；复位后由 Bootloader 安装。

装机/升级请按扇区擦除，避免整片擦除损坏其他区域。以上为操作说明，本次开发未执行烧录。

## 编译和测试

```powershell
cmake -S . -B build/bootloader -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/bootloader
```

生成 `build/bootloader/stm32f405_boot_loader.elf`、`.hex`、`.bin`。
`cmake/bootloader_sources.cmake` 显式列出需要编译的启动、Bootloader、HAL 基础计时、
RCC 清理和 Flash 驱动。硬件 CRC 由 `boot_port_stm32.c` 直接访问寄存器，
CubeMX 生成的 `crc.c` / HAL CRC 驱动不参与 CMake 构建，不需要调用 `MX_CRC_Init()`。
GPIO、USART、DMA、堆及系统调用文件也不参与 CMake 构建，
使用默认的空 `HAL_MspInit()`，避免开启不需要的 SYSCFG/PWR 外设时钟。
CubeMX 的 HSI 配置及生成的 GPIO/USART 文件保留供以后实验使用；下次重新生成后，
仍应检查 `main.c` 和链接脚本，确保 Bootloader 的初始化路径及分区限制符合本说明。
原 `flash.cfg` 仍指向 CLion 的 `cmake-build-debug` 产物；若使用它烧录，应先重新配置并编译该目录。

主机测试直接编译生产版 `bootloader.c`，用固定多项式的硬件 CRC 模型与独立的软件 CRC
逐项比较不同长度及非对齐缓冲区；Flash 模型覆盖 CRC、非法入口、损坏固件、
Flash 错误以及每个擦写操作中途断电后的重试，并检查受保护分区没有被写入。

```powershell
# MSYS2 UCRT64 测试编译器需要优先使用自己的 DLL，仅修改当前进程。
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
gcc -std=c11 -Wall -Wextra -Werror -ICore/Inc tests/test_bootloader.c Core/Src/bootloader.c -o build/test_bootloader.exe
./build/test_bootloader.exe
python -B -m unittest discover -s tests -p test_manifest.py -v
```

主机模型不能替代实板验证。实板还需检查 APP 中断/时钟初始化、完整大小固件安装、
真实擦写断电重启和板卡供电条件。
