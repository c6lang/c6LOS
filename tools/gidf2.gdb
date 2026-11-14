set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
echo \n=== 原厂 IDF 固件运行时 ===\n
x/1xw 0x600C4060
x/1xw 0x600C4064
x/1xw 0x600C4068
x/1xw 0x600C407C
x/1xw 0x600C5000
x/1xw 0x600C5100
x/1xw 0x42000000
x/1xw 0x42000004
x/1xw 0x3C000000
echo \n=== pc ===\n
p $pc
