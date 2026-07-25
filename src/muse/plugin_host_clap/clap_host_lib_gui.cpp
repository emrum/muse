//=============================================================================
//  MusE
//  Linux Music Editor
//
//  clap_host_lib_gui.cpp
//  CLAP host GUI integration (window embedding + size negotiation) for the
//  shared ClapInstanceCore. See clap_host_lib.h for the class contract and
//  clap_host_lib_core.cpp for the non-GUI half. All definitions here are
//  members of ClapInstanceCore, plus the three host-extension vtables
//  (clap.gui / clap.timer-support / clap.posix-fd-support) that core's
//  hostGetExtension() returns.
//
//  (C) Copyright 2026 - Ruwig Faldagon - faldagon[AT]gmx.net
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=============================================================================

#include "config.h"
#ifdef CLAP_SUPPORT

// Turn on debugging messages
//#define CLAP_DEBUG

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>   // fcntl(F_GETFD) - fd liveness check, see clapFdStillOpen()

#include <QWidget>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QCoreApplication>
#include <QTimer>
#include <QSocketNotifier>

#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/posix-fd-support.h>

#include "clap_host_lib.h"

namespace MusECore {

//---------------------------------------------------------
//   clap_host_gui vtable + accessor
//   (trampolines forward into the per-instance ClapInstanceCore)
//---------------------------------------------------------

static void CLAP_ABI clapHostGuiResizeHintsChanged(const clap_host_t* /*host*/)
{
  // We re-query size on demand, so nothing cached to invalidate here.
}

static bool CLAP_ABI clapHostGuiRequestResize(const clap_host_t* host,
                                              uint32_t w, uint32_t h)
{ return coreFromClap(host)->hostGuiRequestResize(w, h); }

static bool CLAP_ABI clapHostGuiRequestShow(const clap_host_t* host)
{ coreFromClap(host)->showNativeGui(true);  return true; }

static bool CLAP_ABI clapHostGuiRequestHide(const clap_host_t* host)
{ coreFromClap(host)->showNativeGui(false); return true; }

static void CLAP_ABI clapHostGuiClosed(const clap_host_t* host, bool was_destroyed)
{ coreFromClap(host)->hostGuiClosed(was_destroyed); }

static const clap_host_gui_t s_hostGuiExt = {
  clapHostGuiResizeHintsChanged,
  clapHostGuiRequestResize,
  clapHostGuiRequestShow,
  clapHostGuiRequestHide,
  clapHostGuiClosed,
};

const clap_host_gui_t* clapCoreGuiHostExt() { return &s_hostGuiExt; }

//---------------------------------------------------------
//   clap_host_timer_support vtable + accessor
//---------------------------------------------------------

static bool CLAP_ABI clapHostTimerRegister(const clap_host_t* host,
                                           uint32_t period_ms, clap_id* timer_id)
{ return coreFromClap(host)->hostTimerRegister(period_ms, timer_id); }

static bool CLAP_ABI clapHostTimerUnregister(const clap_host_t* host, clap_id timer_id)
{ return coreFromClap(host)->hostTimerUnregister(timer_id); }

static const clap_host_timer_support_t s_hostTimerExt = {
  clapHostTimerRegister,
  clapHostTimerUnregister,
};

const clap_host_timer_support_t* clapCoreTimerHostExt() { return &s_hostTimerExt; }

//---------------------------------------------------------
//   clap_host_posix_fd_support vtable + accessor
//---------------------------------------------------------

static bool CLAP_ABI clapHostFdRegister(const clap_host_t* host,
                                        int fd, clap_posix_fd_flags_t flags)
{ return coreFromClap(host)->hostFdRegister(fd, flags); }

static bool CLAP_ABI clapHostFdModify(const clap_host_t* host,
                                      int fd, clap_posix_fd_flags_t flags)
{ return coreFromClap(host)->hostFdModify(fd, flags); }

static bool CLAP_ABI clapHostFdUnregister(const clap_host_t* host, int fd)
{ return coreFromClap(host)->hostFdUnregister(fd); }

static const clap_host_posix_fd_support_t s_hostPosixFdExt = {
  clapHostFdRegister,
  clapHostFdModify,
  clapHostFdUnregister,
};

const clap_host_posix_fd_support_t* clapCorePosixFdHostExt() { return &s_hostPosixFdExt; }

//---------------------------------------------------------
//   clapFdStillOpen
//   True if fd is still a valid open descriptor in this process.
//   Tells apart the two plugin styles of X11 display ownership: plugins that
//   keep one display connection for the whole instance lifetime (u-he) leave
//   the fd open across GUI create/destroy cycles, while plugins that open the
//   display inside gui->create() and close it in gui->destroy() (the common
//   toolkit pattern) leave us holding a QSocketNotifier on a closed fd - which
//   Qt permanently disables, after which the plugin never receives another X11
//   event and its re-created GUI stays black.
//---------------------------------------------------------

static bool clapFdStillOpen(int fd)
{
  return fd >= 0 && ::fcntl(fd, F_GETFD) != -1;
}

//---------------------------------------------------------
//   ClapEditorWindow
//   The container widget a plugin's X11/Win32/Cocoa view is embedded into.
//   Exists only to catch the close event: clicking the window manager's X on
//   the decoration bypasses MusE completely - Qt accepts the QCloseEvent and
//   simply hides the widget. Nothing then tells the PLUGIN, so it keeps
//   believing its GUI is visible, and MusE keeps believing so too. On the next
//   open, _extGui->show() is therefore a no-op in the many plugins that
//   early-return when already shown (and that is also where they'd force a
//   full repaint), so the window maps but nothing ever paints into it: the
//   black window that only happened on the WM-close path, never on MusE's own
//   GUI toggle. Routing the close through onEditorWindowClosed() makes both
//   paths identical.
//   No Q_OBJECT macro on purpose - no signals/slots here, so no moc needed.
//---------------------------------------------------------

class ClapEditorWindow : public QWidget
{
public:
  explicit ClapEditorWindow(ClapInstanceCore* core)
    : QWidget(nullptr), _core(core) { }

protected:
  void closeEvent(QCloseEvent* e) override
  {
    if(!_core)
    {
      fprintf(stderr, "ClapEditorWindow::closeEvent: no core - just hiding\n");
      QWidget::closeEvent(e);
      return;
    }
    // Accept first: _core may (via the GUI-closed callback) run MusE code that
    // ends up in destroyGui(), which deletes this widget - deferred through
    // deleteLater() precisely so we can still be inside our own event handler.
    e->accept();
    _core->onEditorWindowClosed();
  }

private:
  ClapInstanceCore* _core = nullptr;
};

//---------------------------------------------------------
//   destroyGui
//   Full teardown: cleanly detach from X11, destroy plugin GUI,
//   then delete the host window.
//---------------------------------------------------------

void ClapInstanceCore::destroyGui()
{
  // WE MUST NOT CALL clearGuiEventSources() HERE!
  // In Linux X11, many CLAP plugins (e.g. u-he) open their X11 display
  // connection once per plugin instance, register the file descriptor,
  // and keep it alive across multiple GUI show/hide (create/destroy) cycles.
  // If we forcefully drop the QSocketNotifiers here, the host stops
  // sending X11 events on the second open, causing a pure black window.

  if(_isGuiCreated && _extGui && _plugin)
  {
    if(_isGuiVisible)
      _extGui->hide(_plugin);

    // Call destroy directly. Passing nullptr to set_parent() is non-standard
    // and causes SIGSEGV in plugins like Surge XT because they attempt to
    // read the window pointer to identify the API.
    _extGui->destroy(_plugin);

    // destroy() may have closed the plugin's X11 display connection. Any
    // notifier we still hold on that fd is dead now, and hostFdRegister() used
    // to silently skip re-registration because the fd was still in our map -
    // so the re-created GUI never got a single X11 event and stayed black.
    // Drop only the notifiers whose fd is really gone; plugins that keep the
    // connection alive (see the note above) keep theirs. Runs in the same call
    // stack as destroy(), before any event loop can recycle the fd number.
    pruneClosedFdNotifiers();
  }

  _isGuiCreated  = false;
  _isGuiVisible  = false;
  _isGuiFloating = false;

  if(_editorWindow)
  {
    // deleteLater(), not delete: destroyGui() can be reached from inside the
    // container's own closeEvent() (ClapEditorWindow -> onEditorWindowClosed()
    // -> MusE's GUI-closed bookkeeping -> closeNativeGui()), and deleting a
    // widget while its event handler is on the stack is a use-after-free.
    // hide() first so no empty frame lingers until the event loop spins.
    _editorWindow->hide();
    _editorWindow->deleteLater();
    _editorWindow = nullptr;
  }
}

//---------------------------------------------------------
//   showNativeGui
//   v == true  : create (if needed) + show
//   v == false : hide only (keep created; destroy happens in closeNativeGui())
//   NOTE: unlike the old ClapSynthIF::showNativeGui(), this does NOT call
//   PluginIBase::showNativeGui(v) — ClapInstanceCore doesn't know about
//   PluginIBase/Plugin. Callers (ClapSynthIF, ClapPluginWrapper_State) do
//   that bookkeeping themselves before/after calling this.
//---------------------------------------------------------

void ClapInstanceCore::showNativeGui(bool v)
{
  if(!_extGui || !_plugin)
  {
    #ifdef CLAP_DEBUG
    printf("ClapInstanceCore::showNativeGui: no GUI extension\n");
    #endif
    return;
  }

  if(v)
  {
    if(!_isGuiCreated)
    {
      const char* api =
#if defined(Q_OS_WIN)
        CLAP_WINDOW_API_WIN32;
#elif defined(Q_OS_MACOS)
        CLAP_WINDOW_API_COCOA;
#else
        CLAP_WINDOW_API_X11;
#endif

      const bool isWayland =
        QGuiApplication::platformName().startsWith("wayland", Qt::CaseInsensitive);
      const bool embedOk = _extGui->is_api_supported(_plugin, api, false);
      const bool floatOk = _extGui->is_api_supported(_plugin, api, true);

      fprintf(stderr,
        "ClapInstanceCore::showNativeGui: platform='%s' api='%s' embeddable=%d floatable=%d\n",
        QGuiApplication::platformName().toLocal8Bit().constData(), api, embedOk, floatOk);

      // Decide embedded vs floating.
      bool floating = false;
      if(isWayland)
      {
        if(floatOk)
          floating = true;
        else
        {
          fprintf(stderr,
            "ClapInstanceCore::showNativeGui: plugin '%s' only supports embedded X11, "
            "which does not work on native Wayland. Run MusE under XWayland "
            "(QT_QPA_PLATFORM=xcb) to embed its GUI.\n",
            _displayName.toLocal8Bit().constData());
          return;
        }
      }
      else if(embedOk)
        floating = false;
      else if(floatOk)
        floating = true;
      else
      {
        fprintf(stderr, "ClapInstanceCore::showNativeGui: no supported GUI api '%s'\n", api);
        return;
      }

      if(!_extGui->create(_plugin, api, floating))
      {
        fprintf(stderr, "ClapInstanceCore::showNativeGui: gui->create(floating=%d) failed\n", floating);
        return;
      }
      _isGuiCreated  = true;
      _isGuiFloating = floating;

      if(floating)
      {
        _extGui->suggest_title(_plugin, _displayName.toUtf8().constData());
        fprintf(stderr, "ClapInstanceCore::showNativeGui: using floating window\n");
      }
      else
      {
        _editorWindow = new ClapEditorWindow(this);
        _editorWindow->setWindowTitle(_displayName);
        _editorWindow->setAttribute(Qt::WA_NativeWindow, true);

        // Prevent Qt from aggressively repainting the background and erasing the plugin
        _editorWindow->setAttribute(Qt::WA_OpaquePaintEvent, true);
        _editorWindow->setAttribute(Qt::WA_NoSystemBackground, true);

        _editorWindow->winId();

        if(_editorWindow->devicePixelRatioF() > 0.0)
          _extGui->set_scale(_plugin, _editorWindow->devicePixelRatioF());

        clap_window_t cw;
        cw.api = api;
#if defined(Q_OS_WIN)
        cw.win32 = reinterpret_cast<clap_hwnd>(_editorWindow->winId());
#elif defined(Q_OS_MACOS)
        cw.cocoa = reinterpret_cast<clap_nsview>(_editorWindow->winId());
#else
        cw.x11   = static_cast<clap_xwnd>(_editorWindow->winId());
#endif
        const bool parented = _extGui->set_parent(_plugin, &cw);
        fprintf(stderr, "ClapInstanceCore::showNativeGui: set_parent=%d xid=0x%lx\n",
                parented, (unsigned long)_editorWindow->winId());
        if(!parented)
          fprintf(stderr, "ClapInstanceCore::showNativeGui: set_parent() failed\n");

        uint32_t w = 0, h = 0;
        const bool gotSize = _extGui->get_size(_plugin, &w, &h);
        fprintf(stderr, "ClapInstanceCore::showNativeGui: embedded; get_size=%d w=%u h=%u\n",
                gotSize, w, h);
        if(gotSize && w > 0 && h > 0)
        {
          if(_extGui->can_resize(_plugin))
            _editorWindow->resize(int(w), int(h));
          else
            _editorWindow->setFixedSize(int(w), int(h));
        }
      }
    }

    if(!_isGuiVisible)
    {
      if(_editorWindow)
      {
        _editorWindow->show();

        // Force the X11 map request to actually land at the server before
        // telling the plugin to paint into the (now newly-mapped) window.
        // On a fresh create()+set_parent() cycle - in particular after a
        // prior destroyGui() - several plugins validate/(re)build their
        // render surface lazily, keyed to a resize/expose signal received
        // AFTER their parent window is mapped, not at set_parent()/
        // resize() time (when the window was still hidden). Without this,
        // the plugin can end up with a technically-visible but
        // never-painted window: no crash, no black frame, just nothing
        // drawn - see the "destroyed GUI shown again doesn't paint" report.
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);

        if(!_isGuiFloating && _extGui->can_resize(_plugin))
        {
          uint32_t w = 0, h = 0;
          if(_extGui->get_size(_plugin, &w, &h) && w > 0 && h > 0)
            _extGui->set_size(_plugin, w, h); // same size - nudges a redraw now that we're mapped
        }
      }
      _extGui->show(_plugin);
      _isGuiVisible = true;
    }
  }
  else
  {
    // Hide only — do NOT destroy the plugin GUI on hide. Destroying on every
    // hide (a) crashes GL plugins like Cardinal, whose destroy() unbinds its GL
    // context against an already-gone drawable (glXMakeCurrent draw=0 -> SIGSEGV
    // in the GLX driver), and (b) forces a full embed/GL-surface recreate on
    // re-show that leaves several plugins black. Keeping the GUI created and
    // just hiding the window is the CLAP-idiomatic model and keeps the plugin's
    // render surface + event sources alive across show/hide. Full teardown is
    // done in closeNativeGui()/destroyGui() at actual close/shutdown.
    if(_isGuiVisible)
    {
      if(_extGui && _plugin)
        _extGui->hide(_plugin);
      if(_editorWindow)
        _editorWindow->hide();
      _isGuiVisible = false;
    }
  }
}

//---------------------------------------------------------
//   closeNativeGui
//   Full teardown (unlike showNativeGui(false) which only hides).
//---------------------------------------------------------

void ClapInstanceCore::closeNativeGui()
{
  destroyGui();
}

//---------------------------------------------------------
//   onEditorWindowClosed
//   The user closed the embedding window via the WM decoration. See
//   ClapEditorWindow above for why this has to do more than nothing.
//---------------------------------------------------------

void ClapInstanceCore::onEditorWindowClosed()
{
  // Same path as MusE's own GUI toggle: this is what actually calls
  // _extGui->hide(_plugin), so the plugin's idea of "am I visible" stays in
  // sync with ours and its next show() really shows and repaints.
  showNativeGui(false);

  // And tell MusE, so its GUI button/pending flag clears - otherwise the first
  // click after a WM close is swallowed toggling a state that is already false.
  if(_onGuiHiddenByPlugin)
    _onGuiHiddenByPlugin();
}

//---------------------------------------------------------
//   hostGuiClosed
//   Plugin/window-manager told us the GUI window was closed.
//---------------------------------------------------------

void ClapInstanceCore::hostGuiClosed(bool was_destroyed)
{
  #ifdef CLAP_DEBUG
  printf("ClapInstanceCore::hostGuiClosed was_destroyed:%d\n", was_destroyed);
  #endif

  if(was_destroyed)
  {
    // Route through destroyGui() rather than tearing down _editorWindow
    // directly here: destroyGui() calls _extGui->destroy(_plugin) BEFORE
    // deleting the parent window. Skipping that step left the plugin's own
    // GUI object holding a stale drawable/render handle into our already-
    // deleted window — the next create() on the same instance then hits
    // "xcb_copy_area: BadDrawable" and shows a black window.
    destroyGui();
  }
  else
  {
    _isGuiVisible = false;
    // Owner (ClapSynthIF / ClapPluginWrapper_State) does its own
    // "native gui pending" bookkeeping via this callback — see
    // setGuiClosedCallback() in clap_host_lib.h.
    if(_onGuiHiddenByPlugin)
      _onGuiHiddenByPlugin();
  }
}

//---------------------------------------------------------
//   hostGuiRequestResize
//   Plugin asked the host to resize its embedding window.
//---------------------------------------------------------

bool ClapInstanceCore::hostGuiRequestResize(uint32_t width, uint32_t height)
{
  #ifdef CLAP_DEBUG
  printf("ClapInstanceCore::hostGuiRequestResize w:%u h:%u\n", width, height);
  #endif
  if(!_editorWindow)
    return false;

  if(_extGui && _isGuiCreated && _extGui->can_resize(_plugin))
    _editorWindow->resize(int(width), int(height));
  else
    _editorWindow->setFixedSize(int(width), int(height));
  return true;
}

//---------------------------------------------------------
//   hostTimerRegister / hostTimerUnregister
//---------------------------------------------------------

bool ClapInstanceCore::hostTimerRegister(uint32_t period_ms, clap_id* timer_id)
{
  // Re-query dynamically in case the extension is only exposed during GUI creation
  if(!_extTimer)
  {
    _extTimer = static_cast<const clap_plugin_timer_support_t*>(
                  _plugin->get_extension(_plugin, CLAP_EXT_TIMER_SUPPORT));
  }

  if(!_extTimer)
  {
    fprintf(stderr, "ClapInstanceCore::hostTimerRegister: plugin has no timer-support ext\n");
    return false;
  }
  if(period_ms < 16)
    period_ms = 16;

  const clap_id id = _nextTimerId++;
  QTimer* t = new QTimer();
  t->setInterval(int(period_ms));

  const clap_plugin_t* plug = _plugin;
  const clap_plugin_timer_support_t* ext = _extTimer;

  // Guard with a shared alive-flag. clearGuiEventSources() resets it to false
  // before stopping timers, so any in-flight timeout fires safely as a no-op
  // rather than calling into a plugin that is mid-teardown.
  auto alive = std::make_shared<bool>(true);
  _timerAlive.insert(id, alive);

  QObject::connect(t, &QTimer::timeout, t,
    [plug, ext, id, alive]()
    {
      if(*alive)
        ext->on_timer(plug, id);
    });

  t->start();
  _timers.insert(id, t);
  *timer_id = id;
  return true;
}

bool ClapInstanceCore::hostTimerUnregister(clap_id timer_id)
{
  const auto it = _timers.find(timer_id);
  if(it == _timers.end())
  {
    // This normally means the plugin called unregister_timer() from its own
    // destructor or shutdown path after the host already cleared _timers in
    // clearGuiEventSources(). The timer is gone and the alive-flag is already
    // false, so no on_timer() call will fire. This is harmless.
    fprintf(stderr,
      "ClapInstanceCore::hostTimerUnregister: timer id %u not found "
      "(plugin called unregister after host teardown — harmless)\n", timer_id);
    return false;
  }
  // Disarm the alive-flag first so any queued timeout that fires before
  // deleteLater() is processed becomes a safe no-op.
  const auto ait = _timerAlive.find(timer_id);
  if(ait != _timerAlive.end())
  {
    *ait.value() = false;
    _timerAlive.erase(ait);
  }
  it.value()->stop();
  it.value()->deleteLater();
  _timers.erase(it);
  return true;
}

//---------------------------------------------------------
//   hostFdRegister / hostFdModify / hostFdUnregister
//---------------------------------------------------------

bool ClapInstanceCore::hostFdRegister(int fd, clap_posix_fd_flags_t flags)
{
  // Re-query dynamically in case the extension is only exposed during GUI creation
  if(!_extPosixFd)
  {
    _extPosixFd = static_cast<const clap_plugin_posix_fd_support_t*>(
                    _plugin->get_extension(_plugin, CLAP_EXT_POSIX_FD_SUPPORT));
  }

  if(!_extPosixFd)
  {
    fprintf(stderr, "ClapInstanceCore::hostFdRegister: plugin has no posix-fd-support ext\n");
    return false;
  }

  if(!clapFdStillOpen(fd))
  {
    fprintf(stderr, "ClapInstanceCore::hostFdRegister: fd %d is not open - ignoring\n", fd);
    return false;
  }

  const clap_plugin_t* plug = _plugin;
  const clap_plugin_posix_fd_support_t* ext = _extPosixFd;

  auto make = [&](QHash<int, QSocketNotifier*>& map,
                  QSocketNotifier::Type type, clap_posix_fd_flags_t f)
  {
    if(!(flags & f))
      return;

    // Always (re)create - never keep an existing notifier. A register_fd() for
    // an fd we already track means the plugin re-opened its display connection
    // (typically inside gui->create(), after a previous gui->destroy() closed
    // it) and the OS handed back the same fd number. The old notifier is then
    // stale, and Qt has usually already auto-disabled it ("QSocketNotifier:
    // Invalid socket ... disabling"). Silently keeping it is exactly what left
    // the re-shown GUI black: mapped window, no X11 events, nothing painted.
    const auto it = map.find(fd);
    if(it != map.end())
    {
      fprintf(stderr, "ClapInstanceCore::hostFdRegister: fd %d re-registered - "
                      "replacing stale notifier\n", fd);
      it.value()->setEnabled(false);
      it.value()->deleteLater();
      map.erase(it);
    }

    QSocketNotifier* n = new QSocketNotifier(fd, type);
    QObject::connect(n, &QSocketNotifier::activated, n,
                     [plug, ext, fd, f]() { ext->on_fd(plug, fd, f); });
    n->setEnabled(true);
    map.insert(fd, n);
  };

  make(_fdRead,  QSocketNotifier::Read,      CLAP_POSIX_FD_READ);
  make(_fdWrite, QSocketNotifier::Write,     CLAP_POSIX_FD_WRITE);
  make(_fdError, QSocketNotifier::Exception, CLAP_POSIX_FD_ERROR);
  return true;
}

bool ClapInstanceCore::hostFdModify(int fd, clap_posix_fd_flags_t flags)
{
  hostFdUnregister(fd);
  return hostFdRegister(fd, flags);
}

bool ClapInstanceCore::hostFdUnregister(int fd)
{
  bool found = false;
  for(QHash<int, QSocketNotifier*>* map : { &_fdRead, &_fdWrite, &_fdError })
  {
    const auto it = map->find(fd);
    if(it != map->end())
    {
      it.value()->setEnabled(false);
      it.value()->deleteLater();
      map->erase(it);
      found = true;
    }
  }

  if(!found && !_teardown)
      fprintf(stderr, "ClapInstanceCore::hostFdUnregister: unknown fd %d\n", fd);
  //
  return found;
}

//---------------------------------------------------------
//   pruneClosedFdNotifiers
//   Drop QSocketNotifiers whose fd the plugin closed behind our back (see
//   destroyGui()). Unlike clearGuiEventSources() this KEEPS notifiers for
//   still-open fds, so plugins holding one display connection for the whole
//   instance lifetime are left untouched.
//---------------------------------------------------------

void ClapInstanceCore::pruneClosedFdNotifiers()
{
  for(QHash<int, QSocketNotifier*>* map : { &_fdRead, &_fdWrite, &_fdError })
  {
    for(auto it = map->begin(); it != map->end(); )
    {
      if(clapFdStillOpen(it.key()))
      {
        ++it;
        continue;
      }
      fprintf(stderr, "ClapInstanceCore::pruneClosedFdNotifiers: fd %d was closed by the "
                      "plugin - dropping its notifier\n", it.key());
      it.value()->setEnabled(false);
      it.value()->deleteLater();
      it = map->erase(it);
    }
  }
}

//---------------------------------------------------------
//   clearGuiEventSources
//   Defensive teardown: ONLY call this from destroyGui()/shutdown()!
//---------------------------------------------------------

void ClapInstanceCore::clearGuiEventSources()
{
  // Disarm all alive-flags FIRST, before stopping timers.
  // This ensures any QTimer::timeout that fires between here and deleteLater()
  // being processed will see *alive == false and skip the on_timer() call,
  // preventing calls into a plugin that is being torn down.
  for(auto& flag : _timerAlive)
    *flag = false;
  _timerAlive.clear();

  for(QTimer* t : _timers)           { t->stop();            t->deleteLater(); }
  _timers.clear();

  for(QSocketNotifier* n : _fdRead)  { n->setEnabled(false); n->deleteLater(); }
  for(QSocketNotifier* n : _fdWrite) { n->setEnabled(false); n->deleteLater(); }
  for(QSocketNotifier* n : _fdError) { n->setEnabled(false); n->deleteLater(); }
  _fdRead.clear();
  _fdWrite.clear();
  _fdError.clear();
}

} // namespace MusECore

#endif // CLAP_SUPPORT
