# c6LOS —— ESP32-S3 构建
# 用法：make C6_ROOT=/path/to/c6 s3

C6_ROOT ?= $(HOME)/superc
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

XTFLAGS := -mabi=$(S3_ABI) $(S3_CDEFS) -Os -ffreestanding -fno-builtin -fno-common \
           -mlongcalls -mtext-section-literals -Wall -Wextra
PORT    ?= /dev/ttyACM0

BUILD   := build/s3
S3_MAIN ?= kernel/os.c6
S3_EXTRA_LIBS ?=
S3_C6FLAGS ?=
S3_EXTRA_C6 ?= kernel/task.c6 kernel/tick.c6 kernel/timer.c6 kernel/timeout.c6 kernel/heap.c6
S3_SKIP_C6 := kernel/heap.c6 kernel/sem.c6 kernel/queue.c6
S3_C6   := $(S3_MAIN) kernel/panic.c6 kernel/log.c6 $(filter-out $(S3_SKIP_C6),$(S3_EXTRA_C6))
S3_NAMES := $(notdir $(S3_C6))
S3_C    := $(addprefix $(BUILD)/c99/,$(S3_NAMES:.c6=.c))
# 内核/控制台等必需对象（LEAN 也保留）
S3_OBJ_CORE := $(BUILD)/obj/start.o \
           $(addprefix $(BUILD)/obj/,$(S3_NAMES:.c6=.o)) \
           $(BUILD)/obj/console.o $(BUILD)/obj/c6rt.o $(BUILD)/obj/kprintf.o \
           $(BUILD)/obj/ctx.o $(BUILD)/obj/ctx_asm.o \
           $(BUILD)/obj/irq.o $(BUILD)/obj/irq_asm.o \
           $(BUILD)/obj/marks.o $(BUILD)/obj/flash.o

# 面板/音频/UI 相关对象：LEAN=1 时全部去掉（给闭源库腾 IRAM）
S3_OBJ_FULL := $(BUILD)/obj/shell.o $(BUILD)/obj/cobs.o \
           $(BUILD)/obj/panel.o $(BUILD)/obj/s3fast.o \
           $(BUILD)/obj/button.o $(BUILD)/obj/i2s.o $(BUILD)/obj/gfx.o \
           $(BUILD)/obj/dash.o $(BUILD)/obj/core1.o

# IDF 闭源库的 ABI 桥与胶水：始终保留
S3_OBJ_WIFI := $(BUILD)/obj/idf_shim.o \
           $(BUILD)/obj/win_island.o $(BUILD)/obj/win_bridge.o \
           $(BUILD)/obj/irampad.o

ifeq ($(S3_LEAN),1)
S3_OBJ := $(S3_OBJ_CORE) $(S3_OBJ_WIFI)
else
S3_OBJ := $(S3_OBJ_CORE) $(S3_OBJ_FULL) $(S3_OBJ_WIFI)
endif

.PHONY: rt-host run-rt-host rv32bare-rt run-rv32bare-rt perf-host run-perf-host rv32bare-perf run-rv32bare-perf all s3 s3-src-check check-all check-sim rv32bare-tick run-rv32bare-tick rv32bare-smp run-rv32bare-smp rv32bare-smp-sem run-rv32bare-smp-sem s3-min check-host clean toolchain-info backup-flash flash image-info x86 run-x86 x86-sched run-x86-sched x86-tick run-x86-tick x86-preempt run-x86-preempt rv32 run-rv32 run-rv32-emu run-rv32-emu-sched rv32-sched run-rv32-sched rv32-tick run-rv32-tick
.PRECIOUS: $(S3_C)
.SECONDARY:

all: s3

s3: $(BUILD)/c6os.bin

S3_HDRS := $(wildcard include/c6os/*.h include/los/*.h)

define S3_C_RULE
$(BUILD)/c99/$(notdir $(1:.c6=.c)): $(1) $(S3_HDRS)
	@mkdir -p $(BUILD)/c99
	$(C6CC) -B $(C6_ROOT)/tcc -Iinclude $(S3_C6FLAGS) -c -o $(BUILD)/c99/ $(1)
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

$(BUILD)/obj/flash.o: port/s3/flash.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/marks.o: port/s3/marks.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/irampad.o: port/s3/irampad.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) $(XTFLAGS) -Iinclude -c $< -o $@

$(BUILD)/obj/win_island.o: port/s3/win_island.c $(S3_HDRS)
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=$(S3_SHIM_ABI) $(S3_CDEFS) -Os -ffreestanding -fno-builtin -fno-common -mlongcalls -mtext-section-literals -Iinclude -c $< -o $@

$(BUILD)/obj/win_bridge.o: port/s3/win_bridge.S
	@mkdir -p $(BUILD)/obj
	$(XTGCC) -mabi=call0 -mtext-section-literals -c $< -o $@

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

$(BUILD)/c6os.elf: $(S3_OBJ) port/s3/link.ld
	$(XTGCC) -mabi=$(S3_ABI) -nostdlib -Wl,-T,port/s3/link.ld \
		-o $@ $(S3_OBJ) $(S3_EXTRA_LIBS) -lgcc
	$(XTP)size $@

$(BUILD)/c6os.bin: $(BUILD)/c6os.elf
	esptool --chip esp32s3 elf2image --flash-mode dio \
		--flash-size 16MB --flash-freq 40m -o $@ $<

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
	esptool --chip esp32s3 elf2image --flash-mode dio --flash-size 16MB --flash-freq 40m -o $@ $<

toolchain-info:
	@echo "C6_ROOT = $(C6_ROOT)"
	@$(C6CC) -v 2>&1 | head -1 || true
	@$(XTGCC) --version | head -1
	@$(XTP)ld --version | head -1
	@esptool version | head -1

# ---- x86-64 参考目标（c6 -> LLVM IR，qemu 裸机） ----
X86        := build/x86
X86CFLAGS  := --target=x86_64-elf -ffreestanding -fno-pic -mno-red-zone -O1
X86IRFLAGS := --target=x86_64-unknown-linux-gnu -ffreestanding -mno-sse -mno-sse2
X86S_NAME  ?= sched
X86S_MAIN  ?= tests/t_x86_sched.c6
X86S_DIR   := $(X86)/$(X86S_NAME)
X86S_C6    := $(X86S_MAIN) kernel/task.c6 kernel/tick.c6 \
              kernel/timer.c6 kernel/log.c6 kernel/panic.c6 kernel/timeout.c6
X86S_LL    := $(addprefix $(X86S_DIR)/ir/,$(notdir $(X86S_C6:.c6=.ll)))
X86S_OBJ   := $(addprefix $(X86S_DIR)/obj/,$(notdir $(X86S_C6:.c6=.o))) \
              $(X86S_DIR)/obj/console.o $(X86S_DIR)/obj/ctx.o \
              $(X86S_DIR)/obj/ctx_asm.o $(X86S_DIR)/obj/irq.o \
              $(X86S_DIR)/obj/irq_asm.o $(X86S_DIR)/obj/kprintf.o \
              $(X86S_DIR)/obj/libc.o

x86: $(X86)/disk.img

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
		$(C6CC) -B $(C6_ROOT)/tcc -Iinclude --emit-llvm \
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

$(X86)/$(X86S_NAME)-disk.img: $(X86)/boot16.bin $(X86S_DIR)/flat.bin
	dd if=/dev/zero of=$@ bs=1M count=1 status=none
	dd if=$(X86)/boot16.bin of=$@ conv=notrunc status=none
	dd if=$(X86S_DIR)/flat.bin of=$@ bs=512 seek=1 conv=notrunc status=none


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
