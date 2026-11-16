/* L1 的最小 ring3 程序：write(1, msg, len) 然后 exit(0)。
 *
 * 整段会被拷贝到用户页再执行，因此必须是位置无关的：
 * 指令与字符串在同一段内，RIP 相对位移在汇编期就已经固定。 */

.section .rodata.userblob, "a"
.globl _user_blob_start
_user_blob_start:
    jmp 1f

user_msg:
    .ascii "hello from ring3\n"
.set user_msglen, . - user_msg

1:
    movl $1, %edi                 /* fd = stdout */
    leaq user_msg(%rip), %rsi     /* buf */
    movl $user_msglen, %edx       /* count */
    movl $1, %eax                 /* SYS_write */
    int $0x80

    xorl %edi, %edi               /* status = 0 */
    movl $60, %eax                /* SYS_exit */
    int $0x80

2:
    hlt
    jmp 2b

.globl _user_blob_end
_user_blob_end:
