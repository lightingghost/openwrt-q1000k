# Q1000K OMCI length and reconfiguration audit — 2026-09-17

## Conclusions

The NAND firmware programs the same **60-byte minimum / 2,000-byte maximum**
that our pipeline uses. Its PON receive path, however, does not reject the
Ethernet runt flag before dispatching OMCI. Our native driver and packet
adapter both do. The previous bench's 48-byte experiment removed the runt
classification and admitted authenticated baseline OMCI. This establishes
an incompatibility between our copied port setting and our new RX policy.

The next observed response failure is a separate local ordering bug. An
authenticated Assign_Alloc-ID overlaps a pending OMCI response. Our allocation
notification closes the backend before the core finishes that request. The
response still has the correct authentication epoch and GEM ID, but fails
because `active` is false. Both the NAND allocation path and the pinned
Sirherobrine driver use more narrowly scoped T-CONT updates.

These are static source/binary findings correlated with existing captures.
This audit does not establish a complete service connection or exercise an
OEM runtime. Implementation and hardware validation of the permanent fixes
remain to be done.

## Evidence and provenance

Evidence directory:

`/home/odin/local/q1000k/build-artifacts/q1000k-xgspon/omci-reconfiguration-research-20260917/`

- `oem/manifest.json` records files extracted from the existing **actual NAND**
  rootfs, their hashes, and the fact that none were executed.
- NAND rootfs SHA-256:
  `156e52d086d9ec9f7b9026ad7c97c9b4502909613b87586062422a8120b2acfb`.
- NAND `xpon_10g.ko` SHA-256:
  `099d4ff0601c263d9a44f2f89d3604c770820936bc94041bf4763d5f10a5f1e5`.
- NAND `fe_core.ko` SHA-256:
  `8571f3969246a8524edc66924600e82cb80f0cde365e80de2bd73d91b5ea60e7`.
- NAND `qdma_wan.ko` SHA-256:
  `09bb59092940902608f14a42954f9e49286bf99aa75cd65b993c8913b4382a18`.
- Sirherobrine kernel commit:
  `2e2cf91fe84467d77649efebd99a28284f2124b3`.
- The generic OMCI source was recovered from unchanged import commit
  `33c035249af6c47be8645147f1a52240de56ff5b`, preserving its provenance.
- The cached `airoha_xpon.c` Git blob matches the cached upstream tree entry
  `73de8ab5a38d596a27f13bb874138d37d17097ad`. That recursive tree is truncated;
  the generic OMCI files are verified against the original local import,
  rather than claimed to be present in the truncated tree.
- `sir/manifest.json` records all 25 source files and their hashes.
- `response-race.json` contains the two matching failure records and their
  preceding authenticated RX / Assign_Alloc-ID events, with source log lines.

Raw GitHub and API retrieval failed during this audit. Browser retrieval also
failed. `sources.json` preserves those limitations. In particular, the pinned
`airoha_eth.c`, `airoha_regs.h`, and `airoha_gpon_omci.c` are not available in
this evidence directory. Do not substitute our current Ethernet driver and
label it as Sirherobrine's implementation.

## 1. Byte limits: distinguish hardware classification from OMCI validation

| Layer | NAND factory implementation | Pinned Sirherobrine implementation | Our captured bench |
| --- | --- | --- | --- |
| GDM short / long threshold | **60 / 2000**, verified in binary | Exact register setting **unverified**; missing Ethernet source | **60 / 2000** initially; experiment **48 / 2000** |
| Baseline OMCI representation | TX normalization accepts **44 or 48**, reduces 48 to 44 before MIC handling | Generic decoder and TX validator accept **44 or 48** | Actual received skb is **48**, including the four-byte MIC |
| Extended OMCI representation | TX normalization uses **10 + declared content length**; handles a trailing four-byte field | **10 + declared content length**, optionally another **4** bytes for MIC | Authenticator requires exactly **10 + content + 4** on RX |
| Ethernet runt descriptor bit | No runt rejection in examined QDMA-to-PON OMCI receive path | Exact descriptor policy not verified without Ethernet source | Rejected independently in native driver and packet adapter |

### NAND register setting

`gwan_channel_init` (`xpon_10g.ko`, `.text` address `0x45818`) loads:

```text
0x45830: w2 = 0x3c    (minimum 60)
0x45834: w1 = 0x7d0   (maximum 2000)
0x45838: w0 = 1       (GDM selector)
0x4583c: call 0x414bc (packet-length hook)
```

The anonymous hook puts maximum/minimum in structure offsets 56/60 and calls
FE operation zero. `fe_api_set_pkt_length` (`fe_core.ko`, `0xfdf0`) selects
OEM register address `0xbfb51514` for selector 1 and installs the limits in
the high/low halves. It does not convert 60 to an OMCI-specific minimum.
See `oem/gwan_channel_init.dis`, `packet_length_hook.dis`, and
`fe_api_set_pkt_length.dis`.

### Why the factory can accept OMCI with that setting

The examined QDMA routines pass receive metadata to the PON callback; they
do not use the PON word's runt bit as a rejection condition. In
`gwan_process_rx_message` (`0x44d64`), the CRC bit is inspected for diagnostic
logging, then bit 8 selects OMCI. If bit 30 (`no_mic`) is present, it calls
`gwan_check_ds_omci_mic`; a failed MIC returns an error. There is no bit-12
runt rejection in this path. This is a binary finding, not an assertion that
every factory error-handling choice should be copied.

Our two unconditional masks are:

- Native `airoha_pon_rx_meta`: `words[0] & GENMASK(13, 11)`.
- `q1000k_pwan_rx_prepare`: the same mask.

In the bench, `0x40001100` marked OMCI + no-MIC + runt. Lowering the threshold
to 48 produced `0x40000100`, and the actual 48-byte frame passed software
authentication. The interface byte counter reported 52 bytes per packet;
that accounting did not mean the skb had a removable four-byte trailer.
No FCS-trimming workaround was enabled.

### Additional NAND byte limits

The anonymous TX normalizer at `0x43358` rejects buffers shorter than 10;
baseline device ID `0x0a` must be exactly 44 or 48. It removes four bytes
from the 48-byte representation. `gwan_add_us_omci_mic` (`0x434c4`) processes
44 baseline bytes and appends four MIC bytes after successful CMAC.

The userspace adapter adds another, distinct rule:
`omci_lib_adpt_socket_transmit` at `0x31d30` in
`libapi_omci_adpt.so.1.0.0` passes **max(length, 44)** to `sendto` on its normal
socket path (`0x31d74`–`0x31d9c`). Its receive buffer capacity is 2,000 bytes.
The `sendto`/`recvfrom` targets are verified by dynamic relocations at
`0xb8ae8`/`0xb8bd8`. These are userspace transport rules, separate from the
60-byte GDM threshold.

### Consequence for our fix

**48 is a proven baseline diagnostic setting, not a universal OMCI minimum.**
An extended authenticated PDU has a 10-byte header plus its content plus a
four-byte MIC and may therefore be shorter than 48.

Preferred production direction: retain normal Ethernet short-frame policy,
but treat the runt bit as an Ethernet classification for OMCI. Apply this
consistently in both guards. Keep CRC, oversize and aggregation checks; require
the OMCI format, exact declared length, expected GEM/session, and a valid MIC
before admitting the packet. A metadata exception alone never authenticates it.

## 2. Factory allocation and pending responses

The inspected factory allocation path is:

```text
gponDevAssignNewAllocId (0x12258)
  allocate -> gwan_create_new_tcont (0x41db4)
                -> gponDevEnableTCont / gponDevSetTCont
                -> channel-specific forwarding / QDMA setup
              xpon_reset_qdma_tx_buf (0x53bc4)
              report allocation event
  remove   -> gwan_remove_tcont (0x41f7c)
                -> retire the selected channel
                -> delay -> gponDevDisableTCont
```

`gponDevSetTCont` updates a selected table entry and checks completion. This
path does not perform our full OMCC/key/pipeline teardown. Despite its name,
`xpon_reset_qdma_tx_buf` recalculates buffer thresholds: its hook is command
`0x12`, matching `QDMA_API_SET_TXBUF_THRESHOLD` in the vendor-source analogue.
It is not evidence of flushing every queued OMCI response. The source
analogue helps name the operation; the binary arguments and calls are the
primary evidence.

Factory OMCI responses follow a separate route:

```text
omciPktSend (userspace 0x40d9b0)
  -> priority-dependent message queue + semaphore
  -> userspace socket transport
  -> pwan_net_start_xmit
  -> gwan_prepare_tx_message
  -> optional gwan_add_us_omci_mic
  -> QDMA, OMCI flag, T-CONT/channel 0
```

The userspace queue does not carry our software authentication-epoch token.
In the software-assisted MIC path, the kernel selects the current OMCI
integrity-key index and invokes the hardware CMAC engine at transmission
preparation time. The examined allocation functions do not rewrite that
OMCI integrity-key index or mark the OMCI backend inactive.

**Interpretation:** ordinary data allocation avoids creating our global
reconfiguration window. I found no corresponding requirement to rescue an
authenticated response from a factory equivalent of `qomci_close()` here.
This is not proof of the factory daemon's correctness under every reset,
rekey, queue-overflow, or loss-of-signal race.

## 3. Sirherobrine allocation and pending responses

Source links at the pinned revision:

- [GPON hardware driver](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/drivers/net/ethernet/airoha/airoha_xpon.c)
- [OMCI core](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/net/xpon/omci/core.c)
- [OMCI decoder](https://github.com/Sirherobrine23/airoha_kernel/blob/2e2cf91fe84467d77649efebd99a28284f2124b3/net/xpon/omci/wire.c)

### Ordinary allocation

`gpon_set_alloc_id_hw` (driver line 942) holds `tcont_lock` and updates the
individual allocation. Lookup/free-slot scans begin at **index 1**; index 0
is reserved for OMCI. `gpon_config_tcont_hw` (line 771) updates the selected
hardware T-CONT and Ethernet channel, rolling back that entry on failure.
`gpon_cb_set_alloc_id` (line 1556) then reconciles OMCI services.

OMCC setup/removal is a separate operation, `gpon_cb_set_omci_gem` (line
1455). It installs/removes T-CONT 0, the OMCI GEM and its valid register.
The ordinary allocation callback does not invoke that OMCC teardown.

### Explicit session changes

`omci_device_reset_session` (core line 1576) first calls
`flush_work(&odev->rx_work)` when the caller is not that worker. Only then
does it advance the generation, invalidate ONU/GEM IDs, close the channel,
cancel remaining work and purge the RX queue.

`gpon_disable` (driver line 2198) explicitly invokes that drain/reset before
disconnecting GDM2. Its comment explains the case where Deactivate_ONU-ID
and a last OMCI request share a downstream frame. This is an ordering intent
to finish response generation/QDMA submission, not proof of optical delivery;
the same function disables frontend TX earlier.

`omci_device_set_channel` (core line 1538) advances the generation only when
channel validity or the valid GEM ID changes. Ordinary data-T-CONT updates
do not by themselves change that generation. The agent also caches the last
request/response for duplicate requests (`agent.c`, lines 4180–4279).

This is the **GPON** reference. Its `ops->xmit(odev, skb, gem)` interface lacks
our XGS authentication-epoch argument. It provides a useful lifecycle
pattern, but does not prove correctness of pending authenticated XGS OMCI
responses across an integrity-key change.

## 4. The captured Q1000K race

Independent repeat log:

`collection-registration-afe59a8089-20260917-205022-omci-response-epoch/activation-reg-combined.log`

Both failures have the same result:

| Field | First activation | Second activation |
| --- | --- | --- |
| Response epoch / published epoch | 9 / 9 | 19 / 19 |
| Backend active | false | false |
| Keys valid / epoch matches / GEM matches / protocol OK | all true | all true |
| Error | `-129`, `EKEYREJECTED` | `-129`, `EKEYREJECTED` |
| Last authenticated RX to rejection | 142.47580 ms | 149.29976 ms |
| Assign_Alloc-ID dispatch completion to rejection | 0.42044 ms | 0.37304 ms |

The PLOAM ID is `0x0a`, verified against `gpon_ploam_raw.h`; this confirms
Assign_Alloc-ID rather than merely inferring it from an ACK sequence.

Relevant current source:

1. Patch `038-q1000k-alloc-service-reconcile.patch` calls
   `q1000k_omci_alloc_changed()` after changing an allocation.
2. That calls `qomci_request()`, which immediately calls `qomci_close()`.
3. `qomci_close()` clears `b->active` and closes service admission.
4. An already-admitted OMCI request still owns the core's `session_lock`.
5. Its response enters `qomci_xmit()` with the same epoch and valid keys,
   and fails the `!b->active` test.
6. The control worker's subsequent `omci_device_set_auth_epoch(0)` waits
   for `session_lock`, but the backend was already closed before that wait.

The queued control transaction also revokes the transport epoch, purges queued
authenticated packets, rebuilds the pipeline and publishes a new epoch even
when the integrity context is unchanged. Native TX metadata has its own DMA
generation. Preserving only the numerical authentication epoch or removing
the `active` check would leave these other failure paths unresolved.

## 5. Implementation and single-image validation plan

Separate three concepts: authenticated session, admission of new requests,
and temporary readiness of the physical TX path.

| Change / experiment | Required observation |
| --- | --- |
| Current min-48 mode as a baseline control; min-60 with OMCI-specific runt handling as the candidate | Same valid baseline OMCI accepted; Ethernet runts still rejected; native and adapter rejection counters distinguish the layer |
| Extended length boundaries in host tests, plus captured extended traffic if the OLT sends it | Correct short extended messages accepted; truncated, oversized, inconsistent-length and bad-MIC messages rejected |
| Individual data-T-CONT updates, preserving channel 0 and unchanged integrity session | Allocation changes do not close OMCC, revoke auth or increment its epoch; register readback confirms the target entry |
| An accepted OMCI request held across an allocation update in a deterministic concurrency test | Exactly one response completes under the original valid session; no `EKEYREJECTED` caused solely by temporary TX pause |
| Bounded response queue if a full physical pause remains necessary | Preserve request/session/GEM/key association; enqueue to native DMA only after readiness returns; record queue age, completion and expiry |
| Real session invalidation while a response is pending | No old-session response is transmitted after reset, ONU reassignment or integrity-key revocation; explicit drop reason |
| Duplicate request after a transient TX failure | Cached response can be resent in the same valid session without reapplying provisioning side effects |

For a full pause, stop admitting new RX requests separately from authenticated
TX eligibility. Wait for already-admitted work only outside locks that its
callbacks need; never wait for the current worker itself. If responses must
wait, hold them above native DMA preparation, preserve their original
authentication context, and assign current DMA metadata only when resuming.
Keep the existing queue bounds/deadlines and add reason-specific counters.
Real authentication/session revocation must still reject obsolete work.

Diagnostics should cover request admission, session/epoch, allocation
generation, response creation, deferred TX, native DMA submission/completion,
timeouts and invalidation reasons. The next connected bench can exercise
these comparisons in one image without requiring a physical fiber outage.
