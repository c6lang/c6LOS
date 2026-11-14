set pagination off
set confirm off
set remotetimeout 30
target extended-remote :3333
set {int}g_gate = 0xDEADBEEF
detach
