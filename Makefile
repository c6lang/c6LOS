# c6LOS —— ESP32-S3 构建
# 用法：make C6_ROOT=/path/to/c6 s3

C6_ROOT ?= $(HOME)/c6
C6CC    := $(C6_ROOT)/tcc/c6
XTP     := $(HOME)/.c6os/toolchain/xtensa-esp-elf/bin/xtensa-esp32s3-elf-
XTGCC   := $(XTP)gcc
# Xtensa 的两种 ABI：call0（本项目原来的选择）/ windowed（IDF 的闭源 WiFi·PHY
# 库都是这个）。想用 WiFi 就得把 S3_ABI 切成 windowed，同时用 windowed 版的
# port/s3/irq.S / ctx.S（见 docs/wifi-plan.md「ABI 迁移」）。
# 注意：必须定义在 XTFLAGS（立即展开）之前。
S3_ABI ?= call0
# 额外 C 宏（排查用，例如 S3_CDEFS=-DS3_IRQ_HOLD_OFF 先不开中断）
# LEAN=1：只保留内核 + 控制台 + WiFi 需要的最小对象（给"闭源库塞进 IRAM"腾地方）
S3_LEAN ?= 0
S3_CDEFS ?=
# idf_shim.c 的 ABI：默认 windowed（要和闭源库一致）；做对照实验时可设 call0
S3_SHIM_ABI ?= windowed

# ---- Linux ELF 二进制兼容层（personality，见 docs/linux-abi.md）----------
# 默认关闭：kernel/linux/ 不参与任何目标的编译，镜像尺寸零影响。
LINUX_ABI ?= 0

# ---- M9：VFS/文件系统（见 docs/vfs-net-design.md）-------------------------
# 默认关闭：C6OS_VFS=0 时 kernel/vfs.c6 等完全不参与编译，镜像尺寸零影响。
C6OS_VFS ?= 0
# M10：网络子系统（SAL + 协议栈 + e1000），默认 0 = S3 等最小构建零侵入。
C6OS_NET ?= 0
# 各 port 的能力位（include/c6os/caps.h）：MMU|USER|IRQ_USER = 7
CAPS_X86  := 7
CAPS_S3   := 0
CAPS_RV32 := 0

# 解析期门禁：make 的依赖判定有可能（在目标不是命令行显式目标时）跳过
# 下面的 caps-check-%，所以在解析阶段就把"无 MMU/无特权级端口 + LINUX_ABI=1"
# 这种自相矛盾的配置直接打断，一个字节的产物都不产出。
ifeq ($(LINUX_ABI),1)
ifneq ($(filter s3 all,$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)),)
$(error c6os: 配置错误：C6OS_LINUX_ABI=1，但 port "s3" 能力不足（需要 MMU|USER|IRQ_USER = 0x7，实际 0x0）。无 MMU / 无特权级的移植层（如 ESP32-S3）不能打开 Linux ABI；要么切回 C6OS_LINUX_ABI=0，要么换到 port/x86_64。依据：docs/linux-abi.md §2)
endif
endif

# 构建期门禁：配置与硬件能力矛盾时立刻失败，不产出半成品镜像。
# 注意：能力位必须在**make 展开期**就填进去。之前写成 shell 里的 $CAPS_x86，
# 那是 make 变量、根本不进 recipe 环境，于是永远算 0 —— 连 x86 都会被拒。
.PHONY: caps-check-x86 caps-check-s3 caps-check-rv32
caps-check-x86:
	@tools/chk-caps.sh x86 '$(CAPS_X86)' '$(LINUX_ABI)'
caps-check-s3:
	@tools/chk-caps.sh s3 '$(CAPS_S3)' '$(LINUX_ABI)'
caps-check-rv32:
	@tools/chk-caps.sh rv32 '$(CAPS_RV32)' '$(LINUX_ABI)'

# 我们自己的代码是 call0，闭源库是 windowed：两边不能共用同一个 memset。
# 库的 memset/memcpy/... 由 port/s3/wifi_glue.c（-mabi=windowed）提供；
# 我们自己的单元统一把这些名字重定向到 c6_*（rt/c6rt.c 里的 call0 实现）。
# 否则 call0 代码调到 windowed 版会因为 entry 不轮转而在 retw 上抛非法指令。
S3_LIBC_RENAME := -Dmemset=c6_memset -Dmemcpy=c6_memcpy -Dmemmove=c6_memmove \
                  -Dmemcmp=c6_memcmp -Dstrlen=c6_strlen -Dstrcmp=c6_strcmp \
                  -Dstrncmp=c6_strncmp -Dstrcpy=c6_strcpy \
                  -Dstrncpy=c6_strncpy -Dstrnlen=c6_strnlen

XTFLAGS := -mabi=$(S3_ABI) $(S3_CDEFS) $(S3_LIBC_RENAME) -Os -ffreestanding -fno-builtin -fno-common \
           -mlongcalls -mtext-section-literals -Wall -Wextra
PORT    ?= /dev/ttyACM0

BUILD   := build/s3
S3_MAIN ?= kernel/os.c6
S3_EXTRA_LIBS ?=
# 闭源库是 windowed ABI，但它们引用的 memcpy/memset 是我们 call0 编译的 ——
# windowed call8 进来、call0 ret 返回会跳飞。所以链接前用 objcopy 把这些引用
# 改名成 win_*，由 port/s3/win_island.c 里的 windowed 版提供。
LIBPHY_SRC ?= /media/winger/inst_26/esp/esp-idf/components/esp_phy/lib/esp32s3/libphy.a
LIBPHY_WIN := $(BUILD)/libphy_win.a
OBJCOPY    := $(dir $(XTGCC))xtensa-esp32s3-elf-objcopy
S3_C6FLAGS ?=
S3_EXTRA_C6 ?= kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 kernel/heap.c6
S3_SKIP_C6 := kernel/heap.c6 kernel/sem.c6 kernel/queue.c6
S3_C6   := $(S3_MAIN) kernel/panic.c6 kernel/log.c6 $(filter-out $(S3_SKIP_C6),$(S3_EXTRA_C6))
S3_NAMES := $(notdir $(S3_C6))
S3_C    := $(addprefix $(BUILD)/c99/,$(S3_NAMES:.c6=.c))
# 内核/控制台等必需对象（LEAN 也保留）
S3_OBJ_CORE := $(BUILD)/obj/start.o \
                   $(BUILD)/obj/w2c.o \
           $(addprefix $(BUILD)/obj/,$(S3_NAMES:.c6=.o)) \
           $(BUILD)/obj/console.o $(BUILD)/obj/c6rt.o $(BUILD)/obj/kprintf.o \
           $(BUILD)/obj/ctx.o $(BUILD)/obj/ctx_asm.o \
           $(BUILD)/obj/irq.o $(BUILD)/obj/irq_asm.o \
           $(BUILD)/obj/marks.o $(BUILD)/obj/flash.o $(BUILD)/obj/reg.o \
           $(BUILD)/obj/app_desc.o

# 面板/音频/UI 相关对象：LEAN=1 时全部去掉（给闭源库腾 IRAM）
S3_OBJ_FULL := $(BUILD)/obj/shell.o $(BUILD)/obj/cobs.o \
           $(BUILD)/obj/panel.o $(BUILD)/obj/s3fast.o \
           $(BUILD)/obj/button.o $(BUILD)/obj/i2s.o $(BUILD)/obj/gfx.o \
           $(BUILD)/obj/dash.o $(BUILD)/obj/core1.o

# IDF 闭源库的 ABI 桥与胶水：始终保留
S3_OBJ_WIFI := $(BUILD)/obj/idf_shim.o \
           $(BUILD)/obj/win_island.o $(BUILD)/obj/win_bridge.o \
           $(BUILD)/obj/irampad.o $(BUILD)/obj/phy_init_data.o \
           $(BUILD)/obj/wifi_glue.o

# 真正链闭源 WiFi 库（libnet80211/libpp）时才需要监管域数据表
ifneq ($(findstring -DS3_WITH_WIFI,$(S3_CDEFS)),)
S3_OBJ_WIFI += $(BUILD)/obj/wifi_regulatory.o
endif

ifeq ($(S3_LEAN),1)
S3_OBJ := $(S3_OBJ_CORE) $(S3_OBJ_WIFI)
else
S3_OBJ := $(S3_OBJ_CORE) $(S3_OBJ_FULL) $(S3_OBJ_WIFI)
endif

.PHONY: rt-host run-rt-host rv32bare-rt run-rv32bare-rt perf-host run-perf-host rv32bare-perf run-rv32bare-perf all s3 s3-src-check check-all check-sim rv32bare-tick run-rv32bare-tick rv32bare-smp run-rv32bare-smp rv32bare-smp-sem run-rv32bare-smp-sem s3-min check-host clean toolchain-info backup-flash flash image-info x86 run-x86 x86-sched run-x86-sched x86-tick run-x86-tick x86-preempt run-x86-preempt rv32 run-rv32 run-rv32-emu run-rv32-emu-sched rv32-sched run-rv32-sched rv32-tick run-rv32-tick wifi wififlash
.PRECIOUS: $(S3_C)
.SECONDARY:

all: s3

s3: caps-check-s3 $(BUILD)/c6os.bin

S3_HDRS := $(wildcard include/c6os/*.h include/los/*.h)

define S3_C_RULE
$(BUILD)/c99/$(notdir $(1:.c6=.c)): $(1) $(S3_HDRS)
	@mkdir -p $(BUILD)/c99
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude -I$(C6_ROOT) $(S3_C6FLAGS) -c -o $(BUILD)/c99/ $(1)
endef
$(foreach src,$(S3_C6),$(eval $(call S3_C_RULE,$(src))))

$(BUILD)/obj/%.o: $(BUILD)/c99/%.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/s3fast.o: port/s3/fast.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/console.o: port/s3/console.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/shell.o: rt/shell.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/cobs.o: rt/cobs.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/kprintf.o: rt/kprintf.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_ABI) -Os -ffreestanding -fno-builtin -mlongcalls -mtext-section-literals \
		-Iinclude -c $< -o $@

$(BUILD)/obj/c6rt.o: rt/c6rt.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/ctx.o: port/s3/ctx.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/core1.o: port/s3/core1.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/button.o: port/s3/button.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/i2s.o: port/s3/i2s.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/panel.o: port/s3/panel.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/gfx.o: rt/gfx.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/dash.o: rt/dash.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/app_desc.o: port/s3/app_desc.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/reg.o: port/s3/reg.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/flash.o: port/s3/flash.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(LIBPHY_WIN): $(LIBPHY_SRC)
	@mkdir -p $(BUILD)
	$(OBJCOPY) --redefine-sym memcpy=win_memcpy --redefine-sym memset=win_memset \
	  --redefine-sym memmove=win_memmove --redefine-sym memcmp=win_memcmp \
	  $< $@

# 监管域数据表（regdomain_table / regulatory_data）：闭源 net80211 需要，
# IDF 由开源组件 esp_wifi/regulatory/esp_wifi_regulatory.c 提供。只有 WiFi 构建要。
$(BUILD)/obj/wifi_regulatory.o: port/s3/wifi_regulatory.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=windowed -Os -ffreestanding -fno-builtin -fno-common \
		-mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/abi_thunk.o: port/s3/abi_thunk.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=call0 -c $< -o $@

# -Wa,--no-transform 只加在**能通过编译**的 windowed 单元上（w2c.S / win_island.c）。
# 原因：汇编器默认会把 retw 收窄成 retw.n，而窄格式在"需要窗口欠载"时是非法
# 指令（真板 JTAG 实测：windowbase=3、windowstart=0x8、epc1=retw.n、cause=0）。
# 关键洞察：**恢复执行后的第一次返回**必须是宽 retw —— 欠载会把下一层窗口填
# 回来，之后各层的 retw.n 就都合法了。
# wifi_glue.c 加不了：它太大，关掉 transform 后分支偏移会越界（汇编报错）。
$(BUILD)/obj/w2c.o: port/s3/w2c.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -Wa,--no-transform -c $< -o $@

$(BUILD)/obj/phy_init_data.o: port/s3/phy_init_data.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=windowed -Os -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/marks.o: port/s3/marks.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/irampad.o: port/s3/irampad.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

# WiFi OS 适配层（纯 call0 即可，不碰闭源库 ABI）
# wifi_glue.c 里那批 wifi_osi_funcs_t 桩函数会被 **windowed ABI 的闭源库回调**，
# 所以必须用 -mabi=windowed 编译；它自己对外暴露的入口（s3_wifi_osi_init 等）
# 由 call0 侧经孤岛桥调用。
$(BUILD)/obj/wifi_glue.o: port/s3/wifi_glue.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) -Os -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/win_island.o: port/s3/win_island.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) -Os -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/win_bridge.o: port/s3/win_bridge.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=call0 -Wa,--no-transform -mtext-section-literals -c $< -o $@

$(BUILD)/obj/irq.o: port/s3/irq.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/irq_asm.o: port/s3/irq.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_ABI) -c $< -o $@

$(BUILD)/obj/ctx_asm.o: port/s3/ctx.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_ABI) -c $< -o $@

# idf_shim.c 是给闭源库（windowed ABI）用的胶水，必须按 windowed 编译
$(BUILD)/obj/idf_shim.o: port/s3/idf_shim.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) -Os -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/start.o: port/s3/start.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_ABI) -c $< -o $@

# 栈顶：普通构建用 link.ld 的默认值；make wphy 这类调试构建传 S3_STACK_TOP
# 把栈挪到 DRAM 坏区(0x3FCD9500+)以下。
# 默认栈顶 0x3FCEF000（大 bss 的音频/图像构建需要）；make wphy 传 0x3FCD9000
# 把栈挪到 DRAM 坏区(0x3FCD9500+)以下，给闭源 PHY 的深调用用。
S3_STACK_TOP ?= 0x3FCEF000
S3_IMG_FLAGS ?=
S3_STACK_LD := -Wl,--defsym=S3_STACK_TOP=$(S3_STACK_TOP)

$(BUILD)/c6os.elf: $(S3_OBJ) port/s3/link.ld
	$(XTGCC) -mabi=$(S3_ABI) -nostdlib -Wl,-T,port/s3/link.ld \
		$(S3_STACK_LD) \
		-o $@ $(S3_OBJ) $(S3_EXTRA_LIBS) -lgcc
	$(XTP)size $@

$(BUILD)/c6os.bin: $(BUILD)/c6os.elf
	esptool --chip esp32s3 elf2image $(S3_IMG_FLAGS) --flash-mode dio \
		--flash-size 16MB --flash-freq 40m -o $@ $<
	@python3 tools/chkimg.py $(BUILD)/c6os.elf $@

image-info: $(BUILD)/c6os.bin
	esptool --chip esp32s3 image-info $<

$(BUILD)/backup/flash-16MB.bin:
	@mkdir -p $(BUILD)/backup
	@echo "备份原厂 16MB flash ..."
	sudo -n esptool --chip esp32s3 --port $(PORT) read-flash 0 0x1000000 $@

backup-flash: $(BUILD)/backup/flash-16MB.bin

flash: $(BUILD)/c6os.bin $(BUILD)/backup/flash-16MB.bin
	sudo -n esptool --chip esp32s3 --port $(PORT) write-flash 0x0 $(BUILD)/c6os.bin

# ---- host 参考机（c6 -> LLVM IR -> clang/lld + libtcc1 + sim port） ----
BUILDH := build/host
HOSTBIN := $(BUILDH)/t_smoke

$(BUILDH)/t_smoke.ll: tests/t_smoke.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/port_sim.o: port/sim/port_sim.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(BUILDH)/port_ctx.o: port/sim/port_ctx.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(HOSTBIN): $(BUILDH)/t_smoke.ll $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_smoke.ll $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/log.ll: kernel/log.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/dash.o: rt/dash.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(BUILDH)/t_dash.ll: tests/t_dash.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_dash: $(BUILDH)/t_dash.ll $(BUILDH)/dash.o $(BUILDH)/gfx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_dash.ll $(BUILDH)/dash.o \
		$(BUILDH)/gfx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/panel.o: rt/panel.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(BUILDH)/t_panel.ll: tests/t_panel.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_panel: $(BUILDH)/t_panel.ll $(BUILDH)/panel.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_panel.ll $(BUILDH)/panel.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/gfx.o: rt/gfx.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(BUILDH)/t_st77916.ll: tests/t_st77916.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_st77916: $(BUILDH)/t_st77916.ll $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_st77916.ll $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_gfx.ll: tests/t_gfx.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_gfx: $(BUILDH)/t_gfx.ll $(BUILDH)/gfx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_gfx.ll $(BUILDH)/gfx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/cobs.o: rt/cobs.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(BUILDH)/t_cobs.ll: tests/t_cobs.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_cobs: $(BUILDH)/t_cobs.ll $(BUILDH)/cobs.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_cobs.ll $(BUILDH)/cobs.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/shell.o: rt/shell.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(BUILDH)/t_shell.ll: tests/t_shell.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_shell: $(BUILDH)/t_shell.ll $(BUILDH)/shell.o $(BUILDH)/cobs.o $(BUILDH)/heap.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_shell.ll $(BUILDH)/shell.o \
		$(BUILDH)/cobs.o $(BUILDH)/heap.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/kprintf.o: rt/kprintf.c
	@mkdir -p $(BUILDH)
	gcc -Iinclude -c $< -o $@

$(BUILDH)/t_log.ll: tests/t_log.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_log: $(BUILDH)/t_log.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_log.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/panic.ll: kernel/panic.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_panic.ll: tests/t_panic.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_panic: $(BUILDH)/t_panic.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_panic.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/tick.ll: kernel/tick.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_tick.ll: tests/t_tick.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_tick: $(BUILDH)/t_tick.ll $(BUILDH)/tick.ll $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_tick.ll $(BUILDH)/tick.ll \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/task.ll $(BUILDH)/timeout.ll: kernel/task.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_sched.ll: tests/t_sched.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_sched: $(BUILDH)/t_sched.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_sched.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_task.ll: tests/t_task.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_task: $(BUILDH)/t_task.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_task.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/sem.ll: kernel/sem.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/mutex.ll: kernel/mutex.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_ipc.ll: tests/t_ipc.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_ipc: $(BUILDH)/t_ipc.ll $(BUILDH)/sem.ll $(BUILDH)/mutex.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_ipc.ll $(BUILDH)/sem.ll \
		$(BUILDH)/mutex.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/event.ll: kernel/event.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_event.ll: tests/t_event.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_event: $(BUILDH)/t_event.ll $(BUILDH)/event.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_event.ll $(BUILDH)/event.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/queue.ll: kernel/queue.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_queue.ll: tests/t_queue.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_queue: $(BUILDH)/t_queue.ll $(BUILDH)/queue.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_queue.ll $(BUILDH)/queue.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/hwi.ll: kernel/hwi.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_hwi.ll: tests/t_hwi.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/los_hwi.ll: compat/los_hwi.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_hwi: $(BUILDH)/t_hwi.ll $(BUILDH)/hwi.ll $(BUILDH)/los_hwi.ll $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_hwi.ll $(BUILDH)/hwi.ll \
		$(BUILDH)/los_hwi.ll $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_race.ll: tests/t_race.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_race: $(BUILDH)/t_race.ll $(BUILDH)/sem.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_race.ll $(BUILDH)/sem.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_queue_generic.ll: tests/t_queue_generic.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_queue_generic: $(BUILDH)/t_queue_generic.ll $(BUILDH)/queue.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_queue_generic.ll $(BUILDH)/queue.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/membox.ll: kernel/membox.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_membox.ll: tests/t_membox.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_membox: $(BUILDH)/t_membox.ll $(BUILDH)/membox.ll $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_membox.ll $(BUILDH)/membox.ll \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/heap.ll: kernel/heap.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_heap.ll: tests/t_heap.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_heap: $(BUILDH)/t_heap.ll $(BUILDH)/heap.ll $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_heap.ll $(BUILDH)/heap.ll \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/timeout.ll: kernel/timeout.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/timer.ll: kernel/timer.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_timer.ll: tests/t_timer.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_timer: $(BUILDH)/t_timer.ll $(BUILDH)/timer.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_timer.ll $(BUILDH)/timer.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_prio.ll: tests/t_prio.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_prio: $(BUILDH)/t_prio.ll $(BUILDH)/sem.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_prio.ll $(BUILDH)/sem.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/los_sem.ll: compat/los_sem.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/los_tick.ll: compat/los_tick.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los.ll: tests/t_los.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los: $(BUILDH)/t_los.ll $(BUILDH)/los_sem.ll $(BUILDH)/los_tick.ll \
		$(BUILDH)/sem.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_los.ll $(BUILDH)/los_sem.ll \
		$(BUILDH)/los_tick.ll $(BUILDH)/sem.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/los_queue.ll: compat/los_queue.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/los_mux.ll: compat/los_mux.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/los_event.ll: compat/los_event.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los2.ll: tests/t_los2.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los2: $(BUILDH)/t_los2.ll $(BUILDH)/los_queue.ll $(BUILDH)/los_mux.ll \
		$(BUILDH)/los_event.ll $(BUILDH)/los_task.ll $(BUILDH)/queue.ll $(BUILDH)/mutex.ll \
		$(BUILDH)/event.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_los2.ll $(BUILDH)/los_queue.ll \
		$(BUILDH)/los_mux.ll $(BUILDH)/los_event.ll $(BUILDH)/los_task.ll $(BUILDH)/queue.ll \
		$(BUILDH)/mutex.ll $(BUILDH)/event.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/los_task.ll: compat/los_task.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_consistency.ll: tests/t_consistency.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_consistency: $(BUILDH)/t_consistency.ll $(BUILDH)/sem.ll $(BUILDH)/queue.ll \
		$(BUILDH)/los_sem.ll $(BUILDH)/los_queue.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_consistency.ll $(BUILDH)/sem.ll \
		$(BUILDH)/queue.ll $(BUILDH)/los_sem.ll $(BUILDH)/los_queue.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_los_event_clr.ll: tests/t_los_event_clr.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los_event_clr: $(BUILDH)/t_los_event_clr.ll $(BUILDH)/los_event.ll \
		$(BUILDH)/event.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_los_event_clr.ll \
		$(BUILDH)/los_event.ll $(BUILDH)/event.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_los_err.ll: tests/t_los_err.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los_err: $(BUILDH)/t_los_err.ll $(BUILDH)/los_sem.ll $(BUILDH)/los_task.ll \
		$(BUILDH)/los_queue.ll $(BUILDH)/sem.ll $(BUILDH)/queue.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_los_err.ll $(BUILDH)/los_sem.ll \
		$(BUILDH)/los_task.ll $(BUILDH)/los_queue.ll $(BUILDH)/sem.ll \
		$(BUILDH)/queue.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/los_membox.ll: compat/los_membox.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/los_mem.ll: compat/los_mem.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/los_swtmr.ll: compat/los_swtmr.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_task2.ll: tests/t_task2.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_task2: $(BUILDH)/t_task2.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_task2.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_los4.ll: tests/t_los4.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los4: $(BUILDH)/t_los4.ll $(BUILDH)/los_task.ll $(BUILDH)/los_membox.ll \
		$(BUILDH)/los_mem.ll $(BUILDH)/los_swtmr.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/membox.ll \
		$(BUILDH)/heap.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_los4.ll $(BUILDH)/los_task.ll \
		$(BUILDH)/los_membox.ll $(BUILDH)/los_mem.ll $(BUILDH)/los_swtmr.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/membox.ll $(BUILDH)/heap.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_los3.ll: tests/t_los3.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_los3: $(BUILDH)/t_los3.ll $(BUILDH)/los_task.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_los3.ll $(BUILDH)/los_task.ll \
		$(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_prio32.ll: tests/t_prio32.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_prio32: $(BUILDH)/t_prio32.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_prio32.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_irq_nest.ll: tests/t_irq_nest.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_irq_nest: $(BUILDH)/t_irq_nest.ll $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_irq_nest.ll $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_preempt_sim.ll: tests/t_preempt_sim.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_preempt_sim: $(BUILDH)/t_preempt_sim.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_preempt_sim.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_stress.ll: tests/t_stress.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_stress: $(BUILDH)/t_stress.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_stress.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_overflow.ll: tests/t_overflow.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_overflow: $(BUILDH)/t_overflow.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_overflow.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_isr.ll: tests/t_isr.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_isr: $(BUILDH)/t_isr.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/sem.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_isr.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/sem.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_pi_chain.ll: tests/t_pi_chain.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_pi_chain: $(BUILDH)/t_pi_chain.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/mutex.ll $(BUILDH)/sem.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_pi_chain.ll $(BUILDH)/task.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/mutex.ll $(BUILDH)/sem.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_timeout.ll: tests/t_timeout.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_timeout: $(BUILDH)/t_timeout.ll $(BUILDH)/timeout.ll $(BUILDH)/task.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_timeout.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/task.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_suspend.ll: tests/t_suspend.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_suspend: $(BUILDH)/t_suspend.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_suspend.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

$(BUILDH)/t_join.ll: tests/t_join.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_join: $(BUILDH)/t_join.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_join.ll $(BUILDH)/task.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o \
		$(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

check-all: check-host x86 rv32 rv32bare-tick

check-host: $(HOSTBIN) $(BUILDH)/t_log $(BUILDH)/t_panic $(BUILDH)/t_tick $(BUILDH)/t_sched $(BUILDH)/t_task $(BUILDH)/t_ipc $(BUILDH)/t_event $(BUILDH)/t_queue $(BUILDH)/t_membox $(BUILDH)/t_heap $(BUILDH)/t_timer $(BUILDH)/t_prio $(BUILDH)/t_los $(BUILDH)/t_los2 $(BUILDH)/t_join $(BUILDH)/t_suspend $(BUILDH)/t_los3 $(BUILDH)/t_timeout $(BUILDH)/t_los4 $(BUILDH)/t_task2 $(BUILDH)/t_los_err $(BUILDH)/t_queue_generic $(BUILDH)/t_race $(BUILDH)/t_hwi $(BUILDH)/t_shell $(BUILDH)/t_cobs \
	$(BUILDH)/t_pi_chain $(BUILDH)/t_isr $(BUILDH)/t_overflow $(BUILDH)/t_stress \
	$(BUILDH)/t_preempt_sim $(BUILDH)/t_irq_nest $(BUILDH)/t_prio32 $(BUILDH)/t_consistency $(BUILDH)/t_gfx $(BUILDH)/t_panel $(BUILDH)/t_dash $(BUILDH)/t_st77916
	./$(HOSTBIN)
	./$(BUILDH)/t_log
	./$(BUILDH)/t_panic
	./$(BUILDH)/t_tick
	./$(BUILDH)/t_sched
	./$(BUILDH)/t_task
	./$(BUILDH)/t_ipc
	./$(BUILDH)/t_event
	./$(BUILDH)/t_queue
	./$(BUILDH)/t_membox
	./$(BUILDH)/t_heap
	./$(BUILDH)/t_timer
	./$(BUILDH)/t_prio
	./$(BUILDH)/t_los
	./$(BUILDH)/t_los2
	./$(BUILDH)/t_join
	./$(BUILDH)/t_suspend
	./$(BUILDH)/t_timeout
	./$(BUILDH)/t_pi_chain
	./$(BUILDH)/t_isr
	./$(BUILDH)/t_overflow
	./$(BUILDH)/t_stress
	./$(BUILDH)/t_preempt_sim
	./$(BUILDH)/t_irq_nest
	./$(BUILDH)/t_prio32
	./$(BUILDH)/t_consistency $(BUILDH)/t_los_event_clr
	./$(BUILDH)/t_gfx
	./$(BUILDH)/t_panel
	./$(BUILDH)/t_dash
	./$(BUILDH)/t_st77916
	./$(BUILDH)/t_los3
	./$(BUILDH)/t_los4
	./$(BUILDH)/t_task2
	./$(BUILDH)/t_los_err
	./$(BUILDH)/t_queue_generic
	./$(BUILDH)/t_race
	./$(BUILDH)/t_hwi
	./$(BUILDH)/t_shell
	./$(BUILDH)/t_cobs

s3-min: $(BUILD)/min.bin

$(BUILD)/min.elf: port/s3/min.S port/s3/min.ld
	@mkdir -p $(BUILD)
	$(XTGCC) -mabi=$(S3_ABI) -nostdlib -Wl,-T,port/s3/min.ld -o $@ port/s3/min.S

$(BUILD)/min.bin: $(BUILD)/min.elf
	esptool --chip esp32s3 elf2image --flash-mode dio --flash-size 16MB --flash-freq 80m -o $@ $<

toolchain-info:
	@echo "C6_ROOT = $(C6_ROOT)"
	@$(C6CC) -v 2>&1 | head -1 || true
	@$(XTGCC) --version | head -1
	@$(XTP)ld --version | head -1
	@esptool version | head -1

# ---- x86-64 参考目标（c6 -> LLVM IR，qemu 裸机） ----
X86        := build/x86
# -MMD -MP：让每个 .o 旁边落一份 .d，记录它包含了哪些头文件。
# 没有这个的话，改一个结构体（比如 LxProc 加字段）只会重编直接依赖它的 .c，
# 用过旧布局的 .o 会继续留在 obj 里，最后表现成"代码看着对、跑起来随机崩"。
# 这类幽灵 bug 排查成本极高，两行 make 就能杜绝。
# 注意不能在这里写 -MF：-MMD 的默认 .d 名字是按**目标文件**取的
# （obj/proc.o → obj/proc.d），换成 $(@:.o=.d) 在这里展开时 $@ 还是空的。
X86CDEPFLAGS := -MMD -MP
X86CFLAGS  := --target=x86_64-elf -ffreestanding -fno-pic -mno-red-zone -O1 \
              -DC6OS_PORT_CAPS=$(CAPS_X86) -DC6OS_LINUX_ABI=$(LINUX_ABI) -DC6OS_VFS=$(C6OS_VFS) \
              -DC6OS_NET=$(C6OS_NET) \
              $(X86CDEPFLAGS)
# c6 产出的 IR 基本是 -O0 形态（locals 全在栈上），IR → .o 这一步不开优化的话
# 一份几百行的内核文件就能吃掉 10KB+ 代码段 —— x86 stage2 只有 255 扇区
# （≈127.5KB），M9 的 VFS 一加就爆。这里统一开 -O1。
X86IRFLAGS := --target=x86_64-unknown-linux-gnu -ffreestanding -mno-sse -mno-sse2 -O1
X86S_NAME  ?= sched
X86S_MAIN  ?= tests/t_x86_sched.c6
X86S_DIR   := $(X86)/$(X86S_NAME)
X86S_C6    := $(X86S_MAIN) kernel/task.c6 kernel/tick.c6 \
              kernel/timer.c6 kernel/log.c6 kernel/panic.c6 kernel/timeout.c6

# personality 的 bring-up 目标（L1 ring3 / L2 ELF）不链接调度器，
# 免得带入 port_cur_ctx 之类的调度钩子。
ifneq ($(filter user elf fs dyn mem vfs m9 m15 m16 m17 m18 m20 m21 m22 m23 m24 m25 net ip ping udp tcp iso fatw alp,$(X86S_NAME)),)
X86S_C6    := $(X86S_MAIN)
endif

# M9：VFS 子系统只在完整内核目标上编（Linux ABI 侧的对接在 M9-4）。
ifeq ($(C6OS_VFS),1)
ifneq ($(filter user elf fs dyn,$(X86S_NAME)),)
$(error C6OS_VFS=1 暂不接在 user/elf/fs/dyn 上（Linux ABI 侧的对接在 M9-4）
endif
X86S_C6        += kernel/vfs.c6 kernel/romfs.c6 kernel/ramfs.c6 \
                  kernel/fsapi.c6 kernel/blkdev.c6 kernel/fatfs.c6 kernel/heap.c6
# vfs 走的是"精简对象列表"，里面本来就有 mmu.o/ata.o；只有默认完整内核目标
# 才需要额外补这两个（重复传同一个 .o 会 multiple definition）。
ifneq ($(filter user elf fs dyn mem vfs m9 m15 m16 m17 m18 m20 m21 m22 m23 m24 m25 net ip ping udp tcp iso fatw alp,$(X86S_NAME)),)
X86S_OBJ_EXTRA :=
else
X86S_OBJ_EXTRA := $(X86S_DIR)/obj/mmu.o $(X86S_DIR)/obj/ata.o
endif
endif
# M10：SAL/协议栈只在 C6OS_NET=1 时编（默认 0 时对象列表完全不变）。
ifeq ($(C6OS_NET),1)
X86S_C6        += kernel/sal.c6 kernel/net/netbuf.c6 kernel/net/netif.c6 \
                  kernel/net/arp.c6 kernel/net/ip.c6 kernel/net/icmp.c6 \
                  kernel/net/udp.c6 kernel/net/sal_udp.c6 \
                  kernel/net/tcp.c6 kernel/net/sal_tcp.c6 \
                  kernel/net/netif_x86.c6
endif

X86S_LL    := $(addprefix $(X86S_DIR)/ir/,$(notdir $(X86S_C6:.c6=.ll)))
# MMU 与内核堆必须同时在场：kmem 的页来自 PMM、指针在直接映射窗口里。集中成一处，
# 免得以后再加"跟着 mmu 走"的文件时又要改三个 X86S_NAME 分支。
X86S_MMU_OBJ := $(X86S_DIR)/obj/mmu.o $(X86S_DIR)/obj/kmem.o
X86S_OBJ   := $(addprefix $(X86S_DIR)/obj/,$(notdir $(X86S_C6:.c6=.o))) \
              $(X86S_DIR)/obj/console.o $(X86S_DIR)/obj/ctx.o \
              $(X86S_DIR)/obj/ctx_asm.o $(X86S_DIR)/obj/irq.o \
              $(X86S_DIR)/obj/irq_asm.o $(X86S_DIR)/obj/kprintf.o \
              $(X86S_DIR)/obj/libc.o

# Linux ABI personality 的宿主只要 port 层 + MMU/特权级单元（不含调度器）。
ifeq ($(X86S_NAME),user)
X86S_OBJ   := $(addprefix $(X86S_DIR)/obj/,$(notdir $(X86S_C6:.c6=.o))) \
              $(X86S_DIR)/obj/console.o $(X86S_DIR)/obj/irq.o \
              $(X86S_DIR)/obj/irq_asm.o $(X86S_DIR)/obj/libc.o \
              $(X86S_MMU_OBJ) $(X86S_DIR)/obj/linux.o \
              $(X86S_DIR)/obj/user_entry.o $(X86S_DIR)/obj/userblob.o \
              $(X86S_DIR)/obj/sys.o $(X86S_DIR)/obj/fs.o \
              $(X86S_DIR)/obj/elf64.o $(X86S_DIR)/obj/sig.o \
              $(X86S_DIR)/obj/proc.o $(X86S_DIR)/obj/tty.o $(X86S_DIR)/obj/fdt.o \
              $(X86S_DIR)/obj/vma.o \
              $(X86S_DIR)/obj/socket.o $(X86S_DIR)/obj/afunix.o
endif

# L2/L3 载荷：再多带磁盘、ELF64 加载器与 initramfs。
ifneq ($(filter elf fs dyn vfs m9 m15 m16 m17 m18 m20 m21 m22 m23 m24 m25 net ip ping udp tcp iso fatw,$(X86S_NAME)),)
X86S_OBJ   := $(addprefix $(X86S_DIR)/obj/,$(notdir $(X86S_C6:.c6=.o))) \
              $(X86S_DIR)/obj/console.o $(X86S_DIR)/obj/irq.o \
              $(X86S_DIR)/obj/irq_asm.o $(X86S_DIR)/obj/libc.o \
              $(X86S_MMU_OBJ) $(X86S_DIR)/obj/linux.o \
              $(X86S_DIR)/obj/user_entry.o $(X86S_DIR)/obj/ata.o \
              $(X86S_DIR)/obj/sys.o $(X86S_DIR)/obj/elf64.o \
              $(X86S_DIR)/obj/fs.o $(X86S_DIR)/obj/sig.o \
              $(X86S_DIR)/obj/proc.o $(X86S_DIR)/obj/tty.o $(X86S_DIR)/obj/vma.o \
              $(X86S_DIR)/obj/socket.o $(X86S_DIR)/obj/afunix.o $(X86S_DIR)/obj/fdt.o
endif

# M13-1：页框/地址空间生命周期（只接 port 层与 MMU，不接调度器/磁盘）。
ifeq ($(X86S_NAME),mem)
X86S_OBJ   := $(addprefix $(X86S_DIR)/obj/,$(notdir $(X86S_C6:.c6=.o))) \
              $(X86S_DIR)/obj/console.o $(X86S_DIR)/obj/irq.o \
              $(X86S_DIR)/obj/irq_asm.o $(X86S_DIR)/obj/libc.o \
              $(X86S_MMU_OBJ) $(X86S_DIR)/obj/vma.o
endif

ifeq ($(C6OS_NET),1)
X86S_OBJ   += $(X86S_DIR)/obj/e1000.o
endif

X86S_OBJ += $(X86S_OBJ_EXTRA)

x86: caps-check-x86 $(X86)/disk.img

run-x86: x86
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

x86-sched:
	$(MAKE) X86S_NAME=sched X86S_MAIN=tests/t_x86_sched.c6 $(X86)/sched-disk.img

x86-tick:
	$(MAKE) X86S_NAME=tick X86S_MAIN=tests/t_x86_tick.c6 $(X86)/tick-disk.img

x86-preempt:
	$(MAKE) X86S_NAME=preempt X86S_MAIN=tests/t_x86_preempt.c6 $(X86)/preempt-disk.img

run-x86-preempt: x86-preempt
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/preempt-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

run-x86-tick: x86-tick
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/tick-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

run-x86-sched: x86-sched
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/$(X86S_NAME)-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

$(X86)/hello.ll: tests/t_x86_hello.c6
	@mkdir -p $(X86)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(X86)/hello_ll.o: $(X86)/hello.ll
	clang $(X86IRFLAGS) -c -o $@ $<

$(X86)/console.o: port/x86_64/console.c
	@mkdir -p $(X86)
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86)/stage2.o: port/x86_64/stage2.S
	@mkdir -p $(X86)
	clang $(X86CFLAGS) -c -o $@ $<

$(X86)/irq.o: port/x86_64/irq.c
	@mkdir -p $(X86)
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86)/irq_asm.o: port/x86_64/irq.S
	@mkdir -p $(X86)
	clang $(X86CFLAGS) -c -o $@ $<

$(X86)/stage2.elf: $(X86)/stage2.o $(X86)/console.o $(X86)/hello_ll.o \
		$(X86)/irq.o $(X86)/irq_asm.o port/x86_64/stage2.ld
	ld -m elf_x86_64 -static -T port/x86_64/stage2.ld -o $@ $(X86)/stage2.o \
		$(X86)/console.o $(X86)/hello_ll.o $(X86)/irq.o $(X86)/irq_asm.o

$(X86)/stage2.bin: $(X86)/stage2.elf
	llvm-objcopy -O binary $< $@

$(X86)/stage1.S: port/x86_64/stage1.S $(X86)/stage2.bin
	sed 's|#BLOB#|$(X86)/stage2.bin|' port/x86_64/stage1.S > $@
	printf '\n.section .stage2, "a"\n.incbin "%s"\n' '$(X86)/stage2.bin' >> $@

$(X86)/stage1.o: $(X86)/stage1.S
	as --64 -o $@ $<

$(X86)/flat.elf: $(X86)/stage1.o port/x86_64/stage1.ld
	ld -m elf_x86_64 -static -T port/x86_64/stage1.ld -o $@ $(X86)/stage1.o

$(X86)/flat.bin: $(X86)/flat.elf
	llvm-objcopy -O binary $< $@

$(X86)/boot16.o: port/x86_64/boot16.S
	@mkdir -p $(X86)
	as --32 -o $@ $<

$(X86)/boot16.bin: $(X86)/boot16.o
	ld -m elf_i386 -Ttext=0x7C00 --oformat binary -o $@ $<

$(X86)/disk.img: $(X86)/boot16.bin $(X86)/flat.bin
	dd if=/dev/zero of=$@ bs=1M count=1 status=none
	dd if=$(X86)/boot16.bin of=$@ conv=notrunc status=none
	dd if=$(X86)/flat.bin of=$@ bs=512 seek=1 conv=notrunc status=none

# sched 载荷：内核 + 上下文切换
$(X86S_DIR)/ir/stamp: $(X86S_C6)
	@mkdir -p $(X86S_DIR)/ir
	for f in $^; do \
		$(C6CC) -B $(C6_ROOT)/tcc -Iinclude $(C6_DEFS) --emit-llvm \
			-o $(X86S_DIR)/ir/$$(basename $$f .c6).ll $$f || exit 1; \
	done
	@touch $@

$(X86S_DIR)/ir/%.ll: $(X86S_DIR)/ir/stamp
	@test -f $@

$(X86S_DIR)/obj/%.o: $(X86S_DIR)/ir/%.ll $(X86S_DIR)/ir/stamp
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86IRFLAGS) -c -o $@ $<

$(X86S_DIR)/obj/console.o: port/x86_64/console.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/ctx.o: port/x86_64/ctx.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/irq.o: port/x86_64/irq.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/irq_asm.o: port/x86_64/irq.S
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -c -o $@ $<

$(X86S_DIR)/obj/ctx_asm.o: port/x86_64/ctx.S
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -c -o $@ $<

$(X86S_DIR)/obj/kprintf.o: rt/kprintf.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -Irt -c -o $@ $<

$(X86S_DIR)/obj/libc.o: rt/x86_libc.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

# ---- Linux ABI personality（x86-64 宿主，L1）----------------------------
$(X86S_DIR)/obj/mmu.o: port/x86_64/mmu.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/kmem.o: port/x86_64/kmem.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/linux.o: port/x86_64/linux.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/user_entry.o: port/x86_64/user_entry.S
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -c -o $@ $<

$(X86S_DIR)/obj/userblob.o: port/x86_64/userblob.S
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -c -o $@ $<

$(X86S_DIR)/obj/ata.o: port/x86_64/ata.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/e1000.o: port/x86_64/e1000.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/sys.o: linux/sys.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/elf64.o: linux/elf64.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/fs.o: linux/fs.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/proc.o: linux/proc.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/fdt.o: linux/fdt.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/tty.o: linux/tty.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/sig.o: linux/sig.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/vma.o: linux/vma.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/socket.o: linux/socket.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

$(X86S_DIR)/obj/afunix.o: linux/afunix.c
	@mkdir -p $(X86S_DIR)/obj
	clang $(X86CFLAGS) -Iinclude -c -o $@ $<

x86-user: caps-check-x86
	$(MAKE) X86S_NAME=user X86S_MAIN=tests/t_x86_user.c6 LINUX_ABI=1 \
		$(X86)/user-disk.img

run-x86-user: x86-user
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/user-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

# M26：统一回归入口——跑完所有 Linux ABI 里程碑 + 网络 + alpine，
# 逐项汇总、单项失败不中断（实现见 tools/check-linuxabi-all.sh）。
.PHONY: check-linuxabi-all
check-linuxabi-all:
	@bash tools/check-linuxabi-all.sh

# L1 验收：真跑一遍"进 ring3 → int 0x80 → 退出"通路，按输出判定。
check-linuxabi: x86-user
	@timeout 20 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/user-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| grep -q "hello from ring3" \
	  && echo "PASS: linux-abi L1（ring3 + int 0x80）" \
	  || { echo "FAIL: linux-abi L1"; exit 1; }

# ---- M13-1：地址空间生命周期（内核侧压力测试）----
# 反复建/映射/改权限/解映射/销毁 300 轮，空闲物理页必须回到基线。这是
# "fork 100 次 OOM" 的回归闸门：漏一页都过不了。
x86-mem: caps-check-x86
	$(MAKE) X86S_NAME=mem X86S_MAIN=tests/t_x86_mem.c6 LINUX_ABI=1 \
		$(X86)/mem-disk.img

check-linuxabi-m13: x86-mem
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/mem-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m13.log \
		| grep -q "^PASS: linux-abi M13" \
	  && echo "PASS: linux-abi M13（AS 生命周期：页框回收 + PTE 真权限）" \
	  || { echo "FAIL: linux-abi M13"; tail -20 /tmp/check-m13.log; exit 1; }

# ---- L2：未修改的 Linux 用户态二进制（宿主 gcc 静态链接，内核侧零改动）----
APP_C   := tests/linux/hello.c
APP_ELF := $(X86)/app.elf

$(APP_ELF): $(APP_C)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- L3：initramfs（newc cpio）+ 文件语义 ----
LS_SRC := tests/linux/ls.c
LS_ELF := $(X86)/ls.elf
FS_IMG := $(X86)/fs.img
FSROOT := $(X86)/fsroot

$(LS_ELF): $(LS_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

$(FS_IMG): $(LS_ELF) tools/mk-initramfs.py
	@rm -rf $(FSROOT)
	@mkdir -p $(FSROOT)/bin $(FSROOT)/etc
	printf 'c6os 1.0\n' > $(FSROOT)/etc/version
	printf 'hello from the c6LOS initramfs\n' > $(FSROOT)/hello.txt
	cp $(LS_ELF) $(FSROOT)/bin/ls
	python3 tools/mk-initramfs.py $@ $(FSROOT)

# ---- M9-4：/tmp 挂 ramfs，用户态 write/read/getdents64 ----
M9_SRC := tests/linux/tmpfs.c
M9_ELF := $(X86)/tmpfs.elf

$(M9_ELF): $(M9_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M15：VFS/设备 syscall 语义矩阵 ----
M15_SRC := tests/linux/m15fs.c
M15_ELF := $(X86)/m15fs.elf

$(M15_ELF): $(M15_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M16：事件与时间 syscall 语义 ----
M16_SRC := tests/linux/m16ev.c
M16_ELF := $(X86)/m16ev.elf

$(M16_ELF): $(M16_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M17：procfs 虚拟文件 + 控制终端 ----
M17_SRC := tests/linux/m17proc.c
M17_ELF := $(X86)/m17proc.elf

$(M17_ELF): $(M17_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M18：Linux socket syscall → SAL ----
M18_SRC := tests/linux/m18sock.c
M18_ELF := $(X86)/m18sock.elf

$(M18_ELF): $(M18_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M19：busybox applet 矩阵 ----
M19_SRC := tests/linux/m19applet.c
M19_ELF := $(X86)/m19applet.elf

$(M19_ELF): $(M19_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

$(X86)/m19-disk.img: $(M19_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(M19_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

# ---- M20：epoll_create1/ctl/wait ----
M20_SRC := tests/linux/m20epoll.c
M20_ELF := $(X86)/m20epoll.elf

$(M20_ELF): $(M20_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M21：timerfd_create/settime/gettime ----
M21_SRC := tests/linux/m21timerfd.c
M21_ELF := $(X86)/m21timerfd.elf

$(M21_ELF): $(M21_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M22：signalfd4 ----
M22_SRC := tests/linux/m22signalfd.c
M22_ELF := $(X86)/m22signalfd.elf

$(M22_ELF): $(M22_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M23：sendmsg/recvmsg iovec ----
M23_SRC := tests/linux/m23msg.c
M23_ELF := $(X86)/m23msg.elf

$(M23_ELF): $(M23_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M24：getaddrinfo / DNS（动态 ELF + 带 /etc 的 initramfs）----
M24_SRC  := tests/linux/m24dns.c
M24_ELF  := $(X86)/m24dns.elf
M24_FS   := $(X86)/m24fs.img
M24ROOT  := $(X86)/m24root
LIBNSSD  := /lib/x86_64-linux-gnu/libnss_dns.so.2
LIBNSSF  := /lib/x86_64-linux-gnu/libnss_files.so.2

# 动态 ELF：解释器/libc 从 initramfs 加载（与 DYNROOT 一致）
$(M24_ELF): $(M24_SRC)
	@mkdir -p $(dir $@)
	gcc -Os -s -o $@ $<

# ---- M25：AF_UNIX socket ----
M25_SRC := tests/linux/m25unix.c
M25_ELF := $(X86)/m25unix.elf

$(M25_ELF): $(M25_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# 复制 dynroot（ld.so/libc/libresolv/busybox）+ /etc 解析配置，打成 initramfs
$(M24_FS): $(DYN_ELF) $(LD_SO) $(LIBC6) $(LIBRES) $(LIBNSSD) $(LIBNSSF) tools/mk-initramfs.py
	@rm -rf $(M24ROOT)
	@mkdir -p $(M24ROOT)/bin $(M24ROOT)/lib64 $(M24ROOT)/lib/x86_64-linux-gnu $(M24ROOT)/etc
	cp $(LD_SO)  $(M24ROOT)/lib64/ld-linux-x86-64.so.2
	cp $(LIBC6)  $(M24ROOT)/lib/x86_64-linux-gnu/libc.so.6
	cp $(LIBRES) $(M24ROOT)/lib/x86_64-linux-gnu/libresolv.so.2
	cp $(LIBNSSD) $(M24ROOT)/lib/x86_64-linux-gnu/libnss_dns.so.2
	cp $(LIBNSSF) $(M24ROOT)/lib/x86_64-linux-gnu/libnss_files.so.2
	cp $(DYN_ELF) $(M24ROOT)/bin/busybox
	printf 'nameserver 10.0.2.3\n' > $(M24ROOT)/etc/resolv.conf
	printf '127.0.0.1 localhost\n10.0.2.15 c6os\n' > $(M24ROOT)/etc/hosts
	printf 'hosts: files dns\n' > $(M24ROOT)/etc/nsswitch.conf
	python3 tools/mk-initramfs.py $@ $(M24ROOT)


# ---- L4：动态链接（ld.so + libc.so 从 initramfs 里由 ld.so 自己 mmap 进来）----
DYN_ELF := $(X86)/busybox
DYNFS   := $(X86)/dynfs.img
DYNROOT := $(X86)/dynroot
LD_SO   := /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2
LIBC6   := /usr/lib/x86_64-linux-gnu/libc.so.6
LIBRES  := /usr/lib/x86_64-linux-gnu/libresolv.so.2

$(DYN_ELF): /usr/bin/busybox
	@mkdir -p $(dir $@)
	cp /usr/bin/busybox $@

$(DYNFS): $(DYN_ELF) tools/mk-initramfs.py
	@rm -rf $(DYNROOT)
	@mkdir -p $(DYNROOT)/bin $(DYNROOT)/lib64 $(DYNROOT)/lib/x86_64-linux-gnu
	cp $(LD_SO)  $(DYNROOT)/lib64/ld-linux-x86-64.so.2
	cp $(LIBC6)  $(DYNROOT)/lib/x86_64-linux-gnu/libc.so.6
	cp $(LIBRES) $(DYNROOT)/lib/x86_64-linux-gnu/libresolv.so.2
	cp $(DYN_ELF) $(DYNROOT)/bin/busybox
	python3 tools/mk-initramfs.py $@ $(DYNROOT)

x86-dyn: caps-check-x86
	$(MAKE) X86S_NAME=dyn X86S_MAIN=tests/t_x86_dyn.c6 LINUX_ABI=1 \
		$(X86)/dyn-disk.img

# dyn-disk.img 只由上面这次递归 make 产出，而 L5..L8 的载荷镜像把它当普通文件
# 依赖。外层 make（X86S_NAME 不是 dyn）没有这条规则，干净构建时 check-linuxabi-l5
# 会直接报 "没有规则可制作目标 build/x86/dyn-disk.img"。这里补一个文件目标：
# order-only 依赖保证递归 make 先跑，文件存在即算完成。
#
# 必须用 X86S_NAME 门禁：递归进去的那次 make 里这个目标就是它自己要构建的，
# 无条件定义会让 x86-dyn 再递归调用自己，变成无限 make 循环。
ifneq ($(X86S_NAME),dyn)
$(X86)/dyn-disk.img: | x86-dyn
endif

run-x86-dyn: x86-dyn
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/dyn-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

# L4 验收：宿主上那个**动态链接的 PIE** busybox 被装载、ld.so 自己把 libc.so
# mmap 进来并做完重定位，然后 busybox 打印它自己的输出。
check-linuxabi-l4: x86-dyn
	@timeout 30 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/dyn-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^lib64\$$" \
	  && echo "PASS: linux-abi L4（未修改的动态 busybox 执行 ls / 成功）" \
	  || { echo "FAIL: linux-abi L4"; exit 1; }

# ---- L5/L6 验收载荷：都跑在动态镜像之上（initramfs 里已经有 libc/ld.so/busybox） ----
SH_ELF  := $(X86)/shpipe.elf
SH_IMG  := $(X86)/shpipe-disk.img
SIG_ELF := $(X86)/sig.elf
SIG_IMG := $(X86)/sig-disk.img

$(SH_ELF): tests/linux/shpipe.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $<

$(SH_IMG): $(SH_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(SH_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

$(SIG_ELF): tests/linux/sig.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $<

$(SIG_IMG): $(SIG_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(SIG_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

# ---- M13-2a：COW fork ------------------------------------------------------
# 48MB 匿名映射 + 6 次 fork（受 LX_MAX_PROC=8 限制）：全量复制会 OOM，COW 只多
# 页表页；父子写必须隔离。
COW_ELF := $(X86)/cow.elf
COW_IMG := $(X86)/cow-disk.img

$(COW_ELF): tests/linux/cow.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $<

# 依赖 x86-dyn（而不是 dyn-disk.img 这个文件）：保证镜像一定在内核重建之后才
# 拷贝；同时避免 check 目标和文件规则各触发一次递归 make（并发时两边会打架，
# 曾出现镜像里是旧内核、调试打印对不上的怪现象）。
$(COW_IMG): $(COW_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(COW_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

check-linuxabi-m13b: $(COW_IMG)
	@timeout 90 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(COW_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m13b.log \
		| grep -q "^PASS: cow" \
	  && echo "PASS: linux-abi M13-2a（COW fork：48MB×6 次 fork + 写隔离）" \
	  || { echo "FAIL: linux-abi M13-2a"; tail -20 /tmp/check-m13b.log; exit 1; }

# ---- M13-2b：惰性 mmap / 栈向下增长 / munmap 真回收 -------------------------
VMA_ELF := $(X86)/vma.elf
VMA_IMG := $(X86)/vma-disk.img

$(VMA_ELF): tests/linux/vma.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $<

$(VMA_IMG): $(VMA_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(VMA_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

check-linuxabi-m13c: $(VMA_IMG)
	@timeout 120 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(VMA_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m13c.log \
		| grep -q "^PASS: vma" \
	  && echo "PASS: linux-abi M13-2b（惰性 mmap/栈增长/munmap 回收）" \
	  || { echo "FAIL: linux-abi M13-2b"; tail -20 /tmp/check-m13c.log; exit 1; }

check-linuxabi-l5: $(SH_IMG)
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(SH_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^GOT:hello-from-pipe" \
	  && echo "PASS: linux-abi L5（fork/execve/pipe/wait4：busybox sh 管道）" \
	  || { echo "FAIL: linux-abi L5"; exit 1; }

# ---- M13-3：人为上限清零（1000 fd / 32 线程 / 64KB 管道）--------------------
SCAL_ELF := $(X86)/scal.elf
SCAL_IMG := $(X86)/scal-disk.img

$(SCAL_ELF): tests/linux/scal.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $< -lpthread

$(SCAL_IMG): $(SCAL_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(SCAL_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

# ---- M14：进程语义完整（进程组 / 作业控制 / wait4 选项 / SIGPIPE / EINTR）-----
PROCGRP_ELF := $(X86)/procgrp.elf
PROCGRP_IMG := $(X86)/procgrp-disk.img

$(PROCGRP_ELF): tests/linux/procgrp.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $<

$(PROCGRP_IMG): $(PROCGRP_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(PROCGRP_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

check-linuxabi-m14: $(PROCGRP_IMG)
	@timeout 120 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(PROCGRP_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m14.log \
		| grep -q "^PASS: procgrp" \
	  && echo "PASS: linux-abi M14（进程组/作业控制/wait4 选项/SIGPIPE/EINTR）" \
	  || { echo "FAIL: linux-abi M14"; tail -30 /tmp/check-m14.log; exit 1; }

check-linuxabi-m13d: $(SCAL_IMG)
	@timeout 180 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(SCAL_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m13d.log \
		| grep -q "^PASS: scal" \
	  && echo "PASS: linux-abi M13-3（1000 fd / 32 线程 / 64KB 管道）" \
	  || { echo "FAIL: linux-abi M13-3"; tail -30 /tmp/check-m13d.log; exit 1; }

THR_ELF := $(X86)/thread.elf
THR_IMG := $(X86)/thread-disk.img

$(THR_ELF): tests/linux/thread.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $< -lpthread

$(THR_IMG): $(THR_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(THR_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

PRE_ELF := $(X86)/preempt.elf
PRE_IMG := $(X86)/preempt-disk.img

$(PRE_ELF): tests/linux/preempt.c
	@mkdir -p $(dir $@)
	gcc -Os -o $@ $<

$(PRE_IMG): $(PRE_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(PRE_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

x86-vfs: caps-check-x86
	$(MAKE) X86S_NAME=vfs X86S_MAIN=tests/t_vfs.c6 C6OS_VFS=1 \
		$(X86)/vfs-disk.img

run-x86-vfs: x86-vfs
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/vfs-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

check-vfs: x86-vfs
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/vfs-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' > $(X86)/vfs.log; \
	  for m in M9-0 M9-1 M9-2 M9-3; do \
	    grep -q "^PASS: vfs $$m" $(X86)/vfs.log || { echo "FAIL: vfs $$m"; exit 1; }; \
	  done; \
	  grep -E "^PASS: vfs M9-" $(X86)/vfs.log

# 零侵入：C6OS_VFS=0 时 S3 的对象列表里不该出现任何 VFS 源文件
check-vfs-ramfs: x86-vfs
	@timeout 40 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/vfs-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^PASS: vfs M9-1" \
	  && echo "PASS: vfs M9-1（ramfs：写/读回/lseek/截断/改名/mkdir/rmdir/unlink）" \
	  || { echo "FAIL: vfs M9-1"; exit 1; }

FAT_IMG := $(X86)/fat.img

$(FAT_IMG): tools/mkfat.py
	@mkdir -p $(X86)
	python3 tools/mkfat.py $@ 4096

check-vfs-fat: x86-vfs
	@timeout 40 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/vfs-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^PASS: vfs M9-3" \
	  && echo "PASS: vfs M9-3（FAT16：mount + stat + read HELLO.TXT + readdir）" \
	  || { echo "FAIL: vfs M9-3"; exit 1; }

check-vfs-blk: x86-vfs
	@timeout 40 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/vfs-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^PASS: vfs M9-2" \
	  && echo "PASS: vfs M9-2（块设备层 + ATA PIO 写：写 8 扇区读回逐字节一致）" \
	  || { echo "FAIL: vfs M9-2"; exit 1; }

check-vfs-zero:
	@n=$$(make -n s3 2>/dev/null | grep -c "kernel/vfs.c6"); \
	 test "$$n" = "0" \
	  && echo "PASS: C6OS_VFS=0 时 S3 不编 VFS（零侵入）" \
	  || { echo "FAIL: VFS 漏进 S3 对象列表"; exit 1; }

# ---- M10-0：SAL 骨架（协议栈注册表 + socket 表 + 函数表分发） ----
x86-net: caps-check-x86
	$(MAKE) X86S_NAME=net X86S_MAIN=tests/t_sal.c6 C6OS_NET=1 \
		$(X86)/net-disk.img

run-x86-net: x86-net
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/net-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

check-sal: x86-net
	@timeout 30 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/net-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^PASS: sal M10-0" \
	  && echo "PASS: sal M10-0（协议栈注册表 + 16 槽 socket 表 + 函数表分发）" \
	  || { echo "FAIL: sal M10-0"; exit 1; }

# ---- M10-1：ARP + IPv4 + ICMP echo + loopback ----
x86-ip: caps-check-x86
	$(MAKE) X86S_NAME=ip X86S_MAIN=tests/t_net.c6 C6OS_NET=1 \
		$(X86)/ip-disk.img

run-x86-ip: x86-ip
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/ip-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

check-ip: x86-ip
	@timeout 30 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/ip-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^PASS: net M10-1" \
	  && echo "PASS: net M10-1（ARP + IPv4 + ICMP echo + loopback）" \
	  || { echo "FAIL: net M10-1"; exit 1; }

# ---- M10-2：e1000 驱动 + 真网络（qemu slirp） ----
QEMU_NIC := -nic user,model=e1000

x86-ping: caps-check-x86
	$(MAKE) X86S_NAME=ping X86S_MAIN=tests/t_ping.c6 C6OS_NET=1 \
		$(X86)/ping-disk.img

run-x86-ping: x86-ping
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/ping-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC)

check-ping: x86-ping
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/ping-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-ping.log | grep -q "^PASS: net M10-2" \
	  && echo "PASS: net M10-2（e1000：ARP + ping 10.0.2.2）" \
	  || { echo "FAIL: net M10-2"; tail -20 /tmp/check-ping.log; exit 1; }

# ---- M10-3：UDP + SAL socket 面（slirp DNS 往返） ----
x86-udp: caps-check-x86
	$(MAKE) X86S_NAME=udp X86S_MAIN=tests/t_udp.c6 C6OS_NET=1 \
		$(X86)/udp-disk.img

run-x86-udp: x86-udp
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/udp-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC)

check-udp: x86-udp
	@timeout 90 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/udp-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-udp.log | grep -q "^PASS: net M10-3" \
	  && echo "PASS: net M10-3（UDP + SAL socket 面，slirp DNS 往返）" \
	  || { echo "FAIL: net M10-3"; tail -20 /tmp/check-udp.log; exit 1; }

# ---- M10-4：TCP 最小子集 + GET / HTTP/1.0 ----
HTTP_DIR := $(X86)/http

$(HTTP_DIR)/index.html:
	@mkdir -p $(HTTP_DIR)
	@printf 'c6LOS M10-4\n' > $@

x86-tcp: caps-check-x86
	$(MAKE) X86S_NAME=tcp X86S_MAIN=tests/t_tcp.c6 C6OS_NET=1 \
		$(X86)/tcp-disk.img

run-x86-tcp: x86-tcp $(HTTP_DIR)/index.html
	@(cd $(HTTP_DIR) && python3 -m http.server 8000 >/dev/null 2>&1 & echo $$! > $(X86)/httpd.pid)
	@sleep 1
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/tcp-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC)
	@kill $$(cat $(X86)/httpd.pid) 2>/dev/null || true

# 宿主起一个 http.server，客户机（slirp 的 10.0.2.2:8000）去 GET /
check-tcp: x86-tcp $(HTTP_DIR)/index.html
	@(cd $(HTTP_DIR) && python3 -m http.server 8000 >/dev/null 2>&1 & echo $$! > $(X86)/httpd.pid) ; \
	 sleep 1 ; \
	 timeout 180 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/tcp-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-tcp.log | grep -q "^PASS: net M10-4" ; \
	 rc=$$? ; kill $$(cat $(X86)/httpd.pid) 2>/dev/null || true ; \
	 if [ $$rc = 0 ]; then echo "PASS: net M10-4（TCP + GET / HTTP/1.0 拿到 200）"; \
	 else echo "FAIL: net M10-4"; tail -25 /tmp/check-tcp.log; exit 1; fi

# ---- M11-0：ISO（El Torito no-emulation）启动 ----
# 光盘的逻辑块是 2048 字节：cd0 用 int 13h AH=42h 按块读，再经 unreal mode 搬到
# 目标物理地址（内核 0x10000、rootfs.cpio 0x00800000）。载荷不再受 boot16 的
# 195KB 上限约束。
ISO_DIR    := $(X86)/iso
ISO_MAIN   ?= tests/t_iso.c6
ISO_KER    := $(ISO_DIR)/flat.bin
ISO_IMG    := $(ISO_DIR)/c6os.iso
ISO_ROOTFS ?=
CD0_BIN    := $(X86)/cd0.bin

$(X86)/cd0.o: port/x86_64/cd0.S
	@mkdir -p $(X86)
	as --32 -o $@ $<

$(CD0_BIN): $(X86)/cd0.o
	ld -m elf_i386 -Ttext=0x7C00 --oformat binary -o $@ $<

x86-iso: caps-check-x86
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=$(ISO_MAIN) $(ISO_KER)
	@mkdir -p $(ISO_DIR)
	python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		$(if $(ISO_ROOTFS),--rootfs $(ISO_ROOTFS),) -o $(ISO_IMG)

# ---- M11-3：docker 镜像 → ISO（内核 + 镜像 rootfs + 可写数据盘）----
DOCKER_APP  := $(X86)/docker-app.elf
DOCKER_ISO  := $(X86)/docker.iso
DOCKER_DATA := $(X86)/docker-data.img
DOCKER_ROOT := $(X86)/docker-root

$(DOCKER_APP): tests/linux/docker_app.c
	@mkdir -p $(X86)
	gcc -static -nostdlib -fno-builtin -fno-stack-protector -Os -s -o $@ $<

x86-docker-iso: caps-check-x86 $(CD0_BIN) $(DOCKER_APP)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_docker.c6 LINUX_ABI=1 C6OS_VFS=1 $(ISO_KER)
	@rm -rf $(DOCKER_ROOT)
	@mkdir -p $(DOCKER_ROOT)/bin
	cp $(DOCKER_APP) $(DOCKER_ROOT)/bin/app
	python3 tools/mkdockeriso.py --rootfs $(DOCKER_ROOT) --cmd /bin/app \
		--boot $(CD0_BIN) --kernel $(ISO_KER) --data $(DOCKER_DATA) -o $(DOCKER_ISO)

run-x86-docker-iso: x86-docker-iso
	qemu-system-x86_64 -m 256M -cdrom $(DOCKER_ISO) -boot d \
		-drive format=raw,file=$(DOCKER_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot

check-docker-iso: x86-docker-iso
	@timeout 90 qemu-system-x86_64 -m 256M -cdrom $(DOCKER_ISO) -boot d \
		-drive format=raw,file=$(DOCKER_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-docker-iso.log | grep -q "^PASS: docker M11-3" \
	  && echo "PASS: docker M11-3（docker 镜像 → ISO → 跑起来 + 写 /data）" \
	  || { echo "FAIL: docker M11-3"; tail -30 /tmp/check-docker-iso.log; exit 1; }
	@7z l $(DOCKER_DATA) 2>/dev/null | grep -qi "OUT.TXT" \
	  && echo "PASS: docker M11-3 宿主侧（数据盘里有镜像程序写的 OUT.TXT）" \
	  || { echo "FAIL: docker M11-3 宿主侧"; 7z l $(DOCKER_DATA) | tail -12; exit 1; }

# ---- M12：真实 Alpine minirootfs（musl 动态链接）从 ISO 起起来，串口当终端 ----
ALPINE_DIR  := $(X86)/alpine
ALPINE_VER  ?= 3.20.0
ALPINE_BASE ?= https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64
ALPINE_TGZ  := $(ALPINE_DIR)/alpine-minirootfs-$(ALPINE_VER)-x86_64.tar.gz
ALPINE_ROOT := $(ALPINE_DIR)/root
ALPINE_CPIO := $(ALPINE_DIR)/full.cpio
ALPINE_DATA := $(ALPINE_DIR)/data.img
ALPINE_ISO  := $(ALPINE_DIR)/alpine.iso
# 1 = 编交互式版本（裸 /bin/sh，自己敲命令），0 = 跑固定的 sh -c 脚本
ALPINE_INTERACTIVE ?= 0

# 3.5MB 的 tarball：只在缓存里没有时下。也可以手工把 tarball 放进
# $(ALPINE_DIR)，免得每次 CI 都去连 CDN。
$(ALPINE_TGZ):
	@mkdir -p $(ALPINE_DIR)
	curl -fL --retry 2 --connect-timeout 20 -o $@.part \
		$(ALPINE_BASE)/$(notdir $@)
	@mv $@.part $@

$(ALPINE_ROOT)/bin/busybox: $(ALPINE_TGZ)
	@rm -rf $(ALPINE_ROOT)
	@mkdir -p $(ALPINE_ROOT)
	tar -xzf $(ALPINE_TGZ) -C $(ALPINE_ROOT)

# 514 个成员、334 个符号链接（/bin/sh → busybox、/lib/ld-musl-x86_64.so.1）
$(ALPINE_CPIO): $(ALPINE_ROOT)/bin/busybox tools/mk-initramfs.py
	@mkdir -p $(ALPINE_DIR)
	python3 tools/mk-initramfs.py $@ $(ALPINE_ROOT)

# 8MB FAT16，给客户机当 /data 写
$(ALPINE_DATA): tools/mkfat.py
	@mkdir -p $(ALPINE_DIR)
	python3 tools/mkfat.py $@ 8192

x86-alpine: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(if $(filter 1,$(ALPINE_INTERACTIVE)),C6_DEFS=-DC6OS_ALPINE_INTERACTIVE) \
		$(ISO_KER)
	@mkdir -p $(ALPINE_DIR)
	python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_ISO)

run-x86-alpine: x86-alpine
	qemu-system-x86_64 -m 256M -cdrom $(ALPINE_ISO) -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot

# 自己敲命令：起来就是 Alpine 的 /bin/sh 提示符（/ #），exit 或 Ctrl-D 退出。
run-x86-alpine-shell:
	@$(MAKE) ALPINE_INTERACTIVE=1 run-x86-alpine

# 验收：`sh -c '...'` 那段脚本的四件事都要在串口上看得到，而且 /data 里写下的
# 文件在宿主侧（数据盘镜像）能查出来 —— 证明真的落盘了。
check-alpine:
	@rm -f $(ALPINE_DATA)
	@$(MAKE) x86-alpine
	@timeout 180 qemu-system-x86_64 -m 256M -cdrom $(ALPINE_ISO) -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-alpine.log >/dev/null
	@grep -q "alpine-M12-alive" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（脚本没跑起来）"; tail -40 /tmp/check-alpine.log; exit 1; }
	@grep -q "m12ok" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（写 /data 再读回失败）"; tail -40 /tmp/check-alpine.log; exit 1; }
	@grep -q "alpine-M12-done" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（脚本没跑完）"; tail -40 /tmp/check-alpine.log; exit 1; }
	@echo "PASS: alpine M12（Alpine minirootfs 从 ISO 起起来：sh -c 脚本跑完 + 写 /data）"
	@7z l $(ALPINE_DATA) 2>/dev/null | grep -qi "M12.TXT" \
	  && echo "PASS: alpine M12 宿主侧（数据盘里有 m12.txt）" \
	  || { echo "FAIL: alpine M12 宿主侧"; 7z l $(ALPINE_DATA) | tail -12; exit 1; }

# ---- M11-2：可读写硬盘（FAT16 写）----
x86-fatw: caps-check-x86
	$(MAKE) X86S_NAME=fatw X86S_MAIN=tests/t_fat_write.c6 C6OS_VFS=1 \
		$(X86)/fatw-disk.img

run-x86-fatw: x86-fatw
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/fatw-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

# 客户机里写，宿主把数据盘那段抠出来用 7z 看——证明真的落盘了
check-fat-write:
	@rm -f $(X86)/fatw-disk.img
	@$(MAKE) x86-fatw
	@timeout 90 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/fatw-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-fat-write.log | grep -q "^PASS: fat M11-2" \
	  && echo "PASS: fat M11-2（/data 可读写：创建/写/读回/追加/截断/目录/删除）" \
	  || { echo "FAIL: fat M11-2"; tail -25 /tmp/check-fat-write.log; exit 1; }
	@dd if=$(X86)/fatw-disk.img of=/tmp/fatw-check.img bs=512 skip=16384 count=8192 status=none
	@7z l /tmp/fatw-check.img 2>/dev/null | grep -q "A\\.TXT" \
	  && echo "PASS: fat M11-2 宿主侧（数据盘里能看到 D1/A.TXT）" \
	  || { echo "FAIL: fat M11-2 宿主侧（数据盘里没有 D1/A.TXT）"; 7z l /tmp/fatw-check.img | tail -12; exit 1; }

# ---- M11-1：ISO 内嵌只读 rootfs（newc cpio 走 romfs 挂 /）----
ISO_ROOTFS_DIR := $(ISO_DIR)/rootfs
ISO_CPIO       := $(ISO_DIR)/rootfs.cpio

$(ISO_CPIO): tools/mk-initramfs.py
	@mkdir -p $(ISO_ROOTFS_DIR)
	@printf 'hello from the ISO rootfs\n' > $(ISO_ROOTFS_DIR)/hello.txt
	@printf 'c6OS M11-1\n' > $(ISO_ROOTFS_DIR)/etc-release
	python3 tools/mk-initramfs.py $@ $(ISO_ROOTFS_DIR)

x86-iso-rootfs: caps-check-x86
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_rootfs.c6 C6OS_VFS=1 $(ISO_KER)
	$(MAKE) $(ISO_CPIO)
	@mkdir -p $(ISO_DIR)
	python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ISO_CPIO) -o $(ISO_IMG)

run-x86-iso-rootfs: x86-iso-rootfs
	qemu-system-x86_64 -m 256M -cdrom $(ISO_IMG) -boot d \
		-nographic -serial stdio -monitor none -no-reboot

check-iso-rootfs: x86-iso-rootfs
	@timeout 60 qemu-system-x86_64 -m 256M -cdrom $(ISO_IMG) -boot d \
		-nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-iso-rootfs.log | grep -q "^PASS: iso M11-1" \
	  && echo "PASS: iso M11-1（ISO 内嵌 rootfs：romfs 挂 /，readdir/stat/open/read 通）" \
	  || { echo "FAIL: iso M11-1"; tail -25 /tmp/check-iso-rootfs.log; exit 1; }

run-x86-iso: x86-iso
	qemu-system-x86_64 -m 256M -cdrom $(ISO_IMG) -boot d \
		-nographic -serial stdio -monitor none -no-reboot

check-iso: x86-iso
	@timeout 60 qemu-system-x86_64 -m 256M -cdrom $(ISO_IMG) -boot d \
		-nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-iso.log | grep -q "^PASS: iso M11-0" \
	  && echo "PASS: iso M11-0（El Torito no-emulation 启动链）" \
	  || { echo "FAIL: iso M11-0"; tail -20 /tmp/check-iso.log; exit 1; }

# 零侵入：C6OS_NET=0 时 S3 的对象列表里不该出现 SAL/协议栈源文件
check-net-zero:
	@n=$$(make -n s3 2>/dev/null | grep -cE "kernel/sal.c6|kernel/net/"); \
	 test "$$n" = "0" \
	  && echo "PASS: C6OS_NET=0 时 S3 不编 SAL/协议栈（零侵入）" \
	  || { echo "FAIL: SAL 漏进 S3 对象列表"; exit 1; }

check-linuxabi-l8: $(PRE_IMG)
	@timeout 20 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(PRE_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot \
		> $(X86)/preempt.log 2>&1 || true
	@grep -q "^preempt: parent runs while child spins" $(X86)/preempt.log \
	  && grep -q "^\[preempt\] ticks=.*switch=[1-9]" $(X86)/preempt.log \
	  && echo "PASS: linux-abi L8（抢占：ring3 定时器中断 + 时间片轮转）" \
	  || { echo "FAIL: linux-abi L8"; exit 1; }

check-linuxabi-l7: $(THR_IMG)
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(THR_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^thr: 4 threads joined, counter=400, mutex=80" \
	  && echo "PASS: linux-abi L7（线程：clone/CLONE_VM、futex、pthread 4 线程 + mutex）" \
	  || { echo "FAIL: linux-abi L7"; exit 1; }

check-linuxabi-l6: $(SIG_IMG)
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(SIG_IMG),if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^sig: SIGSEGV caught, back via siglongjmp" \
	  && echo "PASS: linux-abi L6（信号：handler/屏蔽/SIGSEGV+siglongjmp/默认动作）" \
	  || { echo "FAIL: linux-abi L6"; exit 1; }

x86-m9: caps-check-x86
	$(MAKE) X86S_NAME=m9 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m9-disk.img

run-x86-m9: x86-m9
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/m9-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

# M9-4 验收：用户态在 /tmp（ramfs）上 create/write/lseek/read/getdents64
check-linuxabi-m9: x86-m9
	@timeout 30 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m9-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | grep -q "^PASS: linux-abi M9-4" \
	  && echo "PASS: linux-abi M9-4（/tmp = ramfs：open/write/lseek/read/getdents64）" \
	  || { echo "FAIL: linux-abi M9-4"; exit 1; }

# ---- M15：VFS/设备 syscall 完整语义 ----
x86-m15: caps-check-x86
	$(MAKE) X86S_NAME=m15 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m15-disk.img

check-linuxabi-m15: x86-m15
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m15-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m15.log \
		| grep -q "^PASS: m15fs" \
	  && echo "PASS: linux-abi M15（VFS 写类/*at/devfs/statfs 语义）" \
	  || { echo "FAIL: linux-abi M15"; tail -30 /tmp/check-m15.log; exit 1; }

# ---- M16：事件与时间 syscall ----
x86-m16: caps-check-x86
	$(MAKE) X86S_NAME=m16 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m16-disk.img

check-linuxabi-m16: x86-m16
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m16-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m16.log \
		| grep -q "^PASS: m16ev" \
	  && echo "PASS: linux-abi M16（select/eventfd/gettimeofday/clock_getres）" \
	  || { echo "FAIL: linux-abi M16"; tail -30 /tmp/check-m16.log; exit 1; }

# ---- M17：procfs + 控制终端 ----
x86-m17: caps-check-x86
	$(MAKE) X86S_NAME=m17 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m17-disk.img

check-linuxabi-m17: x86-m17
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m17-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m17.log \
		| grep -q "^PASS: m17proc" \
	  && echo "PASS: linux-abi M17（procfs 虚拟文件 + 控制终端 ioctl）" \
	  || { echo "FAIL: linux-abi M17"; tail -30 /tmp/check-m17.log; exit 1; }

# ---- M18：Linux socket syscall → SAL（UDP 环回）----
x86-m18: caps-check-x86
	$(MAKE) X86S_NAME=m18 X86S_MAIN=tests/t_x86_m18.c6 LINUX_ABI=1 C6OS_NET=1 \
		$(X86)/m18-disk.img

check-linuxabi-m18: x86-m18
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m18-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-m18.log \
		| grep -q "^PASS: m18sock" \
	  && echo "PASS: linux-abi M18（Linux socket syscall → SAL：UDP 环回）" \
	  || { echo "FAIL: linux-abi M18"; tail -30 /tmp/check-m18.log; exit 1; }

# ---- M19：busybox applet 矩阵（未修改的动态 busybox）----
check-linuxabi-m19: $(X86)/m19-disk.img
	@timeout 180 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m19-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m19.log \
		| grep -q "^PASS: m19applet" \
	  && echo "PASS: linux-abi M19（busybox applet 矩阵）" \
	  || { echo "FAIL: linux-abi M19"; tail -40 /tmp/check-m19.log; exit 1; }

# ---- M20：epoll 事件接口 ----
x86-m20: caps-check-x86
	$(MAKE) X86S_NAME=m20 X86S_MAIN=tests/t_x86_m20.c6 LINUX_ABI=1 \
		$(X86)/m20-disk.img

check-linuxabi-m20: x86-m20
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m20-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m20.log \
		| grep -q "^PASS: m20epoll" \
	  && echo "PASS: linux-abi M20（epoll_create1/ctl/wait）" \
	  || { echo "FAIL: linux-abi M20"; tail -30 /tmp/check-m20.log; exit 1; }

# ---- M21：timerfd ----
x86-m21: caps-check-x86
	$(MAKE) X86S_NAME=m21 X86S_MAIN=tests/t_x86_m21.c6 LINUX_ABI=1 \
		$(X86)/m21-disk.img

check-linuxabi-m21: x86-m21
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m21-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m21.log \
		| grep -q "^PASS: m21timerfd" \
	  && echo "PASS: linux-abi M21（timerfd_create/settime/gettime）" \
	  || { echo "FAIL: linux-abi M21"; tail -30 /tmp/check-m21.log; exit 1; }

# ---- M22：signalfd ----
x86-m22: caps-check-x86
	$(MAKE) X86S_NAME=m22 X86S_MAIN=tests/t_x86_m22.c6 LINUX_ABI=1 \
		$(X86)/m22-disk.img

check-linuxabi-m22: x86-m22
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m22-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m22.log \
		| grep -q "^PASS: m22signalfd" \
	  && echo "PASS: linux-abi M22（signalfd4）" \
	  || { echo "FAIL: linux-abi M22"; tail -30 /tmp/check-m22.log; exit 1; }

# ---- M23：sendmsg/recvmsg iovec ----
x86-m23: caps-check-x86
	$(MAKE) X86S_NAME=m23 X86S_MAIN=tests/t_x86_m18.c6 LINUX_ABI=1 C6OS_NET=1 \
		$(X86)/m23-disk.img

check-linuxabi-m23: x86-m23
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m23-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-m23.log \
		| grep -q "^PASS: m23msg" \
	  && echo "PASS: linux-abi M23（sendmsg/recvmsg iovec）" \
	  || { echo "FAIL: linux-abi M23"; tail -30 /tmp/check-m23.log; exit 1; }

# ---- M24：getaddrinfo / DNS ----
# 复用通用 disk 规则：递归 make 构建内核 flat.bin，DISK_APP 塞动态 ELF +
# 带 /etc/resolv.conf|hosts 的 initramfs（M24_FS）。
x86-m24: caps-check-x86 $(M24_ELF) $(M24_FS)
	$(MAKE) X86S_NAME=m24 X86S_MAIN=tests/t_x86_m24.c6 LINUX_ABI=1 C6OS_NET=1 \
		$(X86)/m24-disk.img

check-linuxabi-m24: x86-m24
	@timeout 120 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m24-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-m24.log \
		| grep -q "^PASS: m24dns" \
	  && echo "PASS: linux-abi M24（getaddrinfo / DNS）" \
	  || { echo "FAIL: linux-abi M24"; tail -30 /tmp/check-m24.log; exit 1; }

# ---- M25：AF_UNIX socket ----
x86-m25: caps-check-x86
	$(MAKE) X86S_NAME=m25 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m25-disk.img

check-linuxabi-m25: x86-m25
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m25-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m25.log \
		| grep -q "^PASS: m25unix" \
	  && echo "PASS: linux-abi M25（AF_UNIX socket）" \
	  || { echo "FAIL: linux-abi M25"; tail -30 /tmp/check-m25.log; exit 1; }

x86-fs: caps-check-x86
	$(MAKE) X86S_NAME=fs X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/fs-disk.img

run-x86-fs: x86-fs
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/fs-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

# L3 验收：用户程序自己 opendir/readdir/open/read，按它读到的内容判定。
check-linuxabi-l3: x86-fs
	@timeout 20 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/fs-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| grep -q "hello from the c6LOS initramfs" \
	  && echo "PASS: linux-abi L3（initramfs + 文件语义）" \
	  || { echo "FAIL: linux-abi L3"; exit 1; }

# 固定参数（X86S_NAME/X86S_MAIN/LINUX_ABI）走 env：命令行传的变量会进
# MAKEOVERRIDES 再经 MAKEFLAGS 传给子 make，等于"强制覆盖"——子 make 里这些
# 变量就成了不可被 Makefile 修改的 override，任何依赖它们的条件分支都会错乱。
# 走 env 则子 make 是普通默认值，Makefile 里的 := / += 全都照常生效。
x86-elf: caps-check-x86
	X86S_NAME=elf X86S_MAIN=tests/t_x86_elf.c6 LINUX_ABI=1 \
		$(MAKE) $(X86)/elf-disk.img

run-x86-elf: x86-elf
	qemu-system-x86_64 -m 256M -drive format=raw,file=$(X86)/elf-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot

# L2 验收：qemu 里跑未修改的 Linux 静态 ELF，用**它自己**打的那行字判定。
check-linuxabi-l2: x86-elf
	@timeout 20 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/elf-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| grep -q "hi from an unmodified Linux x86-64 ELF" \
	  && echo "PASS: linux-abi L2（未修改的 Linux 静态 ELF 在 ring3 跑通）" \
	  || { echo "FAIL: linux-abi L2"; exit 1; }

$(X86S_DIR)/stage2.elf: $(X86)/stage2.o $(X86S_OBJ) port/x86_64/stage2.ld
	ld -m elf_x86_64 -static -T port/x86_64/stage2.ld -o $@ \
		$(X86)/stage2.o $(X86S_OBJ)

$(X86S_DIR)/stage2.bin: $(X86S_DIR)/stage2.elf
	llvm-objcopy -O binary $< $@

$(X86S_DIR)/stage1.S: port/x86_64/stage1.S $(X86S_DIR)/stage2.bin
	sed 's|#BLOB#|$(X86S_DIR)/stage2.bin|' port/x86_64/stage1.S > $@
	printf '\n.section .stage2, "a"\n.incbin "%s"\n' '$(X86S_DIR)/stage2.bin' >> $@

$(X86S_DIR)/stage1.o: $(X86S_DIR)/stage1.S
	as --64 -o $@ $<

$(X86S_DIR)/flat.elf: $(X86S_DIR)/stage1.o port/x86_64/stage1.ld
	ld -m elf_x86_64 -static -T port/x86_64/stage1.ld -o $@ $<

$(X86S_DIR)/flat.bin: $(X86S_DIR)/flat.elf
	llvm-objcopy -O binary $< $@

# elf/fs 目标把用户态 ELF 写进 LBA 1024；fs 再在 LBA 2048 放 initramfs。
ifeq ($(X86S_NAME),elf)
DISK_DEPS  := $(APP_ELF)
DISK_MB    := 4
DISK_APP   = dd if=$(APP_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none
else ifeq ($(X86S_NAME),fs)
DISK_DEPS  := $(LS_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(LS_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),vfs)
DISK_DEPS  := $(FS_IMG) $(FAT_IMG)
DISK_MB    := 32
DISK_APP   = dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none; \
             dd if=$(FAT_IMG) of=$@ bs=512 seek=16384 conv=notrunc status=none
else ifeq ($(X86S_NAME),fatw)
DISK_DEPS  := $(FAT_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(FAT_IMG) of=$@ bs=512 seek=16384 conv=notrunc status=none
else ifeq ($(X86S_NAME),m9)
DISK_DEPS  := $(M9_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M9_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m15)
DISK_DEPS  := $(M15_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M15_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m16)
DISK_DEPS  := $(M16_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M16_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m17)
DISK_DEPS  := $(M17_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M17_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m18)
DISK_DEPS  := $(M18_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M18_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m20)
DISK_DEPS  := $(M20_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M20_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m21)
DISK_DEPS  := $(M21_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M21_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m22)
DISK_DEPS  := $(M22_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M22_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m23)
DISK_DEPS  := $(M23_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M23_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m24)
DISK_DEPS  := $(M24_ELF) $(M24_FS)
DISK_MB    := 16
DISK_APP   = dd if=$(M24_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(M24_FS) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m25)
DISK_DEPS  := $(M25_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M25_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),net)
DISK_DEPS  :=
DISK_MB    := 1
DISK_APP   := @true
else ifeq ($(X86S_NAME),ip)
DISK_DEPS  :=
DISK_MB    := 1
DISK_APP   := @true
else ifeq ($(X86S_NAME),ping)
DISK_DEPS  :=
DISK_MB    := 1
DISK_APP   := @true
else ifeq ($(X86S_NAME),udp)
DISK_DEPS  :=
DISK_MB    := 1
DISK_APP   := @true
else ifeq ($(X86S_NAME),tcp)
DISK_DEPS  :=
DISK_MB    := 1
DISK_APP   := @true
else ifeq ($(X86S_NAME),dyn)
DISK_DEPS  := $(DYN_ELF) $(DYNFS)
DISK_MB    := 16
DISK_APP   = dd if=$(DYN_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(DYNFS) of=$@ bs=512 seek=4096 conv=notrunc status=none
else
DISK_DEPS  :=
DISK_MB    := 1
DISK_APP   := @true
endif

$(X86)/$(X86S_NAME)-disk.img: $(X86)/boot16.bin $(X86S_DIR)/flat.bin $(DISK_DEPS)
	dd if=/dev/zero of=$@ bs=1M count=$(DISK_MB) status=none
	dd if=$(X86)/boot16.bin of=$@ conv=notrunc status=none
	dd if=$(X86S_DIR)/flat.bin of=$@ bs=512 seek=1 conv=notrunc status=none
	$(DISK_APP)


# ---- rv32 参考目标（c6 -> LLVM IR，qemu-riscv32 user-mode） ----
RV32        := build/rv32
RV32CFLAGS  := --target=riscv32-unknown-linux-gnu -march=rv32imac -mabi=ilp32 \
               -nostdlib -ffreestanding
RV32BFLAGS  := --target=riscv32-unknown-linux-gnu -march=rv32imac -mabi=ilp32 \
               -nostdlib -ffreestanding -msmall-data-limit=0
RV32_MAIN   ?= tests/t_rv32_hello.c6

rv32: $(RV32)/hello

run-rv32: rv32
	qemu-riscv32 $(RV32)/hello

# risc6 模拟器（~/risc6）：RV32 用户态 + 指令边界注入 MTIP，无需 qemu
RISC6 ?= $(HOME)/risc6/build/risc6
run-rv32-emu: rv32
	$(RISC6) $(RV32)/hello

run-rv32-emu-sched: rv32-sched
	$(RISC6) $(RV32S_DIR)/sched

$(RV32)/hello_x86.ll: $(RV32_MAIN)
	@mkdir -p $(RV32)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(RV32)/hello.ll: $(RV32)/hello_x86.ll
	sed -e 's|^target datalayout = .*|target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"|' \
	    -e 's|^target triple = .*|target triple = "riscv32-unknown-linux-gnu"|' $< > $@

$(RV32)/hello.o: $(RV32)/hello.ll
	clang $(RV32CFLAGS) -c -o $@ $<

$(RV32)/start.o: port/rv32/start.S
	@mkdir -p $(RV32)
	clang $(RV32CFLAGS) -c -o $@ $<

$(RV32)/console.o: port/rv32/console.c
	@mkdir -p $(RV32)
	clang $(RV32CFLAGS) -Iinclude -c -o $@ $<

# hello 不链接调度器，console.o 里 port_idle 引用的 os_sched_tick 由弱符号补齐
$(RV32)/stub.o: port/rv32/stub.c
	@mkdir -p $(RV32)
	clang $(RV32CFLAGS) -c -o $@ $<

$(RV32)/hello: $(RV32)/start.o $(RV32)/console.o $(RV32)/hello.o $(RV32)/stub.o
	clang $(RV32CFLAGS) -fuse-ld=lld -static -o $@ $^


RV32S_DIR  ?= $(RV32)/sched
RV32S_C6   ?= tests/t_rv32_sched.c6 kernel/task.c6 kernel/tick.c6 \
              kernel/timer.c6 kernel/log.c6 kernel/panic.c6 kernel/timeout.c6
RV32S_LL   := $(addprefix $(RV32S_DIR)/ir/,$(notdir $(RV32S_C6:.c6=.ll)))
RV32S_OBJ  := $(addprefix $(RV32S_DIR)/obj/,$(notdir $(RV32S_C6:.c6=.o))) \
              $(RV32S_DIR)/obj/start.o $(RV32S_DIR)/obj/console.o \
              $(RV32S_DIR)/obj/ctx.o $(RV32S_DIR)/obj/ctx_asm.o \
              $(RV32S_DIR)/obj/kprintf.o $(RV32S_DIR)/obj/libc.o

rv32-sched: $(RV32S_DIR)/sched

rv32-tick:
	$(MAKE) RV32S_DIR=$(RV32)/tick \
		RV32S_C6="tests/t_rv32_tick.c6 kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/log.c6 kernel/panic.c6 kernel/timeout.c6" \
		$(RV32)/tick/sched

run-rv32-tick: rv32-tick
	qemu-riscv32 $(RV32)/tick/sched

run-rv32-sched: rv32-sched
	qemu-riscv32 $(RV32S_DIR)/sched

$(RV32S_DIR)/ir/stamp: $(RV32S_C6)
	@mkdir -p $(RV32S_DIR)/ir
	for f in $^; do \
		$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $(RV32S_DIR)/ir/$$(basename $$f .c6).raw.ll $$f || exit 1; \
		sed -e 's|^target datalayout = .*|target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"|' \
		    -e 's|^target triple = .*|target triple = "riscv32-unknown-linux-gnu"|' \
		    $(RV32S_DIR)/ir/$$(basename $$f .c6).raw.ll > $(RV32S_DIR)/ir/$$(basename $$f .c6).ll || exit 1; \
	done
	@touch $@

$(RV32S_DIR)/ir/%.ll: $(RV32S_DIR)/ir/stamp
	@test -f $@

$(RV32S_DIR)/obj/%.o: $(RV32S_DIR)/ir/%.ll $(RV32S_DIR)/ir/stamp
	@mkdir -p $(RV32S_DIR)/obj
	clang $(RV32CFLAGS) -c -o $@ $<

$(RV32S_DIR)/obj/start.o: port/rv32/start.S
	@mkdir -p $(RV32S_DIR)/obj
	clang $(RV32CFLAGS) -c -o $@ $<

$(RV32S_DIR)/obj/console.o: port/rv32/console.c
	@mkdir -p $(RV32S_DIR)/obj
	clang $(RV32CFLAGS) -Iinclude -c -o $@ $<

$(RV32S_DIR)/obj/ctx.o: port/rv32/ctx.c
	@mkdir -p $(RV32S_DIR)/obj
	clang $(RV32CFLAGS) -Iinclude -c -o $@ $<

$(RV32S_DIR)/obj/ctx_asm.o: port/rv32/ctx.S
	@mkdir -p $(RV32S_DIR)/obj
	clang $(RV32CFLAGS) -c -o $@ $<

$(RV32S_DIR)/obj/kprintf.o: rt/kprintf.c
	@mkdir -p $(RV32S_DIR)/obj
	clang $(RV32CFLAGS) -Iinclude -Irt -c -o $@ $<

$(RV32S_DIR)/obj/libc.o: rt/x86_libc.c
	@mkdir -p $(RV32S_DIR)/obj
	clang $(RV32CFLAGS) -Iinclude -c -o $@ $<

$(RV32S_DIR)/sched: $(RV32S_OBJ)
	clang $(RV32CFLAGS) -fuse-ld=lld -static -o $@ $^



# ---- rv32 bare 参考目标（risc6 bare：真定时器陷阱/MTIP） ----
RV32B_DIR  := build/rv32bare
RV32B_MAIN ?= tests/t_rv32bare_tick.c6
RV32B_C6   := $(RV32B_MAIN) kernel/task.c6 kernel/tick.c6 \
              kernel/timer.c6 kernel/timeout.c6 kernel/panic.c6 kernel/log.c6
RV32B_LL   := $(addprefix $(RV32B_DIR)/ir/,$(notdir $(RV32B_C6:.c6=.ll)))
RV32B_OBJ  := $(addprefix $(RV32B_DIR)/obj/,$(notdir $(RV32B_C6:.c6=.o))) \
              $(RV32B_DIR)/obj/start.o $(RV32B_DIR)/obj/trap.o \
              $(RV32B_DIR)/obj/console.o $(RV32B_DIR)/obj/ctx.o \
              $(RV32B_DIR)/obj/ctx_asm.o $(RV32B_DIR)/obj/kprintf.o \
              $(RV32B_DIR)/obj/libc.o

rv32bare-tick: $(RV32B_DIR)/tick

run-rv32bare-tick: rv32bare-tick
	$(RISC6) --bare $(RV32B_DIR)/tick

check-sim: run-rv32-emu run-rv32-emu-sched run-rv32bare-tick run-rv32bare-smp run-rv32bare-smp-sem

$(RV32B_DIR)/ir/stamp: $(RV32B_C6)
	@mkdir -p $(RV32B_DIR)/ir
	for f in $^; do \
		$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $(RV32B_DIR)/ir/$$(basename $$f .c6).raw.ll $$f || exit 1; \
		sed -e 's|^target datalayout = .*|target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"|' \
		    -e 's|^target triple = .*|target triple = "riscv32-unknown-linux-gnu"|' \
		    $(RV32B_DIR)/ir/$$(basename $$f .c6).raw.ll > $(RV32B_DIR)/ir/$$(basename $$f .c6).ll || exit 1; \
	done
	@touch $@

$(RV32B_DIR)/ir/%.ll: $(RV32B_DIR)/ir/stamp
	@test -f $@

$(RV32B_DIR)/obj/%.o: $(RV32B_DIR)/ir/%.ll $(RV32B_DIR)/ir/stamp
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -c -o $@ $<

$(RV32B_DIR)/obj/start.o: port/rv32bare/start.S
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -c -o $@ $<

$(RV32B_DIR)/obj/trap.o: port/rv32bare/trap.S
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -c -o $@ $<

$(RV32B_DIR)/obj/console.o: port/rv32bare/console.c
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -c -o $@ $<

$(RV32B_DIR)/obj/ctx.o: port/rv32/ctx.c
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -DC6OS_BARE -Iinclude -c -o $@ $<

$(RV32B_DIR)/obj/ctx_asm.o: port/rv32/ctx.S
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -c -o $@ $<

$(RV32B_DIR)/obj/kprintf.o: rt/kprintf.c
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -Irt -c -o $@ $<

$(RV32B_DIR)/obj/libc.o: rt/x86_libc.c
	@mkdir -p $(RV32B_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -c -o $@ $<

$(RV32B_DIR)/tick: $(RV32B_OBJ)
	clang $(RV32BFLAGS) -static -fuse-ld=lld -o $@ $^

# ---- rv32 bare SMP 参考目标（两 hart 各跑一个任务） ----
RV32BS_DIR  := build/rv32bare-smp
RV32BS_MAIN ?= tests/t_rv32bare_smp.c6
RV32BS_C6   := $(RV32BS_MAIN) kernel/task.c6 kernel/tick.c6 \
              kernel/timer.c6 kernel/timeout.c6 kernel/panic.c6 kernel/log.c6
RV32BS_OBJ  := $(addprefix $(RV32BS_DIR)/obj/,$(notdir $(RV32BS_C6:.c6=.o))) \
              $(RV32BS_DIR)/obj/start.o $(RV32BS_DIR)/obj/trap.o \
              $(RV32BS_DIR)/obj/console.o $(RV32BS_DIR)/obj/ctx.o \
              $(RV32BS_DIR)/obj/ctx_asm.o $(RV32BS_DIR)/obj/kprintf.o \
              $(RV32BS_DIR)/obj/libc.o

rv32bare-smp: $(RV32BS_DIR)/smp

run-rv32bare-smp: rv32bare-smp
	$(RISC6) --bare --harts 2 $(RV32BS_DIR)/smp

$(RV32BS_DIR)/ir/stamp: $(RV32BS_C6)
	@mkdir -p $(RV32BS_DIR)/ir
	for f in $^; do \
		$(C6CC) -B $(C6_ROOT)/tcc -Iinclude -DC6OS_BARE --emit-llvm -o $(RV32BS_DIR)/ir/$$(basename $$f .c6).raw.ll $$f || exit 1; \
		sed -e 's|^target datalayout = .*|target datalayout = "e-m:e-p:32:32-i64:64-n32-S128"|' \
		    -e 's|^target triple = .*|target triple = "riscv32-unknown-linux-gnu"|' \
		    $(RV32BS_DIR)/ir/$$(basename $$f .c6).raw.ll > $(RV32BS_DIR)/ir/$$(basename $$f .c6).ll || exit 1; \
	done
	@touch $@

$(RV32BS_DIR)/ir/%.ll: $(RV32BS_DIR)/ir/stamp
	@test -f $@

$(RV32BS_DIR)/obj/%.o: $(RV32BS_DIR)/ir/%.ll $(RV32BS_DIR)/ir/stamp
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -c -o $@ $<

$(RV32BS_DIR)/obj/start.o: port/rv32bare/start.S
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -c -o $@ $<

$(RV32BS_DIR)/obj/trap.o: port/rv32bare/trap.S
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -c -o $@ $<

$(RV32BS_DIR)/obj/console.o: port/rv32bare/console.c
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -c -o $@ $<

$(RV32BS_DIR)/obj/ctx.o: port/rv32/ctx.c
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -DC6OS_BARE -Iinclude -c -o $@ $<

$(RV32BS_DIR)/obj/ctx_asm.o: port/rv32/ctx.S
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -c -o $@ $<

$(RV32BS_DIR)/obj/kprintf.o: rt/kprintf.c
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -Irt -c -o $@ $<

$(RV32BS_DIR)/obj/libc.o: rt/x86_libc.c
	@mkdir -p $(RV32BS_DIR)/obj
	clang $(RV32BFLAGS) -Iinclude -c -o $@ $<

$(RV32BS_DIR)/smp: $(RV32BS_OBJ)
	clang $(RV32BFLAGS) -static -fuse-ld=lld -o $@ $^


# ---- rv32 bare SMP 跨核信号量 ----
RV32BSS_DIR := build/rv32bare-smp-sem
rv32bare-smp-sem:
	$(MAKE) RV32BS_DIR=$(RV32BSS_DIR) \
		RV32BS_MAIN=tests/t_rv32bare_smp_sem.c6 \
		RV32BS_C6="tests/t_rv32bare_smp_sem.c6 kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 kernel/panic.c6 kernel/log.c6 kernel/sem.c6" \
		$(RV32BSS_DIR)/smp

run-rv32bare-smp-sem: rv32bare-smp-sem
	$(RISC6) --bare --harts 2 $(RV32BSS_DIR)/smp


# ---- 性能微基准 ----
perf-host: $(BUILDH)/t_perf

$(BUILDH)/t_perf.ll: tests/t_perf.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_perf: $(BUILDH)/t_perf.ll $(BUILDH)/task.ll $(BUILDH)/sem.ll $(BUILDH)/queue.ll \
		$(BUILDH)/heap.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/timeout.ll \
		$(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_perf.ll $(BUILDH)/task.ll $(BUILDH)/sem.ll \
		$(BUILDH)/queue.ll $(BUILDH)/heap.ll $(BUILDH)/tick.ll $(BUILDH)/timer.ll \
		$(BUILDH)/timeout.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll $(BUILDH)/kprintf.o \
		$(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o -L$(C6_ROOT)/tcc -ltcc1

run-perf-host: perf-host
	./$(BUILDH)/t_perf


# ---- rv32 bare 性能微基准 ----
RV32BP_DIR := build/rv32bare-perf
rv32bare-perf:
	$(MAKE) RV32BS_DIR=$(RV32BP_DIR) \
		RV32BS_MAIN=tests/t_perf.c6 \
		RV32BS_C6="tests/t_perf.c6 kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 kernel/panic.c6 kernel/log.c6 kernel/sem.c6 kernel/queue.c6 kernel/heap.c6" \
		$(RV32BP_DIR)/smp

run-rv32bare-perf: rv32bare-perf
	$(RISC6) --bare $(RV32BP_DIR)/smp


rt-host: $(BUILDH)/t_rt

$(BUILDH)/t_rt.ll: tests/t_rt.c6
	@mkdir -p $(BUILDH)
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm -o $@ $<

$(BUILDH)/t_rt: $(BUILDH)/t_rt.ll $(BUILDH)/task.ll $(BUILDH)/sem.ll $(BUILDH)/tick.ll \
		$(BUILDH)/timer.ll $(BUILDH)/timeout.ll $(BUILDH)/panic.ll $(BUILDH)/log.ll \
		$(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o
	clang -fuse-ld=lld -o $@ $(BUILDH)/t_rt.ll $(BUILDH)/task.ll $(BUILDH)/sem.ll \
		$(BUILDH)/tick.ll $(BUILDH)/timer.ll $(BUILDH)/timeout.ll $(BUILDH)/panic.ll \
		$(BUILDH)/log.ll $(BUILDH)/kprintf.o $(BUILDH)/port_ctx.o $(BUILDH)/port_sim.o \
		-L$(C6_ROOT)/tcc -ltcc1

run-rt-host: rt-host
	./$(BUILDH)/t_rt


RV32BR_DIR := build/rv32bare-rt
rv32bare-rt:
	$(MAKE) RV32BS_DIR=$(RV32BR_DIR) \
		RV32BS_MAIN=tests/t_rt.c6 \
		RV32BS_C6="tests/t_rt.c6 kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 kernel/panic.c6 kernel/log.c6 kernel/sem.c6" \
		$(RV32BR_DIR)/smp

run-rv32bare-rt: rv32bare-rt
	$(RISC6) --bare $(RV32BR_DIR)/smp


clean:
	rm -rf $(BUILD) $(BUILDH)

# ---- 便利目标（PHY/WiFi 调试用） ----------------------------------------
# make ... wphy        构建"背光常亮 + PHY 分步诊断 + SWD 自恢复"的用例
# make ... wphyflash   构建并刷机+即时监视（相当于 idf.py flash monitor）
PHY_CDEFS = -DS3_WITH_LIBPHY -DS3_KEEP_SWD
wphy:
	@rm -f build/phy/c6os.elf build/phy/c6os.bin
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=build/phy S3_CDEFS="$(PHY_CDEFS)" build/phy/libphy_win.a
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=build/phy S3_LEAN=1 S3_CDEFS="$(PHY_CDEFS)" \
		S3_STACK_TOP=0x3FCD9000 \
		S3_MAIN=tests/t_s3_phy.c6 S3_EXTRA_LIBS=build/phy/libphy_win.a s3
wphyflash: wphy
	./tools/flashmon.sh build/phy/c6os.bin 15

# ---- WiFi 构建（闭源 libnet80211 + libpp + libphy）-----------------------
# 用法：make C6_ROOT=/home/winger/c6 wifi
#       make ... wififlash       构建 + 刷机 + 串口监视
#
# 这一版才是"真的 WiFi 镜像"：把 IDF 的三个闭源 .a 全链进来，缺的开源侧
# 符号由我们自己的 port/s3/wifi_glue.c（OSI 适配 + libc/printf 出口）和
# port/s3/wifi_regulatory.c（监管域数据表）提供。
# 链接顺序用 --start-group 包起来：libnet80211 ↔ libpp 之间有强相互引用。
IDF_ROOT   ?= /media/winger/inst_26/esp/esp-idf
WIFI_LIBDIR := $(IDF_ROOT)/components/esp_wifi/lib/esp32s3
# 顺序无所谓：全部包在 --start-group 里（libnet80211 ↔ libpp ↔ libmesh 相互引用）
WIFI_LIBS   := $(WIFI_LIBDIR)/libnet80211.a $(WIFI_LIBDIR)/libpp.a \
               $(WIFI_LIBDIR)/libcore.a $(WIFI_LIBDIR)/libmesh.a \
               $(WIFI_LIBDIR)/libespnow.a
WIFI_DIR    := build/wifi
S3_WIFI_MAIN ?= tests/t_s3_wifi3.c6
# --wrap 让 wifi_glue.c 里的 __wrap_* 计数生效（init 走到哪一步一眼可见）
WIFI_WRAP := -Wl,--wrap=wifi_menuconfig_init -Wl,--wrap=misc_nvs_init \
             -Wl,--wrap=ic_create_wifi_task -Wl,--wrap=ieee80211_ioctl_init \
             -Wl,--wrap=pp_create_task -Wl,--wrap=pp_post \
             -Wl,--wrap=wifi_init_in_caller_task
# S3_FLASH_AUTO_MAP：镜像里有 IROM 段（闭源库代码在 flash 里）时必须打开 ——
# 没有二级 bootloader 时没人配置 I-cache 的模式，取指会拿到 IllegalInstruction
# （典型签名：PC = 函数首地址 + 2、cause=0）。见 docs/wifi-plan.md §15~24、§202。
WIFI_CDEFS = -DS3_WITH_WIFI -DS3_WITH_LIBPHY -DS3_KEEP_SWD -DS3_FLASH_AUTO_MAP

# 注意：目标文件的规则里**没有**把 S3_CDEFS 记成依赖，改了 -D 之后 make 不会
# 重编已有对象（踩过一次：S3_FLASH_AUTO_MAP 没生效，镜像行为完全没变）。
# 所以这里无条件全量重建。
wifi:
	@rm -rf $(WIFI_DIR)
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=$(WIFI_DIR) $(WIFI_DIR)/libphy_win.a
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=$(WIFI_DIR) S3_LEAN=1 \
		S3_SKIP_C6="" \
		S3_EXTRA_C6="kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 \
		             kernel/heap.c6 kernel/sem.c6 kernel/mutex.c6 kernel/queue.c6 kernel/event.c6" \
		S3_CDEFS="$(WIFI_CDEFS)" S3_STACK_TOP=0x3FCD9000 \
		S3_MAIN=$(S3_WIFI_MAIN) \
		S3_EXTRA_LIBS="-Wl,--start-group $(WIFI_DIR)/libphy_win.a $(WIFI_LIBS) -Wl,--end-group $(WIFI_WRAP)" \
		s3
wififlash: wifi
	./tools/flashmon.sh build/wifi/c6os.bin 15

# ---- 头文件依赖（见 X86CDEPFLAGS） ------------------------------------------
# 放在最后：此时所有 obj 目录规则都已定义，-include 只是把 .d 读进来当普通规则。
-include $(wildcard $(X86)/*/obj/*.d)
