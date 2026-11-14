set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
set {int}0x600080C0 = 0xDEADBEEF
detach
