# Minimal STM32F0 RCC stub for the V6 stock-binary trace (PROJECT_PLAN 0.6 V6).
# Renode 1.16.1 PythonPeripheral API is PascalCase (IsRead/IsWrite/Offset/Value).
# Store-and-return with ready-bit mirroring so HAL clock init completes:
#   CR   (0x00): HSIRDY<-HSION, HSERDY<-HSEON, PLLRDY<-PLLON
#   CFGR (0x04): SWS<-SW
#   CR2  (0x34): HSI14RDY<-HSI14ON, HSI48RDY<-HSI48ON
if 'regs' not in globals():
    regs = {0x00: 0x83}      # reset: HSION|HSIRDY|HSITRIM default

if request.IsRead:
    v = regs.get(request.Offset, 0)
    if request.Offset == 0x00:
        if v & 0x1:
            v |= 0x2                 # HSIRDY
        if v & (1 << 16):
            v |= (1 << 17)           # HSERDY
        if v & (1 << 24):
            v |= (1 << 25)           # PLLRDY
    elif request.Offset == 0x04:
        v = (v & ~0xC) | ((v & 0x3) << 2)   # SWS mirrors SW
    elif request.Offset == 0x34:
        if v & 0x1:
            v |= 0x2                 # HSI14RDY
        if v & (1 << 16):
            v |= (1 << 17)           # HSI48RDY
    request.Value = v
elif request.IsWrite:
    regs[request.Offset] = request.Value
