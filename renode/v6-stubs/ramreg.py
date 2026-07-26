# Generic store-and-return register block (FLASH interface: ACR latency
# readback must match what HAL wrote). Renode 1.16.1 PascalCase request API.
if 'regs' not in globals():
    regs = {}

if request.IsRead:
    request.Value = regs.get(request.Offset, 0)
elif request.IsWrite:
    regs[request.Offset] = request.Value
