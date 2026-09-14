# Q1000K unicast data keys

Vendor r54 implements the AES-128 Key_Control/Key_Report path on the owned
protocol executor. Authentication and ONU addressing precede dispatch. Only
O5 with registration-derived PIK/KEK bank zero and completed ranging accepts
requests. AES-256 and secure mutual-authentication rekey remain unsupported.

The implementation follows [G.9807.1](https://www.itu.int/epublications/ar/publication/itu-t-g-9807-1-2023-02-10-gigabit-capable-symmetric-passive-optical-network-xgs-pon)
15.5.3 and the clarifications in
[G Supplement 81](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.Sup81-202510-I%21%21PDF-E&lang=e&type=items)
7.2.5, particularly Figure 7-8 and Table 7-4. Duplicate Generate reuses the
pending key; active-key regeneration discards incompatible pending state.
Confirmation activates a generated key and removes the old key. An inquiry
about an invalid key uses the specified swapped-index response. Retransmission
and lifetime management belong to the OLT; legacy ONU TK4/TK5 are not started.

Only one authenticated request is pending at a time. Other key requests get
EBUSY for OLT retry. A reset cancels pending work. Preparation runs outside
the executor while admission is closed; generation checks reject stale work.
Kernel randomness must be initialized before new key generation. ECB and
CMAC own different transforms because their imported helpers have separate
locks. Temporary and discarded key material is erased and never logged.

The complete CPU/FE/MAC/PHY drain precedes replacement of the two unicast AES
banks. Register writes are read back; invalid banks are zeroed. The activation
callback checks the key-switch event before optical/CPU admission and clears
only its W1C status bit. A report is submitted after successful activation;
software state is published after successful submission. Provider faults,
FIFO exhaustion/overrun and partial writes cause protocol containment.

PLOAM FIFO availability is checked for the complete 11-word MAC message.
The prefix selects the installed integrity key; hardware adds the wire MIC.
The report contains a wrapped 16-byte key or CMAC key name with zero padding.
All other legacy PLOAM senders now use the same owned, bounded FIFO helper.

Local evidence: 72 PON host tests, AN7581 vendor r54 build, and real Linux
UML AES/CMAC/key-state tests. Hardware key-switch event timing, FIFO semantics,
OLT interoperability and encrypted OMCC/data traffic remain untested. No
module loading, device writes or flashing occurred. Hardware acceptance remains pending.

## GEM key rings (vendor r55, core r9)

The class 268 key-ring attribute is one byte at attribute 10 (mask 0x0040),
read/write and set-by-create, as specified in
[G.988 9.2.3](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.988-202211-I%21%21PDF-E&lang=e&type=items).
The inherited two-byte read/write-only layout and unconditional unencrypted
provider call were corrected. Ring 0 disables payload encryption policy;
ring 1 selects unicast bidirectional encryption; ring 3 selects unicast
downstream encryption. Ring 2 (broadcast) and reserved values are rejected
by this provider. The core retains the ring in complete service snapshots.

The vendor GEM table bit selects upstream encryption, following the imported
10G implementation. Downstream policy is retained separately in record
snapshots and publication, including unrelated GEM/T-CONT changes and reset.
The MAC selects receive keys from XGEM headers and discards invalid keys;
this behavior still needs hardware traffic validation. The provider's
Encryption state Get reports AES-128 only when the port permits encryption
and a receive key is installed. A failed query is not reported as unencrypted
success. ONU2-G rejects unadvertised security modes.

The 72 host tests include both encryption directions, record preservation,
service reconciliation and stale-policy rejection. The full OMCI UML suite
covers one-byte Create/Set encoding, provider forwarding and state Get
success/failure. Matching core r9/vendor r55 packages build for AN7581.
