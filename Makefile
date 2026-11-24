# c6LOS —— ESP32-S3 构建
# 用法：make C6_ROOT=/path/to/c6 s3

C6_ROOT ?= $(HOME)/c6
# c6 编译器：默认用 c6 自己那份（$(C6_ROOT)/tcc/c6）。
# 注意：新项目 c6c（/home/winger/c6/c6c/build/c6c）**不能**用来做这套构建 ——
# 它只出 native ELF/汇编，没有"输出 C99 源码"的模式，而我们的流程是
# c6→C99→xtensa-gcc。2026-11-16 老 c6 的可执行文件丢过一次，用
#   make -C /home/winger/c6/tcc -j8 c6
# 重新编出来即可。
C6CC    ?= $(C6_ROOT)/tcc/c6
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
S3_EXTRA_C6 ?= kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 \
               kernel/mutex.c6 kernel/event.c6 kernel/heap.c6
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
           $(BUILD)/obj/wifi_glue.o $(BUILD)/obj/wifi_stub.o \
           $(BUILD)/obj/wpa_crypto.o

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
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude -I$(C6_ROOT) $(S3_C6FLAGS) -DC6OS_S3_DIAG -c -o $(BUILD)/c99/ $(1)
	@sed -i -E 's/unsigned char (__c99_d[0-9]+)\[/unsigned char __attribute__((aligned(4))) \1[/g' $(BUILD)/c99/$(notdir $(1:.c6=.c))
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
# W2C 壳的两种实现（§231）：默认带着窗口 spill；S3_W2C_MIN=1 时换成极简版
# （只 entry+callx0+retw，不做任何窗口操作），用来对照真板上的窗口相关故障。
W2C_DEFS :=
ifneq ($(S3_W2C_MIN),)
W2C_DEFS += -DS3_W2C_MIN
endif
# S3_W2C_SAVEALL=1：壳保存/恢复全部 a0..a15（§245，理论正确版）
ifneq ($(S3_W2C_SAVEALL),)
W2C_DEFS += -DS3_W2C_SAVEALL
endif

$(BUILD)/obj/w2c.o: port/s3/w2c.S
	@mkdir -p $(BUILD)/obj
	# S3_CDEFS 必须传进来：全 windowed 构建里 w2c.S 要据此走「W2C 名字直接
	# .set 到 windowed 目标」的分支；否则仍生成 call0 壳，用 callx0 调 windowed
	# 内核函数 → a0 没有窗口标签 → 内核 retw 立刻 exccause=0（真板定死在
	# os_task_current_slot 的 retw，g_sw_cnt 才 1）。
	$(XTGCC) -Wa,--no-transform $(S3_CDEFS) $(W2C_DEFS) -c $< -o $@

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
# windowed 胶水的优化级别：默认 -Os；用 -O0 可以强制 GCC 在 retw 前显式恢复
# a0（§255 的对照实验），也可以用来排查"寄存器被 call0 改掉"类问题。
S3_SHIM_OPT ?= -Os

$(BUILD)/obj/wifi_glue.o: port/s3/wifi_glue.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) $(S3_SHIM_OPT) -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/wpa_crypto.o: port/s3/wpa_crypto.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) $(S3_SHIM_OPT) -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/win_island.o: port/s3/win_island.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) $(S3_SHIM_OPT) -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/win_bridge.o: port/s3/win_bridge.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=call0 $(S3_CDEFS) -Wa,--no-transform -mtext-section-literals -c $< -o $@

$(BUILD)/obj/wifi_stub.o: port/s3/wifi_stub.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) -Os -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/irq.o: port/s3/irq.c
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/irq_asm.o: port/s3/irq.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_ABI) -c $< -o $@

$(BUILD)/obj/ctx_asm.o: port/s3/ctx.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_ABI) $(S3_CDEFS) -c $< -o $@


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
# §289：应用基址。0 = 我们的镜像自己当二级 bootloader（ROM 直接引导，历史默认）；
# 0x20000 = 交给 IDF 真正的二级 bootloader 引导（先刷 bootloader@0x0 + 分区表@0x8000）。
S3_APP_BASE ?= 0x0
S3_APP_LD := -Wl,--defsym=__app_base=$(S3_APP_BASE)

# 全 windowed 构建：闭源库回调使用的 s3_w2c_* 名字直接用 --defsym 指向
# windowed 内核函数。GAS 的 .set 对跨对象符号无效、j 跳不到 IRAM，只有链接器
# 能在符号解析后把名字绑到目标；否则 w2c.S 会退回 call0 壳，用 callx0 调
# windowed 函数 → a0 无窗口标签 → 内核 retw exccause=0（g_sw_cnt=1 就崩）。
ifneq ($(findstring -DS3_ONE_ABI,$(S3_CDEFS)),)
S3_W2C_ALIAS := \
	-Wl,--defsym=s3_w2c_delay=os_task_delay \
	-Wl,--defsym=s3_w2c_wdt_feed=os_wdt_feed \
	-Wl,--defsym=s3_w2c_wdt_start=os_wdt_start \
	-Wl,--defsym=s3_w2c_irq_save=port_irq_save \
	-Wl,--defsym=s3_w2c_irq_restore=port_irq_restore \
	-Wl,--defsym=s3_w2c_sem_init=os_sem_init \
	-Wl,--defsym=s3_w2c_sem_destroy=os_sem_destroy \
	 \
	-Wl,--defsym=s3_w2c_sem_post=os_sem_post \
	-Wl,--defsym=s3_w2c_mutex_init=os_mutex_init \
	-Wl,--defsym=s3_w2c_mutex_destroy=os_mutex_destroy \
	 \
	-Wl,--defsym=s3_w2c_mutex_post=os_mutex_post \
	-Wl,--defsym=s3_w2c_queue_init=os_queue_init_ex \
	-Wl,--defsym=s3_w2c_queue_put=os_queue_put_item \
	-Wl,--defsym=s3_w2c_queue_get=os_queue_get_item \
	-Wl,--defsym=s3_w2c_queue_count=os_queue_count \
	-Wl,--defsym=s3_w2c_queue_try_get=os_queue_try_get_item \
	-Wl,--defsym=s3_w2c_queue_try_put=os_queue_try_put_item \
	-Wl,--defsym=s3_w2c_queue_destroy=os_queue_destroy \
	-Wl,--defsym=s3_w2c_event_init=os_event_init \
	-Wl,--defsym=s3_w2c_event_set=os_event_set \
	-Wl,--defsym=s3_w2c_event_clear=os_event_clear \
	 \
	-Wl,--defsym=s3_w2c_event_destroy=os_event_destroy \
	-Wl,--defsym=s3_w2c_event_get=os_event_get \
	-Wl,--defsym=s3_w2c_task_create=os_task_create \
	-Wl,--defsym=s3_w2c_task_delete=os_task_delete \
	-Wl,--defsym=s3_w2c_task_suspend=os_task_suspend \
	-Wl,--defsym=s3_w2c_task_resume=os_task_resume \
	 \
	-Wl,--defsym=s3_w2c_task_yield=os_task_yield \
	-Wl,--defsym=s3_w2c_tick_get=os_tick_get \
	-Wl,--defsym=s3_w2c_task_probe=os_task_probe \
	-Wl,--defsym=s3_w2c_tick_hz=os_tick_hz \
	-Wl,--defsym=s3_w2c_current_task=os_task_current_slot \
	-Wl,--defsym=s3_w2c_slot_of_id=os_task_slot_of_id
endif

$(BUILD)/c6os.elf: $(S3_OBJ) port/s3/link.ld
	$(XTGCC) -mabi=$(S3_ABI) -nostdlib -Wl,-T,port/s3/link.ld \
		$(S3_STACK_LD) $(S3_APP_LD) $(S3_W2C_ALIAS) \
		-o $@ $(S3_OBJ) $(S3_EXTRA_LIBS) -lgcc
	$(XTP)size $@

$(BUILD)/c6os.bin: $(BUILD)/c6os.elf
	esptool --chip esp32s3 elf2image $(S3_IMG_FLAGS) --flash-mode dio \
		--flash-size 16MB --flash-freq 40m -o $@ $<
	@python3 tools/chkimg.py $(BUILD)/c6os.elf $@ $(S3_APP_BASE)
	@python3 tools/patch_halinit.py $@ $(BUILD)/c6os.elf

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

$(BUILDH)/task.ll: kernel/task.c6
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
ifneq ($(filter user elf fs dyn mem vfs m9 m15 m16 m17 m18 m20 m21 m22 m23 m24 m25 m27 m35 m38 m39 m40 m41 m42 m43 m44 m45 m46 m47 m48 m49 m50 m51 m52 m53 m54 m55 m56 m57 m59 net ip ping udp tcp iso fatw alp,$(X86S_NAME)),)
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
ifneq ($(filter user elf fs dyn mem vfs m9 m15 m16 m17 m18 m20 m21 m22 m23 m24 m25 m27 m35 m38 m39 m40 m41 m42 m43 m44 m45 m46 m47 m48 m49 m50 m51 m52 m53 m54 m55 m56 m57 m59 net ip ping udp tcp iso fatw alp,$(X86S_NAME)),)
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
                  kernel/net/sal_raw.c6 \
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
              $(X86S_DIR)/obj/socket.o $(X86S_DIR)/obj/afunix.o \
              $(X86S_DIR)/obj/pty.o
endif

# L2/L3 载荷：再多带磁盘、ELF64 加载器与 initramfs。
ifneq ($(filter elf fs dyn vfs m9 m15 m16 m17 m18 m20 m21 m22 m23 m24 m25 m27 m35 m38 m39 m40 m41 m42 m43 m44 m45 m46 m47 m48 m49 m50 m51 m52 m53 m54 m55 m56 m57 m59 net ip ping udp tcp iso fatw,$(X86S_NAME)),)
X86S_OBJ   := $(addprefix $(X86S_DIR)/obj/,$(notdir $(X86S_C6:.c6=.o))) \
              $(X86S_DIR)/obj/console.o $(X86S_DIR)/obj/irq.o \
              $(X86S_DIR)/obj/irq_asm.o $(X86S_DIR)/obj/libc.o \
              $(X86S_MMU_OBJ) $(X86S_DIR)/obj/linux.o \
              $(X86S_DIR)/obj/user_entry.o $(X86S_DIR)/obj/ata.o \
              $(X86S_DIR)/obj/sys.o $(X86S_DIR)/obj/elf64.o \
              $(X86S_DIR)/obj/fs.o $(X86S_DIR)/obj/sig.o \
              $(X86S_DIR)/obj/proc.o $(X86S_DIR)/obj/tty.o $(X86S_DIR)/obj/vma.o \
              $(X86S_DIR)/obj/socket.o $(X86S_DIR)/obj/afunix.o $(X86S_DIR)/obj/fdt.o \
              $(X86S_DIR)/obj/pty.o
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
# c6 产出的 IR 里 triple 是 x86_64-pc-linux-gnu，而 clang 的目标是
# x86_64-unknown-linux-gnu —— 不改写就每条都报一次 -Woverride-module。
# 生成阶段统一 sed 成 clang 的目标（和 rv32 的两步做法一致）。
$(X86S_DIR)/ir/stamp: $(X86S_C6)
	@mkdir -p $(X86S_DIR)/ir
	for f in $^; do \
		$(C6CC) -B $(C6_ROOT)/tcc -Iinclude -DC6OS_NET=$(C6OS_NET) $(C6_DEFS) --emit-llvm \
			-o $(X86S_DIR)/ir/$$(basename $$f .c6).raw.ll $$f || exit 1; \
		sed -e 's|^target triple = .*|target triple = "x86_64-unknown-linux-gnu"|' \
			$(X86S_DIR)/ir/$$(basename $$f .c6).raw.ll \
			> $(X86S_DIR)/ir/$$(basename $$f .c6).ll || exit 1; \
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

$(X86S_DIR)/obj/pty.o: linux/pty.c
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
	ln -sfn /hello.txt $(FSROOT)/hello-link
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

# ---- M27：musl libc-test functional 子集 ----
M27_SRC  := tests/linux/m27libctest.c
M27_ELF  := $(X86)/m27libctest.elf
M27_FS   := $(X86)/m27fs.img
M27ROOT  := $(X86)/m27root

$(M27_ELF): $(M27_SRC)
	@mkdir -p $(dir $@)
	gcc -Os -s -o $@ $<

M35_SRC := tests/linux/m35fifo.c
M35_ELF := $(X86)/m35fifo.elf
$(M35_ELF): $(M35_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

$(X86)/m35-disk.img: $(M35_ELF) x86-dyn
	cp $(X86)/dyn-disk.img $@
	dd if=$(M35_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none

# ---- M38：waitid(2) ----
M38_SRC := tests/linux/m38waitid.c
M38_ELF := $(X86)/m38waitid.elf
$(M38_ELF): $(M38_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M39：statx ----
M39_SRC := tests/linux/m39statx.c
M39_ELF := $(X86)/m39statx.elf
$(M39_ELF): $(M39_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M40：copy_file_range ----
M40_SRC := tests/linux/m40cfr.c
M40_ELF := $(X86)/m40cfr.elf
$(M40_ELF): $(M40_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M41：AF_UNIX SOCK_STREAM ----
M41_SRC := tests/linux/m41unixstream.c
M41_ELF := $(X86)/m41unixstream.elf
$(M41_ELF): $(M41_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M42：AF_UNIX SCM_RIGHTS ----
M42_SRC := tests/linux/m42scmright.c
M42_ELF := $(X86)/m42scmright.elf
$(M42_ELF): $(M42_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M43：fallocate ----
M43_SRC := tests/linux/m43fallocate.c
M43_ELF := $(X86)/m43fallocate.elf
$(M43_ELF): $(M43_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M44：pwrite/preadv/pwritev ----
M44_SRC := tests/linux/m44posio.c
M44_ELF := $(X86)/m44posio.elf
$(M44_ELF): $(M44_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M45：pty ----
M45_SRC := tests/linux/m45pty.c
M45_ELF := $(X86)/m45pty.elf
$(M45_ELF): $(M45_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M46：AF_UNIX STREAM 字节流 ----
M46_SRC := tests/linux/m46stream.c
M46_ELF := $(X86)/m46stream.elf
$(M46_ELF): $(M46_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M47：epoll ET/ONESHOT ----
M47_SRC := tests/linux/m47epollflags.c
M47_ELF := $(X86)/m47epollflags.elf
$(M47_ELF): $(M47_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M48：AF_UNIX abstract namespace ----
M48_SRC := tests/linux/m48abstract.c
M48_ELF := $(X86)/m48abstract.elf
$(M48_ELF): $(M48_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M49：epoll_pwait ----
M49_SRC := tests/linux/m49epollpwait.c
M49_ELF := $(X86)/m49epollpwait.elf
$(M49_ELF): $(M49_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M50：pty per-instance termios ----
M50_SRC := tests/linux/m50ptytermios.c
M50_ELF := $(X86)/m50ptytermios.elf
$(M50_ELF): $(M50_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M51：资源/调度类 syscall ----
M51_SRC := tests/linux/m51rlimit.c
M51_ELF := $(X86)/m51rlimit.elf
$(M51_ELF): $(M51_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M52：文件同步与咨询锁 ----
M52_SRC := tests/linux/m52sync.c
M52_ELF := $(X86)/m52sync.elf
$(M52_ELF): $(M52_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M53：getitimer/setitimer ----
M53_SRC := tests/linux/m53itimer.c
M53_ELF := $(X86)/m53itimer.elf
$(M53_ELF): $(M53_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M54：readlink/faccessat/fchdir/accept4 ----
M54_SRC := tests/linux/m54dirfd.c
M54_ELF := $(X86)/m54dirfd.elf
$(M54_ELF): $(M54_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M55：rt_sigtimedwait + send/recv 号冲突修复 ----
M55_SRC := tests/linux/m55sigtimedwait.c
M55_ELF := $(X86)/m55sigtimedwait.elf
$(M55_ELF): $(M55_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M56：readlink 真实符号链接 ----
M56_SRC := tests/linux/m56readlink.c
M56_ELF := $(X86)/m56readlink.elf
$(M56_ELF): $(M56_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M57：chown/symlink/link/mknod 族 ----
M57_SRC := tests/linux/m57meta.c
M57_ELF := $(X86)/m57meta.elf
$(M57_ELF): $(M57_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

# ---- M59：socketpair(AF_UNIX)（busybox wget SSL helper 前置） ----
M59_SRC := tests/linux/m59socketpair.c
M59_ELF := $(X86)/m59socketpair.elf
$(M59_ELF): $(M59_SRC)
	@mkdir -p $(dir $@)
	gcc -static -Os -s -o $@ $<

$(M27_FS): tools/build-libctest.sh $(M27_ELF) /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 /usr/lib/x86_64-linux-gnu/libc.so.6 tools/mk-initramfs.py
	@rm -rf $(M27ROOT)
	@mkdir -p $(M27ROOT)/lt $(M27ROOT)/lib64 $(M27ROOT)/lib/x86_64-linux-gnu
	bash tools/build-libctest.sh $(M27ROOT)
	cp $(M27_ELF) $(M27ROOT)/lt/runner
	cp /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 $(M27ROOT)/lib64/ld-linux-x86-64.so.2
	cp /usr/lib/x86_64-linux-gnu/libc.so.6 $(M27ROOT)/lib/x86_64-linux-gnu/libc.so.6
	python3 tools/mk-initramfs.py $@ $(M27ROOT)

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
# minirootfs 不带 /etc/resolv.conf，musl 的 getaddrinfo 没有可用 nameserver，
# `ping qq.com` 就会报 bad address。打包前写进 slirp 的 DNS（10.0.2.3）。
# resolv.conf 必须单独做成目标：只改打包配方并不会让旧的 full.cpio 过期（make
# 只比依赖的时间戳），旧构建树拉到修复后仍带着没有 DNS 的 initramfs。这里每次
# 都检查内容，仅当缺失/内容不同才写；mtime 变了才触发 cpio 与 ISO 重打包。
ALPINE_RESOLV  := $(ALPINE_ROOT)/etc/resolv.conf
.PHONY: alpine-dns-force
alpine-dns-force:

# 依赖 busybox：tarball 更新时 busybox 那步会 rm -rf 整个 rootfs，必须等它
# 抽完再写 resolv.conf（并行 make 下也不能让两步抢同一个目录）。
$(ALPINE_RESOLV): $(ALPINE_ROOT)/bin/busybox alpine-dns-force
	@mkdir -p $(@D)
	@printf 'nameserver 10.0.2.3\n' | cmp -s - $@ 2>/dev/null \
	  || printf 'nameserver 10.0.2.3\n' > $@

# ---- 额外 apk 包（M64：默认装 curl）----
# minirootfs 只有 busybox，交互式 shell 里没有 curl。这里把 curl 的依赖闭包
# （libcurl / OpenSSL / c-ares / brotli / zstd / nghttp2 / idn2 / psl …）按
# APKINDEX 递归解出来、下载 .apk 后离线解包进 rootfs，客户机启动即可用。
# 已由 minirootfs 提供的包（musl/OpenSSL/zlib）保持自带版本，只补缺的。
# 不想带额外包：`make ALPINE_EXTRA_PKGS=`。
ALPINE_REPO       ?= https://dl-cdn.alpinelinux.org/alpine/v3.20
ALPINE_EXTRA_PKGS ?= curl
ALPINE_PKG_CACHE  := $(ALPINE_DIR)/pkgs
ALPINE_EXTRA_MARK := $(ALPINE_ROOT)/.extra-pkgs

# 标记文件放在 rootfs 内部：tarball 更新时 busybox 那步会 rm -rf 整个 rootfs，
# 标记跟着消失，额外包自然重装。内容没变时工具直接返回（不碰文件），避免每次
# make 都重新解包、把 ISO 也带着重打一遍。
$(ALPINE_EXTRA_MARK): $(ALPINE_ROOT)/bin/busybox tools/fetch-apk.py Makefile
	@python3 tools/fetch-apk.py --root $(ALPINE_ROOT) --cache $(ALPINE_PKG_CACHE) \
		--base $(ALPINE_REPO) --marker $@ $(ALPINE_EXTRA_PKGS)

$(ALPINE_CPIO): $(ALPINE_ROOT)/bin/busybox $(ALPINE_RESOLV) $(ALPINE_EXTRA_MARK) tools/mk-initramfs.py
	python3 tools/mk-initramfs.py $@ $(ALPINE_ROOT)

# 8MB FAT16，给客户机当 /data 写
$(ALPINE_DATA): tools/mkfat.py
	@mkdir -p $(ALPINE_DIR)
	python3 tools/mkfat.py $@ 8192

x86-alpine: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		C6OS_NET=1 \
		$(if $(filter 1,$(ALPINE_INTERACTIVE)),C6_DEFS=-DC6OS_ALPINE_INTERACTIVE) \
		$(ISO_KER)
	@mkdir -p $(ALPINE_DIR)
	python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_ISO)

# 交互式会话的串口挂 `-serial mon:stdio`（mux），**不能**用 `-serial stdio`：
# 后者让 QEMU 把宿主终端设成"自己接管 Ctrl-C"（host tty 的 ISIG 被清掉、^C
# 不再产生 SIGINT），然后 QEMU 自己解释这个字节 —— 一按 Ctrl-C 就把整个虚拟机
# SIGINT 掉（屏幕上 `qemu: terminating on signal 2`），客户机根本收不到 ^C。
#
# mon:stdio 保留 mux 的转义（Ctrl-A X 退出 QEMU），同时把 0x03 原样转给串口
# → 客户机行规程 → 前台进程组的 SIGINT。两种"退出"因此各归各位：
#   - 在 shell 里跑的程序：Ctrl-C 打断它，回到 / # 提示符；
#   - 想关掉 QEMU：Ctrl-A X（或 exit 退掉 shell）。
# 注意 mon:stdio 自己就带 mux，不能再加独立的 `-monitor none`。
# 批处理验收（check-*）仍然用 -serial stdio：它们不需要 ^C。
# 交互式额外挂 isa-debug-exit：guest 里 shell exit / 所有进程退出时内核写 0x501，
# QEMU 随之真正关机回到宿主终端（否则只停在 hlt、QEMU 进程不退，像"exit 退不出"）。
# 退出码是 (val<<1)|1 = 1，属预期；批处理 QEMU 没这个设备，写端口是无害 no-op。
QEMU_SERIAL_INTERACTIVE := -serial mon:stdio
QEMU_EXIT_DEVICE := -device isa-debug-exit,iobase=0x501,iosize=0x02

run-x86-alpine: x86-alpine
	-qemu-system-x86_64 -m 256M -cdrom $(ALPINE_ISO) -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic $(QEMU_SERIAL_INTERACTIVE) -no-reboot $(QEMU_NIC) $(QEMU_EXIT_DEVICE)

# 自己敲命令：起来就是 Alpine 的 /bin/sh 提示符（/ #），exit 或 Ctrl-D 退出。
run-x86-alpine-shell:
	@$(MAKE) ALPINE_INTERACTIVE=1 run-x86-alpine

# 验收：`sh -c '...'` 那段脚本的四件事都要在串口上看得到，而且 /data 里写下的
# 文件在宿主侧（数据盘镜像）能查出来 —— 证明真的落盘了。
# M28：真实 Alpine 程序冒烟（awk/sed/grep/sort/tar/gzip + 管道重定向）。
# 用独立的 ISO 名，避免覆盖 check-alpine 的常规镜像。
x86-alpine-apps: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		C6_DEFS=-DC6OS_ALPINE_APPS \
		$(ISO_KER)
	@python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_DIR)/apps.iso

# ---- 网络端到端：真实 Alpine busybox 的 ifconfig + ping ----
x86-alpine-net: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		C6OS_NET=1 C6_DEFS=-DC6OS_ALPINE_NET \
		$(ISO_KER)
	@python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_DIR)/net.iso

check-alpine-net: x86-alpine-net
	@timeout 200 qemu-system-x86_64 -m 256M -cdrom $(ALPINE_DIR)/net.iso -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-alpine-net.log >/dev/null
	@for tag in net-iface-ok net-ifaddr-ok net-ping-loop-ok net-ping-gw-ok net-dns-ok net-ping-host-ok net-ping-ext-ok net-done; do \
		grep -q "$$tag" /tmp/check-alpine-net.log \
		  || { echo "FAIL: alpine net（缺 $$tag）"; tail -60 /tmp/check-alpine-net.log; exit 1; }; \
	done
	@echo "PASS: alpine net（ifconfig eth0 + ping 127.0.0.1/10.0.2.2/8.8.8.8/qq.com + DNS）"

# ---- M63：真实 Alpine busybox wget 的 HTTPS 端到端（socketpair + ssl_client） ----
x86-alpine-wget: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		C6OS_NET=1 C6_DEFS=-DC6OS_ALPINE_WGET \
		$(ISO_KER)
	@python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_DIR)/wget.iso

check-alpine-wget: x86-alpine-wget
	@timeout 180 qemu-system-x86_64 -m 256M -cdrom $(ALPINE_DIR)/wget.iso -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-alpine-wget.log >/dev/null
	@grep -q "wget-https-ok" /tmp/check-alpine-wget.log \
	  || { echo "FAIL: alpine wget（HTTPS 下载失败）"; tail -60 /tmp/check-alpine-wget.log; exit 1; }
	@grep -q "wget-done" /tmp/check-alpine-wget.log \
	  || { echo "FAIL: alpine wget（脚本没跑完）"; tail -60 /tmp/check-alpine-wget.log; exit 1; }
	@echo "PASS: alpine wget（busybox HTTPS：socketpair + ssl_client + shutdown）"

# ---- M64：真实 Alpine curl（libcurl/OpenSSL/c-ares 动态链接）的 HTTPS 端到端 ----
# curl 由 ALPINE_EXTRA_PKGS 在构建 rootfs 时离线装进镜像（见 libcurl/apk 那段），
# 这里只负责起客户机跑一次真实下载：libcurl 自己解 URL、自己找 CA
# （/etc/ssl/certs/ca-certificates.crt）、自己轮询握手与响应。
x86-alpine-curl: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		C6OS_NET=1 C6_DEFS=-DC6OS_ALPINE_CURL \
		$(ISO_KER)
	@python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_DIR)/curl.iso

check-alpine-curl: x86-alpine-curl
	@timeout 220 qemu-system-x86_64 -m 256M -cdrom $(ALPINE_DIR)/curl.iso -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-alpine-curl.log >/dev/null
	@for tag in curl-version-ok curl-https-ok curl-file-ok curl-tty-ok curl-done; do \
		grep -q "$$tag" /tmp/check-alpine-curl.log \
		  || { echo "FAIL: alpine curl（缺 $$tag）"; tail -80 /tmp/check-alpine-curl.log; exit 1; }; \
	done
	@echo "PASS: alpine curl（libcurl + OpenSSL：https 下载到 /data，写 stdout 正常）"

check-alpine-apps: x86-alpine-apps
	@timeout 200 qemu-system-x86_64 -m 256M -cdrom $(ALPINE_DIR)/apps.iso -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-alpine-apps.log >/dev/null
	@grep -q "apps-cat-ok" /tmp/check-alpine-apps.log \
	  || { echo "FAIL: alpine apps（cat 读 FAT 文件失败）"; tail -40 /tmp/check-alpine-apps.log; exit 1; }
	@grep -q "apps-tar-ok" /tmp/check-alpine-apps.log \
	  || { echo "FAIL: alpine apps（tar 写 /data 失败）"; tail -40 /tmp/check-alpine-apps.log; exit 1; }
	@grep -q "apps-done" /tmp/check-alpine-apps.log \
	  || { echo "FAIL: alpine apps（脚本没跑完）"; tail -40 /tmp/check-alpine-apps.log; exit 1; }
	@echo "PASS: alpine apps（cat/tar + /data FAT 读写 + 句柄回收）"

# ---- 全量 busybox applet 矩阵：真实 Alpine busybox 3.20 的 304 个 applet ----
# 扫描脚本 tests/alpine/busybox-sweep.sh 拷进 initramfs 的 /etc/，由
# t_iso_alpine.c6 的 C6OS_ALPINE_BUSYBOX 分支用 `sh` 跑：
#   A 段 304 个 applet × 两种派发形式（argv[1] / argv[0] 符号链接）
#   F 段在 /data 上逐条验证真实输出（文本/文件/进程/压缩/网络/错误路径）
# 期望值 tests/alpine/busybox-baseline.tsv 是宿主上同一份 minirootfs 的结果，
# 差异即本内核的 Linux ABI 缺口；豁免清单与汇总在 tools/busybox-matrix.py。
# 独立 ISO 名，并在本目标里显式重打 cpio（其它 alpine 目标不感知这个文件）。
ALPINE_BX_SH  := $(ALPINE_ROOT)/etc/busybox-sweep.sh
ALPINE_BX_ISO := $(ALPINE_DIR)/busybox.iso

x86-alpine-busybox: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@mkdir -p $(ALPINE_ROOT)/etc
	cp tests/alpine/busybox-sweep.sh $(ALPINE_BX_SH)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		C6OS_NET=1 C6_DEFS=-DC6OS_ALPINE_BUSYBOX \
		$(ISO_KER)
	python3 tools/mk-initramfs.py $(ALPINE_CPIO) $(ALPINE_ROOT)
	@python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_BX_ISO)

# 总预算 1800s：F 段 140 多次 + A 段 608 次 fork + 动态装载，全程确定（无网络
# 等待），但单次 fork+exec 在本内核上约 1 秒，整轮 15 分钟左右。
# 引导/串口看门狗和重试逻辑在 tools/run-alpine-sweep.sh：串口日志必须直接重
# 定向到文件（`| tr | tee` 这条管道会在大约 4KB 处整体停住，客户机卡在串口
# 轮询里）；90s 没看到 alive 标记就杀掉重试，最多 3 次。
check-alpine-busybox: x86-alpine-busybox
	sh tools/run-alpine-sweep.sh $(ALPINE_BX_ISO) $(ALPINE_DATA) \
		/tmp/check-alpine-busybox.log bx-sweep-alive bx-sweep-done 1800 \
	  || { echo "FAIL: alpine busybox（扫描没跑完）"; tail -40 /tmp/check-alpine-busybox.log; exit 1; }
	python3 tools/busybox-matrix.py /tmp/check-alpine-busybox.log \
		tests/alpine/busybox-baseline.tsv

check-alpine:
	@rm -f $(ALPINE_DATA)
	@$(MAKE) x86-alpine
	@timeout 180 qemu-system-x86_64 -m 256M -cdrom $(ALPINE_ISO) -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot $(QEMU_NIC) 2>&1 \
		| tr -d '\r' | tee /tmp/check-alpine.log >/dev/null
	@grep -q "alpine-M12-alive" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（脚本没跑起来）"; tail -40 /tmp/check-alpine.log; exit 1; }
	@grep -q "m12ok" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（写 /data 再读回失败）"; tail -40 /tmp/check-alpine.log; exit 1; }
	@grep -q "alpine-cd-ok" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（挂载点 chdir：cd /data 失败）"; tail -40 /tmp/check-alpine.log; exit 1; }
	@grep -Eq "^/dev/ata0 .*/data$$" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（df -h 看不到 /data）"; grep -E "Filesystem|ata0|/data" /tmp/check-alpine.log; exit 1; }
	@grep -q "alpine-M12-done" /tmp/check-alpine.log \
	  || { echo "FAIL: alpine M12（脚本没跑完）"; tail -40 /tmp/check-alpine.log; exit 1; }
	@echo "PASS: alpine M12（Alpine minirootfs 从 ISO 起起来：sh -c 脚本跑完 + 写 /data + cd /data + df 列出 /data）"
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

# ---- M27：libc-test functional 子集 ----
x86-m27: caps-check-x86 $(M27_FS)
	$(MAKE) X86S_NAME=m27 X86S_MAIN=tests/t_x86_m27.c6 LINUX_ABI=1 \
		$(X86)/m27-disk.img

check-linuxabi-m27: x86-m27
	@timeout 120 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m27-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m27.log \
		| grep -q "^PASS: libc-test functional" \
	  && echo "PASS: linux-abi M27（musl libc-test functional 子集）" \
	  || { echo "FAIL: linux-abi M27"; tail -40 /tmp/check-m27.log; exit 1; }

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
else ifeq ($(X86S_NAME),m38)
DISK_DEPS  := $(M38_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M38_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m39)
DISK_DEPS  := $(M39_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M39_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m40)
DISK_DEPS  := $(M40_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M40_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m41)
DISK_DEPS  := $(M41_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M41_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m42)
DISK_DEPS  := $(M42_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M42_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m43)
DISK_DEPS  := $(M43_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M43_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m44)
DISK_DEPS  := $(M44_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M44_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m45)
DISK_DEPS  := $(M45_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M45_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m46)
DISK_DEPS  := $(M46_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M46_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m47)
DISK_DEPS  := $(M47_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M47_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m48)
DISK_DEPS  := $(M48_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M48_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m49)
DISK_DEPS  := $(M49_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M49_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m50)
DISK_DEPS  := $(M50_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M50_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m51)
DISK_DEPS  := $(M51_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M51_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m52)
DISK_DEPS  := $(M52_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M52_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m53)
DISK_DEPS  := $(M53_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M53_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m54)
DISK_DEPS  := $(M54_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M54_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m55)
DISK_DEPS  := $(M55_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M55_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m56)
DISK_DEPS  := $(M56_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M56_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m57)
DISK_DEPS  := $(M57_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M57_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m59)
DISK_DEPS  := $(M59_ELF) $(FS_IMG)
DISK_MB    := 16
DISK_APP   = dd if=$(M59_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(FS_IMG) of=$@ bs=512 seek=4096 conv=notrunc status=none
else ifeq ($(X86S_NAME),m27)
DISK_DEPS  := $(M27_ELF) $(M27_FS)
DISK_MB    := 16
DISK_APP   = dd if=$(M27_ELF) of=$@ bs=512 seek=1024 conv=notrunc status=none; \
             dd if=$(M27_FS) of=$@ bs=512 seek=4096 conv=notrunc status=none
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
             -Wl,--wrap=wifi_init_in_caller_task \
             -Wl,--wrap=ieee80211_ioctl_process -Wl,--wrap=wifi_scan_start_process -Wl,--wrap=ieee80211_sta_scan \
             -Wl,--wrap=pm_offchan_ready_or_defer -Wl,--wrap=chm_acquire_lock \
             -Wl,--wrap=config_is_cache_tx_buf_enabled \
             -Wl,--wrap=ppTask -Wl,--wrap=ieee80211_timer_process -Wl,--wrap=ieee80211_send_probereq -Wl,--wrap=ieee80211_regdomain_is_active_scan -Wl,--wrap=ieee80211_regdomain_check_channel -Wl,--wrap=scan_bitmap_to_channel_list \
             -Wl,--wrap=ic_tx_pkt
# §846: S3_TARGET_BSSID 时在 flash 侧 wrap get_ap_records，把扫描结果的 BSSID
# 改成目标 weforpay.com（wifi_glue.c 保持字节不变，避免破坏脆弱的同步扫描）
ifdef S3_TARGET_BSSID
WIFI_WRAP += -Wl,--wrap=cnx_add_rc
WIFI_WRAP += -Wl,--wrap=ieee80211_send_setup -Wl,--wrap=ieee80211_send_mgmt
WIFI_WRAP += -Wl,--wrap=sta_recv_mgmt
endif
# S3_FLASH_AUTO_MAP：镜像里有 IROM 段（闭源库代码在 flash 里）时必须打开 ——
# 没有二级 bootloader 时没人配置 I-cache 的模式，取指会拿到 IllegalInstruction
# （典型签名：PC = 函数首地址 + 2、cause=0）。见 docs/wifi-plan.md §15~24、§202。
WIFI_CDEFS = -DS3_WITH_WIFI -DS3_WITH_LIBPHY -DS3_KEEP_SWD -DS3_FLASH_AUTO_MAP
# S3_PHY_BISECT=1：打开 port/s3/win_island.c 里的"跨复位自动二分"（docs/wifi-plan.md §204）。
# 一进 PHY 就整片从 USB 消失、只能人工断电的阶段，用它一次上电拿到故障子步骤。
ifneq ($(S3_PHY_BISECT),)
WIFI_CDEFS += -DS3_PHY_BISECT
endif

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


check-linuxabi-m35: $(X86)/m35-disk.img
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m35-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m35.log \
		| grep -q "PASS: m35fifo" \
	  && echo "PASS: linux-abi M35（管道 fstat=S_IFIFO + isatty=ENOTTY）" \
	  || { echo "FAIL: linux-abi M35"; tail -20 /tmp/check-m35.log; exit 1; }

# ---- M38：waitid ----
x86-m38: caps-check-x86
	$(MAKE) X86S_NAME=m38 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m38-disk.img

check-linuxabi-m38: x86-m38
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m38-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m38.log \
		| grep -q "^PASS: m38waitid" \
	  && echo "PASS: linux-abi M38（waitid）" \
	  || { echo "FAIL: linux-abi M38"; tail -30 /tmp/check-m38.log; exit 1; }

# ---- M39：statx ----
x86-m39: caps-check-x86
	$(MAKE) X86S_NAME=m39 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m39-disk.img

check-linuxabi-m39: x86-m39
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m39-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m39.log \
		| grep -q "^PASS: m39statx" \
	  && echo "PASS: linux-abi M39（statx）" \
	  || { echo "FAIL: linux-abi M39"; tail -30 /tmp/check-m39.log; exit 1; }

# ---- M40：copy_file_range ----
x86-m40: caps-check-x86
	$(MAKE) X86S_NAME=m40 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m40-disk.img

check-linuxabi-m40: x86-m40
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m40-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m40.log \
		| grep -q "^PASS: m40cfr" \
	  && echo "PASS: linux-abi M40（copy_file_range）" \
	  || { echo "FAIL: linux-abi M40"; tail -30 /tmp/check-m40.log; exit 1; }

# ---- M41：AF_UNIX SOCK_STREAM ----
x86-m41: caps-check-x86
	$(MAKE) X86S_NAME=m41 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m41-disk.img

check-linuxabi-m41: x86-m41
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m41-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m41.log \
		| grep -q "^PASS: m41unixstream" \
	  && echo "PASS: linux-abi M41（AF_UNIX SOCK_STREAM）" \
	  || { echo "FAIL: linux-abi M41"; tail -30 /tmp/check-m41.log; exit 1; }

# ---- M42：AF_UNIX SCM_RIGHTS ----
x86-m42: caps-check-x86
	$(MAKE) X86S_NAME=m42 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m42-disk.img

check-linuxabi-m42: x86-m42
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m42-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m42.log \
		| grep -q "^PASS: m42scmright" \
	  && echo "PASS: linux-abi M42（AF_UNIX SCM_RIGHTS）" \
	  || { echo "FAIL: linux-abi M42"; tail -30 /tmp/check-m42.log; exit 1; }

# ---- M43：fallocate ----
x86-m43: caps-check-x86
	$(MAKE) X86S_NAME=m43 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m43-disk.img

check-linuxabi-m43: x86-m43
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m43-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m43.log \
		| grep -q "^PASS: m43fallocate" \
	  && echo "PASS: linux-abi M43（fallocate）" \
	  || { echo "FAIL: linux-abi M43"; tail -30 /tmp/check-m43.log; exit 1; }

# ---- M44：pwrite/preadv/pwritev ----
x86-m44: caps-check-x86
	$(MAKE) X86S_NAME=m44 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m44-disk.img

check-linuxabi-m44: x86-m44
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m44-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m44.log \
		| grep -q "^PASS: m44posio" \
	  && echo "PASS: linux-abi M44（pwrite/preadv/pwritev）" \
	  || { echo "FAIL: linux-abi M44"; tail -30 /tmp/check-m44.log; exit 1; }

# ---- M45：pty ----
x86-m45: caps-check-x86
	$(MAKE) X86S_NAME=m45 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 \
		$(X86)/m45-disk.img

check-linuxabi-m45: x86-m45
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m45-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m45.log \
		| grep -q "^PASS: m45pty" \
	  && echo "PASS: linux-abi M45（pty）" \
	  || { echo "FAIL: linux-abi M45"; tail -30 /tmp/check-m45.log; exit 1; }

# ---- M46：AF_UNIX STREAM 字节流 ----
x86-m46: caps-check-x86
	$(MAKE) X86S_NAME=m46 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m46-disk.img

check-linuxabi-m46: x86-m46
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m46-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m46.log \
		| grep -q "^PASS: m46stream" \
	  && echo "PASS: linux-abi M46（AF_UNIX STREAM 字节流）" \
	  || { echo "FAIL: linux-abi M46"; tail -30 /tmp/check-m46.log; exit 1; }

# ---- M47：epoll ET/ONESHOT ----
x86-m47: caps-check-x86
	$(MAKE) X86S_NAME=m47 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m47-disk.img

check-linuxabi-m47: x86-m47
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m47-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m47.log \
		| grep -q "^PASS: m47epollflags" \
	  && echo "PASS: linux-abi M47（epoll ET/ONESHOT）" \
	  || { echo "FAIL: linux-abi M47"; tail -30 /tmp/check-m47.log; exit 1; }

# ---- M48：AF_UNIX abstract namespace ----
x86-m48: caps-check-x86
	$(MAKE) X86S_NAME=m48 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m48-disk.img

check-linuxabi-m48: x86-m48
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m48-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m48.log \
		| grep -q "^PASS: m48abstract" \
	  && echo "PASS: linux-abi M48（AF_UNIX abstract namespace）" \
	  || { echo "FAIL: linux-abi M48"; tail -30 /tmp/check-m48.log; exit 1; }

# ---- M49：epoll_pwait ----
x86-m49: caps-check-x86
	$(MAKE) X86S_NAME=m49 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m49-disk.img

check-linuxabi-m49: x86-m49
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m49-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m49.log \
		| grep -q "^PASS: m49epollpwait" \
	  && echo "PASS: linux-abi M49（epoll_pwait）" \
	  || { echo "FAIL: linux-abi M49"; tail -30 /tmp/check-m49.log; exit 1; }

# ---- M50：pty per-instance termios ----
x86-m50: caps-check-x86
	$(MAKE) X86S_NAME=m50 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m50-disk.img

check-linuxabi-m50: x86-m50
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m50-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m50.log \
		| grep -q "^PASS: m50ptytermios" \
	  && echo "PASS: linux-abi M50（pty per-instance termios）" \
	  || { echo "FAIL: linux-abi M50"; tail -30 /tmp/check-m50.log; exit 1; }

# ---- M51：资源/调度类 syscall ----
x86-m51: caps-check-x86
	$(MAKE) X86S_NAME=m51 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m51-disk.img

check-linuxabi-m51: x86-m51
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m51-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m51.log \
		| grep -q "^PASS: m51rlimit" \
	  && echo "PASS: linux-abi M51（资源/调度类 syscall）" \
	  || { echo "FAIL: linux-abi M51"; tail -30 /tmp/check-m51.log; exit 1; }

# ---- M52：文件同步与咨询锁 ----
x86-m52: caps-check-x86
	$(MAKE) X86S_NAME=m52 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m52-disk.img

check-linuxabi-m52: x86-m52
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m52-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m52.log \
		| grep -q "^PASS: m52sync" \
	  && echo "PASS: linux-abi M52（文件同步与咨询锁）" \
	  || { echo "FAIL: linux-abi M52"; tail -30 /tmp/check-m52.log; exit 1; }

# ---- M53：getitimer/setitimer ----
x86-m53: caps-check-x86
	$(MAKE) X86S_NAME=m53 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m53-disk.img

check-linuxabi-m53: x86-m53
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m53-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m53.log \
		| grep -q "^PASS: m53itimer" \
	  && echo "PASS: linux-abi M53（getitimer/setitimer）" \
	  || { echo "FAIL: linux-abi M53"; tail -30 /tmp/check-m53.log; exit 1; }

# ---- M54：readlink/faccessat/fchdir/accept4 ----
x86-m54: caps-check-x86
	$(MAKE) X86S_NAME=m54 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m54-disk.img

check-linuxabi-m54: x86-m54
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m54-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m54.log \
		| grep -q "^PASS: m54dirfd" \
	  && echo "PASS: linux-abi M54（readlink/faccessat/fchdir/accept4）" \
	  || { echo "FAIL: linux-abi M54"; tail -30 /tmp/check-m54.log; exit 1; }

# ---- M55：rt_sigtimedwait ----
x86-m55: caps-check-x86
	$(MAKE) X86S_NAME=m55 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m55-disk.img

check-linuxabi-m55: x86-m55
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m55-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m55.log \
		| grep -q "^PASS: m55sigtimedwait" \
	  && echo "PASS: linux-abi M55（rt_sigtimedwait）" \
	  || { echo "FAIL: linux-abi M55"; tail -30 /tmp/check-m55.log; exit 1; }

# ---- M56：readlink 真实符号链接 ----
x86-m56: caps-check-x86
	$(MAKE) X86S_NAME=m56 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m56-disk.img

check-linuxabi-m56: x86-m56
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m56-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m56.log \
		| grep -q "^PASS: m56readlink" \
	  && echo "PASS: linux-abi M56（readlink 真实符号链接）" \
	  || { echo "FAIL: linux-abi M56"; tail -30 /tmp/check-m56.log; exit 1; }

# ---- M57：chown/symlink/link/mknod 族 ----
x86-m57: caps-check-x86
	$(MAKE) X86S_NAME=m57 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m57-disk.img

check-linuxabi-m57: x86-m57
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m57-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m57.log \
		| grep -q "^PASS: m57meta" \
	  && echo "PASS: linux-abi M57（chown/symlink/link/mknod 族）" \
	  || { echo "FAIL: linux-abi M57"; tail -30 /tmp/check-m57.log; exit 1; }

# ---- M58：/proc 端到端（busybox cat/ps/top via sendfile 内核缓冲） ----
x86-alpine-proc: caps-check-x86 $(CD0_BIN) $(ALPINE_CPIO) $(ALPINE_DATA)
	@rm -rf $(ISO_DIR)
	@mkdir -p $(ISO_DIR)
	$(MAKE) X86S_NAME=iso X86S_MAIN=tests/t_iso_alpine.c6 LINUX_ABI=1 C6OS_VFS=1 \
		C6_DEFS=-DC6OS_ALPINE_PROC \
		$(ISO_KER)
	@python3 tools/mkiso.py --boot $(CD0_BIN) --kernel $(ISO_KER) \
		--rootfs $(ALPINE_CPIO) -o $(ALPINE_DIR)/proc.iso

check-linuxabi-m58: x86-alpine-proc
	@timeout 200 qemu-system-x86_64 -m 256M -cdrom $(ALPINE_DIR)/proc.iso -boot d \
		-drive format=raw,file=$(ALPINE_DATA),if=ide \
		-nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m58.log >/dev/null
	@for tag in m58-cpuinfo-ok m58-meminfo-ok m58-procdir-ok m58-ps-ok m58-top-ok m58-done; do \
		grep -q "$$tag" /tmp/check-m58.log \
		  || { echo "FAIL: linux-abi M58（缺 $$tag）"; tail -40 /tmp/check-m58.log; exit 1; }; \
	done
	@grep -q "processor" /tmp/check-m58.log \
	  || { echo "FAIL: linux-abi M58（cpuinfo 为空）"; tail -40 /tmp/check-m58.log; exit 1; }
	@grep -q "PID.*COMMAND" /tmp/check-m58.log \
	  || { echo "FAIL: linux-abi M58（ps 无表头）"; tail -40 /tmp/check-m58.log; exit 1; }
	@echo "PASS: linux-abi M58（/proc 端到端：cat/stat/ps/top）"

# ---- M59：socketpair(AF_UNIX)（busybox wget SSL helper 前置） ----
x86-m59: caps-check-x86
	$(MAKE) X86S_NAME=m59 X86S_MAIN=tests/t_x86_fs.c6 LINUX_ABI=1 C6OS_VFS=1 \
		$(X86)/m59-disk.img

check-linuxabi-m59: x86-m59
	@timeout 60 qemu-system-x86_64 -m 256M \
		-drive format=raw,file=$(X86)/m59-disk.img,if=ide \
		-boot c -nographic -serial stdio -monitor none -no-reboot 2>&1 \
		| tr -d '\r' | tee /tmp/check-m59.log \
		| grep -q "^PASS: m59socketpair" \
	  && echo "PASS: linux-abi M59（socketpair）" \
	  || { echo "FAIL: linux-abi M59"; tail -30 /tmp/check-m59.log; exit 1; }
# ---------------------------------------------------------------------------
# lwIP（IP 栈）—— 见 docs/wifi-plan.md §207
#
# 这一步**不**把 lwIP 接进 c6os.elf：WiFi 关联本身还没在真板上跑通，先接进去
# 只会让镜像多一堆无法验证的代码。这里只做两件事，都可以离线验证：
#   1) 用我们自己的 port/s3/lwip/{lwipopts.h,arch/cc.h} 把 IDF 的 lwIP 源码
#      编成 build/lwip/liblwip.a（证明工具链/可移植性没问题）；
#   2) 让 port/s3/lwip/c6_lwip_port.c（平台钩子 + netif 粘合）一起编进来，
#      这样"要接的面"是编译期就确认过的，而不是等上板才发现缺符号。
#
# 为什么用 -mabi=windowed：linkoutput 要调 libnet80211 的
# esp_wifi_internal_tx()（windowed ABI）。内核侧（call0）要调这些函数时
# 一律走 port/s3/w2c.S 的正规壳，和现有 WiFi 岛的做法一致。
LWIP_SRC  := $(IDF_ROOT)/components/lwip/lwip/src
LWIP_DIR  := build/lwip
LWIP_INC  := -Iport/s3/lwip -I$(LWIP_SRC)/include
# 注意：这里**不能**加 -Wa,--no-transform。加上之后 gas 不再做分支松弛，
# 编译器在 -Os 下生成的 bnez.n（窄分支）一旦位移超过 ±127 就报
#   Error: operand 2 of 'bnez.n' has out of range value '4294967280'
# （def.c 就是这么挂的）。--no-transform 只是我们给手写汇编（win_bridge.S）
# 防 retw 缩窄用的，编译器生成的 windowed 代码不需要它。
LWIP_CF   := -mabi=windowed -Os -ffreestanding -fno-builtin -fno-common \
             -mlongcalls -mtext-section-literals \
             -Wall -Wextra -Wno-unused-parameter
ifdef S3_TARGET_BSSID
LWIP_CF   += -DS3_TARGET_BSSID
endif
LWIP_SRCS := $(wildcard $(LWIP_SRC)/core/*.c) \
             $(wildcard $(LWIP_SRC)/core/ipv4/*.c) \
             $(wildcard $(LWIP_SRC)/api/*.c) \
             $(wildcard $(LWIP_SRC)/netif/ethernet.c) \
             port/s3/lwip/c6_lwip_port.c \
             port/s3/lwip/c6_lwip_posix.c \
             port/s3/lwip/sys_arch.c
LWIP_HDRS := port/s3/lwip/lwipopts.h port/s3/lwip/arch/cc.h port/s3/lwip/arch/sys_arch.h

define C6_LWIP_RULE
$(LWIP_DIR)/obj/$(notdir $(basename $(1))).o: $(1) $(LWIP_HDRS)
	@mkdir -p $(LWIP_DIR)/obj
	$$(XTGCC) $(LWIP_CF) $(LWIP_INC) -c $$< -o $$@
endef
$(foreach f,$(LWIP_SRCS),$(eval $(call C6_LWIP_RULE,$(f))))

LWIP_OBJS := $(addprefix $(LWIP_DIR)/obj/,$(addsuffix .o,$(notdir $(basename $(LWIP_SRCS)))))

lwip: $(LWIP_DIR)/liblwip.a
	@echo "lwip: $(words $(LWIP_OBJS)) 个对象 -> $<"

$(LWIP_DIR)/liblwip.a: $(LWIP_OBJS)
	@mkdir -p $(dir $@)
	$(XTP)ar rcs $@ $^
	@$(XTP)size $(LWIP_DIR)/obj/*.o | tail -1

# 编译期冒烟测试：c6 侧用我们的 POSIX socket 头能不能编（不进镜像，纯检查）
# 见 docs/wifi-plan.md §209。
netcheck:
	@mkdir -p $(BUILD)/netcheck
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude -I$(C6_ROOT) -Iport/s3/lwip/posix \
		-c -o $(BUILD)/netcheck/ tests/t_s3_net.c6
	@$(XTGCC) -mabi=windowed -Os -ffreestanding -fno-builtin -fno-common -mlongcalls \
		-Iport/s3/lwip -I$(LWIP_SRC)/include -c $(BUILD)/netcheck/t_s3_net.c -o $(BUILD)/netcheck/t_s3_net.o
	@echo "netcheck: OK（c6 头 + lwIP POSIX 层两侧都能编）"

# ---------------------------------------------------------------------------
# wifip：WiFi + lwIP 一起链的镜像（docs/wifi-plan.md §210）
#
# 和 wifi 的区别只有两点：-DS3_WITH_LWIP（bringup 成功后接 IP 栈），
# 以及把 build/lwip/liblwip.a 加进链接。目的有两个，都不需要真板：
#   1) 验证"71KB 的 lwIP 和闭源 WiFi 库能一起塞进这颗芯片"（IRAM/DRAM 预算）；
#   2) 验证 link.ld 把 lwIP 的 .text 正确路由进 .flash.text（否则 IRAM 直接爆）。
IP_CDEFS = $(WIFI_CDEFS) -DS3_WITH_LWIP
wifip: 
	@rm -rf $(WIFI_DIR)
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=$(WIFI_DIR) $(WIFI_DIR)/libphy_win.a
	$(MAKE) C6_ROOT=$(C6_ROOT) lwip
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=$(WIFI_DIR) S3_LEAN=1 \
		S3_SKIP_C6="" \
		S3_EXTRA_C6="kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 \
		             kernel/heap.c6 kernel/sem.c6 kernel/mutex.c6 kernel/queue.c6 kernel/event.c6" \
		S3_CDEFS="$(IP_CDEFS)" S3_STACK_TOP=0x3FCD9000 \
		S3_MAIN=$(S3_WIFI_MAIN) \
		S3_EXTRA_LIBS="-Wl,--start-group $(WIFI_DIR)/libphy_win.a build/lwip/liblwip.a $(WIFI_LIBS) -Wl,--end-group $(WIFI_WRAP)" \
		s3

# TLS 1.2 基础件自测（宿主上跑，不需要板子）：docs/wifi-plan.md §212
tls12check:
	./tools/tls12_selftest.sh

# 宿主上真连 qq.com 跑 TLS 1.2 握手 + HTTP GET（不需要板子）：§213
tls12live:
	./tools/tls12_live.sh

# ---------------------------------------------------------------------------
# TLS 1.2 客户端（.c6 写的）编成 windowed 静态库 —— 见 docs/wifi-plan.md §214
#
# 为什么要 windowed：它要和 lwIP（windowed，§210）以及闭源 WiFi 库直接互相调用，
# 而内核是 call0。同一侧（windowed）内部调用没有 ABI 混用问题。
#
# 三个步骤：
#   1) 剥掉 c6 标准库模块里的 `test "…" { … }` 自测块（否则会拖进 c6 切片运行时，
#      详见 tools/c6_strip_tests.py 的说明）；
#   2) c6c 编成 .c99；
#   3) xtensa-gcc 按 -mabi=windowed 编成对象，归档成 build/wifi/tls/libtls.a。
TLS_DIR   := $(WIFI_DIR)/tls
TLS_SRC   := $(C6_ROOT)/std/core/types.c6 $(C6_ROOT)/std/core/bits.c6 \
             $(C6_ROOT)/std/core/rand.c6 $(C6_ROOT)/std/encoding/aes.c6 \
             $(C6_ROOT)/std/encoding/sha256.c6 $(C6_ROOT)/std/encoding/sha1.c6 \
             $(C6_ROOT)/std/net/x25519.c6 \
             net/aes_gcm.c6 net/tls12_prf.c6 net/rsa_verify.c6 \
             net/cert_parse.c6 net/tls12_hs.c6
TLS_NAMES := $(notdir $(basename $(TLS_SRC)))
TLS_OBJS  := $(addprefix $(TLS_DIR)/obj/,$(addsuffix .o,$(TLS_NAMES)))

define C6_TLS_RULES
$(TLS_DIR)/src/$(notdir $(1)): $(1) tools/c6_strip_tests.py
	@mkdir -p $(TLS_DIR)/src
	python3 tools/c6_strip_tests.py $$< $$@
$(TLS_DIR)/c99/$(notdir $(basename $(1))).c: $(TLS_DIR)/src/$(notdir $(1)) $(S3_HDRS)
	@mkdir -p $(TLS_DIR)/c99
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude -I$(C6_ROOT) -Iport/s3/lwip/posix \
		-c -o $(TLS_DIR)/c99/ $$<
$(TLS_DIR)/obj/$(notdir $(basename $(1))).o: $(TLS_DIR)/c99/$(notdir $(basename $(1))).c
	@mkdir -p $(TLS_DIR)/obj
	$(XTGCC) -mabi=windowed -Os -ffreestanding -fno-builtin -fno-common \
		-mlongcalls -mtext-section-literals -Iinclude -c $$< -o $$@
endef
$(foreach src,$(TLS_SRC),$(eval $(call C6_TLS_RULES,$(src))))

$(TLS_DIR)/libtls.a: $(TLS_OBJS)
	@mkdir -p $(dir $@)
	$(XTP)ar rcs $@ $^
	@echo "libtls.a: $(words $(TLS_OBJS)) 个对象"

# 板级 HTTPS 演示（windowed C）：bringup 之后用 lwIP socket 跑 TLS 1.2 + HTTP GET
$(WIFI_DIR)/obj/c6_https_demo.o: port/s3/c6_https_demo.c
	@mkdir -p $(WIFI_DIR)/obj
	$(XTGCC) -mabi=windowed -DS3_WITH_WIFI $(S3_CDEFS) -Os -ffreestanding \
		-fno-builtin -fno-common -mlongcalls -mtext-section-literals \
		-Iinclude -Iport/s3/lwip -I$(LWIP_SRC)/include -c $< -o $@

# https：在 wifip 的基础上加 TLS 1.2 客户端 + 板级 HTTPS 演示
# WiFi 账号：make S3_WIFI_SSID='"名字"' S3_WIFI_PASS='"口令"'（见 port/s3/wifi_creds.h）
# 没传就不定义，交给 port/s3/wifi_creds.h 里的默认（空串 = 跳过关联）
WIFI_CREDS = $(if $(S3_WIFI_SSID),-DS3_WIFI_SSID=$(S3_WIFI_SSID)) \
             $(if $(S3_WIFI_PASS),-DS3_WIFI_PASS=$(S3_WIFI_PASS))
# PHY 校准模式（win_island.c 的 S3_PHY_CAL_MODE）：0=PARTIAL（默认）1=NONE 2=FULL。
# 排查"register_chipv7_phy 挂死"时可以用 make ... S3_PHY_CAL_MODE=1 换个路径试试。
ifneq ($(S3_PHY_CAL_MODE),)
WIFI_CDEFS += -DS3_PHY_CAL_MODE=$(S3_PHY_CAL_MODE)
endif
# S3_PHY_VIA_OSI=1：不在 bringup 之前手动做 PHY 初始化，改成让 WiFi 库通过
# OSI 的 _phy_enable 回调我们（IDF 的做法，§228）
ifneq ($(S3_PHY_VIA_OSI),)
WIFI_CDEFS += -DS3_PHY_VIA_OSI
endif
# S3_NO_PREEMPT=1：关掉定时器抢占切换，改成只在显式阻塞点切换（§235）
ifneq ($(S3_NO_PREEMPT),)
WIFI_CDEFS += -DS3_NO_PREEMPT
endif
# S3_NO_WIN_SPILL=1：w2c 壳不再 spill 祖先窗口（对照实验，§228）
ifneq ($(S3_NO_WIN_SPILL),)
WIFI_CDEFS += -DS3_NO_WIN_SPILL
endif
HTTPS_CDEFS = $(WIFI_CDEFS) -DS3_WITH_LWIP -DS3_WITH_HTTPS $(WIFI_CREDS)
https:
	@rm -rf $(WIFI_DIR)
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=$(WIFI_DIR) $(WIFI_DIR)/libphy_win.a
	$(MAKE) C6_ROOT=$(C6_ROOT) lwip
	$(MAKE) C6_ROOT=$(C6_ROOT) BUILD=$(WIFI_DIR) S3_LEAN=1 \
		S3_SKIP_C6="" \
		S3_EXTRA_C6="kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 \
		             kernel/heap.c6 kernel/sem.c6 kernel/mutex.c6 kernel/queue.c6 kernel/event.c6" \
		S3_CDEFS="$(HTTPS_CDEFS) $(S3_CDEFS)" S3_STACK_TOP=0x3FCD9500 \
		S3_MAIN=$(S3_WIFI_MAIN) \
		$(WIFI_DIR)/obj/c6_https_demo.o $(TLS_DIR)/libtls.a \
		S3_EXTRA_LIBS="-Wl,--start-group $(WIFI_DIR)/libphy_win.a build/lwip/liblwip.a $(TLS_DIR)/libtls.a $(WIFI_DIR)/obj/c6_https_demo.o $(WIFI_LIBS) -Wl,--end-group $(WIFI_WRAP)" \
		s3

# ---- 一键上板（§217）--------------------------------------------------------
# bringup：先刷"自动二分 + PHY 进度打印"的镜像（板子回来第一步用这个）
bringup:
	$(MAKE) C6_ROOT=$(C6_ROOT) S3_PHY_BISECT=1 wifi
	./tools/s3run.sh $(WIFI_DIR)/c6os.bin 60

# httpsflash：刷完整镜像（WiFi + lwIP + TLS），账号用 S3_WIFI_SSID/S3_WIFI_PASS 传
httpsflash:
	$(MAKE) C6_ROOT=$(C6_ROOT) https
	./tools/s3run.sh $(WIFI_DIR)/c6os.bin 120

# ---------------------------------------------------------------------------
# lwipcheck：宿主上验证 lwIP 线程模型 + 我们的 sys_arch（docs/wifi-plan.md §227）
#
# 板子上除了 WiFi/PHY（闭源库）以外的软件层 —— lwIP、sys_arch、tcpip 线程、
# tcpip_input 收包、BSD socket —— 都能在这里跑起来：两个 netif 交叉互联，
# 跑一次完整的 TCP 回显。
LWIP_HOST_DIR := build/hostlwip
LWIP_HOST_SRCS := $(wildcard $(LWIP_SRC)/core/*.c) \
                  $(wildcard $(LWIP_SRC)/core/ipv4/*.c) \
                  $(wildcard $(LWIP_SRC)/api/*.c) \
                  $(LWIP_SRC)/netif/ethernet.c
lwipcheck:
	@mkdir -p $(LWIP_HOST_DIR)
	cc -O2 -g -Wall -Wno-unused-parameter -Wno-address-of-packed-member \
	   -DC6_LWIP_HOST -Iport/s3/lwip -Iport/s3/lwip/host -I$(LWIP_SRC)/include \
	   -o $(LWIP_HOST_DIR)/sys_arch_test \
	   $(LWIP_HOST_SRCS) port/s3/lwip/sys_arch.c \
	   port/s3/lwip/host/arch_stub.c tests/sys_arch_host_test.c
	./$(LWIP_HOST_DIR)/sys_arch_test
