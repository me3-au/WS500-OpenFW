# STM32F0 ADC stub for the V6 stock trace: store-and-return, with the HAL
# handshakes mirrored so init/calibration/enable complete:
#   CR  (0x08): ADCAL (bit31) self-clears on read; ADSTP self-clears
#   ISR (0x00): ADRDY (bit0) mirrors CR.ADEN; EOC/EOS always set when enabled
#   DR  (0x40): plausible mid-scale sample
if 'regs' not in globals():
    regs = {}

if request.IsRead:
    off = request.Offset
    if off == 0x00:                       # ISR
        v = regs.get(0x00, 0)
        cr = regs.get(0x08, 0)
        if cr & 0x1:
            v |= 0x1 | 0x4 | 0x8          # ADRDY | EOC | EOS
        request.Value = v
    elif off == 0x08:                     # CR: ADCAL/ADSTP read back as done
        request.Value = regs.get(0x08, 0) & ~((1 << 31) | (1 << 4))
    elif off == 0x40:                     # DR
        request.Value = 0x800
    else:
        request.Value = regs.get(off, 0)
elif request.IsWrite:
    if request.Offset == 0x00:            # ISR is write-1-to-clear
        regs[0x00] = regs.get(0x00, 0) & ~request.Value
    else:
        regs[request.Offset] = request.Value
