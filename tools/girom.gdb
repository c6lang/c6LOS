set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break s3_trap
break os_flash_map
break os_icache_enable
set {int}0x600080C0 = 0xDEADBEEF
continue
printf "\n=== 第一站 pc=%p\n", $pc
info registers pc a0 ps
continue
printf "\n=== 第二站 pc=%p\n", $pc
info registers pc a0 ps
continue
printf "\n=== 第三站(陷阱?) pc=%p\n", $pc
info registers pc a0 ps
p $exccause
p $epc1
