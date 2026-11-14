set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break s3_win_phy_step
set {int}0x600080C0 = 0xDEADBEEF
printf "\n== PHY 之前 ==\n"
x/1xw 0x600C001C
x/1xw 0x600C0024
x/1xw 0x60008120
x/1xw 0x60038018
set $n = 1
while $n <= 5
  continue
  finish
  set $n = $n + 1
end
printf "\n== PHY 之后 ==\n"
x/1xw 0x600C001C
x/1xw 0x600C0024
x/1xw 0x60008120
x/1xw 0x60038018
