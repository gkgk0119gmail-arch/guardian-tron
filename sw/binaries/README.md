# Ready-to-flash images

> **These are firmware v2** (commit `a965d06`), which is v1 plus the fix for the silent
> console on an NPU failure. The board was shipped with **v1** (commit `1e59965`), which is
> also the version every measurement in the repository was taken with. See
> [Firmware versions](../../README.md#firmware-versions). `ai_fsbl.hex` and
> `network_data.hex` are ST files and are identical in both.


| File | Flash address | What |
|---|---|---|
| `ai_fsbl.hex` | 0x70000000 (address inside the hex) | ST first-stage boot loader v1.4.0 (SLA0044) |
| `guardian_sign.bin` | **0x70100000** | HIBIKI firmware, signed + aligned for boot from flash |
| `network_data.hex` | 0x70380000 (address inside the hex) | YOLOX-nano weights for the NPU (ST, SLA0044) |
| `guardian.bin` | RAM 0x34000400 (development mode, start at 0x34000400) | same firmware, unsigned |
| `guardian.elf` | — | symbols for debugging / `addr2line` |

Flash with STM32CubeProgrammer (BOOT1 right = development mode), then BOOT0/BOOT1 left and power-cycle:

    STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el <ExternalLoader>/MX66UW1G45G_STM32N6570-DK.stldr -hardRst -w ai_fsbl.hex
    STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el <ExternalLoader>/MX66UW1G45G_STM32N6570-DK.stldr -hardRst -w network_data.hex
    STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el <ExternalLoader>/MX66UW1G45G_STM32N6570-DK.stldr -hardRst -w guardian_sign.bin 0x70100000

`SHA256SUMS` lists the checksums. Build date: see `strings guardian.elf | grep "Guardian-TRON vision"`.
