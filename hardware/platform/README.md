# hardware/platform/

PL logic that is not cryptographic. It is kept apart from `hardware/rtl/` so the source
tree shows that the fan controller touches no crypto signal. It shares the clock and
reset with the crypto cores and nothing else. There is one PL and one bitstream, so the
fan controller is built into the same bitstream as the cores (`syn/impl_bitstream.tcl`
reads this directory and `board_pins.xdc` constrains the one pin it drives).

## `fan_ctrl/`

Fan control from junction temperature: `SYSMONE4` -> DRP polling -> temperature -> duty
cycle -> PWM on pin `AA11`. The pin is active low (low = fan on); the only inversion is
`fan_pin = ~pwm_on` in `fan_ctrl.v`.

| File | Role |
|---|---|
| `sysmon_drp.v` | DRP polling, plain RTL |
| `fan_sysmon.v` | `SYSMONE4` instantiation and configuration (`SIM_DEVICE` is set explicitly to `ZYNQ_ULTRASCALE`) |
| `fan_ctrl.v` | Temperature -> duty cycle -> PWM (25 kHz at 75 MHz) |
| `fan_ctrl_axi.v` | AXI4-Lite observation port at slot 5: temperature, duty and status read-back, a debug override, and a SYSMON DRP read window |

The temperature is read by the PL itself, so cooling works without Linux: during early
boot, while U-Boot waits at a prompt, and after a kernel hang. Slot 5 is for
observation. Its override cannot beat the over-temperature force, and the fan still runs
if the slot is removed.

Duty steps and thresholds (SYSMON codes in `fan_ctrl.v`, shown in degrees C):

| Step | Duty | Enter at or above | Leave at or below |
|---|---|---|---|
| 0 | 8 % | (lowest step) | |
| 1 | 25 % | 37 | 35 |
| 2 | 40 % | 45 | 40 |
| 3 | 60 % | 55 | 50 |
| 4 | 80 % | 65 | 60 |
| 5 | 100 % | 75 | 70 |

Three conditions force 100 %:

- Temperature at or above 80 C, held until it falls to 74 C.
- No valid reading for `STALE_LIMIT` clocks (about 0.9 s at 75 MHz).
- A reading that does not change for `STUCK_LIMIT` consecutive samples (30000 samples at
  1 ms, about 30 s). A SYSMON misconfigured so that the ADC does not convert still
  answers DRP with a plausible constant, which the staleness check does not catch; a real
  junction temperature always jitters.

The lowest step is 8 %, not 0. The fan has no tach, and 8 % was measured on this board as
able to start from standstill and to keep running for 3 minutes. Remeasure with
`board/scripts/fan_minduty_sweep.sh` after changing the fan or enclosure. The thresholds
are module parameters; the characterisation bitstream (`PQC_CHARACTERIZE=1`) lowers them
to the range this board reaches.

`tools/rtl_lint.sh` and `tools/rtl_synth_check.sh` cover this directory, and
`test_fan_ctrl.py` and `test_sysmon_drp.py` are in the cocotb suite.
