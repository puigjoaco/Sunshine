#!/usr/bin/python3
"""Measure Sunshine packet mapping in private KWin at 1x/2.5x, portrait/landscape.

Usage: ~/.local/bin/heavy python3 tests/integration/linux/kwin_mouse_nested.py
Sunshine uses a fake HID backend; its recorded mapping is replayed through the
production shared fake-input source. This intentionally does NOT test uinput/libinput:
uinput is a shared kernel seat and would move the user's live desktop cursor.
"""
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]


def run(*args, cwd=None):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"{args}: {result.stdout}\n{result.stderr}")
    return result.stdout.strip()


def outer():
    with tempfile.TemporaryDirectory(prefix="sunshine-mouse-") as directory:
        env = os.environ.copy()
        for key in ("DISPLAY", "SESSION_MANAGER", "DBUS_SESSION_BUS_ADDRESS", "DBUS_STARTER_ADDRESS", "DBUS_STARTER_BUS_TYPE", "PIPEWIRE_REMOTE"):
            env.pop(key, None)
        for name in ("RUNTIME", "CONFIG", "DATA", "STATE", "CACHE"):
            path = Path(directory) / name.lower()
            path.mkdir(mode=0o700)
            env[f"XDG_{name}_DIR" if name == "RUNTIME" else f"XDG_{name}_HOME"] = str(path)
        env.update(WAYLAND_DISPLAY="mouse-test", QT_QPA_PLATFORM="wayland", QT_QUICK_BACKEND="software",
                   KWIN_WAYLAND_NO_PERMISSION_CHECKS="1", KWIN_COMPOSE="O2", QT_NO_XDG_DESKTOP_PORTAL="1",
                   KDE_DEBUG="1", XDG_CURRENT_DESKTOP="KDE", SUNSHINE_MOUSE_ROOT=directory)
        env["GCOV_PREFIX"] = str(Path(directory) / "gcov")
        process = subprocess.Popen(["dbus-run-session", "--", sys.executable, str(__file__), "--inner"], env=env, start_new_session=True)
        try:
            return process.wait(timeout=150)
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


def inner():
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    import threading

    assert "/sunshine-mouse-" in os.environ["XDG_RUNTIME_DIR"]
    directory = Path(os.environ["SUNSHINE_MOUSE_ROOT"])
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    positions = []

    class Reporter(dbus.service.Object):
        def __init__(self):
            self.name = dbus.service.BusName("org.sunshine.MouseTest", bus)
            super().__init__(bus, "/MouseTest")

        @dbus.service.method("org.sunshine.MouseTest", in_signature="dd", out_signature="")
        def Position(self, x, y):
            positions.append((float(x), float(y)))

    reporter = Reporter()
    loop = GLib.MainLoop()
    dispatch = threading.Thread(target=loop.run)
    dispatch.start()
    children = []
    logs = []
    try:
        protocol = ROOT / "third-party/plasma-wayland-protocols/src/protocols/fake-input.xml"
        run("wayland-scanner", "client-header", str(protocol), str(directory / "fake-input.h"))
        run("wayland-scanner", "private-code", str(protocol), str(directory / "fake-input.c"))
        keystate = ROOT / "third-party/plasma-wayland-protocols/src/protocols/keystate.xml"
        run("wayland-scanner", "client-header", str(keystate), str(directory / "keystate.h"))
        run("wayland-scanner", "private-code", str(keystate), str(directory / "keystate.c"))
        for protocol in ("fake-input", "keystate"):
            run("cc", "-c", str(directory / (protocol + ".c")), "-o", str(directory / (protocol + ".o")))
        run("c++", "-std=c++23", "-o", str(directory / "inject"), "-I" + str(directory), "-I" + str(ROOT),
            str(Path(__file__).with_name("kwin_mouse_inject.cc")), str(ROOT / "src/platform/linux/kwin_text.cpp"),
            str(directory / "fake-input.o"), str(directory / "keystate.o"), "-lwayland-client",
            *run("pkg-config", "--cflags", "--libs", "gio-2.0", "xkbcommon").split())
        run("c++", str(Path(__file__).with_name("kwin_pointer_qt.cc")), "-o", str(directory / "pointer-app"),
            *run("pkg-config", "--cflags", "--libs", "Qt6Widgets").split())
        results = directory / "sunshine.json"
        run(str(ROOT / "cmake-build-kwin/tests/test_sunshine"),
            "--gtest_filter=InputGamepadSessionTest.MapsMousePacketsAcrossOrientationScaleAndDesktopOffset",
            "--gtest_output=json:" + str(results), cwd=directory)
        test = json.loads(results.read_text())["testsuites"][0]["testsuite"][0]
        cases = [json.loads(value) for key, value in test.items() if key.startswith("mouse_case_")]
        assert len(cases) == 8
        log = (directory / "kwin.log").open("w")
        logs.append(log)
        kwin = subprocess.Popen(["kwin_wayland", "--virtual", "--width", "1440", "--height", "3120", "--output-count", "1", "--socket", "mouse-test", "--no-lockscreen", "--no-kactivities"], stdout=log, stderr=log)
        children.append(kwin)
        deadline = time.monotonic() + 15
        while not (Path(os.environ["XDG_RUNTIME_DIR"]) / "mouse-test").is_socket() or not bus.name_has_owner("org.kde.KWin"):
            assert kwin.poll() is None, (directory / "kwin.log").read_text()
            assert time.monotonic() < deadline, "KWin startup timed out"
            time.sleep(.05)

        def outputs():
            return json.loads(run("kscreen-doctor", "-j"))["outputs"]

        injector = subprocess.Popen([str(directory / "inject")], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        children.append(injector)

        def inject(kind, x, y, expected="ok"):
            injector.stdin.write(f"{kind} {x} {y}\n")
            injector.stdin.flush()
            assert select.select([injector.stdout], [], [], 5)[0], "input acknowledgment timeout"
            assert injector.stdout.readline().strip() == expected, "private input failed"

        def cursor():
            index = len(positions)
            script = directory / "cursor.js"
            script.write_text('callDBus("org.sunshine.MouseTest","/MouseTest","org.sunshine.MouseTest","Position",workspace.cursorPos.x,workspace.cursorPos.y);')
            name = "sunshine-mouse-test"
            script_id = run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.loadScript", str(script), name)
            try:
                run("qdbus6", "org.kde.KWin", "/Scripting/Script" + script_id, "org.kde.kwin.Script.run")
                deadline = time.monotonic() + 5
                while len(positions) == index:
                    assert time.monotonic() < deadline, "cursor script timed out"
                    time.sleep(.01)
                return positions[-1]
            finally:
                run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.unloadScript", name)

        for case in cases:
            if case["offset"]:
                continue  # Offset/global-desktop mapping is asserted by the real packet test.
            width, height = case["pixels"]
            scale = case["scale"]
            output = outputs()[0]
            size = {"width": width, "height": height}
            if not any(mode["size"] == size for mode in output["modes"]):
                run("kscreen-doctor", f"output.{output['id']}.addCustomMode.{width}.{height}.60000.reduced")
                output = outputs()[0]
            mode = next(mode["id"] for mode in output["modes"] if mode["size"] == size)
            run("kscreen-doctor", f"output.{output['id']}.mode.{mode}", f"output.{output['id']}.scale.{scale}")
            time.sleep(.1)
            measured = []
            for x, y in case["absolute"]:
                # Recorded Sunshine event coordinates are already logical, including scale.
                inject("a", x, y)
                actual = cursor()
                expected = (min(x, case["desktop"][0] - 1), min(y, case["desktop"][1] - 1))
                assert all(abs(a - b) <= 1 for a, b in zip(actual, expected)), (case, actual, expected)
                measured.append(actual)
            inject("a", case["desktop"][0] / 2, case["desktop"][1] / 2)
            start = cursor()
            inject("r", 100, -100)
            end = cursor()
            delta = tuple(b - a for a, b in zip(start, end))
            assert delta == (100, -100), (case, delta)
            print(json.dumps({"pixels": case["pixels"], "scale": scale, "absolute_cursor": measured, "relative_cursor_delta": delta}), flush=True)
        events = directory / "pointer-events.txt"
        app = subprocess.Popen([str(directory / "pointer-app"), str(events)], stdout=log, stderr=log)
        children.append(app)
        deadline = time.monotonic() + 10
        while not events.exists():
            assert app.poll() is None and time.monotonic() < deadline
            time.sleep(.05)
        time.sleep(.5)
        inject("a", 200, 200)
        for i in range(100): inject("c", 1 + i % 4, 1 + i % 3)
        for i in range(25): inject("s", 1 + i % 4, 1 + i % 3)
        for button in (4, 5):
            inject("b", button, 1); inject("b", button, 0)
        inject("v", 1, 0, "uinput")
        inject("h", -1, 0, "uinput")
        deadline = time.monotonic() + 5
        while len(events.read_text().splitlines()) < 127:
            assert time.monotonic() < deadline, events.read_text()
            time.sleep(.05)
        records = events.read_text().splitlines()
        assert records[:100] == ["button 1 ctrl 1"] * 100, records
        assert records[100:125] == ["wheel ctrl 1"] * 25, records
        assert records[125:] == ["button 8 ctrl 0", "button 16 ctrl 0"], records
        run(str(ROOT / "cmake-build-kwin/tests/test_sunshine"),
            "--gtest_filter=VirtualHidDeviceTest.KwinPlainScrollPreservesValue120AndModifiedScrollUsesTheSharedSource", cwd=directory)
        print("PASS 100 delayed Ctrl-clicks, 25 Ctrl-scrolls, side/extra buttons, plain value120 routing", flush=True)
        print("PASS: real Sunshine packet mapping → production shared source → private KWin cursor; no shared-seat events", flush=True)
        return 0
    finally:
        for child in reversed(children):
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=5)
        loop.quit()
        dispatch.join(timeout=5)
        for log in logs:
            log.close()


if __name__ == "__main__":
    sys.exit(inner() if "--inner" in sys.argv else outer())
