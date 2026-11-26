#!/usr/bin/python3
"""Exercise the real guardian inside headless KWin; never inherits desktop sockets.

Usage: ~/.local/bin/heavy python3 tests/integration/linux/kwin_exclusive_nested.py
All configuration, D-Bus, Wayland, PipeWire, windows and process groups are private.
"""
import importlib.util
import json
import os
from pathlib import Path
import signal
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
HELPER = ROOT / "src_assets/linux/assets/kwin-exclusive.py"


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=15).stdout.strip()


def wait(predicate, description, timeout=10):
    end = time.monotonic() + timeout
    last = None
    while time.monotonic() < end:
        try:
            result = predicate()
            if result:
                return result
        except Exception as error:
            last = error
        time.sleep(.05)
    raise RuntimeError(f"Timeout: {description}: {last}")


def outer():
    """Re-exec in a private bus and remove only this test's process group/state."""
    with tempfile.TemporaryDirectory(prefix="sunshine-exclusive-") as directory:
        env = os.environ.copy()
        for key in ("DISPLAY", "SESSION_MANAGER", "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "PIPEWIRE_REMOTE"):
            env.pop(key, None)
        for name in ("RUNTIME", "CONFIG", "DATA", "STATE", "CACHE"):
            path = Path(directory) / name.lower()
            path.mkdir(mode=0o700)
            env[f"XDG_{name}_DIR" if name == "RUNTIME" else f"XDG_{name}_HOME"] = str(path)
        env.update(WAYLAND_DISPLAY="exclusive-test", QT_QPA_PLATFORM="wayland", QT_QUICK_BACKEND="software",
                   KWIN_WAYLAND_NO_PERMISSION_CHECKS="1", KWIN_COMPOSE="O2", QT_NO_XDG_DESKTOP_PORTAL="1", KDE_DEBUG="1", XDG_CURRENT_DESKTOP="KDE", EXCLUSIVE_TEST_ROOT=directory)
        env["GCOV_PREFIX"] = str(Path(directory) / "gcov")
        process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(__file__), "--inner"], env=env, start_new_session=True)
        try:
            status = process.wait(timeout=180)
            print("Nested session exit:", status, flush=True)
            return status
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


def inner():
    """Use actual KScreen, scripting, Wayland windows and EOF crash recovery."""
    assert "/sunshine-exclusive-" in os.environ["XDG_RUNTIME_DIR"]
    directory = Path(os.environ["EXCLUSIVE_TEST_ROOT"])
    global HELPER
    source_helper = HELPER
    HELPER = directory / "kwin-exclusive.py"
    shutil.copy2(source_helper, HELPER)
    shutil.copy2(ROOT / "cmake-build-kwin/sunshine-kwin-output", directory / "sunshine-kwin-output")
    children = []
    groups = set()
    logs = []

    def start(name, args):
        log = (directory / (name + ".log")).open("w")
        logs.append(log)
        process = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=log, stderr=log, text=True, start_new_session=True)
        groups.add(process.pid)
        children.append(process)
        return process

    spec = importlib.util.spec_from_file_location("exclusive", HELPER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    def state():
        socket = Path(os.environ["XDG_RUNTIME_DIR"]) / "exclusive-test"
        if not socket.is_socket() or not bus.name_has_owner("org.kde.KWin"):
            raise RuntimeError("Private KWin is not ready; refusing to launch Qt display tools")
        return json.loads(run("kscreen-doctor", "-j"))["outputs"]

    def guardian():
        log = (directory / "guardian.log").open("a")
        logs.append(log)
        p = subprocess.Popen([sys.executable, str(HELPER), "watch"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log, text=True)
        children.append(p)
        return p

    def plasma():
        return json.loads(run("qdbus6", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript",
                              "print(JSON.stringify({panels:panels().map(function(p){return p.screen;}).filter(function(s){return s>=0;}).sort(),desktops:desktops().map(function(d){return d.screen;}).filter(function(s){return s>=0;}).sort()}));"))

    def plasma_exclusive():
        actual = plasma()
        # Inactive containments have screen=-1 and are excluded by plasma().
        return len(actual["desktops"]) == 1 and actual["panels"] and all(s == actual["desktops"][0] for s in actual["panels"])

    def mirrors(name):
        actual = state()
        target = next(o for o in actual if o["name"] == name)
        originals = [o for o in actual if not module.owned(o)]
        assert all(o["enabled"] for o in actual), "A physical connector was disabled"
        by_name = {o["name"]: o for o in saved}
        for output in originals:
            assert module.mode_of(output)["size"] == by_name[output["name"]]["mode"]["size"]
            assert output["scale"] == by_name[output["name"]]["scale"]
        return target["priority"] == 1 and all(o.get("replicationSource", 0) == target["id"] for o in originals)

    def command(p, request, ok=True):
        import select
        p.stdin.write(json.dumps(request) + "\n")
        p.stdin.flush()
        assert select.select([p.stdout], [], [], 35)[0], "guardian response timeout"
        line = p.stdout.readline()
        assert line, (directory / "guardian.log").read_text()
        response = json.loads(line)
        assert response["ok"] is ok, (directory / "guardian.log").read_text()

    try:
        run("cc", "-o", str(directory / "owner"), str(Path(__file__).with_name("kwin_virtual_owner.c")),
            "-I" + str(ROOT / "cmake-build-kwin/generated-src"), str(ROOT / "cmake-build-kwin/generated-src/zkde-screencast-unstable-v1.c"), "-lwayland-client")
        start("pipewire", ["pipewire"])
        start("kwin", ["kwin_wayland", "--virtual", "--width", "1280", "--height", "900", "--output-count", "2", "--socket", "exclusive-test", "--no-lockscreen", "--no-kactivities"])
        import dbus
        bus = dbus.SessionBus()
        wait(lambda: (Path(os.environ["XDG_RUNTIME_DIR"]) / "exclusive-test").is_socket() and bus.name_has_owner("org.kde.KWin"), "private Wayland socket and KWin D-Bus ready")
        wait(lambda: len(state()) == 2, "two private outputs")
        original = state()
        # Unequal layout, fractional scale and reversed primary exercise exact restore.
        run("kscreen-doctor", f"output.{original[0]['id']}.position.0,100", f"output.{original[1]['id']}.position.1280,0", f"output.{original[1]['id']}.scale.1.25", f"output.{original[1]['id']}.priority.1")
        original = state()
        saved = module.snapshot_outputs(original)
        start("plasmashell", ["plasmashell", "--no-respawn"])
        wait(lambda: bus.name_has_owner("org.kde.plasmashell"), "private Plasma shell")
        run("qdbus6", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript",
            "if(panels().length===0){var p=new Panel; p.location='bottom'; p.addWidget('org.kde.plasma.icontasks');}")
        plasma_before = wait(lambda: plasma() if len(plasma()["desktops"]) == 2 and plasma()["panels"] else None, "two desktops and panel")
        app = directory / "windows.py"
        app.write_text('''from PyQt6.QtWidgets import QApplication, QWidget\napp=QApplication([])\nwindows=[]\nfor i in range(4):\n w=QWidget();w.setWindowTitle("exclusive-test-"+str(i));w.resize(500,350);w.show();windows.append(w)\napp.exec()\n''')
        start("windows", [sys.executable, str(app)])
        time.sleep(.7)
        # Give each test window a distinct original connector and geometry.
        placement = directory / "place.js"
        placement.write_text("""
workspace.stackingOrder.filter(function(w) { return w.caption.indexOf('exclusive-test-')===0; }).forEach(function(w) {
    var i=Number(w.caption.slice(-1));
    workspace.sendClientToScreen(w, workspace.screens[i%2]);
    w.frameGeometry={x:i%2 ? 1400 : 150, y:i%2 ? 120 : 220, width:500, height:350};
    if(i===2) w.setMaximize(true,true);
    if(i===3) w.fullScreen=true;
});
""")
        index = run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.loadScript", str(placement), "exclusive-test-placement")
        run("qdbus6", "org.kde.KWin", "/Scripting/Script" + index, "org.kde.kwin.Script.run")
        time.sleep(.2)
        run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.unloadScript", "exclusive-test-placement")
        first = start("landscape", [str(directory / "owner"), "Sunshine-test-1", "3120", "1440"])
        wait(lambda: any(o["name"] == "Virtual-Sunshine-test-1" for o in state()), "landscape owned output")
        p = guardian()
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-1"})
        assert mirrors("Virtual-Sunshine-test-1")
        wait(plasma_exclusive, "Plasma desktop and panels on sole virtual output")
        snapshot = Path(os.environ["XDG_STATE_HOME"]) / "sunshine/kwin-exclusive.json"
        snapshot_before = snapshot.read_text()
        assert len(json.loads(snapshot_before)["windows"]) >= 4
        assert len({w["output"] for w in json.loads(snapshot_before)["windows"]}) >= 2
        print("PASS: virtual primary, physical connectors remain enabled and mirror its UUID", flush=True)
        print("PASS: Plasma desktop and panels follow sole virtual output", flush=True)
        # Reuse the output as the C++ lease does: hotplug can restore KWin's layout.
        target = next(o for o in state() if o["name"] == "Virtual-Sunshine-test-1")
        run("kscreen-doctor", f"output.{target['id']}.addCustomMode.1440.3120.60000.reduced")
        target = next(o for o in state() if o["name"] == "Virtual-Sunshine-test-1")
        portrait_mode = next(m["id"] for m in target["modes"] if m["size"] == {"width": 1440, "height": 3120})
        run("kscreen-doctor", f"output.{target['id']}.mode.{portrait_mode}")
        assert mirrors("Virtual-Sunshine-test-1")
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-1"})
        assert snapshot.read_text() == snapshot_before, "reconnect overwrote original snapshot"
        assert mirrors("Virtual-Sunshine-test-1")
        target = next(o for o in state() if o["name"] == "Virtual-Sunshine-test-1")
        assert module.mode_of(target)["size"] == {"width": 1440, "height": 3120}
        print("PASS: landscape→portrait resize keeps physical mirrors enabled and original snapshot", flush=True)
        command(p, {"action": "restore"})
        assert module.restored(saved, state())
        assert not snapshot.exists()
        first.stdin.close()
        first.wait(timeout=5)
        wait(lambda: plasma() == plasma_before, "original Plasma desktop/panel placement")
        print("PASS: exact modes/positions/scale/priority and original window geometry restored", flush=True)
        second = start("portrait", [str(directory / "owner"), "Sunshine-test-2", "1440", "3120"])
        wait(lambda: any(o["name"] == "Virtual-Sunshine-test-2" for o in state()), "portrait owned output")
        portrait = next(o for o in state() if o["name"] == "Virtual-Sunshine-test-2")
        assert module.mode_of(portrait)["size"] == {"width": 1440, "height": 3120}
        print("PASS: portrait 1440×3120 protocol-owned output", flush=True)
        # A closed parent lifeline simulates Sunshine dying without teardown.
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-2"})
        p.stdin.close()
        assert p.wait(timeout=25) == 0, (directory / "guardian.log").read_text()
        assert module.restored(saved, state()) and not snapshot.exists()
        print("PASS: parent crash/EOF restores immediately", flush=True)
        # Guardian killed with the service: ExecStopPost restores persisted data.
        p = guardian()
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-2"})
        p.kill()
        p.wait(timeout=5)
        run(sys.executable, str(HELPER), "restore")
        assert module.restored(saved, state()) and not snapshot.exists()
        print("PASS: killed guardian recovered from disk by service/emergency restore", flush=True)
        # Real C++ lease: retains ownership, cancels on reconnect and expires after disconnect.
        p.stdin.close()
        p.wait(timeout=10)
        env = os.environ.copy()
        env["SUNSHINE_TEST_KWIN_EXCLUSIVE_OUTPUT"] = "Virtual-Sunshine-test-2"
        env["SUNSHINE_SKIP_CONFIG_MIGRATION"] = "1"
        result = subprocess.run([str(ROOT / "cmake-build-kwin/tests/test_sunshine"),
                                 "--gtest_filter=KwinExclusiveNestedTest.*"],
                                env=env, cwd=directory, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        assert module.restored(saved, state()) and not snapshot.exists()
        assert result.stdout.count("[       OK ]") == 3, result.stdout
        print("PASS: C++ grace/resume/expiration, killed-guardian recovery, immediate exit restore (3 tests)", flush=True)
        # Live manual restore while the helper still owns the lock.
        p = guardian()
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-2"})
        run(sys.executable, str(HELPER), "restore")
        assert module.restored(saved, state()) and not snapshot.exists()
        print("PASS: manual emergency restore reaches active guardian", flush=True)
        p.wait(timeout=10)
        # Simulate an older compositor/missing replication helper. Nothing powers
        # off: independent physical desktops are moved outside the stream viewport.
        (directory / "sunshine-kwin-output").rename(directory / "sunshine-kwin-output.unavailable")
        p = guardian()
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-2"})
        fallback = json.loads(snapshot.read_text())
        assert fallback["policy"] == "move"
        actual = state()
        assert all(o["enabled"] for o in actual)
        assert all(o.get("replicationSource", 0) == 0 for o in actual)
        assert all(o["pos"]["x"] >= 1540 for o in actual if not module.owned(o))
        index = json.loads(run("qdbus6", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript",
                               "print(JSON.stringify(screenForConnector('Virtual-Sunshine-test-2')));"))
        assert plasma()["panels"] and all(s == index for s in plasma()["panels"])
        assert len(plasma()["desktops"]) == 3
        def windows_in_virtual_viewport():
            for row in fallback["windows"]:
                info = run("qdbus6", "org.kde.KWin", "/KWin", "org.kde.KWin.getWindowInfo", row["id"])
                values = dict(line.split(": ", 1) for line in info.splitlines() if ": " in line)
                if "x" in values:
                    if float(values["x"]) < 0 or float(values["y"]) < 0 or float(values["x"]) + float(values["width"]) > 1442 or float(values["y"]) + float(values["height"]) > 3122:
                        return False
            return True
        wait(windows_in_virtual_viewport, "all saved windows inside virtual viewport")
        print("PASS: unavailable-replication fallback keeps physical desktops on, panels/windows on virtual; physical wallpaper desktops remain", flush=True)
        command(p, {"action": "restore"})
        assert module.restored(saved, state()) and not snapshot.exists()
        p.stdin.close()
        assert p.wait(timeout=10) == 0
        (directory / "sunshine-kwin-output.unavailable").rename(directory / "sunshine-kwin-output")

        # Missing Plasma containments must keep recovery state after restoring
        # outputs. A later guardian must preserve a newly chosen monitor layout.
        p = guardian()
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-2"})
        p.kill()
        p.wait(timeout=5)
        pending = json.loads(snapshot.read_text())
        pending["plasma"]["panels"].append({"id": 99999999, "output": saved[0]["name"]})
        module.atomic_save(snapshot, pending)
        def restore_cli():
            return subprocess.run([sys.executable, str(HELPER), "restore"], capture_output=True, text=True, timeout=25)
        result = restore_cli()
        assert result.returncode != 0 and "Plasma restore incomplete" in result.stderr, result.stderr
        assert json.loads(snapshot.read_text())["output_recovery"] == "exact"
        assert module.restored(saved, state())
        current = {o["name"]: o for o in state()}
        run("kscreen-doctor", f"output.{current[saved[0]['name']]['id']}.position.50,75",
            f"output.{current[saved[1]['name']]['id']}.position.1600,25")
        newer = module.snapshot_outputs(state())
        result = restore_cli()
        assert result.returncode != 0 and snapshot.exists()
        assert module.restored(newer, state()), "Completed recovery overwrote a newer layout"
        print("PASS: missing Plasma assignment retains state; repeated recovery preserves a newer output layout", flush=True)
        pending = json.loads(snapshot.read_text())
        pending["identity"]["boot"] = "previous-test-boot"
        module.atomic_save(snapshot, pending)
        assert restore_cli().returncode == 0
        assert not snapshot.exists() and list(snapshot.parent.glob("kwin-exclusive.stale-*.json"))
        assert module.restored(newer, state())
        print("PASS: foreign-boot snapshot archived without changing the current layout", flush=True)
        module.doctor(module.restore_args(saved, state()))  # Reset only the private test fixture.

        # Remove a task-owned original connector while mirrored. Degraded
        # recovery must bring the desktop back to the remaining physical outputs.
        unplug = start("unplugged", [str(directory / "owner"), "Desk-recovery-unplug", "1280", "900"])
        unplugged_name = "Virtual-Desk-recovery-unplug"
        extra = wait(lambda: next((o for o in state() if o["name"] == unplugged_name), None), "removable original connector")
        run("kscreen-doctor", f"output.{extra['id']}.priority.1")
        p = guardian()
        command(p, {"action": "enter", "output": "Virtual-Sunshine-test-2"})
        unplug.stdin.close()
        unplug.wait(timeout=5)
        wait(lambda: all(o["name"] != unplugged_name for o in state()), "original connector unplugged")
        log_offset = (directory / "guardian.log").stat().st_size
        command(p, {"action": "restore"}, ok=False)
        assert json.loads(snapshot.read_text())["output_recovery"] == "degraded"
        assert "Degraded desktop recovery completed" in (directory / "guardian.log").read_text()[log_offset:]
        physical = [o for o in state() if not module.owned(o)]
        assert physical and all(o["enabled"] and not o.get("replicationSource", 0) for o in physical)
        indices = json.loads(run("qdbus6", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript",
                                 "print(JSON.stringify(" + json.dumps([o["name"] for o in physical]) + ".map(function(n){return screenForConnector(n);})));"))
        assert plasma()["panels"] and all(s in indices for s in plasma()["panels"])
        print("PASS: unplugged original triggers preferred physical modes and verified panel/window recovery; original snapshot retained", flush=True)
        p.stdin.close()
        assert p.wait(timeout=15) != 0  # Deliberately retained degraded state.
        second.stdin.close()
        second.wait(timeout=5)
        # Retain the degraded fixture for inspection but remove it from the live
        # recovery path before starting a fresh, real capture lifecycle.
        snapshot.rename(snapshot.with_suffix(".degraded-fixture.json"))
        # Explicit stream links need a session policy manager. Its policy-only
        # profile uses this private PipeWire socket and loads no hardware monitors
        # (ALSA, Bluetooth or cameras), leaving the user's media services alone.
        start("wireplumber-policy", ["wireplumber", "--profile", "policy"])
        wait(lambda: any(str(o.get("info", {}).get("props", {}).get("application.name", "")).startswith("WirePlumber")
                         for o in json.loads(run("pw-dump"))), "private PipeWire policy manager")
        env["SUNSHINE_TEST_KWIN_CAPTURE_RECONNECT"] = "1"
        result = subprocess.run([str(ROOT / "cmake-build-kwin/tests/test_sunshine"),
                                 "--gtest_filter=KwinCaptureReconnectNestedTest.*"],
                                env=env, cwd=directory, capture_output=True, text=True, timeout=30)
        assert result.returncode == 0 and "[       OK ]" in result.stdout, result.stdout + result.stderr
        assert not any(module.owned(o) for o in state())
        print("PASS: real kwin_display landscape capture, retained screencast portrait reconnect, portrait PipeWire frames and final removal", flush=True)
    except Exception:
        for path in directory.glob("*.log"):
            print(f"--- {path.name} ---\n{path.read_text()[-10000:]}", file=sys.stderr)
        raise
    finally:
        for process in reversed(children):
            if process.pid in groups:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
            if process.poll() is None:
                if process.pid not in groups:
                    process.terminate()
                try:
                    process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    if process.pid in groups:
                        os.killpg(process.pid, signal.SIGKILL)
                    else:
                        process.kill()
                    process.wait(timeout=5)
        for log in logs:
            log.close()
    return 0


if __name__ == "__main__":
    sys.exit(inner() if "--inner" in sys.argv else outer())
