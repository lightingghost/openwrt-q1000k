# Q1000K VLAN service contract

Vendor r52 / generic OMCI core r8, 2026-09-14. This is locally tested software,
not evidence of optical interoperability or AT&T Internet access.

`pon0` presents the customer/UNI side of a supported class 171 service. The
OMCI resolver retains the entire rule, input/output TPIDs and downstream mode.
The backend compiles every rule before physically replacing and publishing
the service table. An unsupported operation preserves the previous table and
returns an error. An empty configured table installs no fallback service.
The older assumption that userspace directly creates the post-treatment
optical VLAN on `pon0` no longer applies to class 171 services.

Supported operations include:

- Untagged, one-tag and two-tag classification; exact/wildcard VID and PCP,
  TPID/DEI criteria and the six defined EtherType filter values.
- Removal, insertion and replacement of up to two tags; fixed fields and
  inner/outer field copies, with input/output TPIDs 0x8100, 0x88a8 and 0x9100.
- Explicit discard and upstream default rules. Normal rules precede defaults,
  with the class 171 filter-byte ordering used between equally ranked rules.
- Downstream mode 0 inverse transformation and mode 1 unchanged forwarding.
  Removed wildcard fields use their lowest legal value for the inverse.
  Repeated copies must agree; upstream default/discard rules do not define an
  inverse. The first matching filter-byte rule resolves a many-to-one mapping.
- Mapper PCP selection after upstream transformation, explicit GEM queue
  pointers, and the existing GEM/Alloc-ID/UNI/direction checks in both directions.

VID 0 is a priority tag and is distinct from an untagged frame. Tag presence
comes from the class 171 priority fields, not from whether a VID is nonzero.
No AT&T optical VLAN ID is hard-coded. For example, an OLT rule mapping an
untagged UNI frame to VID 123 inserts that tag before native transmission and
removes it on the inverse path. The number 123 appears only in test fixtures.

DSCP-derived priority, downstream modes 2 and above, more than two tags and
combined class 84/171 pipelines currently return unsupported. The class 84
standalone forwarding-operation semantics still require completion. No broad
classifier substitutes for an unsupported class 171 operation. Multicast and
encrypted GEM activation remain separate incomplete integrations.

Tests compile the production compiler and packet selectors with failure
fixtures. They cover all TCI/EtherType values, tag copies and inverse mappings,
discard/default behavior, VLAN 0, table rejection and payload preservation.
The full OMCI UML suite verifies preservation of the raw rule and TPIDs and
that an empty/unsupported path cannot become a default service. A separate
UML test exercises actual Linux skb parsing and edits for every fragmentation
boundary and several headroom sizes, cloned buffers and hardware-offloaded
VLAN tags. No hardware provider is loaded in those guests.

Specification: [ITU-T G.988, class 171](https://www.itu.int/rec/dologin_pub.asp?id=T-REC-G.988-202211-I%21%21PDF-E&lang=e&type=items).
The supported subset and rejection behavior above are implementation limits.
