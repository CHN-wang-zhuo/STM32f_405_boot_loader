# STM32F405 Bootloader

本工程用于 STM32F405RGT6（1 MiB 内部 Flash）：**APP 下载到 W25Q16，Bootloader 校验并安装到内部 Flash，再启动 APP**。
Bootloader 没有下载协议，也不使用 DMA/RTOS。

Bootloader 与配套 APP 是两个独立项目。APP 项目见 [STM32f_405_FreeRTOS](https://github.com/CHN-wang-zhuo/STM32f_405_FreeRTOS)。
外部存储格式遵循该 APP 项目中 `docs/w25q16-update-storage.md` 定义的「W25Q16 升级存储规范 v1」。
APP 负责下载固件并提交升级请求，Bootloader 负责校验、安装和启动固件。

## 启动与硬件

复位 → `HAL_Init()` → 初始化轮询 SPI1/检查 JEDEC → 读取升级请求 → 校验候选包 → 安装 → 校验运行区 → 关闭 SPI1 → 跳转 APP。
使用复位后的 HSI 16 MHz，不调用 `SystemClock_Config()`；HAL 的 1 ms SysTick 为 Flash 操作和 SPI 轮询提供超时计时。
无有效候选包或外部设备缺失时，仍可启动已有的合法 APP；没有合法 APP 时停止 SysTick 并等待调试。
调试器可查看 `boot_status` 和 `g_w25q16_jedec_id`。

| 信号 | 引脚/配置 |
| --- | --- |
| SPI1 SCK/MISO/MOSI | PA5/PA6/PA7，AF5 |
| W25Q16 /CS | PA2 (`F_CS`)，低有效，先置高再配置为输出 |
| SPI | Mode 0、8 bit、MSB first、16 MHz / 2 = 8 MHz |
| 传输 | 全双工轮询，始终读取 DR，不使用中断或 DMA |
| 设备识别 | JEDEC manufacturer = `0xEF`，density = `0x15`，例如 `0xEF4015` |

引脚及 SPI 初始化在 `Core/Src/boot_spi_stm32.c` 中直接操作寄存器。
CubeMX 的 GPIO 配置也已同步为 CS 初始高电平；生成的 `spi.c`、`gpio.c`、HAL SPI 和 DMA 驱动不参与 CMake 构建。
W25Q16 的电源应匹配具体型号后缀，标准 SPI 的 /WP、/HOLD 应保持高电平。

## 内部 Flash：Bootloader、APP 与安装描述信息

内部 Flash 仍用于存放和执行 Bootloader、APP，并保存已安装 APP 的描述信息。
W25Q16 只暂存待升级固件：Bootloader 通过 SPI1 读取、校验后，将其写入内部 APP 运行区，再跳转执行。
改用外部暂存后，旧内部升级请求区和固件暂存区已停用，APP 的运行地址和容量保持不变。

| 区域 | 扇区 | 起始地址 | 大小 | 当前用途 |
| --- | --- | --- | --- | --- |
| Bootloader | 0 | `0x08000000` | 16 KiB | 存放并执行启动、校验和安装程序 |
| 保留 | 1 | `0x08004000` | 16 KiB | 未使用 |
| 已安装 APP 的描述信息 | 2 | `0x08008000` | 16 KiB | 保存长度、CRC、版本及提交标记 |
| 保留（旧内部升级请求区） | 3 | `0x0800C000` | 16 KiB | Bootloader 不再使用 |
| 保留 | 4 | `0x08010000` | 64 KiB | 未使用 |
| APP 运行区 | 5–7 | `0x08020000` | 384 KiB | 安装并执行 APP |
| 保留（旧内部固件暂存区） | 8–10 | `0x08080000` | 384 KiB | Bootloader 不再使用 |
| 保留 | 11 | `0x080E0000` | 128 KiB | 未使用 |

Bootloader 链接脚本限制为 `0x08000000`–`0x08003FFF`，超过 16 KiB 会链接失败。
内部 Flash 只允许擦写 sector 2 和 sectors 5–7；不再读写旧内部请求或暂存区。
内部 Flash 使用 32 位编程，供电要求 2.7–3.6 V。

## W25Q16 分区与升级记录

以下是外部芯片的字节偏移，不是 STM32 的内存地址，定义位于 `Core/Inc/boot_update_storage.h`。

| 偏移范围（含首尾） | 用途 |
| --- | --- |
| `0x000000`–`0x000FFF` | 请求扇区，开头 64 字节为升级记录 |
| `0x001000`–`0x00FFFF` | 保留 |
| `0x010000`–`0x06FFFF` | 最多 384 KiB 的 APP 原始 BIN |
| `0x070000`–`0x1FFFFF` | 保留 |

`BootUpdateRecord` 恰好 64 字节、全小端：

| 偏移 | 字段 | 内容 |
| --- | --- | --- |
| 0–31 | `BootImageManifest` | 与既有固件描述格式一致，详见 `boot_protocol.h` |
| 32 | storage_magic | `0x31505557`，ASCII `WUP1` |
| 36 | layout_version | `1` |
| 40 | image_offset | `0x010000` |
| 44 | image_capacity | `0x060000` |
| 48 | apply_mode | `0` 或 `1`，均在下一次复位时安装 |
| 52 | reserved | `0xFFFFFFFF` |
| 56 | metadata_crc | 拼接 bytes 0–23 和 bytes 28–55 的 CRC-32 |
| 60 | reserved_tail | `0xFFFFFFFF` |

前 32 字节字段依次为 magic=`0x424C4657`、format=`1`、image_size、image_crc、version、header_crc、commit=`0xB007C0DE`、consumed。
header_crc 覆盖 bytes 0–19；新请求的 consumed 必须为 `0xFFFFFFFF`，长度为 8–393216 字节。
apply_mode 的区别由 APP 决定何时复位；Bootloader 不主动按它再次复位。

## 安装、消费与断电重试

1. 读取 64 字节记录，检查 magic、格式、长度、两层元数据 CRC、commit、布局、apply_mode、consumed 和保留字段。
2. 如果相同长度/CRC/版本的 APP 已安装且校验正确，只补写消费标记，不重复擦写内部 Flash。
3. 否则，按 256 字节缓冲分块读取整个候选固件；检查初始 MSP、Thumb 入口、入口范围和整包 CRC。
4. 全部校验通过后才解锁内部 Flash，先擦除 sector 2，再擦除 APP sectors 5–7。
5. 再次分块读取外部候选固件，按 32 位写入 APP；最后不足 4 字节时仅在内部 Flash 填充 FF。
6. 校验内部运行区的完整 CRC，写入内部 32 字节已安装描述信息，最后写入其 commit，然后锁定内部 Flash。
7. 只将 W25Q16 偏移 28–31 的 consumed 写为零，并检查读回；不擦除请求扇区，不改动候选固件或其他外部数据。
8. 关闭 SPI1，清理时钟/外设、SysTick/NVIC，设置 VTOR 和 MSP，跳转到 APP Reset_Handler。

v1 的 metadata_crc 包含 consumed，因此清零消费标记后，原 metadata_crc 会自然失效。
这是撤销该待升级请求的单向操作，Bootloader 不尝试改写 CRC。
消费期间断电可能留下部分清零的 consumed，也不会再作为新请求安装；该步骤始终发生在内部 APP 和已安装描述信息提交成功之后。
如果消费命令完全未生效，下一次复位发现内部 APP 已安装，继续尝试补写消费标记。

安装期间断电或 SPI/内部 Flash 操作失败时，外部候选包和未消费请求保持可重试，下一次复位从头安装。
候选包不完整、CRC 错误、元数据损坏或设备缺失时，不擦除仍有效的旧 APP。
内部擦除开始后没有旧 APP 回滚槽；恢复依赖有效外部候选包及 Bootloader 未受损。
如果运行区和候选包都无效，需要通过 SWD 恢复，因为 Bootloader 不提供下载服务。

CRC 始终是 CRC-32/ISO-HDLC，与 Python `zlib.crc32()` 一致；`123456789` 为 `0xCBF43926`。
完整 32 位字通过硬件 CRC 和 RBIT 处理，跨 SPI 块连续累计，最后 0–3 字节软件续算；不将外部固件整体读入 RAM。
CRC 由主循环独占，不在中断中调用。版本号不要求递增；没有签名认证、试运行确认或自动回滚。

## 烧录 Bootloader

使用 ST-Link，通过 SWD 连接并给开发板供电，BOOT0 保持低电平：

| ST-Link 信号 | 开发板连接 |
| --- | --- |
| SWDIO | PA13 |
| SWCLK | PA14 |
| GND | GND |
| VTref | 板上 3.3 V，用于目标电压参考 |
| NRST（可选） | NRST |

在 CLion 中重新加载 CMake，选择 Debug Profile 和 `stm32f405_boot_loader.elf` 对应的 **OpenOCD 下载并运行** 配置，然后点击运行。
运行配置会先编译再烧录；仅点击编译只生成固件文件。
现有 `flash.cfg` 指向本机工程的 ELF 绝对路径，移动工程或在其他电脑上使用时，应同步调整该路径及 OpenOCD 运行配置。

也可以在工程根目录的 PowerShell 中使用相对路径烧录：

```powershell
cmake --build cmake-build-debug
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program {cmake-build-debug/stm32f405_boot_loader.elf} verify reset exit"
```

需要先将 ARM GCC、CMake、Ninja 和 OpenOCD 加入 PATH，并安装 ST-Link 驱动。
OpenOCD 按 ELF 中的地址烧录 Bootloader，随后校验并复位，不执行全片擦除。
烧录文件位于第 0 扇区；复位后，Bootloader 会检查 W25Q16，存在有效升级请求时按安装流程更新 APP。

## APP 配置、首次安装与后续升级

APP 保持原有链接配置：

```ld
FLASH (rx) : ORIGIN = 0x08020000, LENGTH = 384K
```

APP 的 `SystemInit()` 应将 VTOR 设置为 `0x08020000`。
同款 Cube 系统文件可以在 APP 构建时定义 `USER_VECT_TAB_ADDRESS` 和 `VECT_TAB_OFFSET=0x00020000U`。
APP 自行初始化时钟和外设，下载并提交外部请求的行为保持原协议。

空白芯片首次使用时，推荐生成包含 Bootloader、APP 和已安装描述信息的工厂组合包。
已有 Bootloader 时，可以单独生成 APP 安装包。将为 `0x08020000` 链接的 APP 原始 BIN 准备为 `app.bin`，在本工程根目录执行：

```powershell
python -B tools/make_manifest.py app.bin --version 1 --output app.manifest.bin --installed-hex APP.installed.hex
python -B tools/make_manifest.py app.bin --version 1 --output app.manifest.bin --bootloader cmake-build-debug/stm32f405_boot_loader.bin --factory-hex factory.hex
```

选择需要的 HEX 包，通过 SWD 烧录（以下两个命令按需选择一个）：

```powershell
# 安装或替换 APP，保留已有 Bootloader
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program {APP.installed.hex} verify reset exit"
# 首次烧录 Bootloader 与 APP 组合包
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg -c "program {factory.hex} verify reset exit"
```

直接烧录 APP 时，将 APP BIN 写入 `0x08020000`，32 字节已安装描述信息写入 `0x08008000`。
工具仍会清除旧内部请求的消费标记，以兼容旧版部署流程；**它不能清除 W25Q16 中的待升级请求**。
如果外部已有待升级包，应先通过现有 APP 的 ABORT 取消，或使用外部 Flash 编程器擦除请求扇区，避免复位后安装另一个候选包。

仅烧录 Bootloader、内部没有有效 APP 且 W25Q16 没有有效候选包时，程序会进入 `Error_Handler()` 等待调试。
Bootloader 不提供下载服务；通过 APP 下载固件的前提是已有可运行的 APP。
后续升级流程为：由 [APP 项目](https://github.com/CHN-wang-zhuo/STM32f_405_FreeRTOS) 下载新固件到 W25Q16 的 `0x010000`，
完成校验后在偏移 `0x000000` 提交有效升级记录，再复位，由 Bootloader 安装并启动 APP。
空白芯片也可以使用外部 Flash 编程器先准备候选固件和有效记录，再由 Bootloader 安装。

需要生成外部记录用于检查或编程器测试时：

```powershell
python -B tools/make_manifest.py app.bin --version 1 --output app.manifest.bin --w25q16-record request.bin --apply-mode 0
```

`request.bin` 是外部偏移 0 的 64 字节记录，`app.bin` 是外部偏移 `0x010000` 的固件。
外部编程器应先写完并校验固件，再写记录，commit 字段最后提交；不能把它们当作内部 Flash 文件烧录。
原 `--update-hex` 内部暂存测试选项已拒绝使用，新 Bootloader 不安装内部暂存包。

## 构建与验证

```powershell
cmake -S . -B cmake-build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build cmake-build-debug
./tests/run_tests.ps1
```

`CMakeLists.txt` 直接维护，`cmake/bootloader_sources.cmake` 显式列出需要的生产源文件，不再使用 `CMakeLists_template.txt`。
CLion 的共享 Debug Profile 将构建目录设为 `cmake-build-debug`；打开项目后重新加载 CMake，选择 Debug 配置即可点击编译。
构建目录由 CLion Profile 或命令行 `-B` 指定；未指定构建类型时默认为 Debug。
ELF、HEX、BIN、MAP 和 CMake 中间文件都位于 `cmake-build-debug`，主机测试产物位于其 `tests` 子目录。
`flash.cfg` 使用该目录中的 ELF；Flash 分区变化会触发重新链接。
CubeMX 重新生成后需要保留 Bootloader 的精简入口、CS 高电平和 16 KiB 链接限制。
继续使用现有 `CMakeLists.txt` 构建，避免通过 CLion 的 STM32CubeMX 转换操作重新生成并覆盖手工维护的 CMake 配置。

主机测试覆盖硬件 CRC 模型、外部记录各字段/两层 CRC、奇数长度与跨块处理、最大 384 KiB，
缺失设备、候选读取失败、安装中读取失败、Flash 错误、消费失败和全部 27 个擦写断电点。
独立 W25Q16 测试链接生产驱动，覆盖 JEDEC、24 位地址、BUSY 超时、读回、65 个 SPI 传输故障点、CS 释放和外部只写 consumed 的边界。
SPI 寄存器测试链接生产轮询代码，检查 Mode 0、8 位传输、禁用 DMA/中断、GPIO 复用、MODF/OVR 和计时回绕。
测试同时检查 Bootloader、保留区、旧内部请求和暂存区，以及外部候选包/保留区未被修改。

### 实板验证记录（2026-10-22）

已通过 ST-Link 烧录当前 Debug Bootloader，并由 OpenOCD 校验成功。该构建占用 Flash 5444 字节，链接器报告 RAM 占用 2040 字节。

| 检查项 | 实测结果 |
| --- | --- |
| SPI1 识别 W25Q16 | `g_w25q16_jedec_id = 0xEF4015` |
| Bootloader 启动结果 | `boot_status = BOOT_READY`，到达 APP 跳转入口 |
| APP 接管中断向量 | `SCB->VTOR = 0x08020000` |
| APP 运行位置 | PC 位于 APP 运行区，采样值为 `0x0802363E` |
| 已安装 APP 描述信息 | 版本 2，烧录 Bootloader 前后内容一致 |

上述结果确认了 Bootloader 烧录、外部芯片识别、已有 APP 校验和跳转。
尚未通过实板验证新候选固件的完整搬运、消费标记写入及安装期间断电恢复；SPI 波形也尚未测量。
这些安装和故障恢复路径已有主机模型测试覆盖，仍需使用真实新固件及硬件进行验证。
