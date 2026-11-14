set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
printf "\n=== 原厂 IDF 固件运行时（能正常 IROM 取指）===\n"
printf "ICACHE_CTRL  = "  ; x/1xw 0x600C4060
printf "ICACHE_CTRL1 = "  ; x/1xw 0x600C4064
printf "0x600C4068   = "  ; x/1xw 0x600C4068
printf "0x600C406C   = "  ; x/1xw 0x600C406C
printf "0x600C407C   = "  ; x/1xw 0x600C407C
printf "MMU[0]       = "  ; x/1xw 0x600C5000
printf "MMU[64]      = "  ; x/1xw 0x600C5100
printf "IROM 0x42000000 = " ; x/1xw 0x42000000
printf "IROM 0x42000004 = " ; x/1xw 0x42000004
printf "DROM 0x3C000000 = " ; x/1xw 0x3C000000
printf "pc = %p\n", $pc
