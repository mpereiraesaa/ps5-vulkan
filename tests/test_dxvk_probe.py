import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


def load_tool(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    assert spec.loader
    spec.loader.exec_module(module)
    return module


derive = load_tool("derive_dxvk_profile")
probe = load_tool("verify_dxvk_probe")


class ProbeFixture:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.run = root / "run"
        self.eboot = root / "eboot.bin"
        self.eboot.write_bytes(b"dxvk-v262-probe")
        profile_bytes = derive.OUTPUT.read_bytes()
        profile = json.loads(profile_bytes)
        matrix_path = ROOT / "conformance_inventory/dxvk_v262_matrix.json"
        self.manifest = root / "artifact.json"
        self.manifest.write_text(json.dumps({
            "title": "PPSA99994", "profile": "dxvk-v262-capability-probe",
            "submit_enabled": False,
            "files": {"eboot.bin": hashlib.sha256(self.eboot.read_bytes()).hexdigest()},
            "dxvk": {
                "version": "2.6.2", "profile_id": probe.EXPECTED_PROFILE,
                "target_api": profile["profile"]["api_version"],
                "requirements": len(profile["requirements"]),
                "profile_sha256": hashlib.sha256(profile_bytes).hexdigest(),
                "matrix_sha256": hashlib.sha256(matrix_path.read_bytes()).hexdigest(),
            },
        }))
        records = [
            f"DXVK262_PROBE_BEGIN schema=1 profile={probe.EXPECTED_PROFILE} "
            f"target_api={profile['profile']['api_version']} device_api=1.0.0"
        ]
        satisfied = 0
        for row in profile["requirements"]:
            expected = probe.wire_expected(row)
            observed = expected if row["id"].endswith(":robustBufferAccess") else 0
            status = "satisfied" if observed >= expected else "blocker"
            satisfied += status == "satisfied"
            records.append(f"DXVK262_REQUIREMENT id={row['id']} expected={expected} "
                           f"observed={observed} status={status}")
        records.append(
            f"DXVK262_PROBE_END valid=1 compatible=0 total={len(profile['requirements'])} "
            f"satisfied={satisfied} blockers={len(profile['requirements']) - satisfied} "
            "device_extensions=3")
        self.records = records
        self.write(records)

    def write(self, records):
        lines = ["HELLO ps5log/1 title=PPSA99994 app=ps5vk boot=0x1234 tag=dxvk"]
        lines += [f"{seq}\t{1000 + seq}\tMARK\t{record}"
                  for seq, record in enumerate(records, 1)]
        lines.append(f"BYE seq={len(records)} reason=dxvk262-capability-probe")
        data = ("\n".join(lines) + "\n").encode()
        self.run.with_suffix(".log").write_bytes(data)
        self.run.with_suffix(".json").write_text(json.dumps({
            "protocol": "ps5log/1", "transport": "tcp", "clean": True,
            "bye": True, "gaps": [], "records": len(records),
            "sha256": hashlib.sha256(data).hexdigest(),
            "identity": {"title": "PPSA99994", "app": "ps5vk", "boot": "0x1234"},
        }))

    def validate(self):
        return probe.validate(self.run, self.manifest, self.eboot)


class DxvkProbeTests(unittest.TestCase):
    def test_probe_build_uses_only_public_driver_abi(self):
        subprocess.run([sys.executable, str(ROOT / "tools/build_consumer.py"),
                        "--dxvk-v262-probe", "--use-staged-sdk"],
                       cwd=ROOT, check=True, stdout=subprocess.DEVNULL)
        dependencies = (ROOT / "examples/native_consumer/build/main.d").read_text()
        self.assertNotIn(str(ROOT / "src"), dependencies)
        self.assertNotIn(str(ROOT / "native"), dependencies)
        undefined = subprocess.check_output(
            ["nm", "-u", str(ROOT / "examples/native_consumer/build/main.o")], text=True)
        expected = {"vkCreateInstance", "vkDestroyInstance",
                    "vkEnumerateDeviceExtensionProperties", "vkEnumeratePhysicalDevices",
                    "vkGetPhysicalDeviceFeatures2KHR", "vkGetPhysicalDeviceProperties",
                    "vkGetPhysicalDeviceProperties2KHR"}
        actual = {line.split()[-1] for line in undefined.splitlines()
                  if line.split() and line.split()[-1].startswith("vk")}
        self.assertEqual(expected, actual)
        source = (ROOT / "examples/native_consumer/dxvk_capability_probe.h").read_text()
        for private in ("vk_internal.h", "src/", "native/"):
            self.assertNotIn(private, source)
        ordered = [
            "DXVK262_REQUIRED_EXTENSIONS(REPORT_EXTENSION)",
            "DXVK262_FEATURES_VK_PHYSICAL_DEVICE_FEATURES(REPORT_CORE_FEATURE)",
            "DXVK262_FEATURES_VK_PHYSICAL_DEVICE_ROBUSTNESS2_FEATURES_EXT(REPORT_ROBUSTNESS2)",
            "DXVK262_FEATURES_VK_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT(REPORT_TRANSFORM_FEEDBACK)",
            "DXVK262_FEATURES_VK_PHYSICAL_DEVICE_VULKAN11_FEATURES(REPORT_FEATURE11)",
            "DXVK262_FEATURES_VK_PHYSICAL_DEVICE_VULKAN12_FEATURES(REPORT_FEATURE12)",
            "DXVK262_FEATURES_VK_PHYSICAL_DEVICE_VULKAN13_FEATURES(REPORT_FEATURE13)",
            "DXVK262_PROPERTIES_VK_PHYSICAL_DEVICE_VULKAN11_PROPERTIES(REPORT_PROPERTY11)",
            "DXVK262_PROPERTIES_VK_PHYSICAL_DEVICE_VULKAN12_PROPERTIES(REPORT_PROPERTY12)",
            "DXVK262_PROPERTIES_VK_PHYSICAL_DEVICE_VULKAN13_PROPERTIES(REPORT_PROPERTY13)",
        ]
        positions = [source.index(item) for item in ordered]
        self.assertEqual(sorted(positions), positions)

    def test_verifier_accepts_exact_fail_closed_report(self):
        fixture = ProbeFixture()
        try:
            result = fixture.validate()
            self.assertEqual((True, False, 62, 1, 61),
                             (result["strict_verified"], result["compatible"],
                              result["requirements"], result["satisfied"], result["blockers"]))
        finally:
            fixture.tmp.cleanup()

    def test_verifier_rejects_duplicate_false_green_and_wrong_eboot(self):
        fixture = ProbeFixture()
        try:
            duplicate = list(fixture.records)
            duplicate.insert(2, duplicate[1])
            fixture.write(duplicate)
            with self.assertRaisesRegex(ValueError, "missing, duplicate or reordered"):
                fixture.validate()
            forged = list(fixture.records)
            forged[1] = forged[1].replace("status=blocker", "status=satisfied")
            fixture.write(forged)
            with self.assertRaisesRegex(ValueError, "payload verdict mismatch"):
                fixture.validate()
            fixture.write(fixture.records)
            fixture.eboot.write_bytes(b"different")
            with self.assertRaisesRegex(ValueError, "artifact identity"):
                fixture.validate()
        finally:
            fixture.tmp.cleanup()

    def test_multiview_requires_explicit_matching_query_route(self):
        fixture = ProbeFixture()
        self.addCleanup(fixture.tmp.cleanup)
        values = {"multiview": 1, "maxMultiviewViewCount": 6,
                  "maxMultiviewInstanceIndex": 134217727}
        records = list(fixture.records)
        for i, record in enumerate(records):
            for field, value in values.items():
                if f":{field} " in record:
                    records[i] = record.replace("observed=0", f"observed={value}").replace(
                        "status=blocker", "status=satisfied")
        records[-1] = records[-1].replace("satisfied=1 blockers=61", "satisfied=4 blockers=58")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit multiview query route"):
            fixture.validate()
        route = ("DXVK262_MULTIVIEW_QUERY route=VK_KHR_multiview "
                 "multiview=1 maxMultiviewViewCount=6 maxMultiviewInstanceIndex=134217727")
        records.insert(1, route)
        fixture.write(records)
        self.assertEqual(4, fixture.validate()["satisfied"])
        bad = list(records)
        bad[1] = route.replace("maxMultiviewViewCount=6", "maxMultiviewViewCount=7")
        fixture.write(bad)
        with self.assertRaisesRegex(ValueError, "route value mismatch"):
            fixture.validate()
        records.insert(1, route)
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit multiview query route"):
            fixture.validate()

    def test_standard_ubo_positive_requires_matching_khr_query_route(self):
        fixture = ProbeFixture()
        self.addCleanup(fixture.tmp.cleanup)
        identifier = "feature:VkPhysicalDeviceVulkan12Features:uniformBufferStandardLayout"
        records = list(fixture.records)
        index = next(i for i, record in enumerate(records) if f"id={identifier} " in record)
        records[index] = records[index].replace("observed=0 status=blocker",
                                                 "observed=1 status=satisfied")
        records[-1] = records[-1].replace("satisfied=1 blockers=61",
                                         "satisfied=2 blockers=60")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit standard UBO query route"):
            fixture.validate()
        route = ("DXVK262_STANDARD_UBO_QUERY route=VK_KHR_uniform_buffer_standard_layout "
                 "uniformBufferStandardLayout=1")
        records.insert(1, route)
        fixture.write(records)
        self.assertEqual(2, fixture.validate()["satisfied"])
        records[1] = route.replace("uniformBufferStandardLayout=1",
                                   "uniformBufferStandardLayout=0")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit standard UBO query route"):
            fixture.validate()

    def test_memory_model_base_and_scope_require_exact_khr_query(self):
        fixture = ProbeFixture()
        self.addCleanup(fixture.tmp.cleanup)
        identifier = "feature:VkPhysicalDeviceVulkan12Features:vulkanMemoryModel"
        records = list(fixture.records)
        index = next(i for i, record in enumerate(records) if f"id={identifier} " in record)
        records[index] = records[index].replace("observed=0 status=blocker",
                                                 "observed=1 status=satisfied")
        records[-1] = records[-1].replace("satisfied=1 blockers=61",
                                         "satisfied=2 blockers=60")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit memory model query route"):
            fixture.validate()
        route = ("DXVK262_MEMORY_MODEL_QUERY route=VK_KHR_vulkan_memory_model "
                 "vulkanMemoryModel=1 vulkanMemoryModelDeviceScope=0")
        records.insert(1, route)
        fixture.write(records)
        self.assertEqual(2, fixture.validate()["satisfied"])
        records[1] = route.replace("vulkanMemoryModelDeviceScope=0",
                                   "vulkanMemoryModelDeviceScope=1")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "memory model route value mismatch"):
            fixture.validate()

    def test_buffer_device_address_positive_requires_exact_khr_query(self):
        fixture = ProbeFixture()
        self.addCleanup(fixture.tmp.cleanup)
        identifier = "feature:VkPhysicalDeviceVulkan12Features:bufferDeviceAddress"
        records = list(fixture.records)
        index = next(i for i, record in enumerate(records) if f"id={identifier} " in record)
        records[index] = records[index].replace("observed=0 status=blocker",
                                                 "observed=1 status=satisfied")
        records[-1] = records[-1].replace("satisfied=1 blockers=61",
                                         "satisfied=2 blockers=60")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit buffer device address query route"):
            fixture.validate()
        route = ("DXVK262_BUFFER_DEVICE_ADDRESS_QUERY "
                 "route=VK_KHR_buffer_device_address bufferDeviceAddress=1")
        records.insert(1, route)
        fixture.write(records)
        self.assertEqual(2, fixture.validate()["satisfied"])

    def test_host_query_reset_positive_requires_exact_ext_query(self):
        fixture = ProbeFixture()
        self.addCleanup(fixture.tmp.cleanup)
        identifier = "feature:VkPhysicalDeviceVulkan12Features:hostQueryReset"
        records = list(fixture.records)
        index = next(i for i, record in enumerate(records) if f"id={identifier} " in record)
        records[index] = records[index].replace("observed=0 status=blocker",
                                                 "observed=1 status=satisfied")
        records[-1] = records[-1].replace("satisfied=1 blockers=61",
                                         "satisfied=2 blockers=60")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit host query reset route"):
            fixture.validate()
        route = ("DXVK262_HOST_QUERY_RESET_QUERY "
                 "route=VK_EXT_host_query_reset hostQueryReset=1")
        records.insert(1, route)
        fixture.write(records)
        self.assertEqual(2, fixture.validate()["satisfied"])
        records[1] = route.replace("hostQueryReset=1", "hostQueryReset=0")
        fixture.write(records)
        with self.assertRaisesRegex(ValueError, "explicit host query reset route"):
            fixture.validate()

    def test_historical_matrix_snapshot_remains_hash_bound(self):
        fixture = ProbeFixture()
        self.addCleanup(fixture.tmp.cleanup)
        snapshot = Path(fixture.tmp.name) / "matrix.json"
        snapshot.write_bytes((ROOT / "conformance_inventory/dxvk_v262_matrix.json").read_bytes())
        self.assertTrue(probe.validate(fixture.run, fixture.manifest, fixture.eboot,
                                       snapshot)["strict_verified"])
        snapshot.write_bytes(b"wrong matrix")
        with self.assertRaisesRegex(ValueError, "artifact DXVK contract"):
            probe.validate(fixture.run, fixture.manifest, fixture.eboot, snapshot)


if __name__ == "__main__":
    unittest.main()
