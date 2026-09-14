# AT&T Fiber integration notes

Researched 2026-09-14 after the user identified AT&T Fiber as the provider.
The user's gateway model, line technology, provisioned identity and actual
OLT service MIB remain unknown. No device connection or configuration change
was made for this research.

For AT&T **XGS-PON**, the working community integration uses **DHCP** on the
router WAN. Its ONT implementation obtains the optical service VLAN from
provisioning and normally presents Internet traffic untagged to the router.
This does not establish a universal optical VLAN ID for AT&T. The value zero
in the referenced `INTERNET_VLAN` option means untagged service; it is not
evidence that the OLT uses a priority-tagged VLAN 0.

Sources:

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

Vendor r52 and core r8 apply supported class 171 transformations with `pon`
facing the UNI/customer side. An OLT-provisioned untagged-to-tagged rule can
therefore present an untagged DHCP WAN without a hard-coded optical VLAN.
This is tested locally, not against AT&T. Combined class 84/171 pipelines,
data encryption and actual OLT interoperability still need completion before
DHCP can establish Internet access. See the [VLAN contract](XGSPON-VLAN.q1000k.md).
The device remains restricted to read-only access, with no firmware flashing.
