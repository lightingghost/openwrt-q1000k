# Q1000K OMCI admission and allocation bench

This bench follows the captured 48-byte runt rejection and the two pending
Ethernet UNI Set response failures documented in
[XGSPON-OMCI-RECONFIGURATION-AUDIT.q1000k.md](XGSPON-OMCI-RECONFIGURATION-AUDIT.q1000k.md).
It uses one RAM image and one generated Python collection script. Fiber stays
connected throughout; no manual outage is required.

## Changes

- Both native Ethernet and MAC packet receive guards exempt **only OMCI**
  from descriptor runt bit 12. CRC bit 11, oversize bit 13, and aggregation
  greater than one remain errors. Ordinary Ethernet runt handling stays in
  force. The software authenticator retains exact baseline/extended length,
  baseline trailer, expected GEM/session and AES-CMAC checks. No FCS trimming
  or padding acceptance is introduced.
- The normal hardware minimum stays **60 bytes**, matching the NAND OEM FE
  setup. An immutable 48-byte comparison remains selectable in this image.
  The Sirherobrine OMCI parser's accepted PDU sizes are not evidence of its
  Ethernet hardware minimum; that exact minimum has not been verified.
- An ordinary Assign_Alloc-ID received in an otherwise unchanged O5 session
  schedules service reconciliation without closing the authenticated session.
  The core waits for the current RX transaction and its reply, then checks
  the exact authentication epoch before reconciliation. Pending reset,
  profile, identity, or security transitions retain the revocation path.
- Authenticated OMCI waits above native DMA admission while the physical
  pipeline is paused. Its worker checks the original authentication epoch,
  acquires fresh DMA admission, and submits under the same barrier used by
  pause/rekey. The queue remains limited to 128 packets and the original
  1000 ms deadline; retries never extend it. Non-OMCI data retains its original
  DMA epoch. Rekey/reset purges old authenticated responses.
- This fixes authentication/response lifetime around the existing physical
  rebuild. It **does not implement the OEM's narrower T-CONT-only hardware
  update**. Full rebuild timing can still matter and is measured.
- OMCI cases enable the previously tested initial-data-key readback experiment.
  Only the first enable from no active TX key can proceed without a switch IRQ,
  after control/validity and all four material words match. Later active-key
  switches still require the interrupt. The option defaults off outside these
  explicit cases. No key material is logged.

## Single-image collection matrix

The portable collector defaults to `--suite omci`. Explicit private identity
is required for activation. Without it the script collects RX only.

| Case | Main hypothesis tested | Settings and evidence |
| --- | --- | --- |
| `rx-startup` | The RX prerequisite is still present | TX inhibited; power, LOS, frame synchronization and cleanup |
| `activation-omci-fixed` | Valid short OMCI was rejected; allocation notification revoked a valid pending reply | Minimum 60; both runt exceptions; stable authentication across ordinary allocations; ranging mode 1; checked initial key enable |
| `activation-omci-oem` | Initial ranging timing affects subsequent provisioning | Same fixes, using NAND direct EqD mode 3 |
| `activation-omci-min48` | The Ethernet minimum alone explains short-frame rejection | Same fixes with minimum 48; compare descriptor runt bits and authenticated RX with minimum 60 |
| `activation-omci-revoke` | The captured `active=false` response error is caused by allocation revocation | Same receive/key settings; explicitly restore the old allocation-revokes-session behavior |
| `activation-omci-repeat` | The corrected path survives repeated provisioning and later updates | Repeat minimum 60 and stable authentication, with a longer observation budget |

All earlier `registration`, `tx`, `output`, `measurement`, `isolated`, and
`legacy` suites remain selectable. The disconnected transmitter suites require
disconnected fiber; they are not part of this connected-fiber default run.
Each case owns a fresh stack and verifies unload and private-input cleanup.

## Diagnostic interpretation

`QT_CONTROL` is event 27. Records are numeric and do not contain payloads,
subscriber identity, key material, or MIC bytes.

| IDs | Observation |
| --- | --- |
| 20–22 | Initial key wait, final control readback, and restricted fallback acceptance |
| 30–32 | Native-delivered OMCI descriptor/length, MAC receive-guard result, session/GEM/MIC authentication result |
| 36 | Hardware minimum before/after and maximum |
| 37–39 | Reply authentication epoch and admission flags, upper epoch words/GEM comparison, software enqueue result |
| 40 | Allocation notification preserving the current session |
| 41 | First physical-admission deferral for a response |
| 42 | Native TX consumption, native DMA epoch and whether previously deferred |
| 43 | Response expiry, stale epoch, detach or admission error |
| 44 | Physical pause/resume result |
| 45 | Epoch-checked service reconciliation result |

The generated `omci-summary.md` and `collection.json` deduplicate repeated
trace snapshots and report missing critical evidence. Zero deferrals do not
prove the deferral path worked: the OLT must actually produce an overlapping
allocation/reply window. Native consumption is not proof of OLT reception.
Provisioning, WAN addressing and traffic retain their independent checks.

## Validation before release

Host fixtures cover both runt guards, exact authenticated framing, key checks,
unchanged-session allocation handling and the old-policy comparison. Linux UML
runs cover real workqueue/lock/RCU/skb behavior, pending responses across pause,
rekey and expiry, and core reconciliation serialized with a pending response.
The image build runs the full PON host suite, status/UI checks, packed image
inspection, module hashes and configuration/branch preservation checks.
Hardware validation of this new image remains the next bench task.
