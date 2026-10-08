"""Wiring checks supplement (do not replace) executable C++/converter tests."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class ReleaseSafety(unittest.TestCase):
    def test_command_mailboxes_are_native_word_atomics(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        self.assertIn('std::atomic<int32_t> requestedClimate', source)
        self.assertIn('std::atomic<uint32_t> requestedMode', source)
        self.assertIn('std::atomic<uint32_t> requestedMaintenance', source)
        self.assertNotIn('std::atomic<int8_t>', source)
        self.assertNotIn('std::atomic<uint16_t>', source)
    def test_normal_profile(self):
        config = (ROOT / 'platformio.ini').read_text()
        self.assertIn('-DPHEV_PROTOCOL_WATCH_TEST=0', config)
        self.assertIn('-DPHEV_RAW_TCP_TEST=0', config)

    def test_identity_is_configurable_and_existing_nvs_is_preserved(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        identity = (ROOT / 'include/PhevIdentity.h').read_text(encoding='utf-8')
        self.assertNotIn('PHEV_REGISTERED_MAC', identity)
        self.assertIn('clonedMacText = prefs.getString("mac", "")', source)
        self.assertIn('mac = web.arg("mac")', source)
        setup = source.split('void setup() {', 1)[1].split('void loop() {', 1)[0]
        self.assertNotIn('prefs.putString("mac"', setup)
        self.assertIn('parsePhevMac(value.c_str(), out)', source)
        self.assertNotIn('WATCH_USE_REALTEK_MAC', source)

    def test_setup_does_not_request_climate_or_reset_zigbee(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        setup = source.split('void setup() {', 1)[1].split('void loop() {', 1)[0]
        self.assertNotIn('requestClimate(', setup)
        self.assertNotIn('factoryReset(', setup)
        self.assertIn('WiFi.mode(WIFI_OFF)', setup)
        self.assertIn('demand.begin(uptimeMs())', setup)

    def test_web_and_callback_boundaries(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        self.assertIn('if (!wifiOnlyBoot && maintenance.active()) web.handleClient()', source)
        self.assertIn('requestedClimate.exchange(-1)', source)

    def test_new_zigbee_reports_bypass_automatic_table_and_capacity_sufficient(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        self.assertIn('esp_zb_aps_src_binding_table_size_set(64)', source)
        self.assertIn('esp_zb_aps_dst_binding_table_size_set(64)', source)
        report = source.split('bool reportPresentValue(', 1)[1].split('void reportState()', 1)[0]
        self.assertIn('esp_zb_aps_data_request(&request)', report)
        self.assertIn('attribute ? encodePresentValueReport', report)
        self.assertIn('esp_zb_lock_acquire', report)
        self.assertNotIn('esp_zb_zcl_report_attr_cmd_req', source)
        self.assertNotIn('esp_zb_zcl_update_reporting_info', source)
        self.assertIn('esp_zb_zcl_reset_all_reporting_info()', source)
        start = source.split('void startOrResumeZigbee()', 1)[1].split('void stopPortal(', 1)[0]
        self.assertLess(start.index('esp_coex_wifi_i154_enable()'), start.index('Zigbee.begin('))
        self.assertNotIn('WiFi.disconnect(', start)
        self.assertNotIn('.reportBinaryInput()', source)

    def test_demand_sessions_gate_normal_wifi_and_do_not_replay_climate(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        protocol = (ROOT / 'src/PhevProtocol.cpp').read_text(encoding='utf-8')
        self.assertIn('!demand.active()) return', source)
        self.assertIn('WiFi.disconnect(false, false); WiFi.mode(WIFI_OFF)', source)
        self.assertIn('tickDemand()', source)
        self.assertIn('demandTransportEnded()', source)
        self.assertIn('demandManaged_ && demandAttempted_', protocol)
        self.assertIn('!demandManaged_ && !watchOnly_', protocol)
        self.assertIn('NetworkClientBounded.cpp', protocol)
        bounded = (ROOT / 'tools/NetworkClientBounded.cpp').read_text(encoding='utf-8')
        self.assertIn('MSG_DONTWAIT', bounded)
        self.assertIn('if (spent >= 25) break', bounded)
        self.assertIn('WIFI_CLIENT_MAX_WRITE_RETRY     (1)', bounded)
        self.assertIn('txHead_ = txCount_ = 0', protocol)
        policy = (ROOT / 'include/PhevDemandPolicy.h').read_text()
        self.assertIn('24ULL * 60 * 60 * 1000', policy)
        self.assertIn('LimitMs = 60000', policy)

    def test_atomic_session_start_terminal_and_prompt_zigbee_reporting(self):
        source = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
        self.assertIn('0.1.0-beta.2-dev', source)
        self.assertIn('ZigbeeAnalog epSessionResult(28)', source)
        self.assertIn('static uint8_t payloads[29][10]', source)
        self.assertIn('ep.getEndpoint() > 28', source)
        self.assertIn('sessionResult.begin(demand.reason()', source)
        self.assertIn('else sessionResult.promoteClimate()', source)
        self.assertIn('sessionResult.finish(acknowledged ?', source)
        self.assertIn('const bool acknowledged = climate && phev.state().commandAck', source)
        self.assertIn('telemetry.capture(phev.state(), uptimeMs(), !climate)', source)
        self.assertIn('PhevSessionResult::Status::CommandAcknowledged', source)
        self.assertIn('PhevSessionResult::Status::Timeout', source)
        self.assertIn('PhevSessionResult::Status::CommandFailed', source)
        self.assertIn('epSessionResult.setAnalogInput(sessionResult.word())', source)
        self.assertIn('sessionResult.word() != lastSessionWord', source)
        self.assertIn('lastSessionWord = sessionResult.word()', source)

if __name__ == '__main__':
    unittest.main()
