# AT&T Fiber integration notes

Researched 2026-09-14 after the user identified AT&T Fiber as the provider.
The user reports a **BGW320-500** with the fiber plugged into a **Nokia
3FE46901AC** module in the gateway's SFP cage. Nokia's product guide identifies
that exact module as **XGS-PON**. This supports targeting the XGS-PON path;
the provisioned identity and actual OLT service MIB remain unknown. No device
connection or configuration change was made for this research.

The module is the pluggable optical transceiver; the BGW320 performs the ONT
functions. Its part number does not identify the OLT vendor, subscriber serial,
registration ID or optical VLAN. The cited Nokia guide describes the BGW320-505;
it is used here to identify the exact optical part, not to relabel the user's
BGW320-500 gateway.

For AT&T **XGS-PON**, the working community integration uses **DHCP** on the
router WAN. Its ONT implementation obtains the optical service VLAN from
provisioning and normally presents Internet traffic untagged to the router.
This does not establish a universal optical VLAN ID for AT&T. The value zero
in the referenced `INTERNET_VLAN` option means untagged service; it is not
evidence that the OLT uses a priority-tagged VLAN 0.

Sources:

- [Nokia product guide, table 3-3, page 20](https://www.manualslib.com/manual/3154842/Nokia-Ont-Bgw320-505.html?page=20)
  identifies 3FE46901AC as XGS-PON and describes the gateway's PON termination.
  The same manufacturer document is filed as
  [FCC exhibit 6215013](https://fccid.io/2ADZRBGW321/User-Manual/User-Manual-6215013).
- [AT&T BGW320 XGS-PON integration guide](https://pon.wiki/guides/masquerade-as-the-att-inc-bgw320-500-505-with-the-was-110/)
  specifies DHCP WAN and device-specific OMCI identity requirements. It also
  requires confirming XGS-PON; AT&T service alone does not identify the PON
  technology. Its WAS-110 instructions are not Q1000K activation instructions.
- [8311 configuration detection](https://github.com/djGrrr/8311-xgspon-bypass/blob/master/8311-detect-config.sh)
  discovers the unicast optical VLAN from the installed service.
- [8311 VLAN mapping implementation](https://github.com/djGrrr/8311-xgspon-bypass/blob/master/8311-fix-vlans.sh)
  maps the configured local Internet VLAN (untagged by default) to that
  discovered optical VLAN. These scripts were read, never executed.

Implementation policy: use DHCP for the eventual subscriber WAN and derive
VLAN treatment from the actual class 171/bridge/GEM provisioning. Keep unknown
identity and VLAN values explicit; do not copy an example subscriber's serial,
equipment ID or credentials. Q1000K factory identity is not proof of AT&T
authorization. The supervisor currently does not create a WAN interface.

Vendor r58 and core r12 apply supported class 171 transformations with `pon`
facing the UNI/customer side. An OLT-provisioned untagged-to-tagged rule can
therefore present an untagged DHCP WAN without a hard-coded optical VLAN.
This is tested locally, not against AT&T. Combined class 84/171 pipelines and
unicast key exchange are implemented, but actual OLT provisioning, encrypted
traffic and successful DHCP still need hardware acceptance. See the
[VLAN contract](XGSPON-VLAN.q1000k.md).
The current RAM bench permits bounded tests with TX inhibited; firmware flashing
remains prohibited. Connected-fiber receive testing does not authorize registration.

## Identity and 8311 compatibility settings

Vendor r72/core r15 and the OMCI CLI/LuCI expose every PON identity control
shown in the user's 8311 screenshot. See the [complete setting table and CLI
examples](../../../package/network/utils/q1000k-omci-tools/README.md).
The source for the screenshot's meanings is the
[8311 firmware builder](https://github.com/djGrrr/8311-was-110-firmware-builder/blob/master/README.md).
The BGW320-500 guide gives these reference values:

- Equipment ID: `iONT320500X`.
- Hardware version: `BGW320-500_2.1`, Circuit Pack synchronization enabled.
- Software A/B: `BGW320_4.27.7` is an example, not an assertion about this
  subscriber's current firmware. Both fields are independently configurable.
- ONT ID: obtain the complete `HUMA…` identity from the user's own gateway;
  it is not available to this development task and has not been invented.

No subscriber values are baked into the image. The registration and logical
credentials remain explicit and unset. An OMCI vendor override does not alter
the loader's PLOAM serial. Software bank overrides change only reported ME7
metadata; there is no flash or boot selection operation.

The `prx300_1U.ini` role maps to `mib_profile=native-pptp`: the hardware provider
pins SFU and one Ethernet UNI and omits the imported VEIP defaults. Foreign
PRX300 MIB files and firmware environment commands are not imported.

`fix_vlans=1` adds the priority-tagged VLAN-0 compatibility at the subscriber
boundary while preserving the OLT's service mapping, filters, drops and queues.
It tries untagged rules first, then retries an unmatched untagged upstream frame
as PCP-0/VLAN-0; downstream it strips a lone 802.1Q VLAN-0 tag. This is narrower
than 8311's multi-service remapping. It does not rewrite arbitrary nonzero WAN
VLANs or provision TV/voice. Actual AT&T class 84/171/GEM provisioning remains
necessary to establish whether this line needs any additional adaptation.

The new RAM bench can use configured OMCI presentation fields for local MIB
inspection, but deliberately retains its synthetic PLOAM serial and registration
ID. Receive-only testing does not send any identity to the OLT and cannot prove
AT&T authentication, O5, encryption, OMCI exchange or DHCP.

## Local inactive WAN configuration

The optional `q1000k-xgspon-wan` package now provides inactive netifd DHCP and
DHCPv6 interfaces on `pon`, using the existing WAN firewall zone. It sets no
optical VLAN, custom DHCP identity or fixed delegated-prefix size. Its setup
was tested with the real UCI parser in temporary directories; nothing was
installed or activated on the Q1000K. See the
[package contract](../../../package/network/utils/q1000k-xgspon-wan/README.md).
