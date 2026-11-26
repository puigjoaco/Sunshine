#!/usr/bin/python3
"""Exercise production KWin keysym injection in private Qt and GTK apps, without a shared input seat."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[3]
EXPECTED = 'año ¿qué? ¡sí! €'


def run(*args, **kwargs):
    result = subprocess.run(args, capture_output=True, text=True, timeout=30, **kwargs)
    if result.returncode:
        raise RuntimeError(f'{args}: {result.stdout}\n{result.stderr}')
    return result.stdout.strip()


def outer():
    with tempfile.TemporaryDirectory(prefix='sunshine-text-') as directory:
        env = os.environ.copy()
        for key in ('DISPLAY', 'SESSION_MANAGER', 'DBUS_SESSION_BUS_ADDRESS', 'DBUS_STARTER_ADDRESS', 'DBUS_STARTER_BUS_TYPE'):
            env.pop(key, None)
        for name in ('RUNTIME', 'CONFIG', 'DATA', 'STATE', 'CACHE'):
            path = Path(directory) / name.lower()
            path.mkdir(mode=0o700)
            env[f'XDG_{name}_DIR' if name == 'RUNTIME' else f'XDG_{name}_HOME'] = str(path)
        env.update(WAYLAND_DISPLAY='text-test', QT_QPA_PLATFORM='wayland', QT_QUICK_BACKEND='software',
                   KWIN_WAYLAND_NO_PERMISSION_CHECKS='1', KWIN_COMPOSE='O2', QT_NO_XDG_DESKTOP_PORTAL='1',
                   XDG_CURRENT_DESKTOP='KDE', SUNSHINE_TEXT_ROOT=directory, GDK_BACKEND='wayland')
        process = subprocess.Popen(['dbus-run-session', '--', sys.executable, str(__file__), '--inner'], env=env, start_new_session=True)
        try:
            return process.wait(timeout=120)
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass


def inner():
    directory = Path(os.environ['SUNSHINE_TEXT_ROOT'])
    assert '/sunshine-text-' in os.environ['XDG_RUNTIME_DIR']
    protocol = ROOT / 'third-party/plasma-wayland-protocols/src/protocols/fake-input.xml'
    run('wayland-scanner', 'client-header', str(protocol), str(directory / 'fake-input.h'))
    run('wayland-scanner', 'private-code', str(protocol), str(directory / 'fake-input.c'))
    run('cc', '-c', str(directory / 'fake-input.c'), '-o', str(directory / 'fake-input.o'), '-I' + str(directory))
    keystate = ROOT / 'third-party/plasma-wayland-protocols/src/protocols/keystate.xml'
    run('wayland-scanner', 'client-header', str(keystate), str(directory / 'keystate.h'))
    run('wayland-scanner', 'private-code', str(keystate), str(directory / 'keystate.c'))
    run('cc', '-c', str(directory / 'keystate.c'), '-o', str(directory / 'keystate.o'), '-I' + str(directory))
    gio_flags = run('pkg-config', '--cflags', '--libs', 'gio-2.0', 'xkbcommon').split()
    run('c++', '-std=c++23', '-I' + str(ROOT), '-I' + str(directory),
        str(Path(__file__).with_name('kwin_text_inject.cc')), str(ROOT / 'src/platform/linux/kwin_text.cpp'),
        str(directory / 'fake-input.o'), str(directory / 'keystate.o'), '-lwayland-client', *gio_flags, '-o', str(directory / 'inject'))
    run('c++', '-std=c++23', '-I' + str(ROOT), '-I' + str(directory), str(Path(__file__).with_name('kwin_text_state.cc')), str(ROOT / 'src/platform/linux/kwin_text.cpp'),
        str(directory / 'fake-input.o'), str(directory / 'keystate.o'), '-lwayland-client', *gio_flags, '-o', str(directory / 'modifier-state'))
    run('c++', '-std=c++23', '-I' + str(ROOT), '-I' + str(directory), str(Path(__file__).with_name('kwin_text_order.cc')),
        str(ROOT / 'src/platform/linux/kwin_text.cpp'), str(directory / 'fake-input.o'), str(directory / 'keystate.o'),
        '-lwayland-client', *gio_flags, '-pthread', '-o', str(directory / 'ordering'))
    qt_flags = run('pkg-config', '--cflags', '--libs', 'Qt6Widgets').split()
    run('c++', '-std=c++23', str(Path(__file__).with_name('kwin_text_qt.cc')), '-o', str(directory / 'qt'), *qt_flags)
    gtk = directory / 'gtk.py'
    gtk.write_text('''import gi,sys
from pathlib import Path
gi.require_version('Gtk','3.0')
from gi.repository import Gtk
window=Gtk.Window(title='Sunshine isolated GTK Unicode test');window.set_default_size(800,200)
entry=Gtk.Entry();window.add(entry);returns=0
p=Path(sys.argv[1])
def save(*args):p.write_text(str(returns)+'\\n'+entry.get_text())
def activate(*args):
 global returns
 returns+=1;save()
entry.connect('changed',save);entry.connect('activate',activate)
window.show_all();entry.grab_focus();save();Gtk.main()
''')
    options = os.environ.get('SUNSHINE_TEXT_XKB_OPTIONS', '')
    caps_key = '1' if options == 'caps:swapescape' else '58'
    (Path(os.environ['XDG_CONFIG_HOME']) / 'kxkbrc').write_text('[Layout]\nUse=true\nLayoutList=us,de\nVariantList=,\nResetOldOptions=true\nOptions=' + options + '\n')
    children = []
    with (directory / 'kwin.log').open('w') as log:
        try:
            kwin = subprocess.Popen(['kwin_wayland', '--virtual', '--width', '1000', '--height', '700',
                                     '--socket', 'text-test', '--no-lockscreen', '--no-kactivities'], stdout=log, stderr=log)
            children.append(kwin)
            deadline = time.monotonic() + 15
            while not (Path(os.environ['XDG_RUNTIME_DIR']) / 'text-test').is_socket():
                assert kwin.poll() is None, (directory / 'kwin.log').read_text()
                assert time.monotonic() < deadline, 'KWin startup timed out'
                time.sleep(.05)
            interfaces = run('wayland-info')
            assert "'org_kde_kwin_fake_input'" in interfaces and 'keyboard_keysym' in protocol.read_text()
            cases = [('Qt', [str(directory / 'qt')]), ('GTK', [sys.executable, str(gtk)]), ('Caps', [str(directory / 'qt')])]
            cases += [(name, [str(directory / 'qt')]) for name in ['OwnedShift','OwnedCtrl','OwnedAlt','OwnedMeta','OwnedRightShift','OwnedRightCtrl','OwnedRightAlt']]
            if os.environ.get('SUNSHINE_TEXT_PROBE_MODIFIERS'):
                cases += [(name, [str(directory / 'qt')]) for name in ['Shift', 'Ctrl', 'Alt']]
            failures = []
            for toolkit, command in cases:
                output = directory / (toolkit + '.txt')
                app = subprocess.Popen(command + [str(output)], stdout=log, stderr=log)
                children.append(app)
                deadline = time.monotonic() + 10
                while not output.exists():
                    assert app.poll() is None, (directory / 'kwin.log').read_text()
                    assert time.monotonic() < deadline, 'Application startup timed out'
                    time.sleep(.05)
                time.sleep(.4)
                run('qdbus6', 'org.kde.keyboard', '/Layouts', 'org.kde.KeyboardLayouts.setLayout', '1')
                assert run('qdbus6', 'org.kde.keyboard', '/Layouts', 'org.kde.KeyboardLayouts.getLayout') == '1'
                holder = None
                if toolkit == 'Caps': run(str(directory / 'modifier-state'), 'toggle', caps_key)
                elif toolkit in ('Shift', 'Ctrl', 'Alt'):
                    code = {'Shift':42,'Ctrl':29,'Alt':56}[toolkit]
                    holder = subprocess.Popen([str(directory / 'modifier-state'),'hold',str(code)],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
                    children.append(holder)
                    assert holder.stdout.readline().strip() == 'held'
                before_state = run(str(directory / 'modifier-state'),'state')
                if toolkit == 'Caps':
                    assert before_state.split()[0] == '2', ('Caps must be enabled for regression', options, before_state)
                if toolkit.startswith('Owned'):
                    code = {'OwnedShift':42,'OwnedCtrl':29,'OwnedAlt':56,'OwnedMeta':125,'OwnedRightShift':54,'OwnedRightCtrl':97,'OwnedRightAlt':100}[toolkit]
                    run(str(directory / 'modifier-state'),'owned',str(code),EXPECTED)
                else: run(str(directory / 'inject'), EXPECTED)
                if toolkit in ('Shift','Ctrl','Alt'):
                    time.sleep(.3)
                    after_state = run(str(directory / 'modifier-state'),'state')
                    actual = output.read_text()
                    print(f'MODIFIER PROBE {toolkit}: before={before_state!r}, after={after_state!r}, text={actual!r}',flush=True)
                    if actual != '0\n' + EXPECTED or before_state != after_state: failures.append(toolkit)
                    if holder:
                        holder.stdin.write('release\n');holder.stdin.flush();holder.wait(timeout=5)
                    if toolkit == 'Caps':run(str(directory / 'modifier-state'),'toggle',caps_key)
                    app.terminate();app.wait(timeout=5)
                    continue
                assert run('qdbus6', 'org.kde.keyboard', '/Layouts', 'org.kde.KeyboardLayouts.getLayout') == '1', 'Layout group was not restored' 
                deadline = time.monotonic() + 5
                while output.read_text() != '0\n' + EXPECTED:
                    assert time.monotonic() < deadline, (toolkit, output.read_text(), (directory / 'kwin.log').read_text())
                    time.sleep(.05)
                after_state = run(str(directory / 'modifier-state'),'state')
                assert before_state == after_state, (toolkit,'Modifier state was not restored',before_state,after_state)
                if command == [str(directory / 'qt')]:
                    assert Path(str(output) + '.escapes').read_text() == '0', (toolkit, 'Unexpected Escape')
                if toolkit == 'Caps':run(str(directory / 'modifier-state'),'toggle',caps_key)
                print(f'PASS {toolkit}: {EXPECTED!r}, Return count 0, layout group 1 and modifier state restored, options={options!r}', flush=True)
                app.terminate(); app.wait(timeout=5)
            ordered = directory / 'ordered.txt'
            app = subprocess.Popen([str(directory / 'qt'), str(ordered), '--lines'], stdout=log, stderr=log)
            children.append(app)
            deadline = time.monotonic() + 10
            while not ordered.exists():
                assert app.poll() is None
                assert time.monotonic() < deadline
                time.sleep(.05)
            time.sleep(.3)
            run(str(directory / 'ordering'))
            deadline = time.monotonic() + 5
            expected = '100\n' + 'año\n' * 100
            while ordered.read_text() != expected:
                assert time.monotonic() < deadline, ('Mixed input order', ordered.read_text())
                time.sleep(.05)
            state = run(str(directory / 'modifier-state'), 'state').split()
            assert state[4] == '0', ('Stale Ctrl after pending release', state)
            print('PASS 100 rapid a-key / ñ-text / o-key / Enter lines, 80ms delays, no stale Ctrl', flush=True)
            app.terminate(); app.wait(timeout=5)
            run(str(directory / 'ordering'), 'release')
            state = run(str(directory / 'modifier-state'), 'state').split()
            assert state[4] == '0', ('Ctrl held after keyboard destruction', state)
            print('PASS keyboard destruction releases held Ctrl', flush=True)
            repeated = directory / 'repeated.txt'
            app = subprocess.Popen([str(directory / 'qt'), str(repeated)], stdout=log, stderr=log)
            children.append(app)
            deadline = time.monotonic() + 10
            while not repeated.exists():
                assert app.poll() is None and time.monotonic() < deadline
                time.sleep(.05)
            time.sleep(.3)
            run(str(directory / 'ordering'), 'repeat')
            deadline = time.monotonic() + 5
            while len(repeated.read_text().split('\n', 1)[1]) < 2:
                assert time.monotonic() < deadline, ('Held-key repeat', repeated.read_text())
                time.sleep(.05)
            assert set(repeated.read_text().split('\n', 1)[1]) == {'x'}
            print('PASS held-key repeat on the shared keyboard source', flush=True)
            app.terminate(); app.wait(timeout=5)
            run(str(directory / 'ordering'), 'pin')
            print('PASS unavailable backend stays pinned; a new stream can select the available source', flush=True)
            held = directory / 'held.txt'
            app = subprocess.Popen([str(directory / 'qt'), str(held)], stdout=log, stderr=log)
            children.append(app)
            deadline = time.monotonic() + 10
            while not held.exists():
                assert app.poll() is None and time.monotonic() < deadline
                time.sleep(.05)
            time.sleep(.3)
            run(str(directory / 'ordering'), 'held')
            deadline = time.monotonic() + 5
            while not held.read_text().startswith('0\nx😀x'):
                assert time.monotonic() < deadline, ('Ordinary key restoration', held.read_text())
                time.sleep(.05)
            print('PASS ordinary held key restored after scratch-keymap text', flush=True)
            app.terminate(); app.wait(timeout=5)
            assert not failures, 'Unresolved external modifier cases: ' + ', '.join(failures)
            print('PASS: production keysym path in isolated KWin, user layout untouched, no shared-seat injection', flush=True)
            return 0
        finally:
            for child in reversed(children):
                if child.poll() is None:
                    child.terminate()
                    try:
                        child.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        child.kill(); child.wait(timeout=5)


if __name__ == '__main__':
    sys.exit(inner() if '--inner' in sys.argv else outer())
