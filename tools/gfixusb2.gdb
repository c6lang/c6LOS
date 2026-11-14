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
monitor mww 0x600C001C 0x00000604
monitor mww 0x600C0024 0x000001CA
monitor mww 0x60008120 0x00100000
monitor mww 0x60038018 0x00004200
monitor mdw 0x600C001C 1
monitor mdw 0x60008120 1
monitor mdw 0x60038018 1
