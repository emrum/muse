# CLAP GUI lifecycle in MusE — close / re-open paths

Scope: `clap_host_lib_gui.cpp`, `clap_host_lib.h`. Applies to the embedded X11
path (XWayland with `QT_QPA_PLATFORM=xcb` included); the floating path is noted
where it differs.

## 1. State model

Three pieces of state have to agree, and every bug below was one of them drifting
out of sync with the others:

| State | Owner | Meaning |
|---|---|---|
| `_isGuiCreated` | `ClapInstanceCore` | `gui->create()` succeeded, plugin holds a GUI object |
| `_isGuiVisible` | `ClapInstanceCore` | we believe the plugin's GUI is shown |
| plugin-internal "am I visible" | the plugin | what `gui->show()` / `gui->hide()` last told it |
| `_editorWindow` | `ClapInstanceCore` | the `ClapEditorWindow` container the plugin view is reparented into (embedded only, `nullptr` when floating) |
| MusE's "native gui pending" | `ClapSynthIF` / `ClapPluginWrapper_State` | drives the GUI toggle button |

The plugin's own visibility flag is the one MusE cannot read back. Keeping it in
step is what the close interception is for.

## 2. Ownership model, on purpose

- **Hide does not destroy.** `showNativeGui(false)` only hides. Destroying on
  every hide crashes GL plugins (Cardinal: `destroy()` unbinds its GL context
  against an already-gone drawable → `glXMakeCurrent draw=0` → SIGSEGV in the
  GLX driver) and forces a full embed/surface recreate on re-show that leaves
  several plugins black. Keeping the GUI created is the CLAP-idiomatic model and
  keeps the render surface and event sources alive across show/hide.
- **Destroy only at real teardown**, via `closeNativeGui()` / `destroyGui()`.
- **`destroyGui()` calls `gui->destroy()` before deleting the container.** The
  reverse order leaves the plugin holding a stale drawable into a deleted window
  → `xcb_copy_area: BadDrawable` and a black window on the next `create()`.
- **Never pass `nullptr` to `set_parent()`** to "detach". It is non-standard;
  Surge XT dereferences the window pointer to identify the API and crashes.

## 3. The paths

### 3.1 First open

```
showNativeGui(true)
  ├─ gui->is_api_supported() ×2   → decide embedded vs floating
  ├─ gui->create(api, floating)   → _isGuiCreated = true
  ├─ embedded only:
  │    new ClapEditorWindow(this)         ← intercepts close (§4)
  │    WA_NativeWindow / WA_OpaquePaintEvent / WA_NoSystemBackground
  │    winId()                            → force native X window
  │    gui->set_scale(devicePixelRatioF)
  │    gui->set_parent({x11 = winId()})    → plugin reparents its window into ours
  │    gui->get_size() → resize() or setFixedSize()
  ├─ _editorWindow->show()                → XMapWindow
  ├─ processEvents(ExcludeUserInputEvents) → let the map land before painting
  ├─ gui->set_size(same size)              → nudge a redraw, resizable plugins only
  └─ gui->show()                           → _isGuiVisible = true
```

`WA_OpaquePaintEvent` + `WA_NoSystemBackground` stop Qt repainting its own
background over the embedded child.

### 3.2 Close via MusE's GUI toggle

```
ClapSynthIF::showNativeGui(false) → core.showNativeGui(false)
  ├─ gui->hide()               ← plugin is told
  ├─ _editorWindow->hide()
  └─ _isGuiVisible = false
```

Re-open takes §3.1's second half only (`_isGuiCreated` already true): map, nudge,
`gui->show()`. This path always worked.

### 3.3 Close via the window manager decoration (the X button)

This is the path that produced a black window on re-open, and the reason for the
interception.

This took three iterations. Recording all of it, because the two dead ends are
easy to re-introduce.

**Stage 1 — no interception at all.** Qt accepted the `QCloseEvent` and hid the
widget; nothing else ran. No `gui->hide()`, `_isGuiVisible` stayed `true`, MusE's
flag stayed set, so the plugin still believed it was visible. Side effect: the
first toggle click afterwards was swallowed toggling a flag that was already
false, so re-opening took two clicks. Intercepting fixed that, but the window was
still black.

**Stage 2 — intercept, then `e->accept()`.** A trace of both paths showed the call
sequences were *byte-identical*: same `gui->hide()`, same map, same `gui->show()`,
same flags — and the WM path was still black. So the bug was never in our call
sequence. The one remaining difference: accepting the close makes Qt run its full
close machinery (`QWidgetPrivate::handleClose()`) on the container, and a
programmatic `hide()` does not.

**Stage 3 — `e->ignore()`.** Fixes it:

```
WM sends WM_DELETE_WINDOW
  → ClapEditorWindow::closeEvent(e)
       e->ignore()                        ← Qt does NOT hide, NOT close, NOT touch
       core->onEditorWindowClosed()          the native window
         ├─ showNativeGui(false)          ← same path as §3.2, so gui->hide() runs
         └─ _onGuiHiddenByPlugin()        ← MusE clears its toggle/pending flag
```

Ignoring the close is the standard Qt idiom for "the X button hides, it does not
destroy". Both paths are now identical in every respect. Verified on u-he Diva
plus one other plugin.

**What is still not understood.** The hypothesis behind stage 3 was that Qt's
close path destroys the widget's *platform window*, so the next `show()` would
create a new X window and orphan the plugin's embedded child. That is **not** what
happens: the trace prints the container XID on every re-open and it is stable
across WM closes (`xid == embedXid` every time), and the re-parent guard in §3.7
never fires. Qt keeps the same X window; what exactly its close machinery does to
break the embedded child is unidentified. The fix is empirical. Consequence: do
not "simplify" `e->ignore()` back to `e->accept()` on the grounds that the XID is
stable — the stable XID is what *disproves* the tidy explanation, not what makes
accept safe.

### 3.4 Plugin-initiated close — `hostGuiClosed(was_destroyed)`

- `was_destroyed == false` → `_isGuiVisible = false` and fire
  `_onGuiHiddenByPlugin`. GUI stays created.
- `was_destroyed == true` → `destroyGui()`. Calling `gui->destroy()` here is
  required by the CLAP spec, not a double-destroy: the host must acknowledge the
  destruction.

### 3.5 Plugin-initiated hide/resize

- `clapHostGuiRequestHide` → `showNativeGui(false)`.
- `hostGuiRequestResize` → `resize()` if `can_resize()`, else `setFixedSize()`.

### 3.6 Full teardown

```
closeNativeGui() / shutdown()
  → destroyGui()
       ├─ gui->hide() if visible
       ├─ gui->destroy()
       ├─ pruneClosedFdNotifiers()        ← §4.2
       ├─ _isGuiCreated / _isGuiVisible / _isGuiFloating = false
       └─ _editorWindow->hide(); deleteLater()   ← §5
```

`shutdown()` additionally calls `clearGuiEventSources()`; `destroyGui()` alone
deliberately does **not** (see §4.2).

### 3.7 Re-parent guard on re-show

Before re-mapping a *reused* container, `showNativeGui(true)` compares
`_editorWindow->winId()` with `_embedXid` (the XID last handed to `set_parent()`).
On a mismatch it logs and calls `set_parent()` again instead of showing an empty
window. It has never fired in practice (§3.3) — cheap insurance against Qt or a WM
swapping the native window, and re-parenting is far cheaper than a destroy/create
cycle since it keeps the plugin's render surface.

## 4. The event-source problem (the other black window)

Plugins drive their GUI event loop through host-registered sources:
`clap_host_timer_support` → `QTimer`, `clap_host_posix_fd_support` → `QSocketNotifier`
on the plugin's X11 display fd. Which of the two a plugin uses varies — observed
in practice: Diva registers both a timer and an fd, while another plugin
registered only timer id 0 and no fd at all. Both must work. If those stop firing, the plugin never processes
Expose/ConfigureNotify and its window stays unpainted — mapped, no crash, just
black.

### 4.1 Two plugin styles of display ownership

| Style | Behaviour | Example |
|---|---|---|
| Per-instance | opens the X display once per plugin instance, keeps the fd across GUI create/destroy cycles | u-he |
| Per-GUI | `XOpenDisplay()` in `gui->create()`, `XCloseDisplay()` in `gui->destroy()` | most toolkits |

`destroyGui()` therefore must **not** blanket-clear the notifiers — that breaks
the per-instance style (its still-live fd loses its notifier and the second open
is black). That was already known and commented.

### 4.2 What was still broken

The per-GUI style hit the mirror-image bug:

1. `gui->destroy()` closes the display fd. Our notifier now watches a closed fd,
   and Qt permanently disables such a notifier
   (`QSocketNotifier: Invalid socket N and type 'Read', disabling...`).
2. The next `gui->create()` re-opens the display and the OS hands back **the same
   fd number** (it was just freed — lowest free fd).
3. The plugin calls `register_fd()` again. The old code did
   `if(map.contains(fd)) return;` — silently keeping the dead, disabled notifier.
4. No `on_fd()` ever fires → no X events → black window.

Timer ids never collided because they are host-allocated (`_nextTimerId++`); fd
numbers are OS-allocated and *are* reused. That asymmetry is why only the fd path
broke.

**Fixes:**

- `clapFdStillOpen(fd)` — `fcntl(fd, F_GETFD)` liveness check.
- `pruneClosedFdNotifiers()` — called from `destroyGui()` right after
  `gui->destroy()`, in the same call stack so no event loop can recycle the fd
  number in between. Drops notifiers whose fd is genuinely gone, keeps the rest,
  so both plugin styles work unchanged.
- `hostFdRegister()` — rejects a non-open fd, and on re-registration of a tracked
  fd **replaces** the notifier instead of returning silently.

All three log to stderr; see §6.

## 5. Re-entrancy: why `deleteLater()`

`onEditorWindowClosed()` fires `_onGuiHiddenByPlugin`, which runs MusE code that
can reach `closeNativeGui()` → `destroyGui()` → destroy the container — while
`ClapEditorWindow::closeEvent()` is still on the stack. `delete` there is a
use-after-free. `destroyGui()` now does `hide()` + `deleteLater()`:

- `hide()` so no empty frame lingers until the event loop spins,
- `deleteLater()` so the object outlives its own event handler.

`closeEvent()` calls `e->ignore()` *before* `onEditorWindowClosed()` for the same
reason: the event must be settled before anything downstream can tear the widget
down.

## 6. Diagnostics

`clap_host_lib_gui.cpp` carries a lifecycle trace, **off by default**. Re-enable by
uncommenting `#define CLAP_GUI_TRACE` near the top, then:

```
muse5 2>&1 | grep -E "CLAPGUI|QSocketNotifier|BadWindow|BadDrawable"
```

It prints entry/leave plus the state tuple
(`created / visible / floating / win / winVisible`, and the XIDs on show) for every
path in §3. That trace is what disproved the stage-2 theory in §3.3 — keep it.

Expected on stderr, all benign:

```
ClapInstanceCore::pruneClosedFdNotifiers: fd N was closed by the plugin - dropping its notifier
ClapInstanceCore::hostFdRegister: fd N re-registered - replacing stale notifier
```

Worth investigating:

| Message | Meaning |
|---|---|
| `QSocketNotifier: Invalid socket N ... disabling` | a dead notifier survived a cycle — prune missed it |
| `hostFdRegister: fd N is not open - ignoring` | plugin registered a bogus fd |
| `showNativeGui: set_parent() failed` | reparent rejected; window will be black |
| `showNativeGui: get_size=0` | no size from the plugin; container may be 0×0 |
| `ClapEditorWindow::closeEvent: no core - just hiding` | container outlived its core |

## 7. Test matrix

For each plugin (embedded X11, and at least one GL plugin such as Cardinal, one
per-instance-display plugin such as a u-he, and one JUCE/clap-wrapper plugin):

1. Open → visible and painting.
2. Close via MusE toggle → re-open → painting.
3. Close via WM decoration X → re-open in **one** click → painting.
4. Alternate 2 and 3 several times.
5. Resize (resizable plugins) after each re-open.
6. Delete the track / remove the effect while the GUI is open.
7. Quit with the GUI open.

## 8. Known remaining items

- **The stage-3 mechanism is unexplained** (§3.3). Read that section before
  touching `ClapEditorWindow::closeEvent()`.
- **`Failed to set the xembed property property: BadAtom`** — Diva emits this while
  embedding: it is the plugin trying to set `_XEMBED_INFO` and failing to intern
  the atom. Harmless here (Diva falls back to `xcb_copy_area()` with shared
  pixmaps and paints fine), but MusE does not implement the XEmbed protocol at
  all. A plugin that genuinely requires XEmbed would not paint, and this message
  is where that would surface first.
- **Fallback if some plugin is still black on the WM path**: destroy instead of
  hide, i.e. call `closeNativeGui()` from `onEditorWindowClosed()`. Reliable
  thanks to §4, but more expensive — full surface recreate on every re-open — so
  hiding stays the default.
