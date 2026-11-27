// nn::nifm: the console's network connection manager. By default it answers as a console with no network (the
// runtime serves no network service): a request "completes" without a connection, so the game takes its offline path.
// ORBITALS_LAN=1 (Settings > Game > LAN play, off by default): a request in local network mode (SetLocalNetworkMode,
// then IsNetworkAvailable) is answered as connected, with the host's address (ov_lan_address); the game then talks over
// the host's sockets (ov_socket.c). An internet request stays unavailable: there is no server to reach. Written for the
// first game's LAN lobby; whether Orbitals makes such a request is not known yet.
#include <stdint.h>
#include <string.h>

#include "host.h"
#include "nvn_api.h"
#include "ov.h"
#include "steam_rpt.h"

// nifm results (module 110); the description number is a hypothesis: any failure keeps the game offline.
#define NIFM_R_NO_NETWORK (110u | (1u << 9))

static int local_mode;   // SetLocalNetworkMode's last value

static int lan_up(void) { return local_mode && ov_lan(); }

// ---- online play through Steam Remote Play Together (the owner's choice, 27/11/2026)
// Orbitals' Online Play needs Nintendo's servers. Which calls its menu entry makes was not seen yet, so each online
// call is logged (the first 4 per kind, then every 100th: IsNetworkAvailable may be polled every frame) with the frame
// and the game's code that made it; the next run's log gives the sequence. The calls a console answers with a screen
// for the player (the network error applet, the network account's sign-in) only follow a player's action: those send
// the player to Remote Play Together (steam_rpt_online), and the game still gets "no network" and takes its offline
// path, where the local mode for 2 is.
void ov_online_call(Cpu *c, const char *what, int ask) {
    static struct { const char *what; volatile int32_t n; } seen[24];
    int32_t n = 0;
    for (int i = 0; i < 24; i++) {
        const char *w = __atomic_load_n(&seen[i].what, __ATOMIC_ACQUIRE);
        if (!w) {
            const char *none = NULL;
            if (!__atomic_compare_exchange_n(&seen[i].what, &none, what, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) &&
                none != what)
                continue;
            w = what;
        }
        if (w == what) { n = __atomic_add_fetch(&seen[i].n, 1, __ATOMIC_RELAXED); break; }
    }
    if (!n || n <= 4 || n % 100 == 0) {
        char lr[160];
        rt_log("online: %s (call %d, frame %llu) from %s%s", what, n,
               (unsigned long long)__atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED),
               ldr_describe(c->x[30] - 4, lr, sizeof lr), ask ? ": a player's request" : "");
    }
    if (ask && !lan_up()) steam_rpt_online(what);
}

static void ov_ok(Cpu *c) { ret_x(c, 0); }
static void ov_nothing(Cpu *c) { (void)c; }
// void SetLocalNetworkMode(bool)
static void ov_local_mode(Cpu *c) { local_mode = (int)(arg_x(c, 0) & 1); }
// bool IsNetworkAvailable()
static void ov_network_available(Cpu *c) {
    ov_online_call(c, "nifm::IsNetworkAvailable", 0);
    ret_x(c, (uint64_t)lan_up());
}
// void SubmitNetworkRequestAndWait(): the request "completes" without a connection
static void ov_submit_request(Cpu *c) { ov_online_call(c, "nifm::SubmitNetworkRequestAndWait", 0); }
// Result HandleNetworkRequestErrorResult(): on a console, the network error applet for the player
static void ov_request_result(Cpu *c) {
    ov_online_call(c, "nifm::HandleNetworkRequestErrorResult", 1);
    ret_x(c, lan_up() ? 0 : NIFM_R_NO_NETWORK);
}
static void ov_no_network(Cpu *c) { ret_x(c, NIFM_R_NO_NETWORK); }
// Result GetCurrentIpConfigInfo(in_addr *ip, in_addr *mask, in_addr *gateway, in_addr *dns1, in_addr *dns2)
static void ov_ip_config(Cpu *c) {
    uint8_t ip[4], mask[4], gw[4];
    if (!lan_up() || ov_lan_address(ip, mask)) {
        ret_x(c, NIFM_R_NO_NETWORK);
        return;
    }
    for (int i = 0; i < 4; i++) gw[i] = ip[i] & mask[i];
    gw[3] |= 1;   // a LAN needs no gateway; the usual first address, for whoever prints it
    const uint8_t *v[5] = {ip, mask, gw, gw, gw};
    for (int i = 0; i < 5; i++)
        if (arg_p(c, i)) memcpy(arg_p(c, i), v[i], 4);
    static int logged;
    if (!logged++) rt_log("nifm: LAN mode, address %u.%u.%u.%u mask %u.%u.%u.%u (ORBITALS_LAN)", ip[0], ip[1], ip[2], ip[3],
                          mask[0], mask[1], mask[2], mask[3]);
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ov_ok);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", ov_network_available);
HOST_OVERRIDE("_ZN2nn4nifm19SetLocalNetworkModeEb", ov_local_mode);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ov_submit_request);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", ov_request_result);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", ov_ip_config);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", ov_no_network);

// ---- nn::friends: no friend list offline; play history registration is accepted and dropped.
#define FRIENDS_R_UNAVAILABLE (121u | (1u << 9))   // module 121; description is a hypothesis
// Result CheckFriendListAvailability(bool *out, const Uid &)
static void ov_friend_list_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, 0);
}
// Result GetFriendList(int *count, Friend *out, const Uid &, int offset, int max, const FriendFilter &)
static void ov_friend_list(Cpu *c) {
    int32_t *count = arg_p(c, 0);
    if (count) *count = 0;
    ret_x(c, 0);
}
static void ov_friends_unavailable(Cpu *c) { ret_x(c, FRIENDS_R_UNAVAILABLE); }
// bool TryPopFriendInvitationNotificationInfo(account::Uid *, size_t *size, void *buffer, size_t capacity): no invitation
// offline. Without an override the SDK's code calls nn::am::GetApplicationFunctions, a console service the runtime does
// not serve, and the run crashed there (pc sdk+0x1c3f40, from sdk+0x44fef8) about 8 s after the frame-history fix let
// the game past frame 21. The return type is not in the mangled name: bool is assumed from the Try prefix, as
// nn::oe::TryPopNotificationMessage; the size is zeroed too, so a caller that reads a Result 0 still finds nothing.
static void ov_no_invitation(Cpu *c) {
    uint64_t *size = arg_p(c, 1);
    if (size) *size = 0;
    static int logged;
    if (!logged++) rt_log("friends: TryPopFriendInvitationNotificationInfo: no invitation (offline)");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn7friends10InitializeEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn7friends12SetAllocatorEPFPvmEPFvS1_mE", ov_nothing);
HOST_OVERRIDE("_ZN2nn7friends27CheckFriendListAvailabilityEPbRKNS_7account3UidE", ov_friend_list_available);
HOST_OVERRIDE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", ov_friend_list);
HOST_OVERRIDE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", ov_friends_unavailable);
HOST_OVERRIDE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb",
              ov_friends_unavailable);
HOST_OVERRIDE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_",
              ov_ok);
HOST_OVERRIDE("_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", ov_no_invitation);

// ---- nn::ldn (local wireless play): disabled, as on a console in airplane mode. Initialize fails, so the game does
// not use the rest; they fail too in case it does.
#define LDN_R_DISABLED (203u | (22u << 9))   // module 203; description is a hypothesis
static void ov_ldn_disabled(Cpu *c) { ret_x(c, LDN_R_DISABLED); }
// State GetState(): 0, None
static void ov_ldn_state(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn3ldn10InitializeEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn8FinalizeEv", ov_nothing);
HOST_OVERRIDE("_ZN2nn3ldn8GetStateEv", ov_ldn_state);
HOST_OVERRIDE("_ZN2nn3ldn19GetDisconnectReasonEv", ov_ldn_state);
HOST_OVERRIDE("_ZN2nn3ldn10DisconnectEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn11OpenStationEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn12CloseStationEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn15OpenAccessPointEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn16CloseAccessPointEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14DestroyNetworkEv", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn4ScanEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn11ScanPrivateEPNS0_11NetworkInfoEPiiRKNS0_10ScanFilterEi", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn13CreateNetworkERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_10UserConfigE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn20CreateNetworkPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiPKNS0_12AddressEntryE",
              ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn7ConnectERKNS0_11NetworkInfoERKNS0_14SecurityConfigERKNS0_10UserConfigEiNS0_13ConnectOptionE",
              ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14ConnectPrivateERKNS0_13NetworkConfigERKNS0_14SecurityConfigERKNS0_17SecurityParameterERKNS0_10UserConfigEiNS0_13ConnectOptionE",
              ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14GetIpv4AddressEPNS0_11Ipv4AddressEPNS0_10SubnetMaskE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn14GetNetworkInfoEPNS0_11NetworkInfoE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn20GetSecurityParameterEPNS0_17SecurityParameterE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn16SetAdvertiseDataEPKvm", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn20AddAcceptFilterEntryENS0_10MacAddressE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn22SetStationAcceptPolicyENS0_12AcceptPolicyE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn6RejectENS0_11Ipv4AddressE", ov_ldn_disabled);
HOST_OVERRIDE("_ZN2nn3ldn22AttachStateChangeEventEPNS_2os15SystemEventTypeE", ov_nothing);

// ---- nn::nsd: no server name resolution offline.
static void ov_nsd_none(Cpu *c) { ret_x(c, NIFM_R_NO_NETWORK); }
HOST_OVERRIDE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", ov_nsd_none);
HOST_OVERRIDE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", ov_nsd_none);

// ---- nn::prepo: play reports are built by the SDK's own code and never sent.
HOST_OVERRIDE("_ZN2nn5prepo28RequestImmediateTransmissionEv", ov_ok);

// ---- nn::socket: ov_socket.c, over the host's sockets (it also covers #63: Initialize needs no bsd service).

// ---- nn::ssl (Orbitals' Unreal HTTP: nnsslInitialize from main+0x70b9e48; the SDK's opens the ssl service through
// sm, svc 0x1f). No internet server is reachable from the runtime (nifm above stays offline), so the library starts
// and every context, certificate or connection request fails with an ssl result: the game sees TLS unavailable, as
// with no network, instead of a session that cannot work. The description number is a hypothesis, as nifm's.
#define SSL_R_UNAVAILABLE (123u | (1u << 9))
static void ov_ssl_fail(Cpu *c) { ret_x(c, SSL_R_UNAVAILABLE); }
HOST_OVERRIDE("_ZN2nn3ssl10InitializeEv", ov_ok);
HOST_OVERRIDE("nnsslInitialize", ov_ok);
HOST_OVERRIDE("nnsslFinalize", ov_ok);
HOST_OVERRIDE("nnsslContextDestroy", ov_ok);
HOST_OVERRIDE("nnsslConnectionDestroy", ov_ok);
HOST_OVERRIDE("_ZN2nn3ssl7Context6CreateENS1_10SslVersionE", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager28GetBuiltInCertificateBufSizeEPjPNS0_15CaCertificateIdEj", ov_ssl_fail);
HOST_OVERRIDE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj",
              ov_ssl_fail);
HOST_OVERRIDE("nnsslContextGetContextId", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionCreate", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionDoHandshakeWithCertBuffer", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionGetConnectionId", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionGetNextAlpnProto", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionGetServerCertDetail", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionGetVerifyCertError", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionPeek", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionPending", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionRead", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetHostName", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetIoMode", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetNextAlpnProto", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetOption", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetServerCertBuffer", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetSessionCacheMode", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetSocketDescriptor", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionSetVerifyOption", ov_ssl_fail);
HOST_OVERRIDE("nnsslConnectionWrite", ov_ssl_fail);

// ---- nn::ec (eShop, nim:eca): Orbitals sets up its shop accessors early in its start (Lichii's log of the game
// shows nim's IsLargeResourceAvailable and CreateAccessorInterface right after its sockets, build-177). The
// accessors start; every request fails as offline, and the shop pages show nothing.
#define EC_R_UNAVAILABLE (164u | (1u << 9))   // a hypothesis, as nifm's: any failure reads as no shop
static void ov_ec_fail(Cpu *c) { ret_x(c, EC_R_UNAVAILABLE); }
HOST_OVERRIDE("_ZN2nn2ec33InitializeForShopServiceAccessorsEPvm", ov_ok);
HOST_OVERRIDE("_ZN2nn2ec31FinalizeForShopServiceAccessorsEv", ov_ok);
HOST_OVERRIDE("_ZN2nn2ec19ShopServiceAccessor10InitializeERKNS0_11ShopServiceE", ov_ok);
HOST_OVERRIDE("_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE",
              ov_ec_fail);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopAddOnContentListENS_13ApplicationIdERKNS_7account10UserHandleE", ov_ok);
HOST_OVERRIDE("_ZN2nn2ec24ShowShopSubscriptionListENS_13ApplicationIdERKNS_7account10UserHandleE", ov_ok);
HOST_OVERRIDE("_ZN2nn2ec26ShowShopConsumableItemListENS_13ApplicationIdERKNS_7account10UserHandleE", ov_ok);
HOST_OVERRIDE("_ZN2nn2ec27ShowShopSubscriptionDetailsERKNS0_8CourseIdENS0_5NsUidERKNS_7account10UserHandleE", ov_ok);
HOST_OVERRIDE("_ZN2nn2ec28ShowShopConsumableItemDetailENS_13ApplicationIdERKNS0_12ConsumableIdENS0_5NsUidERKNS_7account10UserHandleE",
              ov_ok);
HOST_OVERRIDE("_ZN2nn2ec30ShowShopApplicationInformationENS_13ApplicationIdERKNS_7account10UserHandleE", ov_ok);
// Result nifm::GetCurrentPrimaryIpAddress(socket::InAddr *): no address, offline
HOST_OVERRIDE("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEPNS_6socket6InAddrE", ov_no_network);