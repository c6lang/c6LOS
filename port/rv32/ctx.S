/* rv32 ilp32 协作上下文：保存 ra/s0-s11/sp。 */
.globl port_task_switch
port_task_switch:
    addi sp, sp, -56
    sw ra, 52(sp)
    sw s0, 4(sp)
    sw s1, 8(sp)
    sw s2, 12(sp)
    sw s3, 16(sp)
    sw s4, 20(sp)
    sw s5, 24(sp)
    sw s6, 28(sp)
    sw s7, 32(sp)
    sw s8, 36(sp)
    sw s9, 40(sp)
    sw s10, 44(sp)
    sw s11, 48(sp)
    la t0, g_sched_sp
    sw sp, 0(t0)
    la t1, g_cur_ctx
    sw a0, 0(t1)
    lw sp, 0(a0)
    lw s11, 48(sp)
    lw s10, 44(sp)
    lw s9, 40(sp)
    lw s8, 36(sp)
    lw s7, 32(sp)
    lw s6, 28(sp)
    lw s5, 24(sp)
    lw s4, 20(sp)
    lw s3, 16(sp)
    lw s2, 12(sp)
    lw s1, 8(sp)
    lw s0, 4(sp)
    lw ra, 52(sp)
    addi sp, sp, 56
    ret

.globl port_task_yield
port_task_yield:
    addi sp, sp, -56
    sw ra, 52(sp)
    sw s0, 4(sp)
    sw s1, 8(sp)
    sw s2, 12(sp)
    sw s3, 16(sp)
    sw s4, 20(sp)
    sw s5, 24(sp)
    sw s6, 28(sp)
    sw s7, 32(sp)
    sw s8, 36(sp)
    sw s9, 40(sp)
    sw s10, 44(sp)
    sw s11, 48(sp)
    la t0, g_cur_ctx
    lw t0, 0(t0)
    sw sp, 0(t0)
    la t1, g_sched_sp
    lw sp, 0(t1)
    lw s11, 48(sp)
    lw s10, 44(sp)
    lw s9, 40(sp)
    lw s8, 36(sp)
    lw s7, 32(sp)
    lw s6, 28(sp)
    lw s5, 24(sp)
    lw s4, 20(sp)
    lw s3, 16(sp)
    lw s2, 12(sp)
    lw s1, 8(sp)
    lw s0, 4(sp)
    lw ra, 52(sp)
    addi sp, sp, 56
    ret

.globl rv32_tramp
rv32_tramp:
    la t0, g_cur_ctx
    lw t0, 0(t0)
    lw t1, 4(t0)
    lw a0, 8(t0)
    jalr t1
    call os_task_exit
1:
    j 1b
