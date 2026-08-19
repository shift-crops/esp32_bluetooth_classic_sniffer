

#include "btstack_config.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "btstack_run_loop.h"
#include "hci_cmd.h"

#include "btstack.h"

#define NUM_SERVICES 20

int conf_iocap = SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT;
int conf_auth_req = SSP_IO_AUTHREQ_MITM_PROTECTION_NOT_REQUIRED_GENERAL_BONDING;
int conf_enable_bounding = 1;
int conf_discoverable = 0;
// Security staging: 0 = no pairing/no encryption (LEVEL_0),
//                   1 = pairing but no encryption (LEVEL_1 via patched BTstack),
//                   2 = pairing + encryption (LEVEL_2, default).
int conf_sec_level = 2;
char conf_pin[5] = {'0', '0', '0', '0', '\x00'};

int flag_bounding_start = 0;
int flag_bounding_done = 0;
int flag_rfcomm_connection_start = 0;
int flag_rfcomm_connection_done = 0;

int var_rfcomm_opened_channels = 0;
int var_rfcomm_closed_channels = 0;
uint16_t var_rfcom_cids[20];

static btstack_timer_source_t timer_rfcomm_query;
static btstack_timer_source_t timer_channel_dwell; // bounds how long we stay on one open channel
static bd_addr_t remote_addr;

// How long to keep a successfully opened RFCOMM channel before closing it and moving
// on to the next service. Prevents getting stuck on a channel that never closes.
#define CHANNEL_DWELL_MS 5000

static struct
{
    uint8_t channel_nr;
    char service_name[SDP_SERVICE_NAME_LEN + 1];
} services[NUM_SERVICES];

static uint8_t service_index = 0;
static uint8_t current_service_index = 0;

// RFCOMM connect-and-send state (plaintext SPP probe)
static uint16_t spp_data_cid = 0;      // active RFCOMM channel id, 0 = none
static uint8_t  rfcomm_try_index = 0;  // which discovered service we are trying in this session
static const char spp_probe[] = "AT\r"; // benign plaintext payload to generate traffic

// Persisted across reconnects so that after a channel is used (or fails) we advance
// to the NEXT discovered service instead of retrying the same channel every SDP cycle.
static uint8_t next_service_to_try = 0; // index a fresh session should start from
static uint8_t last_service_count = 0;  // service_index observed in the latest SDP query

static btstack_packet_callback_registration_t hci_event_callback_registration;

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size);

static btstack_context_callback_registration_t handle_sdp_client_query_request;

static void handle_start_sdp_client_query(void *context)
{
    UNUSED(context);
    printf("Starting RFCOMM Query\n");
    sdp_client_query_rfcomm_channel_and_name_for_uuid(&packet_handler, remote_addr, BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
}

static void store_found_service(const char *name, uint8_t port)
{
    printf("APP: Service name: '%s', RFCOMM port %u\n", name, port);
    if (service_index < NUM_SERVICES) {
        services[service_index].channel_nr = port;
        strncpy(services[service_index].service_name, (char *)name, SDP_SERVICE_NAME_LEN);
        services[service_index].service_name[SDP_SERVICE_NAME_LEN] = 0;
        service_index++;
    }
    else {
        printf("APP: list full - ignore\n");
        return;
    }
}

static void report_found_services(void)
{
    printf("\n *** Client query response done. ");
    if (service_index == 0) {
        printf("No service found.\n\n");
    }
    else {
        printf("Found following %d services:\n", service_index);
    }
    int i;
    for (i = 0; i < service_index; i++) {
        printf("     Service name %s, RFCOMM port %u\n", services[i].service_name, services[i].channel_nr);
    }
    printf(" ***\n\n");
}

static void reset_vars()
{
    flag_bounding_start = 0;
    flag_bounding_done = 0;
    flag_rfcomm_connection_start = 0;
    flag_rfcomm_connection_done = 0;
    var_rfcomm_closed_channels = 0;
    var_rfcomm_opened_channels = 0;
    service_index = 0;
}

static void start_rfcomm_query()
{
    static int started = 0;
    static int created = 0;
    if (!started) {
        started = 1;
        // set one-shot timer
        timer_rfcomm_query.process = &start_rfcomm_query;
        btstack_run_loop_set_timer(&timer_rfcomm_query, 1000);
        btstack_run_loop_remove_timer(&timer_rfcomm_query);
        btstack_run_loop_add_timer(&timer_rfcomm_query);
    }
    else {
        started = 0;
        handle_sdp_client_query_request.callback = &handle_start_sdp_client_query;
        (void)sdp_client_register_query_callback(&handle_sdp_client_query_request);
    }
}

// Try to open an RFCOMM channel to the next discovered service on the CURRENT
// ACL link (no disconnect/reconnect churn). Advances past services whose
// rfcomm_create_channel() fails synchronously.
static void connect_next_rfcomm(void)
{
    while (rfcomm_try_index < service_index) {
        uint8_t j = rfcomm_try_index;
        printf("Connecting RFCOMM to '%s' (server channel %u)\n",
               services[j].service_name, services[j].channel_nr);
        uint8_t res = rfcomm_create_channel(&packet_handler, remote_addr,
                                            services[j].channel_nr, &var_rfcom_cids[j]);
        if (res == 0) {
            return; // request queued; wait for RFCOMM_EVENT_CHANNEL_OPENED
        }
        printf("  rfcomm_create_channel failed, status 0x%02x - trying next\n", res);
        rfcomm_try_index++;
    }
    printf("No (more) RFCOMM services could be opened.\n");
}

// Dwell timeout: a channel opened but never closed on its own. Close it so the
// RFCOMM_EVENT_CHANNEL_CLOSED handler advances us to the next service.
static void channel_dwell_timeout(btstack_timer_source_t *ts)
{
    UNUSED(ts);
    if (spp_data_cid) {
        printf("Dwell timeout (%d ms): closing RFCOMM cid 0x%04x to move to next service\n",
               CHANNEL_DWELL_MS, spp_data_cid);
        rfcomm_disconnect(spp_data_cid); // -> RFCOMM_EVENT_CHANNEL_CLOSED
    }
}

static void arm_channel_dwell_timer(void)
{
    timer_channel_dwell.process = &channel_dwell_timeout;
    btstack_run_loop_set_timer(&timer_channel_dwell, CHANNEL_DWELL_MS);
    btstack_run_loop_remove_timer(&timer_channel_dwell);
    btstack_run_loop_add_timer(&timer_channel_dwell);
}

static void packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size)
{
    UNUSED(channel);

    // RFCOMM payload received from the remote SPP service (plaintext observation)
    if (packet_type == RFCOMM_DATA_PACKET) {
        printf("RFCOMM RX (%u bytes):", size);
        for (uint16_t i = 0; i < size; i++) printf(" %02x", packet[i]);
        printf("  | \"");
        for (uint16_t i = 0; i < size; i++) putchar((packet[i] >= 0x20 && packet[i] < 0x7f) ? packet[i] : '.');
        printf("\"\n");
        return;
    }

    // L2CAP payload on an accepted incoming channel (plaintext observation)
    if (packet_type == L2CAP_DATA_PACKET) {
        printf("L2CAP RX (cid 0x%04x, %u bytes):", channel, size);
        for (uint16_t i = 0; i < size; i++) printf(" %02x", packet[i]);
        printf("\n");
        return;
    }

    // printf("type:%d\n", packet_type);

    // HCI Events
    if (packet_type == HCI_EVENT_PACKET) {
        switch (hci_event_packet_get_type(packet)) {

        case BTSTACK_EVENT_STATE:
            // BTstack activated, get started
            if (btstack_event_state_get_state(packet) == HCI_STATE_WORKING) {
                // Start SDP Query
                start_rfcomm_query();
            }
            break;

        case HCI_EVENT_DISCONNECTION_COMPLETE:
            puts("HCI Disconnection received");
            spp_data_cid = 0;
            // Link dropped. Reconnect and continue with the NEXT untried service.
            // next_service_to_try is preserved (reset_vars does not touch it), so we do
            // not retry the channel we just used. Stop once all services are exhausted.
            if (last_service_count == 0 || next_service_to_try < last_service_count) {
                reset_vars(); // clears the per-session service list; SDP re-populates it
                start_rfcomm_query();
            }
            else {
                printf("All discovered RFCOMM services tried; not reconnecting.\n");
            }
            break;
        case HCI_EVENT_PIN_CODE_REQUEST:
            // inform about pin code request
            printf("Pin code request - using %s\n", remote_addr);
            hci_event_pin_code_request_get_bd_addr(packet, remote_addr);
            // baseband address, pin length, PIN: c-string
            hci_send_cmd(&hci_pin_code_request_reply, &remote_addr, 4, conf_pin);
            break;
        case HCI_EVENT_SIMPLE_PAIRING_COMPLETE: {
            // Informational only. We keep the link up and let the RFCOMM flow proceed;
            // no disconnect churn here.
            uint8_t res = hci_event_simple_pairing_complete_get_status(packet);
            printf("Secure Simple Pairing complete status: %d\n", res);
            break;
        }

        case RFCOMM_EVENT_CHANNEL_OPENED:
            // data: event(8), len(8), status (8), address (48), server channel(8), rfcomm_cid(16), max frame size(16)
            if (rfcomm_event_channel_opened_get_status(packet)) {
                printf("RFCOMM channel open failed (service idx %u), status %u - trying next service\n",
                       rfcomm_try_index, rfcomm_event_channel_opened_get_status(packet));
                // This index is done (failed); a fresh session must not retry it.
                next_service_to_try = (uint8_t)(rfcomm_try_index + 1);
                rfcomm_try_index++;
                connect_next_rfcomm(); // stay on the same ACL link, try next service
            }
            else {
                spp_data_cid = rfcomm_event_channel_opened_get_rfcomm_cid(packet);
                int mtu = rfcomm_event_channel_opened_get_max_frame_size(packet);
                // This index succeeded; after it is torn down, move on to the next one.
                next_service_to_try = (uint8_t)(rfcomm_try_index + 1);
                printf("RFCOMM channel OPEN: service idx %u, cid 0x%04x, mtu %u. Sending %u-byte plaintext probe.\n",
                       rfcomm_try_index, spp_data_cid, mtu, (unsigned)strlen(spp_probe));
                // Request a send slot; actual write happens in RFCOMM_EVENT_CAN_SEND_NOW.
                rfcomm_request_can_send_now_event(spp_data_cid);
                // Bound the dwell so a channel that never closes stalls the sweep -- but
                // only if there are MORE services to try. Keep the LAST channel open (no
                // dwell) so the link is held: at LEVEL_1 we want to verify the target does
                // NOT drop an authenticated link even when idle.
                if (next_service_to_try < service_index) {
                    arm_channel_dwell_timer();
                } else {
                    printf("Last RFCOMM service (idx %u) open - HOLDING link open (no dwell, no detach).\n",
                           rfcomm_try_index);
                }
            }
            break;

        case RFCOMM_EVENT_CAN_SEND_NOW:
            if (spp_data_cid) {
                rfcomm_send(spp_data_cid, (uint8_t *)spp_probe, (uint16_t)strlen(spp_probe));
                printf("RFCOMM TX: sent %u plaintext bytes on cid 0x%04x\n",
                       (unsigned)strlen(spp_probe), spp_data_cid);
            }
            break;

        case RFCOMM_EVENT_CHANNEL_CLOSED:
            printf("RFCOMM channel closed (cid 0x%04x)\n", spp_data_cid);
            spp_data_cid = 0;
            btstack_run_loop_remove_timer(&timer_channel_dwell); // cancel pending dwell
            // Advance to the next service on the SAME ACL link (no re-page needed).
            if (next_service_to_try < service_index) {
                rfcomm_try_index = next_service_to_try;
                connect_next_rfcomm();
            }
            else {
                // Do NOT disconnect: keep the ACL up so we can verify the target does not
                // drop the (authenticated) link. Unauthenticated links get dropped after
                // tens of seconds; an authenticated LEVEL_1 link should persist.
                printf("All %u discovered RFCOMM services tried - HOLDING link (NOT disconnecting).\n", service_index);
            }
            break;

        default:
            switch (packet[0]) {
            case SDP_EVENT_QUERY_RFCOMM_SERVICE:
                store_found_service(sdp_event_query_rfcomm_service_get_name(packet),
                                    sdp_event_query_rfcomm_service_get_rfcomm_channel(packet));
                break;
            case SDP_EVENT_QUERY_COMPLETE:
                if (sdp_event_query_complete_get_status(packet)) {
                    printf("SDP query failed 0x%02x, retrying...\n", sdp_event_query_complete_get_status(packet));
                    sdp_client_query_rfcomm_channel_and_name_for_uuid(&packet_handler, remote_addr, BLUETOOTH_ATTRIBUTE_PUBLIC_BROWSE_ROOT);
                }
                else {
                    printf("SDP query done.\n");
                    report_found_services();
                    last_service_count = service_index;
                    // Open RFCOMM on the SAME ACL link (no disconnect / re-page churn).
                    // Resume from next_service_to_try so each reconnect advances to the
                    // NEXT discovered service instead of retrying the same channel.
                    if (next_service_to_try >= service_index) {
                        printf("All %u discovered RFCOMM services have been tried - stopping.\n", service_index);
                    }
                    else {
                        rfcomm_try_index = next_service_to_try;
                        connect_next_rfcomm();
                    }
                }

                break;
            }
        }
    }
}

#ifdef HAVE_POSIX_FILE_IO
static void usage(const char *name)
{
    printf("\nUsage: %s -a|--address aa:bb:cc:dd:ee:ff\n", name);
    printf("Use argument -a to connect to a specific device and dump the result of SDP query for L2CAP services.\n\n");
}
#endif

void signal_handler(int signal)
{
    exit(0);
}

int btstack_main(int argc, const char *argv[]);
int btstack_main(int argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    int remote_addr_found = 0;
    for (size_t i = 0; i < argc; i++) {
        // valueless flag alias (may appear as the last argument): --no-encryption == level 0
        if (!strcmp(argv[i], "--no-encryption")) {
            conf_sec_level = 0;
        }
        if (argc > i + 1) {
            if (!strcmp(argv[i], "--sec-level")) {
                conf_sec_level = atoi(argv[i + 1]);
                printf("sec-level=%d\n", conf_sec_level);
            }
            else if (!strcmp(argv[i], "-a") || !strcmp(argv[i], "--address")) {
                remote_addr_found = sscanf_bd_addr(argv[i + 1], remote_addr);
                printf("address=%s\n", argv[i + 1]);
            }
            else if (!strcmp(argv[i], "--iocap")) {
                conf_iocap = atoi(argv[i + 1]);
                printf("iocap=%d\n", conf_iocap);
            }
            else if (!strcmp(argv[i], "--authreq")) {
                conf_auth_req = atoi(argv[i + 1]);
                printf("authreq=%d\n", conf_auth_req);
            }
            else if (!strcmp(argv[i], "--bounding")) {
                conf_enable_bounding = atoi(argv[i + 1]);
                printf("bouding=%d\n", conf_enable_bounding);
            }
            else if (!strcmp(argv[i], "--discoverable")) {
                conf_discoverable = atoi(argv[i + 1]);
                printf("discoverable=%d\n", conf_discoverable);
            }
        }
    }

    if (!remote_addr_found) {
        usage(argv[0]);
        exit(1);
    }

    signal(SIGTERM, signal_handler);

    // init L2CAP
    l2cap_init();
    rfcomm_init();
    // Security Manager: newer btstack initialises it in the examples; it also sets up the
    // crypto subsystem used by BR/EDR Secure Simple Pairing, so init it here too.
    sm_init();

    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // GAP Config
    gap_delete_all_link_keys();
    // Security staging (see conf_sec_level):
    //   0 -> LEVEL_0: no pairing, no encryption
    //   1 -> LEVEL_1: authenticate/pair, but no encryption. Relies on the patched BTstack
    //        (hci.c) which, for LEVEL_1, reports the authenticated link without waiting for
    //        encryption -- so RFCOMM opens on a plaintext authenticated link.
    //   2 -> LEVEL_2: pairing + encryption (default)
    // For levels 0/1 the firmware additionally strips encryption on air (force_no_encryption).
    gap_security_level_t sec = LEVEL_2;
    if (conf_sec_level == 0) sec = LEVEL_0;
    else if (conf_sec_level == 1) sec = LEVEL_1;
    printf("GAP security: sec-level %d -> LEVEL_%d\n", conf_sec_level, (int)sec);
    gap_set_security_level(sec);
    gap_ssp_set_io_capability(conf_iocap);
    // Newer btstack defaults ssp_auto_accept to 0 (older defaulted to 1). Without this the
    // SSP user-confirmation is never answered -> LMP_numeric_comparison_failed -> detach.
    // This is an unattended sniffer, so auto-accept the Just Works / numeric comparison.
    gap_ssp_set_auto_accept(1);
    gap_ssp_set_authentication_requirement(conf_auth_req);
    gap_set_bondable_mode(conf_enable_bounding);
    gap_discoverable_control(conf_discoverable);
    gap_set_local_name("BT ESP32 00:00:00:00:00:00");

    // Modes
    hci_set_inquiry_mode(INQUIRY_MODE_STANDARD);
    hci_set_master_slave_policy(HCI_ROLE_MASTER);

    // turn on!
    hci_power_control(HCI_POWER_ON);

    return 0;
}
