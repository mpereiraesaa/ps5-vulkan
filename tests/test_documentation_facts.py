"""Documentation facts that must not silently drift away from the tree.

Statements had already gone stale on ``main`` while every host gate stayed
green, because nothing tied the prose to the machine source it describes:

* the narrative documents described the command surface as ``131/137`` with six
  image transfer/clear commands absent, while the parity gate reports 137/137
  structurally wired commands with zero asymmetries;
* ``API.md`` and ``PHYSICAL_DEVICE_REPORTING.md`` described
  ``VK_FORMAT_D32_SFLOAT`` as a depth attachment only, after the merged
  whole-subresource depth-only clear started advertising
  ``VK_FORMAT_FEATURE_TRANSFER_DST_BIT`` for it;
* ``VALIDATION.md`` still listed "depth/stencil clear" among wholly unsupported
  semantics.

Every expectation below is derived from the canonical machine source - the
command-surface audit and ``src/texture_format.c`` - instead of restating a
second copy of that number, so the gate fails when the tree moves and the prose
does not. This is a documentation gate: it proves nothing about GPU behaviour.
"""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from check_command_surface import (  # noqa: E402
    REQUIRED_FAIL_CLOSED_COMMANDS,
    audit_command_surface,
)

# Narrative documents that state command-surface or format-fact status.
DOCUMENTS = ("API.md", "PHYSICAL_DEVICE_REPORTING.md", "README.md", "VALIDATION.md")

LEDGER_PATH = ROOT / "VALIDATION.md"
LEDGER_BEGIN = "<!-- capability-gap-ledger:begin -->"
LEDGER_END = "<!-- capability-gap-ledger:end -->"


def ledger_block():
    text = LEDGER_PATH.read_text()
    start = text.index(LEDGER_BEGIN)
    end = text.index(LEDGER_END)
    if start >= end:
        raise AssertionError("the capability-gap ledger markers are reversed")
    return text[start + len(LEDGER_BEGIN):end]


def ledger_entries():
    entries, current = [], None
    for line in ledger_block().splitlines():
        if line.startswith("- "):
            if current is not None:
                entries.append(current)
            current = line[2:].strip()
        elif current is not None and line.strip():
            current += " " + line.strip()
        else:
            if current is not None:
                entries.append(current)
            current = None
    if current is not None:
        entries.append(current)
    return entries


def d32_capabilities():
    """Capability tokens of the D32 row, read from the deciding table."""
    match = re.search(r"BUFFER\(VK_FORMAT_D32_SFLOAT,\s*([A-Z_ |]+)\)",
                      (ROOT / "src/texture_format.c").read_text())
    if not match:
        raise AssertionError("the D32 row left src/texture_format.c")
    return {token.strip().replace("CAP_", "", 1)
            for token in match.group(1).split("|") if token.strip()}


class TestCommandSurfaceDocumentation(unittest.TestCase):
    def test_public_tree_excludes_internal_milestones_and_session_links(self):
        """Internal work labels and agent-session URLs are not public API facts."""
        roots = [ROOT / name for name in ("src", "native", "include", "examples", "tests")]
        paths = [ROOT / name for name in DOCUMENTS]
        for root in roots:
            if not root.exists():
                continue
            paths.extend(path for path in root.rglob("*")
                         if path.is_file()
                         and path.resolve() != Path(__file__).resolve()
                         and path.suffix in {".c", ".h", ".md", ".py"})
        milestone = re.compile(r"\bM[0-9]+\b")
        for path in paths:
            text = path.read_text(errors="replace")
            relative = path.relative_to(ROOT)
            self.assertIsNone(milestone.search(text),
                              f"{relative} exposes an internal milestone label")
            self.assertNotIn("claude.ai/code/session", text.lower(),
                             f"{relative} exposes an agent-session URL")

    def test_public_header_describes_the_current_subpass_surface(self):
        header = (ROOT / "include/ps5vk/ps5vk.h").read_text()
        self.assertNotIn("exactly one subpass", header)
        self.assertNotIn("Secondary allocation is not supported", header)
        self.assertIn("exact subpass", header)

    def test_native_two_subpass_status_cannot_revert_to_host_only(self):
        """The native witness must move every public status statement with it."""
        api = (ROOT / "API.md").read_text()
        validation = (ROOT / "VALIDATION.md").read_text()
        stale = (
            "exactly one is **executed**",
            "Execution stops at one subpass",
            "no multi-subpass transition executes",
            "multi-subpass execution and sparse binding retain",
        )
        for phrase in stale:
            self.assertNotIn(phrase, api + validation)
        self.assertIn("`vkCmdNextSubpass` — supported in a bounded profile",
                      validation)
        self.assertIn("PS5VK_CONSUMER_TWO_SUBPASS_SUCCESS", validation)

    def test_structural_parity_claims_match_the_parity_gate(self):
        """No document may restate a parity count the audit no longer reports."""
        audit = audit_command_surface(ROOT)
        self.assertTrue(audit["passed"], audit["errors"])
        self.assertEqual(audit["missing_total"], 0)
        total, wired = audit["core_total"], audit["fully_wired_total"]
        pattern = re.compile(r"\b(\d+)/(\d+)\b")
        claimed = {}
        for name in DOCUMENTS:
            for numerator, denominator in pattern.findall((ROOT / name).read_text()):
                if int(denominator) != total:
                    continue
                self.assertEqual(
                    int(numerator), wired,
                    f"{name} claims {numerator}/{denominator} mandatory Vulkan 1.0 "
                    f"commands but the parity gate reports {wired}/{total}")
                claimed.setdefault(f"{wired}/{total}", []).append(name)
        self.assertTrue(
            claimed,
            f"no document states the current structural parity count {wired}/{total}")

    def test_parity_statements_are_labelled_structural(self):
        """A structural count must never read as a conformance claim."""
        audit = audit_command_surface(ROOT)
        figure = f"{audit['fully_wired_total']}/{audit['core_total']}"
        for name in DOCUMENTS:
            for paragraph in (ROOT / name).read_text().split("\n\n"):
                if figure in paragraph:
                    self.assertIn(
                        "structural", paragraph.lower(),
                        f"{name} states {figure} without labelling it structural")

    def test_gap_ledger_names_every_fail_closed_boundary(self):
        """The audit's own boundary set is what the ledger must carry."""
        block = ledger_block()
        for command in sorted(REQUIRED_FAIL_CLOSED_COMMANDS):
            self.assertIn(f"`{command}`", block,
                          f"{command} left the capability gap ledger")

    def test_gap_ledger_is_a_list_of_gaps_not_plans(self):
        entries = ledger_entries()
        self.assertTrue(entries, "the capability gap ledger has no entries")
        for entry in entries:
            lowered = entry.lower()
            self.assertTrue(
                "unsupported" in lowered or "bounded" in lowered,
                f"ledger entry is not classified as a gap: {entry}")


class TestFormatDocumentationFacts(unittest.TestCase):
    def test_d32_table_matches_the_documentation(self):
        """Both D32 rows must describe the roles the capability table enables."""
        self.assertEqual(
            d32_capabilities(), {"DEPTH", "DST", "SRC"},
            "the D32 row changed; reconcile API.md and "
            "PHYSICAL_DEVICE_REPORTING.md and this gate before promoting it")
        for name, prefix in (("API.md", "| `VK_FORMAT_D32_SFLOAT` |"),
                             ("PHYSICAL_DEVICE_REPORTING.md", "| `D32_SFLOAT` |")):
            rows = [line for line in (ROOT / name).read_text().splitlines()
                    if line.startswith(prefix)]
            self.assertEqual(len(rows), 1, f"{name} has no single D32 row")
            lowered = rows[0].lower()
            for token in ("depth", "clear", "transfer", "readback"):
                self.assertIn(
                    token, lowered,
                    f"{name} does not describe the merged D32 {token} role")

    def test_d32_clear_role_is_the_advertised_transfer_destination(self):
        """The documented bit must still be the one the table reports."""
        text = (ROOT / "src/texture_format.c").read_text()
        self.assertRegex(
            text,
            r"PS5VK_FORMAT_CAP_TRANSFER_DST\)\s*\n\s*"
            r"properties\.optimalTilingFeatures \|= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;",
            "the transfer-destination bit is no longer reported from that capability")


if __name__ == "__main__":
    unittest.main()
