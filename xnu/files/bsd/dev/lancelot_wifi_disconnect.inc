/* wf_lock held. A lost link can still own firmware BSS/STA/WTBL state.
 * Retire that ownership before allowing the next association. */
static bool
wf_disconnect(bool notify_peer)
{
    if (notify_peer && wf_net.joined) {
        const uint8_t reason[2] = {3, 0};
        (void)wf_tx_mgmt(12, reason, sizeof(reason));
    }
    wf_net.joined = wf_net.linked = wf_net.keyed = false;
    redwood_net_link_state(false);
    if (!wf_net.context_live) {
        bzero(&wf_net, sizeof(wf_net));
        wf_mgmt.full = false;
        return true;
    }
    bool ok = wf_channel(false);
    wf_bss_info(false);
    ok = wf_tx_ok && ok;
    if (wf_net.pairwise_live) {
        bool removed = wf_remove_key(true, 0);
        if (removed) wf_net.pairwise_live = false;
        ok = removed && ok;
    }
    if (wf_net.group_live) {
        bool removed = wf_remove_key(false, wf_net.group_in_index);
        if (removed) wf_net.group_live = false;
        ok = removed && ok;
    }
    const uint8_t remove[4] = {0, 0, 0, 0}; /* STA record 0 on BSS 0 */
    (void)wf_cmd(CMD_ID_REMOVE_STA_RECORD, true, remove, sizeof(remove));
    ok = wf_tx_ok && ok;
    bool inactive = wf_bss_active(false);
    ok = inactive && ok;
    wf_mgmt.full = false;
    if (ok) {
        bzero(&wf_net, sizeof(wf_net)); /* PMK, PTK/GTK, nonce and replay state */
    } else {
        /* Keep peer/key indices for another cleanup attempt; a new SSID
         * must not overwrite a context with uncertain firmware ownership. */
        lancelot_wifi_log("lancelot-wifi: disconnect cleanup incomplete; next join must retire the old firmware context");
    }
    return ok;
}
