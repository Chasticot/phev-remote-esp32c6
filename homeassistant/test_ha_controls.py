"""Local YAML/safety and boundary tests. Never connects to HA/MQTT/hardware.

Run with PlatformIO Python (PyYAML available). If Jinja2 is available, the real
artifact's templates and payloads are additionally rendered in a fake HA state
environment. Without Jinja2 the policy equivalents below are boundary checks,
not a claim that Home Assistant has validated or rendered its configuration.
"""
import datetime as dt
import json
import math
from pathlib import Path
from types import SimpleNamespace
import unittest

import yaml

ROOT = Path(__file__).parent
DOC = yaml.safe_load((ROOT / "PHEV-HA-controls.yaml").read_text(encoding="utf-8"))
SENSORS = {s["unique_id"]: s for block in DOC["template"] for s in block.get("sensor", [])}
MASTER = DOC["script"]["phev_execute"]
try:
    import jinja2
except ImportError:
    jinja2 = None


def walk(value):
    yield value
    if isinstance(value, dict):
        for item in value.values():
            yield from walk(item)
    elif isinstance(value, list):
        for item in value:
            yield from walk(item)


def age_policy(base_age, reception_stamp, now_stamp):
    try:
        base_age = float(base_age)
        reception_stamp = float(reception_stamp)
        now_stamp = float(now_stamp)
    except (TypeError, ValueError):
        return None
    if not (0 <= base_age < 1_000_000_000 and 0 < reception_stamp <= now_stamp):
        return None
    return base_age + now_stamp - reception_stamp


def soc_policy(soc, valid, age, stamp, now):
    age = age_policy(age, stamp, now)
    try:
        soc = float(soc)
    except (ValueError, TypeError):
        return None
    return soc if valid and 0 <= soc <= 100 and age is not None and age <= 900 else None


def battery_report_proven(latch_on, source_updated, latch_changed):
    try:
        source_updated = float(source_updated)
        latch_changed = float(latch_changed)
    except (ValueError, TypeError):
        return False
    return latch_on and source_updated >= latch_changed > 0


def word_policy(value):
    try:
        number = float(value)
    except (ValueError, TypeError):
        return None
    if not (0 <= number <= 16_777_215 and number.is_integer()):
        return None
    word = int(number)
    return word if word == 0 or (word // 8 > 0 and word % 8 > 0) else None


def heartbeat_policy(previous_latch, trigger_id, previous_uptime, current_uptime, seconds_since_report):
    try:
        current_uptime = float(current_uptime)
        seconds_since_report = float(seconds_since_report)
    except (TypeError, ValueError):
        return False
    if trigger_id == "demarrage" or not (0 <= seconds_since_report <= 120 and 0 <= current_uptime < 1_000_000_000):
        return False
    try:
        previous_uptime = float(previous_uptime)
    except (TypeError, ValueError):
        previous_uptime = -1
    if trigger_id == "heartbeat" and 0 <= previous_uptime < 1_000_000_000 and previous_uptime != current_uptime:
        return True
    return previous_latch


def new_terminal(start, received):
    start, received = word_policy(start), word_policy(received)
    return (start is not None and received is not None
            and received // 8 != start // 8 and received % 8 >= 3)


class ArtifactTests(unittest.TestCase):
    def test_top_level_only_scoped_controls(self):
        self.assertEqual(set(DOC), {"input_select", "input_boolean", "input_text", "template", "script"})
        self.assertNotIn("automation", DOC)
        self.assertFalse(DOC["input_boolean"]["phev_notifications"]["initial"])
        self.assertEqual(DOC["input_select"]["phev_duree"]["options"], ["10", "20", "30"])
        self.assertEqual(DOC["input_select"]["phev_duree"]["initial"], "10")

    def test_stable_template_ids(self):
        self.assertEqual(set(SENSORS), {"phev_soc_pilotage", "phev_age_mesure", "phev_derniere_mesure", "phev_etat_passerelle"})
        for key, sensor in SENSORS.items():
            self.assertEqual(sensor["default_entity_id"], "sensor." + key)

    def test_one_trigger_latch_with_start_and_expiry(self):
        blocks = [block for block in DOC["template"] if "binary_sensor" in block]
        self.assertEqual(len(blocks), 1)
        self.assertEqual([trigger["id"] for trigger in blocks[0]["triggers"]], ["demarrage", "heartbeat", "expiration"])
        self.assertEqual(blocks[0]["binary_sensor"][0]["default_entity_id"], "binary_sensor.phev_rapports_recents")
        template = blocks[0]["binary_sensor"][0]["state"]
        self.assertIn("trigger.from_state.state", template)
        self.assertIn("this is defined", template)
        self.assertIn("0 <= age <= 120", template)

    def test_every_script_single_not_queued_and_no_retry(self):
        for script in DOC["script"].values():
            self.assertEqual(script["mode"], "single")
            self.assertFalse(any(isinstance(node, dict) and "repeat" in node for node in walk(script)))

    def test_publish_single_with_no_retain(self):
        publishes = [node for node in walk(MASTER) if isinstance(node, dict) and node.get("action") == "mqtt.publish"]
        self.assertEqual(len(publishes), 1)
        for node in walk(DOC["script"]):
            if isinstance(node, dict) and node.get("action") == "mqtt.publish":
                self.assertIs(node["data"]["retain"], False)
                self.assertEqual(node["data"]["qos"], 0)
                self.assertEqual(node["data"]["topic"], "zigbee2mqtt/Outlander-PHEV-Remote/set")
        self.assertIn("'mode_command'", publishes[0]["data"]["payload"])
        self.assertIn("'duration_command'", publishes[0]["data"]["payload"])
        self.assertIn("'climate_command': requested != 'stop'", publishes[0]["data"]["payload"])

    def test_block_before_mqtt_and_only_information(self):
        sequence = MASTER["sequence"]
        publish_index = next(i for i, step in enumerate(sequence) if step.get("action") == "mqtt.publish")
        block_index = next(i for i, step in enumerate(sequence) if "if" in step)
        self.assertLess(block_index, publish_index)
        blocked = sequence[block_index]["then"]
        self.assertEqual([step.get("action", "stop") for step in blocked], ["input_text.set_value", "stop"])

    def test_atomic_wait_not_old_flags(self):
        wait = next(step for step in MASTER["sequence"] if "wait_template" in step)
        self.assertEqual(wait["timeout"], "00:01:15")
        self.assertIs(wait["continue_on_timeout"], True)
        self.assertIn("session_word", wait["wait_template"])
        self.assertIn("!= starting_sequence", wait["wait_template"])
        self.assertIn("% 8 >= 3", wait["wait_template"])
        self.assertNotIn("command_ack", json.dumps(MASTER))
        self.assertNotIn("command_failed", json.dumps(MASTER))

    def test_wrappers_preserve_master_gate(self):
        mapping = {"phev_actualiser": "refresh", "phev_chauffer": "heat", "phev_refroidir": "cool",
                   "phev_degivrer": "windscreen", "phev_arreter": "stop"}
        for name, operation in mapping.items():
            self.assertEqual(DOC["script"][name]["sequence"], [{"action": "script.phev_execute", "data": {"operation": operation}}])

    def test_maintenance_payloads_no_climate(self):
        for name, mode in [("phev_maintenance_ap", "ap"), ("phev_maintenance_wifi", "wifi-seb")]:
            sequence = DOC["script"][name]["sequence"]
            publish = [step for step in sequence if step.get("action") == "mqtt.publish"]
            self.assertEqual(len(publish), 1)
            self.assertEqual(json.loads(publish[0]["data"]["payload"]), {"maintenance_mode": mode})
            self.assertIn("stop", sequence[1]["then"][-1])

    def test_notification_only_opt_in_and_precise_ack(self):
        gate = MASTER["sequence"][-1]
        self.assertEqual(gate["if"][0], {"condition": "state", "entity_id": "input_boolean.phev_notifications", "state": "on"})
        self.assertIn("request_failed or terminal_status == 4", gate["if"][1]["value_template"])
        self.assertEqual(gate["then"][0]["action"], "notify.mobile_app_s26u_seb")
        result = next(step["variables"]["result_message"] for step in MASTER["sequence"] if "result_message" in step.get("variables", {}))
        self.assertIn("non confirmé", result)
        self.assertIn("terminal_status == 3 and requested == 'refresh'", result)
        self.assertIn("terminal_status == 4 and requested != 'refresh'", result)

    def test_freshness_checks_real_age_plus_elapsed(self):
        availability = SENSORS["phev_soc_pilotage"]["availability"]
        self.assertIn("battery_valid", availability)
        self.assertIn("battery_age", availability)
        self.assertIn("s.last_updated", availability)
        self.assertIn("age + elapsed <= 900", availability)
        self.assertIn("0 <= soc <= 100", availability)
        self.assertNotIn("phev_online", availability)
        self.assertNotIn("sensor.phev_age_mesure", availability)
        self.assertIn("binary_sensor.phev_rapports_recents", availability)
        self.assertIn("binary_sensor.phev_rapports_recents", MASTER["sequence"][0]["variables"]["gateway_recent"])

    def test_all_battery_derived_sensors_require_report_after_live_arm(self):
        for key in ["phev_soc_pilotage", "phev_age_mesure", "phev_derniere_mesure"]:
            availability = SENSORS[key]["availability"]
            self.assertIn("is_state('binary_sensor.phev_rapports_recents', 'on')", availability)
            self.assertIn("as_timestamp(s.last_updated, 0) >= as_timestamp(live.last_changed, 0) > 0", availability)


class BoundaryTests(unittest.TestCase):
    def test_soc_exactly_15_minute_boundary(self):
        self.assertEqual(soc_policy(94, True, 870, 1000, 1030), 94)
        self.assertIsNone(soc_policy(94, True, 870, 1000, 1030.01))

    def test_idle_tcp_not_a_freshness_condition(self):
        self.assertEqual(soc_policy(94, True, 60, 1000, 1100), 94)

    def test_gateway_lost_age_does_not_freeze(self):
        self.assertEqual(age_policy(90, 1000, 1090), 180)
        self.assertIsNone(soc_policy(94, True, 90, 1000, 1811))

    def test_missing_invalid_negative_nonfinite(self):
        for age in [None, "unknown", "unavailable", -1, float("nan"), float("inf")]:
            self.assertIsNone(soc_policy(94, True, age, 1000, 1000))
        for soc in [None, "unknown", -1, 101, float("nan"), float("inf")]:
            self.assertIsNone(soc_policy(soc, True, 0, 1000, 1000))
        self.assertIsNone(soc_policy(94, False, 0, 1000, 1000))
        self.assertIsNone(soc_policy(94, True, 0, None, 1000))
        self.assertIsNone(soc_policy(94, True, 0, 1001, 1000))

    def test_measurement_date_stays_same(self):
        self.assertEqual(1000 - 90, 1030 - 120)
        self.assertEqual(1000 - 90, 1090 - age_policy(90, 1000, 1090))

    def test_stale_ack_same_sequence_never_completes(self):
        old_ack = 123 * 8 + 4
        self.assertFalse(new_terminal(old_ack, old_ack))
        self.assertFalse(new_terminal(old_ack, 123 * 8 + 3))
        self.assertFalse(new_terminal(old_ack, 124 * 8 + 1))
        self.assertFalse(new_terminal(old_ack, 124 * 8 + 2))
        self.assertTrue(new_terminal(old_ack, 124 * 8 + 4))

    def test_atomic_timeout_transport_command_failure(self):
        for status in [3, 4, 5, 6, 7]:
            self.assertTrue(new_terminal(123 * 8 + 4, 124 * 8 + status))

    def test_packed_word_range_and_wrap(self):
        for value in [None, "unknown", -1, 16_777_216, 9.5, 5, 8, float("nan"), float("inf")]:
            self.assertIsNone(word_policy(value))
        self.assertEqual(word_policy(16_777_215), 16_777_215)
        self.assertTrue(new_terminal(16_777_215, 1 * 8 + 4))

    def test_start_resets_restored_latch(self):
        self.assertFalse(heartbeat_policy(True, "demarrage", 300, 300, 0))

    def test_retained_initial_is_not_live_proof(self):
        self.assertFalse(heartbeat_policy(False, "heartbeat", "unknown", 300, 0))
        self.assertFalse(heartbeat_policy(False, "heartbeat", "unavailable", 300, 0))
        self.assertFalse(heartbeat_policy(False, "heartbeat", None, 300, 0))
        self.assertFalse(heartbeat_policy(False, "expiration", 300, 300, 30))

    def test_fresh_heartbeat_arms_then_expires(self):
        self.assertTrue(heartbeat_policy(False, "heartbeat", 300, 330, 0))
        self.assertTrue(heartbeat_policy(True, "expiration", 330, 330, 120))
        self.assertFalse(heartbeat_policy(True, "expiration", 330, 330, 120.01))
        self.assertFalse(heartbeat_policy(False, "expiration", 330, 330, 0))

    def test_reboot_counter_change_is_live_heartbeat(self):
        self.assertTrue(heartbeat_policy(False, "heartbeat", 330, 0, 0))
        self.assertFalse(heartbeat_policy(False, "heartbeat", 330, 330, 0))
        self.assertFalse(heartbeat_policy(False, "heartbeat", 330, float("nan"), 0))

    def test_retained_battery_before_arm_cannot_look_fresh(self):
        self.assertFalse(battery_report_proven(True, 1000, 1030))
        self.assertTrue(battery_report_proven(True, 1030, 1030))
        self.assertTrue(battery_report_proven(True, 1031, 1030))
        self.assertFalse(battery_report_proven(False, 1031, 1030))
        for bad in [None, "unknown", 0, float("nan"), float("inf")]:
            self.assertFalse(battery_report_proven(True, 1031, bad))


class FakeStates:
    def __init__(self, values):
        self.values = values
    def __call__(self, entity):
        value = self.values.get(entity)
        return value.state if value else "unknown"
    def __getattr__(self, domain):
        return SimpleNamespace(**{entity.split(".", 1)[1]: value for entity, value in self.values.items() if entity.startswith(domain + ".")})


@unittest.skipIf(jinja2 is None, "Jinja2 absent : rendu réel à compléter avec validation HA")
class OptionalRealTemplateTests(unittest.TestCase):
    def setUp(self):
        self.env = jinja2.Environment(undefined=jinja2.StrictUndefined)
        self.env.filters["float"] = lambda value, default=0: self.convert(float, value, default)
        self.env.filters["int"] = lambda value, default=0: self.convert(int, value, default)
        self.env.filters["to_json"] = json.dumps
        self.env.filters["timestamp_local"] = lambda stamp: dt.datetime.fromtimestamp(float(stamp), dt.timezone.utc).isoformat()
        self.now = dt.datetime(2026, 10, 7, 12, tzinfo=dt.timezone.utc)
        stamp = self.now - dt.timedelta(seconds=30)
        self.values = {
            "sensor.outlander_phev_remote_battery": SimpleNamespace(state="94", last_updated=stamp),
            "sensor.outlander_phev_remote_battery_age": SimpleNamespace(state="870", last_updated=stamp),
            "binary_sensor.outlander_phev_remote_battery_valid": SimpleNamespace(state="on", last_updated=stamp),
            "sensor.outlander_phev_remote_uptime": SimpleNamespace(state="240", last_updated=stamp),
            "sensor.outlander_phev_remote_gateway_mode": SimpleNamespace(state="normal", last_updated=stamp),
            "sensor.outlander_phev_remote_session_word": SimpleNamespace(state="988", last_updated=stamp),
            "binary_sensor.phev_rapports_recents": SimpleNamespace(state="on", last_updated=stamp, last_changed=stamp),
        }
        self.states = FakeStates(self.values)
        self.env.globals.update(states=self.states, now=lambda: self.now,
                                is_state=lambda entity, state: self.states(entity) == state,
                                as_timestamp=self.timestamp)

    @staticmethod
    def convert(kind, value, default):
        try:
            return kind(value)
        except (ValueError, TypeError, OverflowError):
            return default

    @staticmethod
    def timestamp(value, default=None):
        try:
            return value.timestamp() if isinstance(value, dt.datetime) else float(value)
        except (ValueError, TypeError):
            return default

    def render(self, template, **variables):
        return self.env.from_string(template).render(**variables).strip()

    def test_compile_every_actual_template(self):
        for value in walk(DOC):
            if isinstance(value, str) and ("{{" in value or "{%" in value):
                self.env.from_string(value)

    def test_actual_soc_boundary_and_gateway_loss(self):
        availability = SENSORS["phev_soc_pilotage"]["availability"]
        self.assertEqual(self.render(availability), "True")
        self.now += dt.timedelta(seconds=1)
        self.assertEqual(self.render(availability), "False")

    def test_actual_missing_invalid_sources(self):
        availability = SENSORS["phev_soc_pilotage"]["availability"]
        for value in ["unknown", "unavailable", "-1", "nan", "inf"]:
            self.values["sensor.outlander_phev_remote_battery_age"].state = value
            self.assertEqual(self.render(availability), "False")

    def test_actual_battery_report_before_arm_rejected_for_all_sensors(self):
        self.values["binary_sensor.phev_rapports_recents"].last_changed = self.now - dt.timedelta(seconds=29)
        for key in ["phev_soc_pilotage", "phev_age_mesure", "phev_derniere_mesure"]:
            self.assertEqual(self.render(SENSORS[key]["availability"]), "False")

    def test_actual_grouped_payload(self):
        payload = next(step["data"]["payload"] for step in MASTER["sequence"] if step.get("action") == "mqtt.publish")
        self.assertEqual(json.loads(self.render(payload, requested="refresh", requested_duration="10")), {"refresh": "refresh"})
        for operation in ["heat", "cool", "windscreen", "stop"]:
            parsed = json.loads(self.render(payload, requested=operation, requested_duration="10"))
            self.assertEqual(set(parsed), {"mode_command", "duration_command", "climate_command"})
            self.assertIs(parsed["climate_command"], operation != "stop")

    def test_native_numeric_duration_and_safe_stop(self):
        from jinja2.nativetypes import NativeEnvironment
        native = NativeEnvironment()
        self.assertEqual(native.from_string("{{ value }}").render(value="10"), 10)
        guard = MASTER["sequence"][1]["variables"]["blocked_reason"]
        payload = next(step["data"]["payload"] for step in MASTER["sequence"] if step.get("action") == "mqtt.publish")
        for duration in [10, 20, 30, "10", "20", "30"]:
            self.assertEqual(self.render(guard, requested="heat", requested_duration=duration,
                                         gateway_recent=True, starting_word=988), "")
            self.assertEqual(json.loads(self.render(payload, requested="heat", requested_duration=duration))["duration_command"], str(duration))
        for bad in ["unknown", "unavailable", "", 0, 11, 10.5]:
            self.assertIn("Durée invalide", self.render(guard, requested="heat", requested_duration=bad,
                                                       gateway_recent=True, starting_word=988))
            self.assertEqual(self.render(guard, requested="stop", requested_duration=bad,
                                         gateway_recent=True, starting_word=988), "")
            self.assertEqual(json.loads(self.render(payload, requested="stop", requested_duration=bad)),
                             {"mode_command": "cool", "duration_command": "10", "climate_command": False})

    def test_actual_wait_rejects_stale_ack(self):
        wait = next(step["wait_template"] for step in MASTER["sequence"] if "wait_template" in step)
        self.assertEqual(self.render(wait, starting_sequence=123), "False")
        self.values["sensor.outlander_phev_remote_session_word"].state = str(124 * 8 + 5)
        self.assertEqual(self.render(wait, starting_sequence=123), "True")


if __name__ == "__main__":
    unittest.main(verbosity=2)
