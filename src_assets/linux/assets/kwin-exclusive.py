#!/usr/bin/python3
"""Crash-safe KWin mirrored exclusive-output guardian and emergency restore command.

The private stdin/stdout channel is a lifeline owned by Sunshine. EOF restores
immediately. The service must also run `restore` in ExecStopPost because systemd
can kill the guardian together with Sunshine. No display is touched without a
synced snapshot and an acknowledged KWin window snapshot. Physical connectors
remain enabled; this helper sends no disable or DPMS-off commands. Requires python3-dbus,
python3-gi and kscreen-doctor. Window titles and application contents are never
collected. Run as the desktop user, in the same Wayland/D-Bus session.
"""
import argparse
import fcntl
import json
import math
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import threading
import time
import uuid

ROTATIONS = {1: "none", 2: "left", 4: "inverted", 8: "right",
             16: "flipped", 32: "flipped90", 64: "flipped180", 128: "flipped270"}
SCRIPT_NAME = "sunshine-exclusive"


def run(*args):
    """Run one bounded subprocess without a shell."""
    return subprocess.run(args, check=True, capture_output=True, text=True,
                          timeout=5).stdout.strip()


def outputs():
    """Read the compositor's actual configuration, including disabled outputs."""
    return json.loads(run("kscreen-doctor", "-j"))["outputs"]


def doctor(args):
    """Apply a single atomic output transaction (success still needs readback)."""
    if args:
        run("kscreen-doctor", *args)


def mode_of(output):
    """Resolve a current mode to its size and refresh, not an unstable ID."""
    return next(m for m in output["modes"] if m["id"] == output["currentModeId"])


def mode_id(output, saved):
    """Resolve saved mode semantics after a reconnect or compositor restart."""
    return next(m["id"] for m in output["modes"] if m["size"] == saved["size"]
                and abs(m["refreshRate"] - saved["refreshRate"]) < 0.01)


def owned(output):
    """Recognize Sunshine's protocol-owned outputs, never unrelated connectors."""
    return output["name"].startswith("Virtual-Sunshine-")


def replication(pairs):
    """Change only replication through the native protocol helper, never power."""
    if pairs:
        run(str(Path(__file__).with_name("sunshine-kwin-output")),
            *[name + "=" + source for name, source in pairs])


def plasma_script(code):
    """Evaluate a bounded Plasma layout operation without collecting app contents."""
    return json.loads(run("qdbus6", "org.kde.plasmashell", "/PlasmaShell",
                          "org.kde.PlasmaShell.evaluateScript", code))


def touch_panel_height(scale):
    """Estimate touch thickness from client scale: 70 logical pixels at 250%."""
    return max(44, round(28 * scale))


def plasma_snapshot(saved):
    """Persist containment IDs, assignments and panel thickness before changes."""
    if run("qdbus6", "org.freedesktop.DBus", "/org/freedesktop/DBus",
           "org.freedesktop.DBus.NameHasOwner", "org.kde.plasmashell") != "true":
        return None
    return plasma_script("var OUTPUTS=" + json.dumps([o["name"] for o in saved]) + ";" + """
function rows(items,isPanel) { return items.filter(function(c){return c.screen>=0;}).map(function(c){
    var row={id:c.id, output:OUTPUTS.find(function(name){return screenForConnector(name)===c.screen;})};
    if(isPanel) row.height=c.height;
    return row;
}).filter(function(s){return typeof s.output==='string';}); }
print(JSON.stringify({desktops:rows(desktops()),panels:rows(panels(),true)}));
""")


def snapshot_outputs(live):
    """Keep only layout properties we change and can restore exactly."""
    saved = []
    for output in live:
        if owned(output) or not output["connected"]:
            continue
        if not output["enabled"]:
            raise RuntimeError("An original connector is disabled; refusing a policy that could require disabling it on restore")
        row = {key: output[key] for key in ("name", "enabled", "pos", "scale", "rotation", "priority")}
        source = output.get("replicationSource", 0)
        row["replication"] = next((o["name"] for o in live if o["id"] == source), "") if source else ""
        if source and not row["replication"]:
            raise RuntimeError("Original replication source is unavailable")
        if row["enabled"]:
            mode = mode_of(output)
            row["mode"] = {"size": mode["size"], "refreshRate": mode["refreshRate"]}
        saved.append(row)
    if not any(o["enabled"] for o in saved):
        raise RuntimeError("No original enabled output; refusing exclusive mode")
    return saved


def restore_args(saved, live):
    """Build an exact atomic restore using connector names and mode semantics."""
    current = {o["name"]: o for o in live if o["connected"]}
    args = []
    for row in saved:
        output = current[row["name"]]
        prefix = f"output.{output['id']}."
        args += [prefix + "enable"]
        if row["enabled"]:
            args += [prefix + "mode." + mode_id(output, row["mode"]),
                     prefix + f"scale.{row['scale']}",
                     prefix + "rotation." + ROTATIONS[row["rotation"]],
                     prefix + f"position.{row['pos']['x']},{row['pos']['y']}"]
    # Priority changes reorder other enabled outputs. Lowest preference first.
    for output in live:
        if owned(output) and output["enabled"]:
            args.append(f"output.{output['id']}.priority.100")
    for row in sorted(saved, key=lambda o: o["priority"], reverse=True):
        if row["enabled"]:
            args.append(f"output.{current[row['name']]['id']}.priority.{row['priority']}")
    return args


def restored(saved, live):
    """Verify all requested properties; kscreen-doctor may silently reject them."""
    current = {o["name"]: o for o in live if o["connected"]}
    for row in saved:
        output = current.get(row["name"])
        if not output or output["enabled"] != row["enabled"]:
            return False
        if row["enabled"]:
            if any(output[key] != row[key] for key in ("pos", "rotation", "priority")):
                return False
            if abs(output["scale"] - row["scale"]) > 0.001:
                return False
            expected_source = row.get("replication", "")
            actual_source = output.get("replicationSource", 0)
            if actual_source != (current[expected_source]["id"] if expected_source else 0):
                return False
            actual = mode_of(output)
            if actual["size"] != row["mode"]["size"] or abs(actual["refreshRate"] - row["mode"]["refreshRate"]) >= 0.01:
                return False
    return True


def fallback_args(live):
    """Enable every connected original output at a usable preferred mode."""
    args, x, primary = [], 0, None
    for output in live:
        if owned(output) or not output["connected"] or not output["modes"]:
            continue
        preferred = output.get("preferredModes", [])
        mode = next((m for m in output["modes"] if m["id"] in preferred), output["modes"][0])
        prefix = f"output.{output['id']}."
        if primary is None:
            primary = prefix
        args += [prefix + "enable", prefix + "mode." + mode["id"], prefix + "scale.1",
                 prefix + "rotation.none", prefix + f"position.{x},0"]
        x += mode["size"]["width"]
    if not args:
        raise RuntimeError("No connected original output available for emergency restore")
    args.append(primary + "priority.1")
    return args


def atomic_save(path, value):
    """Sync both the private snapshot and its directory before changing outputs."""
    temporary = path.with_suffix(".new")
    with temporary.open("w") as stream:
        os.chmod(temporary, 0o600)
        json.dump(value, stream)
        stream.flush()
        os.fsync(stream.fileno())
    temporary.replace(path)
    sync_directory(path.parent)


def sync_directory(path):
    """Persist recovery file creation, archival and deletion across a crash."""
    fd = os.open(path, os.O_DIRECTORY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def recovery_identity():
    """Scope recovery to this boot and this exact KWin/Wayland session."""
    socket = Path(os.environ["XDG_RUNTIME_DIR"]) / os.environ.get("WAYLAND_DISPLAY", "wayland-0")
    stat = socket.stat()
    return {"boot": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
            "session": run("qdbus6", "org.freedesktop.DBus", "/org/freedesktop/DBus",
                           "org.freedesktop.DBus.GetNameOwner", "org.kde.KWin"),
            "socket": [stat.st_dev, stat.st_ino]}


COMMON_SCRIPT = """
function windows() {
    return workspace.stackingOrder.filter(function(w) {
        return !w.deleted && !w.desktopWindow && !w.dock &&
               (w.normalWindow || w.dialog || w.moveableAcrossScreens);
    });
}
function rect(g) { return {x:g.x, y:g.y, width:g.width, height:g.height}; }
function screen(name) {
    return workspace.screens.find(function(s) { return s.name === name; });
}
function report(value) {
    callDBus(SERVICE, '/Exclusive', 'org.sunshine.Exclusive', 'Report', TOKEN, JSON.stringify(value));
}
"""


class Guardian:
    """Serialize monitor transactions and keep recovery data until verified."""

    def __init__(self, path, reporter):
        self.path, self.reporter = path, reporter
        self.pointer = None
        try:
            self.state = json.loads(path.read_text()) if path.exists() else None
            if self.state is not None and (self.state.get("version", 1) not in (1, 2, 3) or not isinstance(self.state.get("outputs"), list) or not isinstance(self.state.get("windows"), list)):
                raise ValueError("Unknown snapshot format")
        except (ValueError, AttributeError):
            self.state = {"invalid": True}  # Preserve the file; still recover visible monitors.

    def script(self, code, keep=False):
        """Run an acknowledged script; stale script instances are always unloaded."""
        self.unload()
        token = str(uuid.uuid4())
        event = threading.Event()
        self.reporter.pending[token] = [event, None]
        path = self.path.with_suffix(".js")
        path.write_text("var SERVICE=" + json.dumps(self.reporter.name) + ";var TOKEN=" + json.dumps(token) + ";\n" + COMMON_SCRIPT + code)
        os.chmod(path, 0o600)
        try:
            index = run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.loadScript", str(path), SCRIPT_NAME)
            if int(index) < 0:
                raise RuntimeError("KWin refused the exclusive script")
            run("qdbus6", "org.kde.KWin", "/Scripting/Script" + index, "org.kde.kwin.Script.run")
            if not event.wait(5):
                raise RuntimeError("KWin script did not acknowledge its operation")
            return self.reporter.pending[token][1]
        finally:
            self.reporter.pending.pop(token, None)
            if not keep:
                self.unload()

    def unload(self):
        """Remove only our script, leaving desktop and user scripts alone."""
        run("qdbus6", "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting.unloadScript", SCRIPT_NAME)

    def prepare(self):
        """Save originals before even creating the first virtual output."""
        if self.state is not None and (self.state.get("output_recovery") or self.state.get("snapshot_pending")):
            raise RuntimeError("Previous recovery is incomplete; original snapshot retained")
        if self.state is None:
            self.pointer = None
            saved = snapshot_outputs(outputs())
            rows = self.script("""
report(windows().map(function(w) {
    return {id:String(w.internalId), output:w.output.name, geometry:rect(w.frameGeometry),
            maximized:Number(w.maximizeMode), fullscreen:w.fullScreen, minimized:w.minimized,
            desktops:w.desktops.map(function(d){return d.id;}), activities:w.activities};
}));
""")
            if not isinstance(rows, list):
                raise RuntimeError("Window snapshot was not acknowledged")
            state = {"version": 3, "identity": recovery_identity(), "outputs": saved, "windows": rows,
                     "plasma": plasma_snapshot(saved), "snapshot_pending": True,
                     "guardian_pid": os.getpid(), "guardian_start": process_start(os.getpid())}
            atomic_save(self.path, state)
            self.state = state
            try:
                rows = self.script("var rows=" + json.dumps(rows) + ";\n" + """
function alive(s) {return windows().find(function(w){return String(w.internalId)===s.id;});}
function rollback() {
    pending.forEach(function(s){var w=alive(s);if(!w) return;
        w.setMaximize(!!(s.maximized & 1),!!(s.maximized & 2));w.fullScreen=s.fullscreen;
    });
}
// KWin does not expose geometryRestore to scripts. Read it by leaving the
// state temporarily, before any output transaction can alter the restore size.
var pending=rows.filter(function(s){return s.maximized || s.fullscreen;}), phase=0, tries=0, stable={};
if(!pending.length) report(rows.filter(alive));
else {
    var timer=new QTimer();timer.interval=50;timer.timeout.connect(function(){
      try {
        ++tries;
        var ready=true;
        pending.forEach(function(s){
            var w=alive(s);if(!w) return;
            var g=rect(w.frameGeometry), signature=JSON.stringify([g,Number(w.maximizeMode),w.fullScreen]);
            stable[s.id]=stable[s.id] && stable[s.id].signature===signature ?
                {signature:signature,count:stable[s.id].count+1} : {signature:signature,count:1};
            if(stable[s.id].count<2 || (phase===0 ? w.fullScreen || Number(w.maximizeMode) :
                w.fullScreen!==s.fullscreen || Number(w.maximizeMode)!==s.maximized)) ready=false;
        });
        if(!ready && tries<40) return; // Both phases fit the five-second script acknowledgement.
        if(phase===0) {
            pending.forEach(function(s){
                var w=alive(s);if(!w) return;
                if(ready) s.normalGeometry=rect(w.frameGeometry);
            });
            rollback();
            phase=1;tries=0;stable={};return;
        }
        timer.stop();rollback();
        report(ready && pending.filter(alive).every(function(s){return !!s.normalGeometry;}) ? rows.filter(alive) : null);
      } catch(error) {timer.stop();rollback();report(null);}
    });
    try {
        pending.forEach(function(s){var w=alive(s);if(!w) return;
            w.fullScreen=false;w.setMaximize(false,false);
        });
        timer.start();
    } catch(error) {rollback();report(null);}
}
""")
                if not isinstance(rows, list):
                    raise RuntimeError("Normal window geometry snapshot did not converge")
                state["windows"] = rows
                del state["snapshot_pending"]
                atomic_save(self.path, state)
            except Exception:
                # script() unloads even on timeout; its timer cannot be our only
                # rollback. The durable originals also survive a killed guardian.
                self.state = json.loads(self.path.read_text())
                self.restore()
                raise

    def rollback_snapshot(self):
        """Undo only temporary state exits, preserving KWin's normal rectangles."""
        result = self.script("var SAVED=" + json.dumps(self.state["windows"]) + ";\n" + """
var tries=0,stable=0;
function rollback() {
    var current=windows(),ready=true;
    SAVED.forEach(function(s){
        if(!s.maximized && !s.fullscreen) return;
        var w=current.find(function(w){return String(w.internalId)===s.id;});if(!w) return;
        w.setMaximize(!!(s.maximized & 1),!!(s.maximized & 2));w.fullScreen=s.fullscreen;
        var g=rect(w.frameGeometry),b=s.geometry;
        if(Number(w.maximizeMode)!==s.maximized || w.fullScreen!==s.fullscreen ||
           Math.abs(g.x-b.x)>=0.5 || Math.abs(g.y-b.y)>=0.5 ||
           Math.abs(g.width-b.width)>=0.5 || Math.abs(g.height-b.height)>=0.5) ready=false;
    });
    stable=ready ? stable+1 : 0;
    if(stable<2 && ++tries<60) return;
    timer.stop();report(stable>=2);
}
var timer=new QTimer();timer.interval=50;timer.timeout.connect(rollback);timer.start();
""")
        if result is not True:
            raise RuntimeError("Window snapshot rollback incomplete; original snapshot retained")

    def enter(self, name):
        """Make virtual primary; mirror it while every physical connector stays on."""
        live = outputs()
        target = next(o for o in live if o["name"] == name and o["enabled"] and owned(o))
        self.prepare()
        # Acknowledged before changing output policy. Keep routing new windows too.
        result = self.script("var TARGET=" + json.dumps(name) + ";\n" + """
var pending={}, stable={}, attempts={};
windows().forEach(function(w){pending[String(w.internalId)]=true;});
workspace.windowAdded.connect(function(w){pending[String(w.internalId)]=true;});
function move() {
    var target = screen(TARGET);
    if (!target) return;
    windows().forEach(function(w) {
        var id=String(w.internalId);
        if (!pending[id]) return;
        if (w.move || w.resize) {delete pending[id];return;}
        var before=rect(w.frameGeometry), viewport=target.geometry;
        // Chromium can treat a normal-state phone resize as its new preferred
        // size and then ignore normal restore configures. Maximization fits the
        // phone without replacing that preferred normal rectangle.
        if (!w.fullScreen && !Number(w.maximizeMode) && w.resizeable &&
            (before.width>viewport.width || before.height>viewport.height)) {
            w.setMaximize(true,true,before);
        }
        if (w.output !== target) workspace.sendClientToScreen(w, target);
        if (!w.fullScreen && !Number(w.maximizeMode) && w.resizeable) {
            var g=rect(w.frameGeometry), area=target.geometry;
            var width=Math.min(g.width, area.width), height=Math.min(g.height, area.height);
            var x=Math.max(area.x, Math.min(g.x, area.x+area.width-width));
            var y=Math.max(area.y, Math.min(g.y, area.y+area.height-height));
            if (x!==g.x || y!==g.y || width!==g.width || height!==g.height)
                w.frameGeometry={x:x, y:y, width:width, height:height};
        }
        var actual=w.frameGeometry, bounds=target.geometry;
        var fits=w.output===target && actual.x>=bounds.x && actual.y>=bounds.y &&
                 actual.x+actual.width<=bounds.x+bounds.width+1 && actual.y+actual.height<=bounds.y+bounds.height+1;
        stable[id]=fits ? (stable[id] || 0)+1 : 0;
        attempts[id]=(attempts[id] || 0)+1;
        if(stable[id]>=3 || attempts[id]>=30) delete pending[id];
    });
}
var timer = new QTimer(); timer.interval=100; timer.timeout.connect(move); timer.start();
move(); report(true);
""", keep=True)
        if result is not True:
            raise RuntimeError("Window routing was not acknowledged")
        prefix = f"output.{target['id']}."
        doctor([prefix + "enable", prefix + "position.0,0", prefix + "priority.1"])
        originals = self.state["outputs"]
        self.state["policy"] = "mirror"
        atomic_save(self.path, self.state)
        try:
            replication([(o["name"], name) for o in originals])
            readback = {o["name"]: o for o in outputs()}
            if not all(readback[o["name"]].get("replicationSource", 0) == readback[name]["id"] for o in originals):
                raise RuntimeError("KWin did not mirror the virtual source")
        except Exception as error:
            print(f"exclusive: replication unavailable; using window/panel routing: {error}", file=sys.stderr)
            # A timed-out request may have applied. Undo any changed replicas first.
            readback = {o["name"]: o for o in outputs()}
            if any(readback[o["name"]].get("replicationSource", 0) for o in originals):
                replication([(o["name"], o.get("replication", "")) for o in originals])
            # Keep independent physical desktops outside the virtual viewport;
            # overlapping coordinates would otherwise show cropped app windows.
            target = readback[name]
            x = int(math.ceil(mode_of(target)["size"]["width"] / target["scale"])) + 100
            positions = []
            for saved in originals:
                output = readback[saved["name"]]
                positions.append(f"output.{output['id']}.position.{x},0")
                size = mode_of(output)["size"]
                width = size["height"] if output["rotation"] in (2, 8, 32, 128) else size["width"]
                x += int(math.ceil(width / output["scale"])) + 100
            doctor(positions)
            self.state["policy"] = "move"
            atomic_save(self.path, self.state)
        self.move_plasma(name, touch_panel_height(target["scale"]))
        actual = outputs()
        target = next(o for o in actual if o["name"] == name)
        if not target["enabled"] or target["priority"] != 1 or target["pos"] != {"x": 0, "y": 0} or any(not o["enabled"] for o in actual if o["name"] in {s["name"] for s in originals}):
            raise RuntimeError("KWin rejected primary/always-enabled policy; restoring originals")
        print("exclusive: policy=" + self.state["policy"] + "; physical connectors remain enabled", file=sys.stderr)

    def move_plasma(self, target, height=None):
        """Move containments, enlarging only snapshotted panels on a virtual output."""
        saved = self.state.get("plasma")
        if saved is None:
            return
        primary = min(self.state["outputs"], key=lambda o: o["priority"] or 1000)["name"]
        ids = [d["id"] for d in saved["desktops"] if d.get("output") == primary]
        result = plasma_script("var TARGET=" + json.dumps(target) + ";var IDS=" + json.dumps(ids) +
                               ";var HEIGHT=" + json.dumps(height) + ";var PANELS=" + json.dumps(saved["panels"]) + ";" + """
var index=screenForConnector(TARGET);
if(index<0) throw new Error('Virtual Plasma screen not found');
desktops().filter(function(d){return IDS.indexOf(d.id)>=0;}).forEach(function(d){d.screen=index;});
var valid=true;
panels().forEach(function(p){
    p.screen=index;
    var original=PANELS.find(function(s){return s.id===p.id;});
    if(HEIGHT!==null && original && typeof original.height==='number' && original.height>0) {
        var wanted=Math.max(original.height,HEIGHT);
        if(p.height!==wanted) p.height=wanted;
        if(p.height!==wanted) valid=false;
    }
});
print(JSON.stringify(valid));
""")
        if result is not True:
            raise RuntimeError("Plasma did not acknowledge virtual desktop/panels")

    def restore_panel_heights(self):
        """Restore thickness once, independently of connector/window recovery retries."""
        saved = self.state.get("plasma")
        panels = [p for p in (saved or {}).get("panels", []) if isinstance(p.get("height"), (int, float))
                  and math.isfinite(p["height"]) and p["height"] > 0]
        if not panels or self.state.get("panel_recovery") == "exact":
            return
        self.state["panel_recovery"] = "restoring"
        atomic_save(self.path, self.state)
        result = plasma_script("var SAVED=" + json.dumps(panels) + ";" + """
var missing=[];
SAVED.forEach(function(s){
    var p=panelById(s.id);
    if(p && p.height!==s.height) p.height=s.height;
    if(!p || p.height!==s.height) missing.push(s.id);
});
print(JSON.stringify(missing.length===0 ? true : {panels:missing}));
""")
        if result is not True:
            raise RuntimeError("Panel thickness restore incomplete; original snapshot retained: " + json.dumps(result))
        self.state["panel_recovery"] = "exact"
        atomic_save(self.path, self.state)

    def restore_plasma(self, timeout=3):
        """Wait for connectors/containments and verify all saved assignments."""
        saved = self.state.get("plasma")
        if saved is None:
            return
        deadline = time.monotonic() + timeout
        while True:
            result = plasma_script("var SAVED=" + json.dumps(saved) + ";" + """
function restore(find, rows) { rows.forEach(function(s){
    if(!s.output) return;
    var c=find(s.id), index=screenForConnector(s.output);
    if(!c || index<0) return;
    if(c.screen!==index) c.screen=index;
}); }
var desktop=function(id){return desktopById(id);},panel=function(id){return panelById(id);};
restore(desktop,SAVED.desktops);restore(panel,SAVED.panels);
var missing=[];
function verify(find,rows) { rows.forEach(function(s){
    var c=find(s.id),index=s.output ? screenForConnector(s.output) : -1;
    if(!c || index<0 || c.screen!==index)
        missing.push({id:s.id,output:s.output,expected:index,actual:c ? c.screen : null});
}); }
verify(desktop,SAVED.desktops);verify(panel,SAVED.panels);
print(JSON.stringify(missing.length===0 ? true : {mismatches:missing}));
""")
            if result is True:
                return
            if time.monotonic() >= deadline:
                raise RuntimeError("Plasma restore incomplete; connector or containment assignment missing; original snapshot retained: " + json.dumps(result))
            time.sleep(0.1)

    def recover_desktop(self):
        """Bring panels and surviving windows onto available physical outputs."""
        live = outputs()
        available = [o for o in live if o["connected"] and o["enabled"] and not owned(o) and not o.get("replicationSource", 0)]
        if not available:
            raise RuntimeError("No available physical desktop for degraded recovery")
        primary = min(available, key=lambda o: o["priority"] or 1000)["name"]
        self.move_plasma(primary)
        result = self.script("var NAMES=" + json.dumps([o["name"] for o in available]) + ";var PRIMARY=" + json.dumps(primary) + ";var IDS=" + json.dumps([w["id"] for w in self.state["windows"]]) + ";" + """
var tries=0;
function recover() {
    var target=screen(PRIMARY);
    if(target) windows().filter(function(w){return IDS.indexOf(String(w.internalId))>=0;}).forEach(function(w){
        if(NAMES.indexOf(w.output.name)<0) workspace.sendClientToScreen(w,target);
        // sendClientToScreen is asynchronous for Wayland clients. Using
        // w.output here can still clamp back onto the removed virtual output.
        var area=target.geometry,g=rect(w.frameGeometry);
        var width=Math.min(g.width,area.width),height=Math.min(g.height,area.height);
        w.frameGeometry={x:Math.max(area.x,Math.min(g.x,area.x+area.width-width)),
                         y:Math.max(area.y,Math.min(g.y,area.y+area.height-height)),width:width,height:height};
    });
    if(++tries<10)return;
    timer.stop();
    var missing=windows().filter(function(w){return IDS.indexOf(String(w.internalId))>=0 && NAMES.indexOf(w.output.name)<0;})
        .map(function(w){return {id:String(w.internalId),output:w.output.name,geometry:rect(w.frameGeometry)};});
    report(missing.length===0 ? true : {mismatches:missing});
}
var timer=new QTimer();timer.interval=100;timer.timeout.connect(recover);timer.start();
""")
        if result is not True:
            raise RuntimeError("Degraded window recovery incomplete; original snapshot retained: " + json.dumps(result))

    def park_virtual_outputs(self):
        """Expose physical Plasma screens while retained virtual outputs still exist."""
        live = outputs()
        right = 0
        for output in live:
            if owned(output) or not output["connected"] or not output["enabled"]:
                continue
            size = mode_of(output)["size"]
            width = size["height"] if output["rotation"] in (2, 8, 32, 128) else size["width"]
            right = max(right, output["pos"]["x"] + math.ceil(width / output["scale"]))
        args = []
        for output in live:
            if owned(output) and output["enabled"]:
                right += 100
                if output["pos"] != {"x": right, "y": 0}:
                    args.append(f"output.{output['id']}.position.{right},0")
                right += math.ceil(mode_of(output)["size"]["width"] / output["scale"])
        if args:
            doctor(args)

    def restore(self, finish=True):
        """Restore monitors first, then surviving windows; retain state on any failure."""
        if self.state is None:
            return
        saved = self.state
        started = time.monotonic()
        if saved.get("identity") != recovery_identity():
            # Legacy, corrupt or foreign-session data cannot authorize a layout
            # change in the current desktop. Keep it for inspection, off the live path.
            archive = self.path.with_name(self.path.stem + ".stale-" + str(uuid.uuid4()) + ".json")
            self.path.replace(archive)
            sync_directory(self.path.parent)
            self.state = None
            print("exclusive: stale recovery snapshot archived; current desktop unchanged", file=sys.stderr)
            return
        try:
            self.unload()
        except Exception as error:
            print(f"exclusive: cannot unload script: {error}", file=sys.stderr)
        if saved.get("snapshot_pending"):
            self.rollback_snapshot()
            self.path.unlink()
            self.path.with_suffix(".js").unlink(missing_ok=True)
            sync_directory(self.path.parent)
            self.state = None
            return
        if not saved.get("output_recovery"):
            # Persist intent before the transaction too: an interrupted or failed
            # marker write must never cause a later process to replay this layout.
            saved["output_recovery"] = "restoring"
            atomic_save(self.path, saved)
            try:
                self.restore_outputs(saved)
                saved["output_recovery"] = "exact"
            except Exception as error:
                print(f"exclusive exact output restore: {error}", file=sys.stderr)
                try:
                    current = outputs()
                    if any(o.get("replicationSource", 0) for o in current if not owned(o)):
                        replication([(o["name"], "") for o in current if not owned(o)])
                except Exception as clear_error:
                    print(f"exclusive fallback replica clear: {clear_error}", file=sys.stderr)
                doctor(fallback_args(outputs()))
                if not any(o["enabled"] and not owned(o) for o in outputs()):
                    raise RuntimeError("Emergency output enable failed readback")
                saved["output_recovery"] = "degraded"
            atomic_save(self.path, saved)
        panel_error = None
        try:
            self.restore_panel_heights()
        except Exception as error:
            panel_error = error
            print(f"exclusive panel thickness restore: {error}", file=sys.stderr)
        if saved["output_recovery"] != "exact":
            # A crash during the output transaction leaves an uncertain result.
            # Clear only our temporary mirrors; never replay saved modes/positions.
            current = outputs()
            by_id = {o["id"]: o for o in current}
            temporary = [o for o in current if not owned(o) and owned(by_id.get(o.get("replicationSource", 0), {"name": ""}))]
            replication([(o["name"], "") for o in temporary])
            self.park_virtual_outputs()
            self.recover_desktop()
            raise RuntimeError("Degraded desktop recovery completed; original snapshot retained")
        self.park_virtual_outputs()
        print(f"exclusive: physical outputs restored elapsed_ms={(time.monotonic()-started)*1000:.1f}", file=sys.stderr)
        # The native owner releases its parked screencast between these phases.
        # Clients must see the virtual output disappear before their final resize.
        if not finish:
            return
        plasma_error = panel_error
        try:
            self.restore_plasma()
        except Exception as error:
            plasma_error = error
        print(f"exclusive: Plasma restored elapsed_ms={(time.monotonic()-started)*1000:.1f}", file=sys.stderr)
        self.pointer = self.cursor_target()
        self.restore_windows(saved)
        print(f"exclusive: windows restored elapsed_ms={(time.monotonic()-started)*1000:.1f}", file=sys.stderr)
        if plasma_error is not None:
            raise plasma_error
        self.path.unlink()
        self.path.with_suffix(".js").unlink(missing_ok=True)
        sync_directory(self.path.parent)
        self.state = None

    def cursor_target(self):
        """Suggest a primary-output position only when the cursor is off all physical outputs."""
        return self.script("""
var physical=workspace.screens.filter(function(s){return s.name.indexOf('Virtual-Sunshine-')!==0;});
var p=workspace.cursorPos;
if(!physical.length || physical.some(function(s){var g=s.geometry;return p.x>=g.x && p.y>=g.y && p.x<g.x+g.width && p.y<g.y+g.height;})) {
    report(null);
} else {
    var primary=screen(PRIMARY) || physical[0],g=primary.geometry;
    var width=Math.max.apply(null,physical.map(function(s){return s.geometry.x+s.geometry.width;}));
    var height=Math.max.apply(null,physical.map(function(s){return s.geometry.y+s.geometry.height;}));
    report({x:g.x+g.width/2,y:g.y+g.height/2,width:width,height:height});
}
""".replace('PRIMARY', json.dumps(min(self.state["outputs"], key=lambda o: o["priority"] or 1000)["name"])))

    def restore_windows(self, saved):
        """Restore surviving originals in phases, stopping early after verified convergence."""
        completed = saved.get("window_recovery", [])
        rows = [w for w in saved["windows"] if w["id"] not in completed]
        if not rows:
            return
        result = self.script("var SAVED=" + json.dumps(rows) + ";\n" + """
var tries=0, stable={}, done={}, skipped={};
function sameRect(a,b) {
    return Math.abs(a.x-b.x)<0.5 && Math.abs(a.y-b.y)<0.5 &&
           Math.abs(a.width-b.width)<0.5 && Math.abs(a.height-b.height)<0.5;
}
function desktops(w) {return w.desktops.map(function(d){return d.id;}).sort().join(',');}
function matches(w,s) {
    return w.output.name===s.output && sameRect(w.frameGeometry,s.geometry) &&
           w.fullScreen===s.fullscreen && Number(w.maximizeMode)===s.maximized && w.minimized===s.minimized &&
           (!s.desktops || desktops(w)===s.desktops.slice().sort().join(',')) &&
           (!s.activities || w.activities.slice().sort().join(',')===s.activities.slice().sort().join(','));
}
function restore() {
    var byId={};windows().forEach(function(w){byId[String(w.internalId)]=w;});
    SAVED.forEach(function(s){
        if(done[s.id] || skipped[s.id]) return;
        var w=byId[s.id], target=screen(s.output);
        if(!w) {done[s.id]=true;return;} // Closed windows are never reopened.
        if(!target) return;
        // Respect a person dragging/resizing during recovery rather than racing them.
        if(w.move || w.resize) {skipped[s.id]=true;return;}
        if(tries===0) {
            w.fullScreen=false;w.setMaximize(false,false);
            if(s.desktops) {
                var ids=s.desktops;
                var available=workspace.desktops.filter(function(d){return ids.indexOf(d.id)>=0;});
                if(!ids.length || available.length===ids.length) w.desktops=available;
            }
            if(s.activities) w.activities=s.activities;
            w.minimized=s.minimized;
            return;
        }
        if(tries===1) {
            if(w.output!==target) workspace.sendClientToScreen(w,target);
            // Stateful frame bounds are not the normal restore rectangle.
            // Legacy snapshots lack it: retain KWin's restored normal geometry.
            if(s.normalGeometry || (!s.maximized && !s.fullscreen))
                w.frameGeometry=s.normalGeometry || s.geometry;
            return;
        }
        if(tries===2) {
            w.setMaximize(!!(s.maximized & 1),!!(s.maximized & 2));
            w.fullScreen=s.fullscreen;
        }
        if(matches(w,s)) {
            if((stable[s.id]=(stable[s.id] || 0)+1)>=2) done[s.id]=true;
        } else {
            stable[s.id]=0;
            // Geometry writes while maximized/fullscreen race asynchronous state
            // configures. Let KWin compute those rectangles from physical work areas.
            if(!s.fullscreen && !s.maximized && tries%4===0) w.frameGeometry=s.geometry;
        }
    });
    ++tries;
    var pending=SAVED.filter(function(s){return !done[s.id] && !skipped[s.id];});
    if(pending.length && tries<60) return;
    timer.stop();
    report({completed:Object.keys(done).concat(Object.keys(skipped)),mismatches:pending.map(function(s){
        var w=byId[s.id];return {id:s.id,expected:s,actual:w ? {geometry:rect(w.frameGeometry),output:w.output.name,
            maximized:Number(w.maximizeMode),fullscreen:w.fullScreen,minimized:w.minimized} : null};
    })});
}
var timer=new QTimer();timer.interval=50;timer.timeout.connect(restore);timer.start();
""")
        if result is True:  # Also supports test doubles/older script adapters.
            result = {"completed": [w["id"] for w in rows], "mismatches": []}
        if isinstance(result, dict):
            saved["window_recovery"] = list(dict.fromkeys(completed + result.get("completed", [])))
            atomic_save(self.path, saved)
        if not isinstance(result, dict) or result.get("mismatches"):
            raise RuntimeError("Window restore incomplete; original snapshot retained: " + json.dumps(result))

    def restore_outputs(self, saved):
        """Apply and verify the original output layout once per recovery cycle."""
        current = outputs()
        if any(o.get("replicationSource", 0) for o in current if not owned(o)):
            replication([(o["name"], "") for o in current if not owned(o)])
        doctor(restore_args(saved["outputs"], outputs()))
        if any(o.get("replication", "") for o in saved["outputs"]):
            replication([(o["name"], o.get("replication", "")) for o in saved["outputs"]])
        if not restored(saved["outputs"], outputs()):
            raise RuntimeError("Exact output restore failed readback")


def process_start(pid):
    """Use Linux process birth time to avoid signaling a reused PID."""
    return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[19]


def main():
    """Run the guardian or forward a manual restore to its existing instance."""
    os.umask(0o077)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=["watch", "restore"])
    args = parser.parse_args()
    directory = Path(os.environ.get("XDG_STATE_HOME", str(Path.home() / ".local/state"))) / "sunshine"
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    path = directory / "kwin-exclusive.json"
    lock = path.with_suffix(".lock").open("a")
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        if args.action == "restore" and not path.exists():
            return
        if args.action != "restore":
            raise RuntimeError("Exclusive guardian already running")
        state = json.loads(path.read_text())
        pid = state["guardian_pid"]
        if process_start(pid) != state["guardian_start"]:
            raise RuntimeError("Guardian identity changed; snapshot retained")
        os.kill(pid, signal.SIGUSR1)
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            if not path.exists():
                return
            time.sleep(0.1)
        raise RuntimeError("Guardian restore failed; snapshot retained")
    if args.action == "restore" and not path.exists():
        return  # Pre-login service stop needs neither KWin nor Python D-Bus.
    # Imported only for an actual transaction, allowing pure tests without KDE.
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import GLib
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    socket = Path(os.environ.get("XDG_RUNTIME_DIR", "")) / os.environ.get("WAYLAND_DISPLAY", "wayland-0")
    if not socket.is_socket() or not bus.name_has_owner("org.kde.KWin"):
        raise RuntimeError("No desktop KWin/Wayland session; saved recovery state retained")

    class Reporter(dbus.service.Object):
        """A private D-Bus callback for acknowledged KWin scripting operations."""
        def __init__(self):
            self.name = "org.sunshine.Exclusive.p" + str(os.getpid())
            self.owner = dbus.service.BusName(self.name, bus)
            self.pending = {}
            super().__init__(bus, "/Exclusive")

        @dbus.service.method("org.sunshine.Exclusive", in_signature="ss", out_signature="")
        def Report(self, token, payload):
            """Receive only the opaque identities and layout of managed windows."""
            pending = self.pending.get(str(token))
            if pending:
                pending[1] = json.loads(str(payload))
                pending[0].set()

    reporter = Reporter()
    guardian = Guardian(path, reporter)
    force = threading.Event()
    for sig in (signal.SIGUSR1, signal.SIGTERM, signal.SIGINT):
        signal.signal(sig, lambda *_: force.set())
    loop = GLib.MainLoop()
    status = [0]

    def worker():
        """Consume lifeline commands while GLib dispatches script callbacks."""
        try:
            if args.action == "restore":
                guardian.restore()
                return
            if guardian.state is not None:
                guardian.restore()  # Recover an interrupted previous process first.
            buffer = b""
            while not force.is_set():
                if not select.select([sys.stdin], [], [], 0.1)[0]:
                    continue
                data = os.read(sys.stdin.fileno(), 4096)
                if not data:
                    break
                buffer += data
                if len(buffer) > 8192:
                    raise RuntimeError("Invalid guardian command")
                while b"\n" in buffer:
                    line, buffer = buffer.split(b"\n", 1)
                    try:
                        request = json.loads(line)
                        if request["action"] == "prepare":
                            guardian.prepare()
                        elif request["action"] == "enter":
                            guardian.enter(request["output"])
                        elif request["action"] == "restore-outputs":
                            guardian.restore(finish=False)
                        elif request["action"] == "restore":
                            guardian.restore()
                        else:
                            raise RuntimeError("Unknown guardian action")
                        response = {"ok": True, "pointer": guardian.pointer}
                    except Exception as error:
                        print(f"exclusive: {error}", file=sys.stderr)
                        # Initialization must recover immediately. A failed restore
                        # retains progress for ExecStopPost/manual retry, without a
                        # second full retry on the latency-sensitive teardown path.
                        if request.get("action") not in ("restore", "restore-outputs"):
                            try:
                                guardian.restore()
                            except Exception as restore_error:
                                print(f"exclusive restore: {restore_error}", file=sys.stderr)
                        response = {"ok": False, "pointer": guardian.pointer}
                    print(json.dumps(response), flush=True)
        except Exception as error:
            print(f"exclusive guardian: {error}", file=sys.stderr)
            status[0] = 1
        finally:
            try:
                guardian.restore()
            except Exception as error:
                print(f"exclusive final restore: {error}", file=sys.stderr)
                status[0] = 1
            GLib.idle_add(loop.quit)

    thread = threading.Thread(target=worker)
    thread.start()
    loop.run()
    thread.join()
    sys.exit(status[0])


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"exclusive: {error}", file=sys.stderr)
        sys.exit(1)
