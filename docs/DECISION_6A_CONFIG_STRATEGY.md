# Decision #6a — Config strategy: stock `$`-protocol vs native JSON schema

**Status: ACCEPTED — Option B (clean break), decided by the project owner
2026-07-26.** Clarification recorded with the decision: **the client app owns
stock-dump translation** — it parses an archived stock `$` config dump *file*
and proposes equivalent native settings (human-reviewed); the firmware itself
never speaks or stores `$`-protocol config. · GH#28 · was gating M4 (config
store, client app) and parts of M3. Drafted 2026-07-26.

## Question

The stock WS500 speaks the `$CPx/$SCx` config protocol (inherited from the VSR
lineage) over USB CDC. Our design corpus specifies a new JSON profile schema
(PROFILE_SPEC_LFP §7). They conflict — not just in syntax but in model: `$CPx`
encodes the 6-stage Pb machine, 12 V/500 Ah normalization, and DIP-switch-era
semantics that the clean-slate design deliberately ditches (PROJECT_PLAN, design
philosophy). Pick one of: full compatibility, clean break, or a translation layer.

## Constraints and facts

- **Semantic mismatch is fundamental.** Two-stage CHARGE/REST with named
  primitives, per-cell voltages, and single-watts arbitration has no faithful
  mapping onto `$CPx`'s stage table. A write-capable `$` shim would be lossy in
  both directions and would silently misrepresent what the regulator will do —
  the worst property for a charging device.
- **The client is ours anyway.** CLIENT_CONNECTIVITY decides a WebSerial/WebUSB
  web app + `ws500ctl` CLI as the only supported clients. Wakespeed's closed
  tools target stock firmware; there is no third-party ecosystem consuming `$`
  *config writes* that we need to serve.
- **The store is small (V7).** Config hardware is a 2 KB 24C16 EEPROM @0x50 on
  I²C2 (/WP = PA15). A 2 KB budget fits a **packed binary record (CRC +
  schema-version)**, not a JSON document. So JSON is a *wire/export format* at
  the client boundary; the EEPROM holds the packed form regardless of which
  protocol wins. (Stock uses 0x84-byte magic+CRC records — same idea.)
- **Stage-A tooling familiarity.** Our own capture scripts and the installed
  unit's workflow use stock `$` *status* output (`AST` lines). That is
  telemetry, not config — worth preserving in spirit, cheap to emulate
  approximately, and already the model for the dialect-neutral snapshot.
- **PROFILE_SPEC §8 open questions** (tail representation, profile defaults)
  are all *within* the JSON schema — none of them favor `$` compatibility.

## Options

| | A — stock `$` compatible | B — clean break (native JSON) | C — translation layer |
|---|---|---|---|
| User-visible model | legacy 6-stage, misrepresents the engine | matches the actual control model | both, inconsistently |
| Effort | high (reimplement legacy semantics) | schema already drafted (§7) | highest (B + lossy mapping + tests) |
| Stock-config import | native | one-time import tool possible | native but lossy |
| Risk | model drift between surface and engine | none structural | silent mismatch bugs |
| Fits design philosophy | ✗ (carries legacy "because that's how it was done") | ✓ | partial |

## Recommendation

**B — clean break.** Native config = PROFILE_SPEC §7 JSON at the client
boundary (WebSerial app, `ws500ctl`, export/import as text), packed
binary + CRC + version in the 24C16 EEPROM. No `$` config writes, ever.

Two compatibility courtesies, both cheap and non-binding:

1. **One-time import tool** (in `ws500ctl` / the web app): parse an archived
   stock `$` config dump (the Stage-A artifact) and propose the nearest
   native profile + limits — human-reviewed, never automatic.
2. **Optional `$AST`-style status line** in the CDC telemetry stream (read-only,
   clearly versioned) so existing capture/monitor scripts keep working. This is
   telemetry formatting, not protocol compatibility, and can be dropped if it
   ever constrains the design.

## Consequences if accepted

- M4 config store: packed-record codec + JSON (de)serializer, schema-version
  field from day one; `ws500ctl` speaks JSON only.
- CONTROL_SPEC/PROFILE_SPEC remain the single source of truth for parameter
  semantics; no legacy parameter aliases in code.
- GH#28 closes; PROJECT_PLAN deliverable #6/#6a statuses update; the M4 design
  work is unblocked.

## Decision

**B — clean break. Decided 2026-07-26 by the project owner.** The stock-dump
import/translation is a client-app feature ("the app translates the file"),
not firmware surface. Consequences above are now active work: packed-record
EEPROM codec + validator first, JSON boundary + client import tool with the
M4 client work.
