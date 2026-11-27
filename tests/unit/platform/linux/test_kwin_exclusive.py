#!/usr/bin/python3
"""Pure guardian regression tests: no desktop, D-Bus, process or monitor access."""
import importlib.util
import json
import os
from pathlib import Path
import shutil
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
            "outputs": exclusive.snapshot_outputs([output()]),
            "windows": [{"id": "saved-window", "output": "DP-1", "geometry": {"x": 150, "y": 100, "width": 600, "height": 400},
                         "maximized": 0, "fullscreen": False, "minimized": False}], "plasma": None}


class ExclusiveTest(unittest.TestCase):
    """Check semantic restore, durable snapshots and degraded recovery."""

    def setUp(self):
        patcher = patch.object(exclusive.Guardian, "cursor_target", return_value=None)
        patcher.start()
        self.addCleanup(patcher.stop)
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
            with patch.object(exclusive, "outputs", return_value=[output()]), patch.object(guardian, "script", return_value=[]) as script, patch.object(exclusive, "atomic_save", side_effect=OSError("disk full")), patch.object(exclusive, "doctor") as doctor:
                with self.assertRaises(OSError):
                    guardian.prepare()
                self.assertIsNone(guardian.state)
                script.assert_called_once()  # Only the read-only window snapshot.
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
            self.assertEqual(observed, [True, True, True, False])

    def test_output_phase_retains_snapshot_and_defers_final_window_configures(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            exclusive.atomic_save(path, recovery_state())
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "unload"), patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor"), patch.object(guardian, "restore_windows") as windows:
                guardian.restore(finish=False)
            windows.assert_not_called()
            self.assertTrue(path.exists())
            self.assertEqual(json.loads(path.read_text())["output_recovery"], "exact")

    def test_completed_windows_are_not_replayed_after_another_window_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            state = recovery_state()
            state["windows"].append(dict(state["windows"][0], id="still-pending"))
            exclusive.atomic_save(path, state)
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "script", return_value={"completed": ["saved-window"], "mismatches": ["still-pending"]}) as attempts, patch.object(exclusive.time, "sleep") as sleep:
                with self.assertRaisesRegex(RuntimeError, "Window restore incomplete"):
                    guardian.restore_windows(guardian.state)
            self.assertEqual(attempts.call_count, 3)
            self.assertEqual(sleep.call_count, 2)
            self.assertEqual(json.loads(path.read_text())["window_recovery"], ["saved-window"])
            for retry in attempts.call_args_list[1:]:
                declaration = retry.args[0].split(";\n", 1)[0]
                self.assertNotIn('"id": "saved-window"', declaration)
                self.assertIn('"id": "still-pending"', declaration)
            restarted = exclusive.Guardian(path, None)
            with patch.object(restarted, "script", return_value=True) as script:
                restarted.restore_windows(restarted.state)
            declaration = script.call_args.args[0].split(";\n", 1)[0]
            self.assertNotIn('"id": "saved-window"', declaration)
            self.assertIn('"id": "still-pending"', declaration)
            self.assertCountEqual(restarted.state["window_recovery"], ["saved-window", "still-pending"])

    def test_pending_window_retries_after_settling_and_finishes_recovery(self):
        """A slow client converges without a new session or replaying completed stages."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            state = recovery_state()
            state["windows"].append(dict(state["windows"][0], id="slow-window"))
            exclusive.atomic_save(path, state)
            guardian = exclusive.Guardian(path, None)
            calls = []

            def script(code):
                calls.append(code.split(";\n", 1)[0])
                if len(calls) == 1:
                    return {"completed": ["saved-window"], "mismatches": ["slow-window"]}
                self.assertEqual(json.loads(path.read_text())["window_recovery"], ["saved-window"])
                return {"completed": ["slow-window"], "mismatches": []}

            with patch.object(guardian, "unload"), patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor") as doctor, patch.object(guardian, "script", side_effect=script), patch.object(exclusive.time, "sleep") as sleep:
                guardian.restore()
            doctor.assert_called_once()
            sleep.assert_called_once_with(0.5)
            self.assertEqual(len(calls), 2)
            self.assertNotIn('"id": "saved-window"', calls[1])
            self.assertIn('"id": "slow-window"', calls[1])
            self.assertFalse(path.exists())
            self.assertIsNone(guardian.state)

    def test_window_fast_path_never_waits_or_retries_completed_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            exclusive.atomic_save(path, recovery_state())
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "script", return_value=True) as script, patch.object(exclusive.time, "sleep") as sleep:
                guardian.restore_windows(guardian.state)
                guardian.restore_windows(guardian.state)
            script.assert_called_once()
            sleep.assert_not_called()

    def test_unsettled_normal_geometry_rolls_back_before_discarding_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            guardian = exclusive.Guardian(path, None)
            with patch.object(exclusive, "outputs", return_value=[output()]), patch.object(guardian, "unload"), patch.object(guardian, "script", side_effect=[[], None, True]):
                with self.assertRaisesRegex(RuntimeError, "Normal window geometry snapshot did not converge"):
                    guardian.prepare()
            self.assertIsNone(guardian.state)
            self.assertFalse(path.exists())

    def test_snapshot_timeout_has_durable_originals_and_rolls_back(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            guardian = exclusive.Guardian(path, None)
            rows = recovery_state()["windows"]
            rows[0]["maximized"] = 3
            calls = []

            def script(code):
                calls.append(code)
                if len(calls) == 1:
                    return rows
                durable = json.loads(path.read_text())
                self.assertTrue(durable["snapshot_pending"])
                self.assertEqual(durable["windows"], rows)
                if len(calls) == 2:
                    raise RuntimeError("KWin script did not acknowledge its operation")
                return True

            with patch.object(exclusive, "outputs", return_value=[output()]), patch.object(guardian, "unload"), patch.object(guardian, "script", side_effect=script), patch.object(exclusive, "doctor") as doctor:
                with self.assertRaisesRegex(RuntimeError, "did not acknowledge"):
                    guardian.prepare()
                doctor.assert_not_called()
            self.assertEqual(len(calls), 3)
            self.assertIsNone(guardian.state)
            self.assertFalse(path.exists())

    def test_interrupted_snapshot_survives_failed_rollback_and_restart(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            guardian = exclusive.Guardian(path, None)
            with patch.object(exclusive, "outputs", return_value=[output()]), patch.object(guardian, "unload"), patch.object(guardian, "script", side_effect=[recovery_state()["windows"], RuntimeError("timeout"), False]):
                with self.assertRaisesRegex(RuntimeError, "snapshot rollback incomplete"):
                    guardian.prepare()
            self.assertTrue(json.loads(path.read_text())["snapshot_pending"])
            restarted = exclusive.Guardian(path, None)
            with self.assertRaisesRegex(RuntimeError, "Previous recovery is incomplete"):
                restarted.prepare()
            with patch.object(restarted, "unload"), patch.object(restarted, "script", return_value=True), patch.object(exclusive, "doctor") as doctor:
                restarted.restore()
                doctor.assert_not_called()
            self.assertIsNone(restarted.state)
            self.assertFalse(path.exists())

    @unittest.skipUnless(shutil.which("node"), "JavaScript replay requires node")
    def test_stateful_windows_keep_normal_rectangle_after_recovery_and_state_exit(self):
        """Replay capture, timeout/unload/close/error rollback and later state exit."""
        with tempfile.TemporaryDirectory() as directory:
            guardian = exclusive.Guardian(Path(directory) / "state.json", None)
            with patch.object(exclusive, "outputs", return_value=[output()]), patch.object(guardian, "script", return_value=[]) as script:
                guardian.prepare()
            snapshot = script.call_args_list[0].args[0]
            prepare = "var rows=result;\n" + script.call_args.args[0].split(";\n", 1)[1]
            guardian.state["windows"] = recovery_state()["windows"]
            with patch.object(guardian, "script", return_value=True) as script:
                guardian.rollback_snapshot()
            rollback = "var SAVED=durableRows;\n" + script.call_args.args[0].split(";\n", 1)[1]
            saved = recovery_state()
            with patch.object(guardian, "script", return_value=True) as script:
                guardian.restore_windows(saved)
            restore = script.call_args.args[0].split(";\n", 1)[1]
            for maximized, fullscreen, scenario in [(m, f, s) for m, f in [(1, False), (2, False), (3, False), (0, True), (3, True)]
                                                    for s in ("restore", "timeout-before-capture", "timeout-after-capture", "closed", "script-error")]:
                with self.subTest(maximized=maximized, fullscreen=fullscreen, scenario=scenario):
                    replay = r"""
const assert=require('node:assert/strict');
const input=JSON.parse(require('node:fs').readFileSync(0,'utf8'));
const normal={x:150,y:100,width:600,height:400}, bounds={x:100,y:0,width:1920,height:1080};
const copy=g=>({...g});
let geometry=copy(normal), normalRestore=copy(normal), fullscreenRestore=copy(normal), result, readError=false;
const target={name:'DP-1'}, timers=[];
const w={internalId:'saved-window',output:target,maximizeMode:0,fullScreen:false,minimized:false,
    desktops:[],activities:[],deleted:false,normalWindow:true,
    get frameGeometry(){if(readError){readError=false;throw Error('capture failure');}return copy(geometry);},set frameGeometry(g){geometry=copy(g);},
    setMaximize(v,h){
        const old=this.maximizeMode, mode=Number(v)|Number(h)*2;
        if(old===mode) return;
        if(!(old&1)){normalRestore.y=geometry.y;normalRestore.height=geometry.height;}
        if(!(old&2)){normalRestore.x=geometry.x;normalRestore.width=geometry.width;}
        this.maximizeMode=mode;
        geometry={x:mode&2?bounds.x:normalRestore.x,y:mode&1?bounds.y:normalRestore.y,
                  width:mode&2?bounds.width:normalRestore.width,height:mode&1?bounds.height:normalRestore.height};
    }};
let full=false;
Object.defineProperty(w,'fullScreen',{get(){return full;},set(value){
    if(value===full) return;
    if(value){fullscreenRestore=copy(geometry);geometry=copy(bounds);}else geometry=copy(fullscreenRestore);
    full=value;
}});
const workspace={stackingOrder:[w],screens:[target],desktops:[],sendClientToScreen(w,s){w.output=s;}};
function QTimer(){this.timeout={connect:f=>{this.callback=f;}};this.start=()=>timers.push(this);this.stop=()=>{this.stopped=true;};}
function tick(){for(let n=0;n<130 && timers.some(t=>!t.stopped);++n) for(const t of timers) if(!t.stopped) t.callback();}
function report(value){result=value;}
function rect(g){return copy(g);}
function windows(){return w.deleted ? [] : [w];}
function screen(){return target;}
w.setMaximize(!!(input.maximized&1),!!(input.maximized&2));w.fullScreen=input.fullscreen;
eval(input.snapshot);
const durableRows=JSON.parse(JSON.stringify(result)), originalBounds=copy(w.frameGeometry);
eval(input.prepare);
if(input.scenario.startsWith('timeout')) {
    assert.equal(w.maximizeMode,0);assert.equal(w.fullScreen,false);
    if(input.scenario==='timeout-after-capture') {timers[0].callback();timers[0].callback();}
    timers.forEach(t=>t.stop()); // Unloading destroys the script's rollback timer.
    eval(input.rollback);tick();
    assert.equal(result,true);
    assert.equal(w.maximizeMode,input.maximized);assert.equal(w.fullScreen,input.fullscreen);
    assert.deepEqual(w.frameGeometry,originalBounds);
    w.fullScreen=false;w.setMaximize(false,false);assert.deepEqual(w.frameGeometry,normal);
} else if(input.scenario==='closed') {
    w.deleted=true;tick();assert.deepEqual(result,[],'closed windows must not prevent convergence');
} else if(input.scenario==='script-error') {
    readError=true;tick();assert.equal(result,null);
    assert.equal(w.maximizeMode,input.maximized);assert.equal(w.fullScreen,input.fullscreen);
    assert.deepEqual(w.frameGeometry,originalBounds);
} else {
tick();
assert.ok(Array.isArray(result),'snapshot must converge');
const SAVED=result;
assert.deepEqual(SAVED[0].normalGeometry,normal);
// Phone routing can alter both the current size and the internal restore size.
w.fullScreen=false;w.setMaximize(false,false);w.frameGeometry={x:0,y:0,width:320,height:700};
w.setMaximize(true,true);
eval(input.restore);tick();
assert.deepEqual(result.mismatches,[]);
assert.equal(w.maximizeMode,input.maximized);assert.equal(w.fullScreen,input.fullscreen);
w.fullScreen=false;w.setMaximize(false,false);
assert.deepEqual(w.frameGeometry,normal,'later state exit must restore original normal rectangle');
}
"""
                    result = subprocess.run(["node", "-e", replay], input=json.dumps({"snapshot": snapshot, "prepare": prepare, "restore": restore,
                                            "rollback": rollback, "scenario": scenario, "maximized": maximized, "fullscreen": fullscreen}),
                                            capture_output=True, text=True, timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)

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

    def test_touch_panel_height_follows_client_scale(self):
        for scale, height in [(1, 44), (1.5, 44), (2, 56), (2.5, 70), (4, 112)]:
            self.assertEqual(exclusive.touch_panel_height(scale), height)

    def test_panel_height_restore_is_durable_and_not_replayed(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            state = recovery_state()
            state["plasma"] = {"panels": [{"id": 7, "output": "DP-1", "height": 44}]}
            exclusive.atomic_save(path, state)
            guardian = exclusive.Guardian(path, None)
            with patch.object(exclusive, "plasma_script", return_value=False):
                with self.assertRaisesRegex(RuntimeError, "Panel thickness restore incomplete"):
                    guardian.restore_panel_heights()
            self.assertEqual(json.loads(path.read_text())["panel_recovery"], "restoring")
            restarted = exclusive.Guardian(path, None)
            with patch.object(exclusive, "plasma_script", return_value=True):
                restarted.restore_panel_heights()
            self.assertEqual(json.loads(path.read_text())["panel_recovery"], "exact")
            restarted = exclusive.Guardian(path, None)
            with patch.object(exclusive, "plasma_script") as script:
                restarted.restore_panel_heights()
            script.assert_not_called()

    def test_panel_height_failure_keeps_snapshot_but_restores_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "state.json"
            state = recovery_state()
            state["plasma"] = {"panels": [{"id": 7, "output": "DP-1", "height": 44}]}
            exclusive.atomic_save(path, state)
            guardian = exclusive.Guardian(path, None)
            with patch.object(guardian, "unload"), patch.object(exclusive, "outputs", return_value=[output()]), patch.object(exclusive, "doctor"), patch.object(exclusive, "plasma_script", return_value=False), patch.object(guardian, "restore_plasma"), patch.object(guardian, "script", return_value=True) as windows:
                with self.assertRaisesRegex(RuntimeError, "Panel thickness restore incomplete"):
                    guardian.restore()
            windows.assert_called_once()
            self.assertEqual(json.loads(path.read_text())["panel_recovery"], "restoring")
            self.assertEqual(json.loads(path.read_text())["output_recovery"], "exact")

    def test_legacy_snapshots_do_not_invent_panel_heights(self):
        guardian = exclusive.Guardian(Path("/not-a-recovery-state-file"), None)
        guardian.state = {"plasma": {"panels": [{"id": 7, "output": "DP-1"}]}}
        with patch.object(exclusive, "plasma_script") as script:
            guardian.restore_panel_heights()
        script.assert_not_called()

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
