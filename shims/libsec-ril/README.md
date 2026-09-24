# libsec-ril shims

Two vendor RIL wrappers chained in front of Samsung's real RIL
(`libsec-ril-impl.so`). Each loads the next library and swaps in its own
`onRequest`:

```text
rild -> libsec-ril.so (SMSC) -> libsec-ril-euicc.so (eUICC) -> libsec-ril-impl.so (stock)
```

`libsec-ril.so` keeps the stock library name, so `rild` loads the chain without
any property changes.

## Layout

| File | Library | Role |
|------|---------|------|
| `libsec_ril_smsc_shim.cpp` | `libsec-ril.so` | CS SMS: force NULL SMSC |
| `libsec_ril_euicc_shim.cpp` | `libsec-ril-euicc.so` | eUICC: CLA fixup, MEP-A1 port inject |
| `Android.bp` | | Builds both; the next library in the chain is set with `REAL_LIB_NAME` |

Related pieces outside this directory:

| Path | Role |
|------|------|
| `../../extract-files.py` | Renames stock blob → `libsec-ril-impl.so`; binary NOPs for UICC enablement |
| `hardware/samsung/packages/SamsungEsimSwitcher` | Settings toggle that switches the tsds2 hybrid slot through `TelephonyManager.setSimSlotMapping()` |

## How the wrap works

Both shims do the same thing:

1. `RIL_Init` `dlopen`s `/vendor/lib64/<REAL_LIB_NAME>` and calls its
   `RIL_Init`.
2. The returned function table's `onRequest` pointer is swapped for the shim's
   own, which forwards to the saved one. The table is modified in place, so the
   outer shim's hook ends up in front of the inner one.
3. `RIL_SAP_Init` is forwarded unchanged.

## tsds2 slot switch

Not handled here. Stock switches the hybrid slot with a plain
`setSimSlotMapping()`, and the stock RIL (`SimManager::DoSetSlotMapping`)
performs the tsds2 remux itself, rewriting the eUICC entry to port 1.

## SMSC shim

**Problem:** Stock RIL mishandles SMSC for circuit-switched SMS.

**Fix:** For `RIL_REQUEST_SEND_SMS` / `SEND_SMS_EXPECT_MORE` only, replace the
SMSC pointer with `nullptr` and pass the PDU through. IMS / other SMS paths are
not touched. Logs are prefixed `sec-ril-smsc-shim:`.

## eUICC shim

Logcat tag `sec-ril-euicc-shim`. Only `RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL`
is touched.

### Samsung session-id CLA fixup

**Problem:** AOSP does `cla | channel` for logical-channel APDUs. Samsung
`OPEN_CHANNEL` returns session ids like **101** (not ISO 1..19). That yields
CLA `0xE5` for STORE DATA (`0x80 | 101`) and the card returns **`6986`**.
Stock's own LPA never hits this: it passes the raw CLA through
`iccTransmitApduLogicalChannelByPort`.

**Fix:** For `TRANSMIT_APDU_CHANNEL` with `sessionid > 3` whose CLA carries
**every** bit of the session id (the mark AOSP's OR leaves):

```text
cla = cla & ~sessionid
```

Example: `E5` → `80`. Modem `+CGLA` already keys off `sessionid`. Callers that
pass a correct raw CLA (ARA-M, OMAPI, other LPAs) rarely contain all session
bits and are left alone. Session ids with bit 7 set could not be undone, but
the RIL has only been seen handing out `101`.

Logged once as `APDU-CLA fix active` — it fires on every ES10 APDU.

### MEP-A1 EnableProfile port inject (`BF31` only, gated)

**Problem:** The RIL reports `MEP_A1` in slot status but `NONE` in card status.
AOSP turns `NONE` on a two-port MEP-capable ATR into `MEP_B`, and whichever
report lands last wins: after boot the framework usually has `MEP_A1`, after a
live slot switch `MEP_B`. With `MEP_B`, AOSP omits `targetPortNumber` (tag `82`)
and EnableProfile returns **`6A80`**. Injecting `82` on cards that are not
MEP-A1 also yields **`6A80`**.

**Fix:** Rewrite EnableProfile only when `ril.esim.mep_mode` is `1` / `0,1` /
`,1`:

- Skip when AOSP already appended `8201xx` (framework had `MEP_A1`).
- Append `820101` (Android port 0 → eUICC port 1). The switcher only ever maps
  eUICC port 0; a profile on port 1 (two active eSIMs) would need `820102`.
- Bump short-form content length and `RIL_SIM_APDU.p3`.
- **Heap-allocate** the rewritten hex (`malloc`); `libril_sem` frees
  `apdu->data` after transmit — pointing at static storage aborts under MTE.

**Do not** inject on **DisableProfile (`BF32`)**.

Logged as `APDU-MEP inject port1`.

## Debugging

The eUICC shim deliberately carries **no per-APDU tracing**: a profile download is
thousands of APDUs. If a future ES10 failure needs APDU-level detail, add the
logging temporarily rather than keeping it in the tree.

Useful SW codes seen during bring-up:

| SW | Meaning in this work |
|----|----------------------|
| `9000` | Success |
| `6986` | Bad CLA (fixed by session CLA strip) |
| `6A80` | Incorrect parameters (missing/extra MEP port TLV) |
| MTE abort in `rild` | Rewritten `BF31` pointed at static storage; `libril_sem` then `free()`d it — use heap (`malloc`) instead |
