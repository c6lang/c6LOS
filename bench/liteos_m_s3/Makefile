# LiteOS-M ESP32-S3（LX7）最小基线构建
# 依赖：LITEOS_ROOT（kernel_liteos_m）、SECUREC_ROOT（third_party_bounds_checking_function）
LITEOS_ROOT ?= /tmp/liteos_m
SECUREC_ROOT ?= /tmp/securec
XT = $(HOME)/.c6os/toolchain/xtensa-esp-elf/bin/xtensa-esp32s3-elf-
CC = $(XT)gcc
LD = $(XT)ld
OBJ = build/obj
BIN = build/liteos_s3

CFLAGS = -mabi=windowed -mlongcalls -mtext-section-literals -Os -ffreestanding \
         -fno-builtin -Wno-implicit-function-declaration
INC = -I. -I$(LITEOS_ROOT)/kernel/include -I$(LITEOS_ROOT)/arch/include \
      -I$(LITEOS_ROOT)/arch/xtensa/common -I$(LITEOS_ROOT)/arch/xtensa/lx6/gcc \
      -I$(LITEOS_ROOT)/utils -I$(SECUREC_ROOT)/include
KSRC = $(wildcard $(LITEOS_ROOT)/kernel/src/*.c) $(wildcard $(LITEOS_ROOT)/kernel/src/mm/*.c)
ASRC = $(LITEOS_ROOT)/arch/xtensa/common/los_common_interrupt.c
XSRC = $(wildcard $(LITEOS_ROOT)/arch/xtensa/lx6/gcc/*.c)
USRC = $(LITEOS_ROOT)/utils/los_debug.c $(LITEOS_ROOT)/utils/los_error.c $(LITEOS_ROOT)/utils/los_hook.c
SSRC = $(SECUREC_ROOT)/src/memcpy_s.c $(SECUREC_ROOT)/src/memset_s.c $(SECUREC_ROOT)/src/strncpy_s.c

.PHONY: all clean image
all: $(BIN).elf
	$(XT)size $<
image: all
	esptool --chip esp32s3 elf2image --flash-mode dio --flash-size 16MB \
		--flash-freq 40m -o $(BIN).bin $(BIN).elf
	esptool --chip esp32s3 image-info $(BIN).bin | head -20

$(OBJ):
	@mkdir -p $@/mm

$(OBJ)/%.o: $(LITEOS_ROOT)/kernel/src/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/%.o: $(LITEOS_ROOT)/kernel/src/mm/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/%.o: $(LITEOS_ROOT)/arch/xtensa/common/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/%.o: $(LITEOS_ROOT)/arch/xtensa/lx6/gcc/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/%.o: $(LITEOS_ROOT)/utils/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/sec_%.o: $(SECUREC_ROOT)/src/%.c | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/%.o: %.c | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/%.o: $(LITEOS_ROOT)/arch/xtensa/lx6/gcc/%.S | $(OBJ)
	$(CC) $(CFLAGS) $(INC) -c $< -o $@

$(OBJ)/start.o: start.S | $(OBJ)
	$(CC) $(CFLAGS) -c $< -o $@

OBJS = $(patsubst $(LITEOS_ROOT)/kernel/src/%.c,$(OBJ)/%.o,$(KSRC)) \
       $(patsubst $(LITEOS_ROOT)/kernel/src/mm/%.c,$(OBJ)/%.o,$(filter %/mm/%,$(KSRC))) \
       $(OBJ)/los_common_interrupt.o $(OBJ)/los_context.o $(OBJ)/los_interrupt.o \
       $(OBJ)/los_timer.o $(OBJ)/los_dispatch.o $(OBJ)/los_exc.o $(OBJ)/los_window.o \
       $(OBJ)/los_debug.o $(OBJ)/los_error.o $(OBJ)/los_hook.o \
       $(OBJ)/sec_memcpy_s.o $(OBJ)/sec_memset_s.o $(OBJ)/sec_strncpy_s.o \
       $(OBJ)/board.o $(OBJ)/mem.o $(OBJ)/start.o

$(BIN).elf: $(OBJS) link.ld
	$(LD) -T link.ld -o $@ $(OBJS) \
		$(shell $(CC) -mabi=windowed -print-libgcc-file-name)

clean:
	rm -rf build
