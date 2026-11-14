set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break s3_win_phy_step
set {int}0x600080C0 = 0xDEADBEEF
set $n = 1
while $n <= 5
  continue
  finish
  set $n = $n + 1
end
printf "\n== 把 USB 寄存器写回 PHY 之前的值 ==\n"
set {int}0x600C001C = 0x00000604
set {int}0x600C0024 = 0x000001CA
set {int}0x60008120 = 0x00100000
set {int}0x60038018 = 0x00004200
x/1xw 0x600C001C
x/1xw 0x60008120
x/1xw 0x60038018
printf "== 继续跑，看 USB 是否回来 ==\n"
continue
