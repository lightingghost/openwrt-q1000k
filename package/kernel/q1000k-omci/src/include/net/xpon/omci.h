/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _NET_OMCI_H
#define _NET_OMCI_H

#include <linux/bits.h>
#include <linux/types.h>
#include <uapi/linux/omci.h>

struct device;
struct omci_device;
struct xpon_device;
struct sk_buff;

#define OMCI_IDENTITY_F_SERIAL_NUMBER	BIT(0)
#define OMCI_IDENTITY_F_VENDOR_ID	BIT(1)
#define OMCI_IDENTITY_F_PASSWORD	BIT(2)
#define OMCI_IDENTITY_F_VERSION	BIT(3)
#define OMCI_IDENTITY_F_EQUIPMENT_ID	BIT(4)
#define OMCI_IDENTITY_F_HARDWARE_VERSION BIT(5)
#define OMCI_IDENTITY_F_SOFTWARE_VERSION_0 BIT(6)
#define OMCI_IDENTITY_F_SOFTWARE_VERSION_1 BIT(7)
#define OMCI_IDENTITY_F_SYNC_CIRCUIT_PACK BIT(8)
#define OMCI_IDENTITY_F_ACTIVE_BANK BIT(9)
#define OMCI_IDENTITY_F_COMMITTED_BANK BIT(10)
#define OMCI_IDENTITY_F_LOGICAL_ONU_ID BIT(11)
#define OMCI_IDENTITY_F_LOGICAL_PASSWORD BIT(12)


/**
 * struct omci_identity - normalized ONU identity and registration data
 * @valid: OMCI_IDENTITY_F_* bitmap
 * @serial_number: canonical eight-byte GPON serial number
 * @vendor_id: canonical four-byte vendor identifier
 * @password: canonical ten-byte GPON registration password
 * @version: ONU-G version field
 * @equipment_id: ONU2-G equipment identifier
 * @serial_source: enum omci_config_source for @serial_number
 * @vendor_source: enum omci_config_source for @vendor_id
 * @password_source: enum omci_config_source for @password
 * @version_source: enum omci_config_source for @version
 * @equipment_source: enum omci_config_source for @equipment_id
 */
struct omci_identity {
	u32 valid;
	u8 serial_number[8];
	u8 vendor_id[OMCI_OLT_VENDOR_ID_LEN];
	u8 password[10];
	u8 version[OMCI_OLT_VERSION_LEN];
	u8 equipment_id[OMCI_OLT_EQUIPMENT_ID_LEN];
	u8 serial_source;
	u8 vendor_source;
	u8 password_source;
	u8 version_source;
	u8 equipment_source;
	/* Optional OMCI presentation; independent of the PLOAM identity. */
	u8 presentation_source;
	u8 software_version[2][14];
	u8 hardware_version[14];
	u8 sync_circuit_pack;
	u8 active_bank;
	u8 committed_bank;
	u8 logical_onu_id[24];
	u8 logical_password[12];
};

/**
 * struct omci_me_class - managed entity class descriptor
 * @class_id: ITU managed entity class identifier
 * @category: enum omci_class_category
 * @support: enum omci_class_support
 * @flags: OMCI_CLASS_F_* bitmap
 * @name: stable human-readable managed entity name
 */
struct omci_me_class {
	u16 class_id;
	u8 category;
	u8 support;
	u32 flags;
	const char *name;
};

enum omci_gem_port_direction {
	OMCI_GEM_PORT_DIRECTION_UNI_TO_ANI = 1,
	OMCI_GEM_PORT_DIRECTION_ANI_TO_UNI = 2,
	OMCI_GEM_PORT_DIRECTION_BIDIRECTIONAL = 3,
};

/**
 * struct omci_olt_g - parsed OLT-G identification attributes
 * @vendor_id: four-character OLT vendor identifier
 * @equipment_id: OLT equipment or model identifier
 * @version: OLT software or hardware version identifier
 * @vendor_id_valid: @vendor_id has been received from the OLT
 * @equipment_id_valid: @equipment_id has been received from the OLT
 * @version_valid: @version has been received from the OLT
 * @valid: at least one identification attribute is available
 */
struct omci_olt_g {
	char vendor_id[OMCI_OLT_VENDOR_ID_LEN + 1];
	char equipment_id[OMCI_OLT_EQUIPMENT_ID_LEN + 1];
	char version[OMCI_OLT_VERSION_LEN + 1];
	bool vendor_id_valid;
	bool equipment_id_valid;
	bool version_valid;
	bool valid;
};

/**
 * struct omci_olt_profile_state - resolved OLT interoperability policy
 * @configured: user-selected profile, including auto
 * @effective: active concrete profile
 * @forced: forced concrete profile, or OMCI_OLT_PROFILE_UNSPEC
 * @quirks: OMCI_OLT_QUIRK_* bitmap for @effective
 * @olt: latest parsed OLT-G identity
 */
struct omci_olt_profile_state {
	u8 configured;
	u8 effective;
	u8 forced;
	u32 quirks;
	struct omci_olt_g olt;
};

/**
 * struct omci_vlan_filter_entry - parsed VLAN tagging filter entry
 * @tci: original VLAN tag control information value
 * @vid: VLAN identifier
 * @pbit: priority code point
 * @dei: drop eligible indicator
 */
struct omci_vlan_filter_entry {
	u16 tci;
	u16 vid;
	u8 pbit;
	u8 dei;
};

/**
 * struct omci_vlan_tagging_filter - parsed class 84 attributes
 * @entries: valid VLAN filter entries
 * @forward_operation: forwarding operation from the managed entity
 * @num_entries: number of valid entries in @entries
 * @valid: parsed data is available
 */
struct omci_vlan_tagging_filter {
	struct omci_vlan_filter_entry entries[OMCI_VLAN_FILTER_MAX_ENTRIES];
	u8 forward_operation;
	u8 num_entries;
	bool valid;
};

/**
 * struct omci_extended_vlan_rule - parsed class 171 table entry
 * @raw: original 16-byte table entry
 * @filter_outer_vid: outer-tag VLAN identifier match
 * @filter_inner_vid: inner-tag VLAN identifier match
 * @treat_outer_vid: outer-tag VLAN identifier treatment
 * @treat_inner_vid: inner-tag VLAN identifier treatment
 * @filter_outer_pbit: outer-tag priority match
 * @filter_outer_tpid_dei: outer-tag TPID/DEI match mode
 * @filter_inner_pbit: inner-tag priority match
 * @filter_inner_tpid_dei: inner-tag TPID/DEI match mode
 * @filter_ethertype: EtherType match mode
 * @tags_to_remove: number of tags removed, or discard mode
 * @treat_outer_pbit: outer-tag priority treatment
 * @treat_outer_tpid_dei: outer-tag TPID/DEI treatment mode
 * @treat_inner_pbit: inner-tag priority treatment
 * @treat_inner_tpid_dei: inner-tag TPID/DEI treatment mode
 * @delete: entry encodes a table deletion
 */
struct omci_extended_vlan_rule {
	u8 raw[OMCI_EXT_VLAN_RULE_LEN];
	u16 filter_outer_vid;
	u16 filter_inner_vid;
	u16 treat_outer_vid;
	u16 treat_inner_vid;
	u8 filter_outer_pbit;
	u8 filter_outer_tpid_dei;
	u8 filter_inner_pbit;
	u8 filter_inner_tpid_dei;
	u8 filter_ethertype;
	u8 tags_to_remove;
	u8 treat_outer_pbit;
	u8 treat_outer_tpid_dei;
	u8 treat_inner_pbit;
	u8 treat_inner_tpid_dei;
	bool delete;
};

/**
 * struct omci_extended_vlan - parsed class 171 attributes and table
 * @rules: parsed VLAN operation table
 * @dscp_to_pbit: DSCP-to-P-bit mapping attribute
 * @input_tpid: input TPID
 * @output_tpid: output TPID
 * @associated_me: associated managed entity pointer
 * @max_table_size: advertised maximum number of table entries
 * @association_type: associated managed entity type
 * @downstream_mode: downstream VLAN processing mode
 * @rule_count: number of valid rules in @rules
 * @valid: parsed data is available
 */
struct omci_extended_vlan {
	struct omci_extended_vlan_rule rules[OMCI_EXT_VLAN_MAX_RULES];
	u8 dscp_to_pbit[24];
	u16 input_tpid;
	u16 output_tpid;
	u16 associated_me;
	u16 max_table_size;
	u8 association_type;
	u8 downstream_mode;
	u8 rule_count;
	bool valid;
};

/**
 * struct omci_telemetry - current PON and optical telemetry
 * @valid: OMCI_TELEMETRY_F_* bitmap
 * @bosa_temperature_mc: BOSA temperature in milli-degrees Celsius
 * @bosa_voltage_uv: optical frontend supply voltage in microvolts
 * @bosa_bias_ua: laser bias current in microamps
 * @bosa_tx_power_nw: transmitted optical power in nanowatts
 * @bosa_rx_power_nw: received optical power in nanowatts
 * @bosa_alarms: hardware-specific optical alarm bitmap
 * @downstream_fec: enum omci_fec_status
 * @upstream_fec: enum omci_fec_status
 */
struct omci_telemetry {
	u32 valid;
	s32 bosa_temperature_mc;
	u32 bosa_voltage_uv;
	u32 bosa_bias_ua;
	u32 bosa_tx_power_nw;
	u32 bosa_rx_power_nw;
	u32 bosa_alarms;
	u8 downstream_fec;
	u8 upstream_fec;
};

/**
 * struct omci_ani_topology - pre-existing upstream traffic resources
 * @tcont_base: first T-CONT managed entity ID
 * @scheduler_base: first Traffic Scheduler managed entity ID
 * @queue_base: first Priority Queue managed entity ID
 * @maximum_queue_size: maximum queue size reported in 2048-byte blocks
 * @allocated_queue_size: initial allocated queue size in 2048-byte blocks
 * @tcont_count: number of upstream T-CONT resources
 * @queues_per_tcont: number of upstream priority queues per T-CONT
 * @queue_config_option: Priority Queue configuration option
 * @scheduler_policy: initial Traffic Scheduler policy
 *
 * The OMCI agent uses this description to seed complete Priority Queue and
 * Traffic Scheduler managed entities before the first MIB upload. Entity IDs
 * are allocated contiguously. Queue entity IDs are grouped by T-CONT.
 */
/* Complete GEM QoS references, retained even when a provider rejects them.
 * Null optional pointers may be encoded as zero or 0xffff by OLT profiles.
 */
struct omci_gem_qos {
	u16 upstream_queue, upstream_descriptor;
	u16 downstream_queue, downstream_descriptor;
	u8 traffic_management_option;
};

struct omci_gem_port_config {
	struct omci_gem_qos qos;
	u16 port_id, tcont_entity_id;
	u8 direction, encryption_key_ring;
};

/**
 * struct omci_service_config - normalized upstream OMCI service
 * @cookie: stable identifier used to replace or delete one service rule
 * @uni_entity_id: normalized PPTP Ethernet UNI or VEIP entity
 * @gem_ctp_entity_id: GEM port network CTP managed entity
 * @gem_qos: complete GEM scheduling and descriptor references
 * @gem_port_id: GEM port identifier programmed in the GPON MAC
 * @tcont_entity_id: T-CONT managed entity associated with the GEM port
 * @alloc_id: Alloc-ID of that T-CONT, so a backend whose entity map was
 *	cleared by a GPON restart can still resolve the channel
 * @vlan_id: VLAN identifier used for upstream classification
 * @mapper_valid: PCP selection comes from an IEEE 802.1p mapper
 * @mapper_unmarked_pcp: fixed implied priority for untagged upstream frames
 * @pcp: IEEE 802.1p priority used for classification and queue selection
 * @queue: hardware upstream queue
 * @encryption_key_ring: G.988 GEM key ring (0 none, 1 unicast both, 2 broadcast, 3 unicast downstream)
 * @direction: enum omci_gem_port_direction
 * @vlan_rule: complete class 171 rule, with explicit tag presence
 * @vlan_filter: class 84 filters on the UNI and ANI bridge ports, in that order
 * @vlan_entity_id: class 171 table identity, for first-match selection
 * @vlan_ani_side: class 171 acts after upstream bridge filtering and mapping
 * @vlan_input_tpid: class 171 input TPID
 * @vlan_output_tpid: class 171 output TPID
 * @vlan_downstream_mode: class 171 downstream processing mode
 * @vlan_treatment: raw class 171 treatment words for the backend
 * @multicast_ani_entity_id: Dasan WAN bridge port used as multicast ANI
 * @vlan_valid: @vlan_id participates in classification
 * @pcp_valid: @pcp participates in classification
 * @vlan_treatment_valid: @vlan_treatment requires backend processing
 * @multicast_ani_valid: @multicast_ani_entity_id was profile-resolved
 * @multicast: service represents a multicast GEM path
 * @default_service: fallback service when no VLAN/PCP rule matches
 */
struct omci_service_config {
	struct omci_gem_qos gem_qos;
	struct omci_vlan_tagging_filter vlan_filter[2];
	struct omci_extended_vlan_rule vlan_rule;
	u16 vlan_entity_id;
	bool vlan_ani_side;
	u16 vlan_input_tpid, vlan_output_tpid;
	u8 vlan_downstream_mode;
	u8 encryption_key_ring;
	u8 mapper_unmarked_pcp;
	bool mapper_valid;
	u32 cookie;
	u16 uni_entity_id;
	u16 gem_ctp_entity_id;
	u16 gem_port_id;
	u16 tcont_entity_id;
	u16 alloc_id;
	u16 vlan_id;
	u8 pcp;
	u8 queue;
	u8 direction;
	u8 vlan_treatment[8];
	u16 multicast_ani_entity_id;
	bool vlan_valid;
	bool pcp_valid;
	bool vlan_treatment_valid;
	bool multicast_ani_valid;
	bool multicast;
	bool default_service;
};

struct omci_ani_topology {
	u16 tcont_base;
	u16 scheduler_base;
	u16 queue_base;
	u16 maximum_queue_size;
	u16 allocated_queue_size;
	u8 tcont_count;
	u8 queues_per_tcont;
	u8 queue_config_option;
	u8 scheduler_policy;
};

/* Complete decoded candidates. Providers must reject fields they cannot apply;
 * returning an error leaves the previous configuration intact, or EUCLEAN
 * contains the datapath if physical restoration cannot be proven.
 */
struct omci_priority_queue_config {
	u16 maximum_size, allocated_size, discard_reset, discard_threshold;
	u16 tcont_entity_id, priority, scheduler_entity_id;
	u16 backpressure_operation, backpressure_occur, backpressure_clear;
	u32 backpressure_time;
	u8 configuration, weight;
};

struct omci_traffic_scheduler_config {
	u16 tcont_entity_id, parent_entity_id;
	u8 policy, priority;
};

/**
 * struct omci_device_ops - hardware transport and provisioning operations
 * @start: acquire and start the OMCI transport independently of netdev state
 * @stop: stop and release the OMCI transport
 * @xmit: transmit an OMCI PDU; consumes @skb only on success
 * @get_ani_topology: describe pre-existing ANI scheduling resources
 * @set_tcont: configure a T-CONT mapping
 * @set_gem_port: validate and configure the complete GEM candidate; reject
 *	unimplemented QoS references before modifying hardware
 * @get_gem_encryption: current GEM encryption mode (0 disabled, 1 AES-128)
 * @set_uni: enable or disable a UNI
 * @set_priority_queue: apply a complete existing upstream queue candidate
 * @set_traffic_scheduler: apply a complete existing scheduler candidate
 * @replace_services: atomically replace the entire service set; an error must
 *	leave the previous set intact, or return -EUCLEAN if that cannot be proven.
 *	The array is borrowed for this call only; NULL with count zero removes all.
 * @replace_service: atomically add or replace a normalized upstream service
 * @delete_service: remove a normalized upstream service by cookie
 * @get_telemetry: refresh PON FEC and optical telemetry
 * @set_olt_profile: apply a resolved OLT interoperability profile
 * @set_operational: report whether the in-kernel OMCI agent is operational
 * @config_changed: report a normalized runtime OMCI configuration change
 * @service_fault: close packet admission after an unverified provisioning
 *	rollback or -EUCLEAN. Called once with the agent mutex held; must not
 *	call back into the core or wait for its workers. Queue physical teardown
 *	if needed. The fault is permanent for this device registration.
 */
struct omci_device_ops {
	/* Optional observational callback. No payload/identity/key bytes. Called
	 * outside agent locks; provider must not re-enter OMCI lifecycle APIs. */
	void (*diagnostic)(struct omci_device *odev, u16 class_id, u8 opcode,
			   int error, u32 flags, u32 result);
	/* Nonzero values pin the physical UNI model before the first MIB. */
	u8 onu_type, uni_count;
	int (*start)(struct omci_device *odev);
	void (*stop)(struct omci_device *odev);
	int (*xmit)(struct omci_device *odev, struct sk_buff *skb,
		    u16 gem_port_id, u64 auth_epoch);
	int (*get_ani_topology)(struct omci_device *odev,
				struct omci_ani_topology *topology);
	int (*set_tcont)(struct omci_device *odev, u16 entity_id,
			 u16 alloc_id, bool valid);
	int (*set_gem_port)(struct omci_device *odev, u16 entity_id,
			    const struct omci_gem_port_config *config, bool valid);
	int (*get_gem_encryption)(struct omci_device *odev, u16 entity_id, u8 *mode);
	int (*set_uni)(struct omci_device *odev, u16 entity_id, bool enable);
	int (*set_priority_queue)(struct omci_device *odev, u16 entity_id,
			 const struct omci_priority_queue_config *config);
	int (*set_traffic_scheduler)(struct omci_device *odev, u16 entity_id,
			 const struct omci_traffic_scheduler_config *config);
	int (*replace_services)(struct omci_device *odev,
				const struct omci_service_config *services,
				size_t count);
	int (*replace_service)(struct omci_device *odev,
			       const struct omci_service_config *service);
	int (*delete_service)(struct omci_device *odev, u32 cookie);
	int (*get_telemetry)(struct omci_device *odev,
			     struct omci_telemetry *telemetry);
	int (*set_olt_profile)(struct omci_device *odev,
			       const struct omci_olt_profile_state *state);
	void (*set_operational)(struct omci_device *odev, bool operational);
	void (*config_changed)(struct omci_device *odev, u16 key,
			       const struct omci_identity *identity);
	void (*service_fault)(struct omci_device *odev, int error);
};

struct omci_device *
omci_device_register(struct xpon_device *xpon, u32 capabilities,
		     const struct omci_device_ops *ops, void *priv);
void omci_device_unregister(struct omci_device *odev);
int omci_device_start(struct omci_device *odev);
void omci_device_stop(struct omci_device *odev);

void *omci_device_priv(const struct omci_device *odev);
u32 omci_device_id(const struct omci_device *odev);
const char *omci_olt_profile_name(u8 profile);

int omci_identity_load(struct device *dev, struct omci_identity *identity);
void gpon_random_serial_number(const u8 vendor[4], struct omci_identity *identity);
void omci_device_set_identity_info(struct omci_device *odev,
				   const struct omci_identity *identity);
void omci_device_set_identity(struct omci_device *odev,
			      const u8 serial_number[8],
			      const u8 password[10]);
void omci_device_set_onu_id(struct omci_device *odev, u16 onu_id);
/* Channel/session transitions and all control calls require process context.
 * Provider callbacks must not re-enter control or session APIs. RX may run in
 * NAPI/softirq context; the provider must quiesce all producers before unregister.
 * MIC_VALID may be passed only after authenticating this packet with the current
 * session key. Descriptor presence bits and global error counters are insufficient.
 */
void omci_device_set_channel(struct omci_device *odev, u16 gem_port_id,
			     bool valid);
void omci_device_set_state(struct omci_device *odev, u8 state);
void omci_device_reset_session(struct omci_device *odev);
/* ONU reassignment discards the previous OLT's MIB and restores defaults.
 * Authentication and channel remain closed. A normal rekey uses auth_epoch
 * instead and preserves the MIB. Provider must retire its old namespace next.
 */
int omci_device_reset_registration(struct omci_device *odev);
int omci_device_set_dying_gasp_enabled(struct omci_device *odev,
				       bool enabled, u8 source);
int omci_device_send_dying_gasp(struct omci_device *odev);
/* Call with 0 to close authentication admission and wait for current RX/TX
 * callbacks before replacing key material. Publish a strictly increasing,
 * nonzero epoch only after the new keys and channel are verified. Rekeying
 * preserves the MIB. Channel changes and reset invalidate authentication.
 * Never hold a provider lock that RX/TX/provisioning callbacks acquire.
 * RX and xmit carry the epoch that authenticated/authorized the packet;
 * queued/retried packets must never be relabelled for a newer epoch.
 */
int omci_device_set_auth_epoch(struct omci_device *odev, u64 auth_epoch);
void omci_device_receive(struct omci_device *odev, struct sk_buff *skb,
			 u16 gem_port_id, u32 flags, u64 auth_epoch);
int omci_device_reconcile_services(struct omci_device *odev);
/* Process-context allocation notification; preserve this authenticated session. */
int omci_device_reconcile_services_epoch(struct omci_device *odev, u64 auth_epoch);

#endif /* _NET_OMCI_H */
