set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break s3_trap
break s3_win_phy_step
set {int}0x600080C0 = 0xDEADBEEF
continue
printf "\n=== 进入 s3_win_phy_step pc=%p a1=%p\n", $pc, $a1
finish
printf "\n=== s3_win_phy_step 返回了！返回值 a2=%p\n", $a2
continue
printf "\n=== 又停住 pc=%p（停在 s3_trap 说明崩溃了）\n", $pc
