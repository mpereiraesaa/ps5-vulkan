"""Unit tests and negative test fixtures for Vulkan 1.0 command surface parity gate."""

from pathlib import Path
import shutil
import tempfile
import unittest

from tools.check_command_surface import (
    audit_command_surface,
    parse_core_commands,
    parse_public_prototypes,
    parse_dispatch_entries,
    parse_implementations,
    EXPECTED_VULKAN10_TOTAL,
    EXPECTED_FULLY_WIRED_TOTAL,
    EXPECTED_MISSING_TOTAL,
    REQUIRED_BOOKKEEPING_COMMANDS,
    REQUIRED_SYNC_OBJECT_COMMANDS,
    REQUIRED_BUFFER_TRANSFER_COMMANDS,
    REQUIRED_INDIRECT_COMMANDS,
    REQUIRED_DYNAMIC_STATE_COMMANDS,
)

REPO_ROOT = Path(__file__).resolve().parents[1]


class TestCommandSurfaceParity(unittest.TestCase):
    """Verify live repository passes the Vulkan 1.0 parity gate."""

    def test_live_repository_passes_parity_gate(self):
        result = audit_command_surface(REPO_ROOT)
        self.assertTrue(result["passed"], f"Audit failed with errors: {result['errors']}")
        self.assertEqual(result["core_total"], EXPECTED_VULKAN10_TOTAL)
        self.assertEqual(result["fully_wired_total"], EXPECTED_FULLY_WIRED_TOTAL)
        self.assertEqual(result["missing_total"], EXPECTED_MISSING_TOTAL)
        self.assertEqual(result["dispatch_not_public"], [])
        self.assertEqual(result["impl_not_public"], [])
        self.assertEqual(result["public_not_dispatch"], [])
        self.assertEqual(result["dispatch_not_impl"], [])
        self.assertEqual(result["missing_required"], [])
        self.assertEqual(result["uncategorized_missing"], [])

    def test_all_five_bookkeeping_commands_are_supported(self):
        result = audit_command_surface(REPO_ROOT)
        fully_wired = set(result["fully_wired"])
        for cmd in REQUIRED_BOOKKEEPING_COMMANDS:
            self.assertIn(cmd, fully_wired, f"Required bookkeeping command {cmd} is not in fully wired surface")

    def test_sync_objects_are_fully_wired(self):
        fully_wired = set(audit_command_surface(REPO_ROOT)["fully_wired"])
        self.assertTrue(REQUIRED_SYNC_OBJECT_COMMANDS <= fully_wired)

    def test_buffer_transfer_commands_are_fully_wired(self):
        fully_wired = set(audit_command_surface(REPO_ROOT)["fully_wired"])
        self.assertTrue(REQUIRED_BUFFER_TRANSFER_COMMANDS <= fully_wired)

    def test_indirect_commands_are_fully_wired(self):
        fully_wired = set(audit_command_surface(REPO_ROOT)["fully_wired"])
        self.assertTrue(REQUIRED_INDIRECT_COMMANDS <= fully_wired)

    def test_core_dynamic_state_commands_are_fully_wired(self):
        fully_wired = set(audit_command_surface(REPO_ROOT)["fully_wired"])
        self.assertTrue(REQUIRED_DYNAMIC_STATE_COMMANDS <= fully_wired)


class TestCommandSurfaceNegativeFixtures(unittest.TestCase):
    """Negative fixtures simulating drift, regressions, and omissions."""

    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.mock_root = Path(self.temp_dir.name)

        # Mirror necessary directory structure
        (self.mock_root / "third_party/vulkan-headers/registry").mkdir(parents=True)
        (self.mock_root / "include/ps5vk").mkdir(parents=True)
        (self.mock_root / "src").mkdir(parents=True)
        (self.mock_root / "native").mkdir(parents=True)

        shutil.copyfile(
            REPO_ROOT / "third_party/vulkan-headers/registry/vk.xml",
            self.mock_root / "third_party/vulkan-headers/registry/vk.xml",
        )
        shutil.copyfile(
            REPO_ROOT / "include/ps5vk/ps5vk.h",
            self.mock_root / "include/ps5vk/ps5vk.h",
        )
        shutil.copyfile(
            REPO_ROOT / "src/vk_dispatch.c",
            self.mock_root / "src/vk_dispatch.c",
        )
        # Copy src *.c files
        for src_file in (REPO_ROOT / "src").glob("*.c"):
            if src_file.name != "vk_dispatch.c":
                shutil.copyfile(src_file, self.mock_root / "src" / src_file.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_missing_public_header_declaration_is_detected(self):
        """Simulate removing vkResetDescriptorPool from include/ps5vk/ps5vk.h."""
        header_path = self.mock_root / "include/ps5vk/ps5vk.h"
        content = header_path.read_text()
        content = content.replace("vkResetDescriptorPool", "vkResetDescriptorPool_DISABLED")
        header_path.write_text(content)

        result = audit_command_surface(self.mock_root)
        self.assertFalse(result["passed"])
        self.assertIn("vkResetDescriptorPool", result["dispatch_not_public"])
        self.assertIn("vkResetDescriptorPool", result["impl_not_public"])
        self.assertIn("vkResetDescriptorPool", result["missing_required"])

    def test_missing_dispatch_entry_is_detected(self):
        """Simulate removing vkGetRenderAreaGranularity from src/vk_dispatch.c."""
        dispatch_path = self.mock_root / "src/vk_dispatch.c"
        content = dispatch_path.read_text()
        content = content.replace("ENTRY(vkGetRenderAreaGranularity, DEVICE),", "")
        dispatch_path.write_text(content)

        result = audit_command_surface(self.mock_root)
        self.assertFalse(result["passed"])
        self.assertIn("vkGetRenderAreaGranularity", result["public_not_dispatch"])
        self.assertIn("vkGetRenderAreaGranularity", result["missing_required"])

    def test_missing_implementation_is_detected(self):
        """Simulate removing vkGetDeviceMemoryCommitment C definition."""
        mem_path = self.mock_root / "src/vk_memory.c"
        content = mem_path.read_text()
        content = content.replace("VKAPI_CALL vkGetDeviceMemoryCommitment", "VKAPI_CALL vkGetDeviceMemoryCommitment_DISABLED")
        mem_path.write_text(content)

        result = audit_command_surface(self.mock_root)
        self.assertFalse(result["passed"])
        self.assertIn("vkGetDeviceMemoryCommitment", result["dispatch_not_impl"])
        self.assertIn("vkGetDeviceMemoryCommitment", result["missing_required"])

    def test_unimplemented_public_declaration_fails_closed(self):
        """Simulate adding an unimplemented command to the public header."""
        header_path = self.mock_root / "include/ps5vk/ps5vk.h"
        content = header_path.read_text()
        content += "\nVKAPI_ATTR VkResult VKAPI_CALL vkQueueBindSparse(VkQueue q, uint32_t n, const VkBindSparseInfo* i, VkFence f);\n"
        header_path.write_text(content)

        result = audit_command_surface(self.mock_root)
        self.assertFalse(result["passed"])
        self.assertIn("vkQueueBindSparse", result["public_not_dispatch"])


if __name__ == "__main__":
    unittest.main()
