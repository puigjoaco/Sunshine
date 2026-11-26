#!/usr/bin/python3
"""Pure guardian regression tests: no desktop, D-Bus, process or monitor access."""
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[4]
spec = importlib.util.spec_from_file_location("exclusive", ROOT / "src_assets/linux/assets/kwin-exclusive.py")
exclusive = importlib.util.module_from_spec(spec)
spec.loader.exec_module(exclusive)
IDENTITY = {"boot": "current-boot", "session": "current-kwin", "socket": [1, 2]}


def output(name="DP-1", enabled=True, priority=1):
    """A connector with mode IDs that are intentionally separate from geometry."""
    return {"name": name, "id": 1, "enabled": enabled, "connected": True,
            "pos": {"x": 100, "y": 0}, "rotation": 1, "scale": 1.25,
            "priority": priority, "replicationSource": 0, "currentModeId": "7",
            "preferredModes": ["7"],
            "modes": [{"id": "7", "size": {"width": 1920, "height": 1080}, "refreshRate": 59.94}]}


def recovery_state():
    """A saved active recovery from the same fake compositor session."""
    return {"version": 2, "identity": dict(IDENTITY),
            "outputs": exclusive.snapshot_outputs([output()]), "windows": [], "plasma": None}


class ExclusiveTest(unittest.TestCase):
    """Check semantic restore, durable snapshots and degraded recovery."""

    def setUp(self):
        patcher = patch.object(exclusive, "plasma_snapshot", return_value=None)
        patcher.start()
        self.addCleanup(patcher.stop)
        patcher = patch.object(exclusive, "recovery_identity", return_value=IDENTITY)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_snapshot_excludes_owned_outputs_and_no_window_content(self):
        saved = exclusive.snapshot_outputs([output(), output("Virtual-Sunshine-test")])
        self.assertEqual([o["name"] for o in saved], ["DP-1"])
        self.assertNotIn("modes", saved[0])
        self.assertEqual(saved[0]["mode"]["refreshRate"], 59.94)

    def test_refuses_disabled_original_or_no_original(self):
        live = output(enabled=False)
        with self.assertRaises(RuntimeError):
            exclusive.snapshot_outputs([live])
        with self.assertRaises(RuntimeError):
            exclusive.snapshot_outputs([output("Virtual-Sunshine-test")])

    def test_replication_snapshot_uses_connector_names_not_unstable_ids(self):
        first, second = output(), output("HDMI-A-1", priority=2)
        second["id"] = 2
        first["replicationSource"] = 2
        saved = exclusive.snapshot_outputs([first, second])
        self.assertEqual(saved[0]["replication"], "HDMI-A-1")

    def test_restore_resolves_mode_ids_and_keeps_fractional_scale(self):
        live = output()
        saved = exclusive.snapshot_outputs([live])
        live["id"] = 9
        live["modes"][0]["id"] = live["currentModeId"] = "changed"
        args = exclusive.restore_args(saved, [live])
        self.assertIn("output.9.mode.changed", args)
        self.assertIn("output.9.scale.1.25", args)
        self.assertIn("output.9.position.100,0", args)
        self.assertTrue(exclusive.restored(saved, [live]))
        self.assertFalse(any("disable" in arg or "dpms" in arg for arg in args))

    def test_emergency_restore_without_snapshot_needs_no_desktop(self):
        with tempfile.TemporaryDirectory() as directory:
            env = os.environ.copy()
            env.update(XDG_STATE_HOME=directory, XDG_RUNTIME_DIR=directory,
                       WAYLAND_DISPLAY="not-a-session", DBUS_SESSION_BUS_ADDRESS="unix:path=" + directory + "/not-a-bus")
            result = subprocess.run([sys.executable, str(Path(exclusive.__file__)), "restore"], env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_readback_rejects_silent_mode_scale_priority_or_position_failure(self):
        saved = exclusive.snapshot_outputs([output()])
        for key, value in [("scale", 1), ("priority", 2), ("enabled", False),
                           ("pos", {"x": 0, "y": 0}), ("rotation", 2)]:
            changed = output()
            changed[key] = value
            self.assertFalse(exclusive.restored(saved, [changed]), key)
        changed = output()
        changed["modes"][0]["size"] = {"width": 1280, "height": 720}
        self.assertFalse(exclusive.restored(saved, [changed]))

    def test_preferred_fallback_enables_physical_outputs_only(self):
        live = [output(enabled=False), output("Virtual-Sunshine-test")]
        args = exclusive.fallback_args(live)
        self.assertIn("output.1.enable", args)
        self.assertIn("output.1.mode.7", args)
        self.assertEqual(sum(a.endswith(".enable") for a in args), 1)

    def test_persisted_snapshot_has_private_mode_and_is_not_replaced_on_reconnect(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            guardian = exclusive.Guardian(path, None)
            with patch.object(exclusive, "outputs", return_value=[output()]), patch.object(guardian, "script", return_value=[]):
                guardian.prepare()
                original = path.read_bytes()
                guardian.prepare()
            self.assertEqual(path.read_bytes(), original)
            self.assertEqual(path.stat().st_mode & 0o777, 0o600)
            self.assertEqual(json.loads(original)["windows"], [])

    def test_failed_persistence_never_runs_an_output_transaction(self):
        with tempfile.TemporaryDirectory() as directory:
            guardian = exclusive.Guardian(Path(directory) / "state.json", None)
            with patch.object(exclusive, "outputs", return_value=[output()]), patch.object(guardian, "script", return_value=[]), patch.object(exclusive, "atomic_save", side_effect=OSError("disk full")), patch.object(exclusive, "doctor") as doctor:
                with self.assertRaises(OSError):
                    guardian.prepare()
                self.assertIsNone(guardian.state)
                doctor.assert_not_called()

    def test_rejected_exact_restore_falls_back_and_retains_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            exclusive.atomic_save(path, recovery_state())
            guardian = exclusive.Guardian(path, None)
            wrong = output()
            wrong["pos"] = {"x": 0, "y": 0}
            with patch.object(guardian, "unload"), patch.object(guardian, "recover_desktop") as desktop, patch.object(exclusive, "outputs", return_value=[wrong]), patch.object(exclusive, "doctor") as doctor:
                with self.assertRaisesRegex(RuntimeError, "snapshot retained"):
                    guardian.restore()
            self.assertTrue(path.exists())
            self.assertEqual(doctor.call_count, 2)
            self.assertIn("output.1.enable", doctor.call_args.args[0])
            desktop.assert_called_once()
            self.assertEqual(json.loads(path.read_text())["output_recovery"], "degraded")

    def test_corrupt_snapshot_has_no_session_authority_and_is_archived(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            path.write_text("{interrupted disk write")
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "unload") as unload, patch.object(exclusive, "doctor") as doctor:
                guardian.restore()
            self.assertFalse(path.exists())
            self.assertEqual(next(Path(directory).glob("state.stale-*.json")).read_text(), "{interrupted disk write")
            doctor.assert_not_called()
            unload.assert_not_called()

    def test_foreign_boot_or_compositor_never_changes_current_layout(self):
        for key in ("boot", "session"):
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "state.json"
                state = recovery_state()
                state["identity"][key] = "previous"
                exclusive.atomic_save(path, state)
                guardian = exclusive.Guardian(path, None)
                with patch.object(exclusive, "doctor") as doctor, patch.object(guardian, "unload") as unload:
                    guardian.restore()
                doctor.assert_not_called()
                unload.assert_not_called()
                self.assertFalse(path.exists())
                self.assertEqual(len(list(Path(directory).glob("state.stale-*.json"))), 1)

    def test_window_mismatch_never_replays_completed_output_restore(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            exclusive.atomic_save(path, recovery_state())
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "unload"), patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor") as doctor, patch.object(guardian, "script", return_value=False):
                with self.assertRaisesRegex(RuntimeError, "Window restore incomplete"):
                    guardian.restore()
                doctor.assert_called_once()
            self.assertEqual(json.loads(path.read_text())["output_recovery"], "exact")
            restarted = exclusive.Guardian(path, None)
            with patch.object(restarted, "unload"), patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor") as doctor, patch.object(restarted, "script", return_value=True):
                restarted.restore()
            doctor.assert_not_called()
            self.assertFalse(path.exists())

    def test_interrupted_output_transaction_is_not_replayed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            state = recovery_state()
            state["output_recovery"] = "restoring"
            exclusive.atomic_save(path, state)
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "unload"), patch.object(guardian, "recover_desktop") as desktop, patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor") as doctor:
                with self.assertRaisesRegex(RuntimeError, "snapshot retained"):
                    guardian.restore()
            doctor.assert_not_called()
            desktop.assert_called_once()

    def test_successful_deletion_syncs_directory_after_unlink(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            exclusive.atomic_save(path, recovery_state())
            guardian = exclusive.Guardian(path, None)
            observed = []
            with patch.object(guardian, "unload"), patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor"), patch.object(guardian, "script", return_value=True), patch.object(exclusive, "sync_directory", side_effect=lambda _: observed.append(path.exists())):
                guardian.restore()
            self.assertEqual(observed, [True, True, False])

    def test_plasma_waits_for_connectors_and_verifies_assignments(self):
        guardian = exclusive.Guardian(Path("/not-a-recovery-state-file"), None)
        guardian.state = {"plasma": {"desktops": [{"id": 1, "output": "DP-1"}], "panels": []}}
        with patch.object(exclusive, "plasma_script", side_effect=[False, True]) as script, patch.object(exclusive.time, "sleep"):
            guardian.restore_plasma()
        self.assertEqual(script.call_count, 2)
        with patch.object(exclusive, "plasma_script", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "Plasma restore incomplete"):
                guardian.restore_plasma(timeout=0)

    def test_plasma_failure_preserves_state_and_still_restores_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            exclusive.atomic_save(path, recovery_state())
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "unload"), patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor"), patch.object(guardian, "restore_plasma", side_effect=RuntimeError("Plasma mismatch")), patch.object(guardian, "script", return_value=True) as windows:
                with self.assertRaisesRegex(RuntimeError, "Plasma mismatch"):
                    guardian.restore()
            windows.assert_called_once()
            self.assertEqual(json.loads(path.read_text())["output_recovery"], "exact")

    def test_degraded_recovery_targets_an_available_physical_desktop(self):
        with tempfile.TemporaryDirectory() as directory:
            guardian = exclusive.Guardian(Path(directory) / "state.json", None)
            guardian.state = recovery_state()
            physical, virtual = output(), output("Virtual-Sunshine-test")
            physical["priority"], virtual["priority"] = 2, 1
            virtual["id"] = 2
            with patch.object(exclusive, "outputs", return_value=[virtual, physical]), patch.object(guardian, "move_plasma") as plasma, patch.object(guardian, "script", return_value=True):
                guardian.recover_desktop()
            plasma.assert_called_once_with("DP-1")

    def test_recovery_parking_moves_only_owned_virtual_outputs(self):
        with tempfile.TemporaryDirectory() as directory:
            guardian = exclusive.Guardian(Path(directory) / "state.json", None)
            physical, virtual = output(), output("Virtual-Sunshine-test")
            virtual["id"] = 2
            with patch.object(exclusive, "outputs", return_value=[physical, virtual]), patch.object(exclusive, "doctor") as doctor:
                guardian.park_virtual_outputs()
            self.assertEqual(doctor.call_args.args[0], ["output.2.position.1736,0"])


if __name__ == "__main__":
    unittest.main()
