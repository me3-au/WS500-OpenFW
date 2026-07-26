# Versioning & Compatibility Standard

The documented contract that keeps **Client ↔ Protocol ↔ Config ↔ Firmware** in
feature sync without lock-step releases. Deliverable #18 companion; binds the M4
client work (#8/GH#30, ws500ctl GH#17) and the config store (#6/GH#15).
Adopted 2026-07-26.

## 1. The four versioned things

| Artifact | Scheme | Lives in | Bumped when |
|---|---|---|---|
| **Firmware** | SemVer `MAJOR.MINOR.PATCH[-dev]` | `VERSION`, `Core/Inc/version.h`, git tag | MAJOR: safety-relevant behavior or update/recovery breaking change · MINOR: features · PATCH: fixes |
| **Config schema** | integer `schema_version` (u16, starts at 1) | EEPROM record header **and** every exported JSON document | **breaking** change only (field removed / meaning changed). Additive tail-append with safe defaults does **not** bump (see §3) |
| **Wire protocol** (USB CDC JSON-lines) | integer `protocol_version` (starts at 1) + **capability flags** | hello handshake (§2) | breaking message-shape change only; additive features are new capability strings, not a bump |
| **Client** (web app + `ws500ctl`) | SemVer, independent cadence | app footer / `ws500ctl --version` | normal SemVer; declares a supported `protocol_version` range |

One rule above all: **the config schema is ONE number for both representations.**
The EEPROM packed record and the JSON document describe the same model, so they
share `schema_version` — there is no separate "file format version".

## 2. Handshake — compatibility is negotiated, never assumed

On every connection the client sends `{"t":"hello","proto":<max supported>}`.
Firmware replies:

```jsonc
{ "t":"hello",
  "fw":"0.1.0-dev", "git":"<short-hash>",
  "proto":1,                 // protocol version the firmware speaks
  "schema":1,                // config schema the firmware is native in
  "caps":["cfg","telem","dfu-handoff","ast-line"] }   // feature flags
```

Rules:
- **Client refuses politely** outside its supported `proto` range: it explains,
  and links the matching pinned client build (static hosting keeps old builds at
  `/v/<x.y.z>/` — a web page is cheap to keep forever).
- **Feature sync is capability-driven, not version-sniffed.** The client UI
  enables a feature iff its flag is in `caps`. New firmware feature ⇒ new flag ⇒
  old clients simply don't show it; new clients on old firmware grey it out.
  Version comparisons in client code (other than the proto-range gate) are a
  code smell.

## 3. Config compatibility & migration

- **Firmware is the schema authority.** It accepts config writes only in its
  native `schema_version`; anything else is rejected with a specific error.
- **Within a schema version**, new fields may be appended at the record tail /
  added as new JSON keys **with safe defaults**; decoders MUST default missing
  tail fields and (per PROFILE_SPEC §7) preserve unknown JSON keys on
  round-trip. That is what lets MINOR firmware releases add parameters without
  a schema bump.
- **Across firmware updates** (config survives an update — deliverable #7):
  the firmware's codec keeps **read-forward migrations** for older record
  versions it may find in the EEPROM: read old → migrate in RAM → validate →
  next save writes native. Firmware never *writes* an old version.
- **The client owns file migrations** (mirror of decision #6a-B "the app
  translates the file"): importing an older-schema JSON export, the client
  migrates it forward — shown to the user as a diff, never silent. The stock
  `$`-dump import wizard is the same pipeline with a different front end.
- Every exported JSON is stamped: `{"schema_version":N, "fw":"x.y.z", ...}`.

## 4. Release discipline

- **CHANGELOG.md** records, per release: firmware SemVer, and — whenever they
  change — `schema_version`, `protocol_version`, and added/removed `caps`
  strings. A schema or proto bump gets its own CHANGELOG heading with the
  migration story.
- **Git tag `vX.Y.Z`** = firmware release (flow exercised at M6, GH#24). The
  released client declares which proto range it was tested against in its own
  changelog.
- **Compatibility matrix** (kept here, one row per released firmware):

| Firmware | proto | schema | caps added | Min client |
|---|---|---|---|---|
| 0.1.0 (planned) | 1 | 1 | cfg, telem, dfu-handoff | 0.1.x |

## 5. Enforcement points (where the standard is code, not prose)

- `Core/Inc/version.h` — firmware SemVer + git hash (exists, #18).
- Config record header `schema_version` u16 + magic + CRC — config store
  (in progress per #6a-B); codec rejects unknown versions.
- `config_protocol.c` hello handler — proto + caps (M4).
- Client proto-range gate + migration module (M4, GH#30/#17).
- CI: a release-tag workflow step should assert VERSION == tag and CHANGELOG
  has an entry (wire up with GH#24 when the tag flow is exercised).
