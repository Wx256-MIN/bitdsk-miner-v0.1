# N8-T Firmware (reference build, inspired by AxeOS)

An ESP-IDF firmware project for a BM1397-based solo Bitcoin miner, written
for the BitDsk N8-T. This is **original code**, architecturally informed by
publicly available documentation of the open-source Bitaxe/ESP-Miner
project (same ASIC family, different hardware) — not a copy of anyone
else's source.

**Read the whole "Status" section before you flash this to a real board.**
Nothing here has run against actual N8-T hardware. That's not false
modesty — it's the single most important fact about this codebase.

## Status: what's grounded vs. what's a placeholder

Everything in this project falls into one of three buckets. Knowing which
bucket a given file is in tells you how much to trust it.

**Verified against real architecture documentation** — high confidence:
- The BM1397 packet shape (preamble, `TYPE|GROUP|COMMAND` header byte,
  CRC5 on command packets, CRC16 on job packets), chip discovery/addressing,
  and PLL-based frequency control in `bm1397.c`.
- The overheat-protection thresholds, shutdown sequence, cooldown timing,
  and reduced-settings recovery ramp in `power_management_task.c`.
- The overall task architecture and priorities (job creation > result
  handling > power management > stratum) in `main.c`.
- The Stratum V1 client in `stratum.c` — this is a stable, publicly
  specified protocol, not something reverse-engineered.
- `sha256_midstate.c` — plain FIPS 180-4 SHA-256, testable against NIST
  test vectors independent of anything mining-specific.

**This project's own best-effort implementation of undocumented details**
— reasonable, internally consistent, *not* verified against a real chip:
- The exact CRC5/CRC16 algorithms in `bm1397.c` (polynomial/init choices
  documented in comments).
- Exact register addresses and the nonce/register response framing in
  `bm1397.c`.
- The block-header byte-ordering in `create_jobs_task.c` (Bitcoin's
  endianness conventions are the single easiest thing to get subtly wrong
  in any stratum client).
- The simplified difficulty calculation in `asic_result_task.c` (uses the
  leading 64 significant bits of the hash rather than full bignum
  division — plenty precise for a threshold comparison, but worth knowing
  it's an approximation).

**Deliberate stubs — do not trust, must be replaced:**
- Every GPIO pin number in `board_config.h`.
- Every I2C register poke in `power_management_task.c`
  (`vcore_set_voltage_mv`, `read_vr_temp_c`, `read_asic_temp_c`,
  `set_fan_percent`). These currently log a warning and return a
  placeholder instead of touching real silicon. **This is the one part of
  the project where guessing instead of verifying could damage your
  hardware** — an incorrect write to a voltage regulator's register can
  over-volt the ASIC. Everything else here either works or it doesn't;
  this specifically can hurt something.

## Why the hard parts are uncertain at all

BitDsk hasn't published a schematic or GPIO map for the N8-T, and Bitmain
has never published a BM1397 datasheet — every open-source BM1397 driver
that exists, including Bitaxe's, is built on community reverse-engineering.
This project's architecture leans on Bitaxe's own published documentation
of *their* implementation (same chip family, different board), which is
why the high-level shape is trustworthy while the exact bytes are a
best-effort reconstruction rather than a copy — copying their actual
source wouldn't be appropriate to do here even where it exists, and
wouldn't fix the deeper problem that your board's wiring is still unknown.

## Architecture

```
 Wi-Fi (AP setup / STA)  →  web_server.c  →  webui/index.html (dashboard)
                                │
 stratum.c  ──notify──▶  work_queue.c  ──▶  create_jobs_task.c
    ▲                                              │
    │                                    sha256_midstate.c (midstate)
    │                                              │
    └──submit──  asic_result_task.c  ◀──nonce──  bm1397.c  ◀──UART──▶  ASIC
                       │
            power_management_task.c (thermal safety, independent loop)
```

All tasks share one `global_state_t` (`global_state.h`) guarded by a
mutex, rather than passing everything through queues — simpler to reason
about at this scale, same pattern ESP-Miner itself documents using.

## Before you flash anything

1. **Determine your real GPIO pin mapping.** See the long comment at the
   top of `board_config.h` for how (identify the ESP32 module, back up
   the stock firmware with `esptool.py`, continuity-test with a
   multimeter, ideally confirm the UART pins with a logic analyzer while
   stock firmware is running).
2. **Implement the regulator/fan-controller I2C functions for real**,
   against your actual chips' datasheets — don't guess register values
   for the voltage regulator specifically.
3. Update every `TODO: VERIFY` in `board_config.h`.
4. Only then build and flash.

Skipping straight to flashing with placeholder pins is unlikely to do
anything worse than "nothing happens" (wrong UART pins just means silence),
but it's not guaranteed, and there's no way to bound that risk in the
abstract — verify first.

## Building

Standard ESP-IDF project layout, so the standard workflow applies:

```bash
. $HOME/esp/esp-idf/export.sh      # or wherever your ESP-IDF install lives
idf.py set-target esp32s3          # adjust if your board uses a different variant
idf.py menuconfig                  # sanity-check sdkconfig.defaults got picked up
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

I haven't run this build myself — my environment doesn't have the ESP-IDF
toolchain or a real chip to test against, so treat first build/flash as
the actual first test of whether this compiles cleanly, not an assumption
already validated for you. Expect to fix at least minor `CMakeLists.txt`
component-dependency issues; those show up as clear compile errors, not
silent bad behavior, so they're the least risky category of bug here.

## First bring-up, recommended order

1. Build and flash with the ASIC-related tasks effectively inert (leave
   `board_config.h` pins as placeholders) and confirm Wi-Fi + the
   dashboard work over your real network. This validates everything
   *except* the ASIC link and costs you nothing hardware-wise.
2. Determine real pins. Re-flash. Watch the boot log for
   `bm1397_detect_chips` — a real chip ID echoed back means the UART
   framing (preamble, CRC5, register read) is fundamentally working. No
   response after several seconds means recheck wiring before suspecting
   the protocol implementation.
3. Only after chip discovery works, implement real voltage-regulator
   control and let `power_management_task` actually own the core rail.
4. Point `nvs_config_set_pool` at a real pool (or run a local
   `public-pool`-style solo endpoint) and confirm `mining.notify` →
   `create_jobs_task` → `bm1397_send_job` → nonce → `asic_result_task` →
   `mining.submit` round-trips, ideally by comparing against a known
   low-difficulty test vector before trusting real traffic.

## Known gaps

- No OTA update support (the partition table has room for it; wiring up
  `esp_https_ota` is a reasonable next step once basic mining works).
- No version-rolling support, matching BM1397's own lack of it.
- Difficulty math is an approximation (see Status above) — fine for
  deciding whether to submit a share, not something to build a payout
  ledger on without upgrading to real bignum arithmetic.
- Single-chip chain only has been reasoned through; `ASIC_CHIP_COUNT > 1`
  should mostly work given the address-interval logic in
  `bm1397_detect_chips`/`assign_chip_addresses`, but it's untested even
  in principle beyond that.
