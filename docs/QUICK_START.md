# WS500-OpenFW — Quick Start

> **Status:** draft. Describes the intended v1 experience; firmware is in active
> development (control engine complete, drivers + config UI in progress). Do not
> deploy to a boat/vehicle you depend on yet.

Open-source replacement firmware for the **Wakespeed WS500** alternator regulator
hardware. LiFePO4-first, two-stage charging, everything configured in real units.

---

## ⚠️ Before you start — safety

- This device drives **alternator field current** and controls **battery charging**.
  A wrong setting can overheat an alternator, over-discharge a belt, or over/under-
  charge a battery.
- **Bench-test first** on a current-limited supply into a dummy field load before
  connecting a real alternator (see `SAFETY.md`). Never spin an alternator with the
  battery disconnected (load dump).
- Lithium: confirm your **cell count** (4S/8S/16S = 12/24/48 V) and **low-temp
  charge cutoff** before charging.

## What you need

- A WS500 running WS500-OpenFW (see *Flashing* below).
- USB cable (data) + a computer or Android phone with **Chrome/Edge** (for the
  WebSerial config tool), or the `ws500ctl` CLI. *(iOS cannot configure over USB —
  see `CLIENT_CONNECTIVITY.md`.)*
- Your install's key numbers: battery **bank capacity (Ah)**, **cell count**,
  desired **max charge power (W)**, and shunt location (battery or alternator side).

## 1. Flash the firmware (DFU)

The STM32 bootloader is in ROM and unbrickable.

1. Put the unit in DFU mode (BOOT0 / DFU entry — see `FLASH_AND_RECOVERY.md`).
2. Flash: `dfu-util -a 0 -s 0x08000000:leave -D ws500-openfw.bin`
   (or use the web flasher's **Flash firmware** button).
3. **Back up the stock image first** if you may want to return to it.

## 2. Connect and confirm

1. Plug in USB; open the WebSerial tool (or `ws500ctl`).
2. Confirm **system voltage auto-detect** (12/24/48 V) and one-time confirm the
   **cell count**.
3. Set **bank capacity (Ah)** and **max charge power (W)** — the two required numbers.
4. Declare the **shunt location** (battery-side recommended; alternator-side disables
   tail-current exit — the tool tells you).

## 3. Pick a profile

All profiles share the same two-stage engine; they just choose voltages and rest
behavior (see `USER_MANUAL.md`).

| # | Profile | Use it when |
|---|---|---|
| 1 | **Bulk, Float Norm** | Default. Engine carries house loads underway |
| 2 | Bulk, Float Low *(Solar Priority)* | You have solar and want it to win the finish |
| 4 | Bulk Conservative, Float Low | Max cycle life / liveaboard daily driver |
| 5 | Bulk, Off | Charge-then-stop; battery rests |
| 6 | **Limp Home** | Safe mode: reduced power, minimal stress |

Start with **Profile 1** unless you have solar (then **2**).

## 4. Run

1. Start the engine. The regulator warms up, then ramps into **CHARGE (BULK)**.
2. Watch the live monitor: voltage, current, power, temperature, **active state**,
   and the **binding ceiling** — the one number that tells you *why* output is what
   it is (thermal / current limit / voltage / engine).
3. When charged, it transitions to **REST (FLOAT)** or stops, per your profile.

## 5. If something looks wrong

- The **status LED** blink-codes faults; the tool shows plain-language fault text.
- **Limp Home** engages automatically on recoverable faults (reduced-power safe hold).
- Hard faults open the field (stop charging) — check wiring, sensors, and config.

## Next

- **How it actually decides** → `USER_MANUAL.md`
- **Connect to a Victron GX / NMEA2000 network** → `CAN_INTEGRATION.md`
- **Build/flash/recovery details** → `FLASH_AND_RECOVERY.md`
- **Contribute** → `OPEN_SOURCE.md`
