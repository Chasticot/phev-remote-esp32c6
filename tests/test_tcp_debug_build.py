"""Host-only regression tests for the file-scoped PlatformIO debug flags."""
from pathlib import Path
import runpy
import unittest


class FakeNode:
    def __init__(self, name):
        self.name = name


class FakeEnvironment(dict):
    def subst(self, value):
        assert value == "$PROJECT_DIR"
        return str(Path(__file__).resolve().parents[1])
    def AddBuildMiddleware(self, callback, pattern):
        self.callback, self.pattern = callback, pattern

    def Object(self, node, **kwargs):
        return node, kwargs


class TcpDebugBuildTests(unittest.TestCase):
    def setUp(self):
        self.env = FakeEnvironment(CPPDEFINES=["ESP32", ("ARDUINO_USB_CDC_ON_BOOT", 1)])
        script = Path(__file__).resolve().parents[1] / "tools" / "tcp_debug_build.py"
        runpy.run_path(str(script), init_globals={"env": self.env, "Import": lambda _: None})

    def test_registered_only_for_network_client(self):
        self.assertEqual(self.env.pattern, "*NetworkClient.cpp")

    def test_only_tcp_object_gets_debug(self):
        before = list(self.env["CPPDEFINES"])
        _, kwargs = self.env.callback(self.env, FakeNode("NetworkClient.cpp"))
        self.assertEqual(kwargs["CPPDEFINES"], before + [("CORE_DEBUG_LEVEL", 4)])
        self.assertEqual(self.env["CPPDEFINES"], before)

    def test_web_parser_is_unchanged(self):
        node = FakeNode("Parsing.cpp")
        self.assertIs(self.env.callback(self.env, node), node)

    def test_only_project_local_transport_replaces_upstream(self):
        node, _ = self.env.callback(self.env, FakeNode("NetworkClient.cpp"))
        self.assertEqual(Path(node), Path(__file__).resolve().parents[1] / "tools" / "NetworkClientBounded.cpp")

    def test_wifi_compatibility_switch_is_preserved(self):
        for value in (0, 1):
            with self.subTest(value=value):
                self.env["CPPDEFINES"] = ["ESP32", ("PHEV_WIFI_LEGACY_BGN", value)]
                _, kwargs = self.env.callback(self.env, FakeNode("NetworkClient.cpp"))
                self.assertIn(("PHEV_WIFI_LEGACY_BGN", value), kwargs["CPPDEFINES"])
                self.assertIn(("PHEV_WIFI_LEGACY_BGN", value), self.env["CPPDEFINES"])

    def test_existing_tuple_level_is_replaced(self):
        self.env["CPPDEFINES"].append(("CORE_DEBUG_LEVEL", 0))
        _, kwargs = self.env.callback(self.env, FakeNode("NetworkClient.cpp"))
        self.assertEqual([d for d in kwargs["CPPDEFINES"] if isinstance(d, tuple) and d[0] == "CORE_DEBUG_LEVEL"], [("CORE_DEBUG_LEVEL", 4)])
        self.assertIn(("CORE_DEBUG_LEVEL", 0), self.env["CPPDEFINES"])

    def test_raw_tcp_switch_is_preserved(self):
        for value in (0, 1):
            with self.subTest(value=value):
                self.env["CPPDEFINES"].append(("PHEV_RAW_TCP_TEST", value))
                _, kwargs = self.env.callback(self.env, FakeNode("NetworkClient.cpp"))
                self.assertIn(("PHEV_RAW_TCP_TEST", value), kwargs["CPPDEFINES"])
                self.env["CPPDEFINES"].pop()

    def test_existing_string_level_is_replaced(self):
        self.env["CPPDEFINES"].append("CORE_DEBUG_LEVEL=0")
        _, kwargs = self.env.callback(self.env, FakeNode("NetworkClient.cpp"))
        self.assertNotIn("CORE_DEBUG_LEVEL=0", kwargs["CPPDEFINES"])
        self.assertIn(("CORE_DEBUG_LEVEL", 4), kwargs["CPPDEFINES"])


if __name__ == "__main__":
    unittest.main()
