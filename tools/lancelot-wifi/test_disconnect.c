#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "lancelot_wifi_wire.h"

#define CMD_ID_REMOVE_STA_RECORD 0x14
enum operation { PEER = 1, ABORT, DISCONNECTED, PTK, GTK, REMOVE_STA, INACTIVE };
static struct {
    bool joined, linked, keyed, context_live, pairwise_live, group_live, key_error;
    unsigned group_in_index;
    uint8_t bssid[6], secrets[160];
} wf_net;
static struct { bool full; } wf_mgmt;
static const uint8_t peer[6] = {2, 0x44, 0x55, 0x66, 0x77, 0x88};
static bool wf_tx_ok;
static enum operation fail;
static enum operation operations[16];
static unsigned operation_count, logs;
static bool record(enum operation op)
{
    assert(operation_count < 16);
    operations[operation_count++] = op;
    wf_tx_ok = op != fail;
    return wf_tx_ok;
}
static bool wf_tx_mgmt(uint8_t subtype, const uint8_t *reason, uint32_t length)
{
    assert(subtype == 12 && length == 2 && reason[0] == 3 && !reason[1]);
    return record(PEER);
}
static void redwood_net_link_state(bool linked) { assert(!linked); }
static bool wf_channel(bool take) { assert(!take); return record(ABORT); }
static void wf_bss_info(bool connected) { assert(!connected); (void)record(DISCONNECTED); }
static bool wf_remove_key(bool pairwise, unsigned index)
{
    uint8_t command[64];
    assert(!memcmp(wf_net.bssid, peer, 6));
    assert(lw_remove_key_command(command, sizeof(command), wf_net.bssid, pairwise, index) == 64);
    assert(command[0] == 0 && command[1] == 0 && command[2] == (pairwise ? 1 : 0));
    assert(!memcmp(command + 4, peer, 6));
    assert(command[12] == (pairwise ? 0 : 2) && command[14] == (pairwise ? 1 : 2));
    for (unsigned i = 16; i < 64; i++) assert(!command[i]);
    return record(pairwise ? PTK : GTK);
}
static uint8_t wf_cmd(uint8_t cid, bool set, const void *data, uint32_t length)
{
    const uint8_t *p = data;
    assert(cid == CMD_ID_REMOVE_STA_RECORD && set && length == 4 && !(p[0] | p[1] | p[2] | p[3]));
    (void)record(REMOVE_STA);
    return 1;
}
static bool wf_bss_active(bool active) { assert(!active); return record(INACTIVE); }
static void lancelot_wifi_log(const char *format, ...) { (void)format; logs++; }
#include "lancelot_wifi_disconnect.inc"

static void setup(bool joined, bool keys)
{
    memset(&wf_net, 0, sizeof(wf_net));
    wf_net.context_live = true;
    wf_net.joined = wf_net.linked = wf_net.keyed = joined;
    wf_net.pairwise_live = wf_net.group_live = keys;
    wf_net.group_in_index = 2;
    memcpy(wf_net.bssid, peer, 6);
    memset(wf_net.secrets, 0xa5, sizeof(wf_net.secrets));
    wf_mgmt.full = true;
    operation_count = logs = 0;
    fail = 0;
}
static void clean(void)
{
    const uint8_t *p = (const uint8_t *)&wf_net;
    for (unsigned i = 0; i < sizeof(wf_net); i++) assert(!p[i]);
    assert(!wf_mgmt.full);
}
int main(void)
{
    setup(true, true);
    assert(wf_disconnect(true));
    const enum operation full[] = {PEER, ABORT, DISCONNECTED, PTK, GTK, REMOVE_STA, INACTIVE};
    assert(operation_count == 7 && !memcmp(operations, full, sizeof(full)));
    clean();
    operation_count = 0;
    assert(wf_disconnect(true) && !operation_count); /* idempotent */
    setup(false, true); /* AP already cleared joined: the W5 regression. */
    assert(wf_disconnect(false));
    assert(operation_count == 6 && !memcmp(operations, full + 1, 6 * sizeof(*full)));
    clean();
    setup(false, false); /* auth/association/handshake failure before keys */
    assert(wf_disconnect(false));
    const enum operation partial[] = {ABORT, DISCONNECTED, REMOVE_STA, INACTIVE};
    assert(operation_count == 4 && !memcmp(operations, partial, sizeof(partial)));
    clean();
    for (enum operation op = ABORT; op <= INACTIVE; op++) {
        setup(false, true);
        fail = op;
        assert(!wf_disconnect(false) && logs == 1);
        assert(wf_net.context_live && !wf_net.joined && !wf_net.linked && !wf_net.keyed);
        assert(!memcmp(wf_net.bssid, peer, 6) && wf_net.group_in_index == 2);
        assert(wf_net.pairwise_live == (op == PTK) && wf_net.group_live == (op == GTK));
        fail = 0;
        operation_count = 0;
        assert(wf_disconnect(false));
        clean();
    }
    setup(true, true); fail = PEER;
    assert(wf_disconnect(true)); /* peer notification does not prevent local cleanup */
    clean();
    uint8_t buf[64]; memset(buf, 0xa5, sizeof(buf));
    assert(!lw_remove_key_command(buf, 63, peer, true, 0) && buf[0] == 0xa5);
    assert(!lw_remove_key_command(buf, 64, peer, false, 4));
    assert(!lw_remove_key_command(buf, 64, peer, true, 1));
    puts("PASS: production disconnect after AP loss, partial join, explicit leave, key deletion ABI, retry after every command failure and secret erasure");
    return 0;
}
