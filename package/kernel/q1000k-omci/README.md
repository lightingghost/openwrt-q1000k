# Experimental Q1000K OMCI core

The original generic PON/OMCI sources were imported unchanged from
[airoha_kernel 2e2cf91fe84467d77649efebd99a28284f2124b3](https://github.com/Sirherobrine23/airoha_kernel/tree/2e2cf91fe84467d77649efebd99a28284f2124b3/net/xpon).
The import is a separate commit retaining the author's identity. The GPON/EPON
hardware driver and EPON OAM implementation are not part of this package.

`kmod-q1000k-omci` builds `xpon.ko` and `omci.ko` for Linux 6.18. It remains
optional, `BROKEN`, and without autoload. Installing the package does not
provide a Q1000K hardware backend, start an OMCC, or enable optics. It must not
be used as evidence that PON service is ready.

## Provider contract

- Startup requires an explicitly supplied serial number, transport start/stop,
  transmit, topology, T-CONT, GEM, UNI, and atomic service-set callbacks. The
  generic default identity cannot start a provider. The provider must also
  supply the board's equipment, deployment role and UNI topology; imported
  defaults are not a validated Q1000K MIB.
- `omci_device_receive()` consumes the skb, including on rejection. Call it
  from process or softirq context with `OMCI_F_MIC_VALID` only after verifying
  that exact packet with the current session key. A descriptor's `no_mic` bit,
  MIC presence, or a global MAC error counter is not an authentication result.
  Pass the captured nonzero authentication epoch as the final argument; do
  not relabel a queued packet after rekeying. CRC errors, wrong OMCC, stopped
  transport, stale epochs and missing authentication are
  rejected. Nonlinear input is linearized before parsing.
- All control/session APIs may sleep. Callbacks must not re-enter them. RX is
  serialized with session transitions through one session mutex; TX and channel
  replacement share a separate TX mutex. Stop closes admission before draining
  accepted work and in-flight transmissions. The provider must independently
  stop all RX producers before unregistering and ensure hardware resources are
  quiescent before destroying itself. These software barriers do not drain FE
  or optical hardware.
- Establish the OMCC, then call `omci_device_set_auth_epoch()` with a strictly
  increasing nonzero token after keys are verified. Before replacing keys,
  call it with zero outside provider locks: it waits for current RX processing
  and provider TX, closes authenticated admission, and purges queued work.
  Rekeying preserves the MIB. ONU/channel changes and reset invalidate the
  epoch; prior tokens cannot be reused. Generations are 64-bit and saturation
  permanently closes admission instead of wrapping.
- `xmit` receives the authorization epoch captured before PDU preparation and
  must use that exact key generation. `OMCI_CAP_PROVIDER_MIC` requests a PDU
  without its trailer MIC so a software provider can append it. It does not
  advertise hardware cryptography.
- Successful `xmit` consumes its skb. On any error it must leave ownership with
  the caller. RX from a synchronous provider `start` callback is discarded;
  the provider may deliver packets after startup returns successfully.
- `replace_services` replaces the entire set atomically, including removal
  through a null pointer/count zero. It borrows the array only for the call.
  Ordinary failure must preserve the previous set. Return `-EUCLEAN` if the
  old state cannot be guaranteed; subsequent provisioning then fails closed.
  The core preallocates the software snapshot before calling the backend and
  publishes it without allocating after success. Legacy per-rule callbacks
  are retained in the imported structure but are not used for Q1000K commits.
- Class 171 service records carry the complete filter/treatment, input/output
  TPIDs and downstream mode. The provider must validate the whole operation;
  `pon0` now represents the customer/UNI side. Missing/unsupported rules cannot
  fall back to a broad classifier. An empty configured table installs no
  service. Combined class 84/171 paths are rejected until both filter stages
  are represented. See the [VLAN contract](../../../target/linux/airoha/XGSPON-VLAN.q1000k.md).
- Failed service removal preserves the previous records and aborts MIB reset.
  Session-transition removal failure latches an error rather than reopening
  admission with unaccounted hardware state. Provider teardown remains
  responsible for retiring its resources; unregister cannot prove this.
- Unsupported T-CONT/GEM/UNI operations return errors. Fake-success and
  permissive configuration are rejected, including profile quirks that would
  hide unsupported operations.

The imported ten-byte GPON password field is separate from XGS-PON
registration/MSK configuration. It must not truncate or stand in for XGS-PON
credentials. No key-generation or hardware MIC-verification callback is
implemented by this core package.

## Local validation

Build using the experimental source checkout only:

```sh
make -j8 package/kernel/q1000k-omci/compile CONFIG_PACKAGE_kmod-q1000k-omci=m V=s
tests/q1000k/run_omci_core_uml.sh
```

The UML test builds the entire production core, adding the fixture to a
temporary source copy. It checks required identity/callbacks, failed startup,
authentication admission flags, CRC and GEM rejection, nonlinear cloned skbs,
baseline/extended bounds, 256 queued packets, 20 concurrent stop cycles,
unsupported provisioning, atomic backend errors, preserved MIB/service state,
cookie collisions, service limits and permanent inconsistency containment.
Lockdep, RCU and atomic-sleep diagnostics are enabled. The guest mounts hostfs
read-only and has no Q1000K hardware access. Passing these tests does not
authenticate an actual OMCI exchange or establish OLT interoperability.
