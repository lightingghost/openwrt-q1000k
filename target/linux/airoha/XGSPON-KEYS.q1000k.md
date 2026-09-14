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
module loading, device writes or flashing occurred. OMCI data-GEM key-ring
provisioning remains incomplete at this checkpoint.
