"""Static web wiring checks complement executable protocol tests, not real HTTP tests."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
MAIN = (ROOT / 'src/main.cpp').read_text(encoding='utf-8')
SERVICE = (ROOT / 'src/MaintenanceService.cpp').read_text(encoding='utf-8')


def route(name):
    return MAIN.split(f'web.on("/registration/{name}", HTTP_POST', 1)[1].split('web.on(', 1)[0]


class RegistrationRoutes(unittest.TestCase):
    def test_registration_requires_admin_and_post_csrf_before_exclusive_guard(self):
        middleware = SERVICE.split('web_.addMiddleware', 1)[1].split('web_.on(', 1)[0]
        self.assertIn('server.uri().startsWith("/registration")', middleware)
        self.assertIn('!authenticate()', middleware)
        self.assertIn('server.method() == HTTP_POST && !tokenValid()', middleware)
        self.assertLess(middleware.index('!authenticate()'), middleware.index('!tokenValid()'))
        self.assertLess(middleware.index('!tokenValid()'), middleware.index('exclusiveOperation_()'))

    def test_ap_interface_is_required_not_just_ap_mode(self):
        guard = MAIN.split('bool registrationLocal()', 1)[1].split('void showRegistrationPage()', 1)[0]
        self.assertIn('portalActive && !maintenance.homeMode()', guard)
        self.assertIn('web.client().localIP() == WiFi.softAPIP()', guard)
        for name in ('start', 'cancel', 'adopt'):
            self.assertIn('!registrationLocal()', route(name))

    def test_start_is_explicit_native_identity_and_no_nvs_intent(self):
        start = route('start')
        for guard in ('PHEV_RAW_TCP_TEST', 'PHEV_PROTOCOL_WATCH_TEST', '!configured',
                      'testMode', 'testConnectPending', 'scanPending', 'scanInProgress',
                      'maintenance.updating()', 'maintenance.rebootPending()', 'web.arg("confirm") != "yes"'):
            self.assertLess(start.index(guard), start.index('registrationQueued = true'))
        self.assertIn('registrationMac = nativeStaMac()', start)
        self.assertIn('registrationHasProtocol = false', start)
        self.assertNotIn('prefs.put', start)
        self.assertNotIn('requestClimate', start)
        setup = MAIN.split('void setup() {', 1)[1].split('void loop()', 1)[0]
        self.assertNotIn('beginRegistrationSession', setup)
        self.assertNotIn('registrationQueued = true', setup)

    def test_one_bounded_operation_no_application_reconnect_and_isolated_loop(self):
        operation = MAIN.split('void tickRegistration()', 1)[1].split('bool registrationLocal()', 1)[0]
        self.assertIn('registrationRunningMs, 45000', operation)
        self.assertEqual(operation.count('WiFi.begin('), 1)
        self.assertNotIn('WiFi.reconnect(', operation)
        self.assertNotIn('connectCarWifi(', operation)
        self.assertNotIn('requestClimate(', operation)
        self.assertIn('beginPhevTcpCompat()', operation)
        self.assertIn('memcmp(actual, clonedMac, 6)', operation)
        loop = MAIN.split('void loop()', 1)[1]
        self.assertIn('if (registrationBusy()) { tickRegistration(); updateLed(); delay(10); return; }', loop)
        self.assertLess(loop.index('tickRegistration()'), loop.index('handleButtons()'))

    def test_other_writes_and_ota_blocked_until_finish_except_cancel(self):
        self.assertIn('maintenance.setExclusiveOperation(registrationBusy)', MAIN)
        self.assertIn('server.uri() != "/registration/cancel"', SERVICE)
        self.assertIn('server.uri() == "/update"', SERVICE)

    def test_old_identity_restored_and_nvs_adoption_requires_separate_current_ack(self):
        finish = MAIN.split('void finishRegistration()', 1)[1].split('void tickRegistration()', 1)[0]
        self.assertIn('clonedMacText = registrationPreviousMac', finish)
        self.assertIn('phev.endRegistrationSession()', finish)
        self.assertNotIn('prefs.put', finish)
        adopt = route('adopt')
        write = adopt.index('prefs.putString("mac", registrationMac)')
        for guard in ('web.arg("confirm") != "yes"', '!registrationHasProtocol',
                      'Registration::Acknowledged', 'registrationMac != nativeStaMac()'):
            self.assertLess(adopt.index(guard), write)
        self.assertLess(write, adopt.index('maintenance.queueReboot(BootMode::Normal)'))

    def test_documentation_distinguishes_old_release_and_unvalidated_feature(self):
        doc = (ROOT / 'docs/MAC-ET-INSCRIPTION.md').read_text(encoding='utf-8')
        for phrase in ('expérimentale', '0.1.0-beta.2-dev', 'v0.1.0-beta.1',
                       'cet essai n\'a pas encore été effectué', 'n\'a pas suffi',
                       'ne démontrent ni', 'résultat **incertain**', 'Feux de détresse éteints'):
            self.assertIn(phrase, doc)
        self.assertIn('Android officielle', doc)
        self.assertIn('cmd/register.go', doc)
        self.assertIn('client/client.go', doc)


if __name__ == '__main__':
    unittest.main()
