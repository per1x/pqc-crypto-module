# hardware/platform/

Fabric logic that is **not cryptographic**, kept in its own tree so that "the
fan never touches a crypto signal" is visible in the filesystem and not only in
a comment. It shares the clock and the reset with the crypto cores and nothing
else.

There is only one PL, and the single bitstream loaded at runtime is everything,
so this does go into the same bitstream as the crypto cores — the separation is
in the source tree, not in the device.

## `fan_ctrl/`

Junction-temperature-driven fan control: `SYSMONE4` → DRP poll → temperature →
duty cycle → PWM on pin `AA11`.

| File | Role |
|---|---|
| `sysmon_drp.v` | DRP polling, pure RTL |
| `fan_sysmon.v` | `SYSMONE4` instantiation and configuration |
| `fan_ctrl.v` | Temperature → duty cycle → PWM |
| `fan_ctrl_axi.v` | AXI observation port — read-only, and removable |

**Cooling must work when Linux does not**: during the first seconds of boot,
while U-Boot sits at a prompt, and when the kernel has hung. So the temperature
is read by the PL itself over DRP and the whole path depends only on the PL
clock. The AXI slave at slot 5 is observation only; delete it and the fan still
runs.

**Every safety rule errs towards more airflow.** Minimum duty is 25 %, not 0.
At ≥80 °C the fan is forced to 100 % and does not release until 74 °C. A stale
temperature forces 100 %, because when the temperature is unknown the only safe
assumption is that it is high.

A fourth rule was added after a board run: a reading whose bits never change
also forces 100 %. The first SYSMON configuration was wrong in a way where DRP
still answered and the register still held a plausible 32.5 °C — the ADC simply
was not converting. "No reading" never became true, so the staleness rule could
not fire. A real junction temperature always jitters, which makes perfect
stability itself the fault signal.

Lint, synthesisability and cocotb coverage apply here exactly as they do to the
crypto RTL. A wrong fan has no runtime symptom; it just quietly overheats the
die, which is a reason for more static checking, not less.
