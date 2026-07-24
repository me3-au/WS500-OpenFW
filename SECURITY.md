# Security Policy

## Supported versions

Pre-release. Only the `main` branch is supported; there are no tagged releases yet
(`v0.1.0` lands at milestone M6 — see `docs/PROJECT_PLAN.md`). No hardware has been
flashed with this firmware yet.

| Version | Supported |
|---|---|
| `main` (0.1.0-dev) | yes |
| anything else | no |

## Reporting a vulnerability

Use **GitHub private vulnerability reporting**: the *Security* tab of
[me3-au/WS500-OpenFW](https://github.com/me3-au/WS500-OpenFW) → *Report a vulnerability*.
Please do **not** open a public issue for security-relevant bugs.

## What counts as security-relevant here

This is charging-system firmware — it drives real field current into a real alternator
charging a real battery bank. **Unsafe-charging bugs are treated with security-level
priority**, the same as a classic exploit would be. That includes anything that could:

- cause **over-voltage** at the battery (CV loop, limit set, or fault-ladder bypass),
- cause **rotor over-duty** (weakening the rotor duty clamp or the compiled max-duty cap),
- defeat the **fail-safe defaults** (field-OFF at boot, field-open on uncertainty,
  TIM1 hardware break, watchdog),
- corrupt configuration or telemetry in a way that misleads the operator into an unsafe
  state.

Conventional firmware security issues (config-protocol parsing over USB/CAN, buffer
handling, update/DFU integrity) are of course in scope too.

Report privately, include how you found it (test, code inspection, emulation), and we'll
triage it ahead of ordinary bugs.
