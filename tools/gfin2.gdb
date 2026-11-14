set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break s3_trap
break s3_win_phy_step
set {int}0x600080C0 = 0xDEADBEEF
set $n = 1
while $n <= 6
  continue
  printf "\n--- STEP%d 进入 pc=%p\n", $n, $pc
  finish
  printf "--- STEP%d 返回 a2=%p\n", $n, $a2
  set $n = $n + 1
end
