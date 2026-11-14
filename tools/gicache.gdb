set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
break os_flash_map
break os_icache_enable
set {int}0x600080C0 = 0xDEADBEEF
continue
printf "\n=== os_flash_map ok\n"
continue
printf "\n=== os_icache_enable 进入 pc=%p\n", $pc
set $i = 0
while $i < 24
  si
  printf "SI %p\n", $pc
  set $i = $i + 1
end
