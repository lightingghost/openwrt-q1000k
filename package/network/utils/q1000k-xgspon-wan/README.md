# Optional optical WAN configuration

This package creates `q1000k_wan` (DHCP) and `q1000k_wan6` (DHCPv6) in netifd,
with `auto=0` on both. It adds those logical interfaces to the existing,
uniquely named `wan` firewall zone. Its existing DHCP, ICMPv6 and LAN-forwarding
policy therefore also covers the optical WAN. A customized WAN zone must have
those rules if that traffic is wanted; this package does not reset its policy.
The package does not invoke ifup, reload networking/firewall, enable the PON
supervisor, load modules or change the lower interface.

Both interfaces use `pon`, the UNI-facing customer interface. OMCI supplies
supported optical tag translations. No provider VLAN ID, priority tag, PPPoE
credential, DHCP vendor identifier, MAC override or IPv6 prefix length is
invented. DHCPv6 requests an address if offered and lets the server select the
prefix size. A line requiring a tagged customer-side service needs explicit
netifd VLAN configuration derived from its actual OMCI service.

Installation requires existing readable network/firewall configurations and
exactly one `wan` zone, with no staged UCI changes. Existing LAN, copper WAN,
bridge membership, routes and firewall rules are retained. Named optical
sections without this package's ownership marker cause setup to stop before
editing anything. A completed installation is not reapplied on reinstall, preserving user
settings, enabled state and removed firewall associations. Pending ownership
markers distinguish interrupted setup from a completed installation. Interrupted installation
can leave only inactive interfaces; the defaults script is retained on failure
and can finish the missing associations on a later run. A failed UCI command
may leave staged changes for inspection; setup refuses to commit them silently
on retry.

The optical supervisor is a separate opt-in. Future hardware acceptance must
first establish the board resources, lower link, calibration, line identity,
OLT registration and a provisioned unicast service. Only then should these
interfaces be enabled explicitly. Under the current read-only-device and
no-flashing restriction, none of these activation steps may be executed.
