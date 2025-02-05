/*
 * IceWM
 *
 * Copyright (C) 1997-2001 Marko Macek
 */
#include "config.h"
#include "yfull.h"
#include "wmclient.h"
#include "prefs.h"

#include "yrect.h"
#include "wmframe.h"
#include "wmmgr.h"
#include "wmapp.h"
#include "sysdep.h"
#include "yxcontext.h"
#include "workspaces.h"
#include "wmminiicon.h"

bool operator==(const XSizeHints& a, const XSizeHints& b) {
    long mask = PMinSize | PMaxSize | PResizeInc |
                PAspect | PBaseSize | PWinGravity;
    return (a.flags & mask) == (b.flags & mask) &&
        (notbit(a.flags, PMinSize) ||
               (a.min_width == b.min_width && a.min_height == b.min_height)) &&
        (notbit(a.flags, PMaxSize) ||
               (a.max_width == b.max_width && a.max_height == b.max_height)) &&
        (notbit(a.flags, PResizeInc) ||
               (a.width_inc == b.width_inc && a.height_inc == b.height_inc)) &&
        (notbit(a.flags, PAspect) ||
               (a.min_aspect.x == b.min_aspect.x &&
                a.min_aspect.y == b.min_aspect.y &&
                a.max_aspect.x == b.max_aspect.x &&
                a.max_aspect.y == b.max_aspect.y)) &&
        (notbit(a.flags, PBaseSize) ||
               (a.base_width == b.base_width &&
                a.base_height == b.base_height)) &&
        (notbit(a.flags, PWinGravity) || a.win_gravity == b.win_gravity) ;
}
bool operator!=(const XSizeHints& a, const XSizeHints& b) {
    return !(a == b);
}

YFrameClient::YFrameClient(YWindow *parent, YFrameWindow *frame, Window win,
                           int depth, Visual *visual, Colormap colormap):
    YDndWindow(parent, win, depth, visual, colormap),
    fWindowTitle(),
    fIconTitle(),
    fWindowRole()
{
    fFrame = frame;
    fBorder = 0;
    fProtocols = 0;
    fColormap = colormap;
    fShaped = false;
    fPinging = false;
    fPingTime = 0;
    fHints = nullptr;
    fWinHints = 0;
    fSavedFrameState = InvalidFrameState;
    fWinStateHint = InvalidFrameState;
    fSizeHints = XAllocSizeHints();
    fTransientFor = None;
    fClientLeader = None;
    fPid = 0;
    prop = {};

    if (win == None) {
        getSizeHints();
    } else {
        getPropertiesList();
        getProtocols(false);
        getNameHint();
        getIconNameHint();
        getNetWmName();
        getNetWmIconName();
        getSizeHints();
        getClassHint();
        getTransient();
        getClientLeader();
        getWMHints();
        getWindowRole();
        getMwmHints();

#ifdef CONFIG_SHAPE
        if (shapes.supported) {
            XShapeSelectInput(xapp->display(), handle(), ShapeNotifyMask);
            queryShape();
        }
#endif
    }

    if (getFrame()) {
        frameContext.save(handle(), getFrame());
    }
    else {
        clientContext.save(handle(), this);
    }
}

YFrameClient::~YFrameClient() {
    if (getFrame()) {
        frameContext.remove(handle());
    }
    else {
        clientContext.remove(handle());
    }

    if (fSizeHints) { XFree(fSizeHints); fSizeHints = nullptr; }
    if (fHints) { XFree(fHints); fHints = nullptr; }
}

void YFrameClient::getProtocols(bool force) {
    if (!prop.wm_protocols && !force)
        return;

    Atom *wmp = nullptr;
    int count = 0;

    fProtocols &= wpDeleteWindow; // always keep WM_DELETE_WINDOW

    if (XGetWMProtocols(xapp->display(), handle(), &wmp, &count) && wmp) {
        prop.wm_protocols = true;
        for (int i = 0; i < count; i++) {
            fProtocols |=
                (wmp[i] == _XA_WM_DELETE_WINDOW) ? wpDeleteWindow :
                (wmp[i] == _XA_WM_TAKE_FOCUS) ? wpTakeFocus :
                (wmp[i] == _XA_NET_WM_PING) ? wpPing :
                0;
        }
        XFree(wmp);
    }
}

void YFrameClient::getSizeHints() {
    if (fSizeHints) {
        long supplied;

        if (!prop.wm_normal_hints ||
            !XGetWMNormalHints(xapp->display(), handle(), fSizeHints, &supplied))
            fSizeHints->flags = 0;

        if (notbit(fSizeHints->flags, PResizeInc)) {
            fSizeHints->width_inc = fSizeHints->height_inc = 1;
        }

        if (!(fSizeHints->flags & PBaseSize)) {
            if (fSizeHints->flags & PMinSize) {
                fSizeHints->base_width = fSizeHints->min_width;
                fSizeHints->base_height = fSizeHints->min_height;
            } else
                fSizeHints->base_width = fSizeHints->base_height = 0;
        }
        if (!(fSizeHints->flags & PMinSize)) {
            fSizeHints->min_width = fSizeHints->base_width;
            fSizeHints->min_height = fSizeHints->base_height;
        }
        if (!(fSizeHints->flags & PMaxSize)) {
            fSizeHints->max_width = 32767;
            fSizeHints->max_height = 32767;
        }
        if (fSizeHints->max_width < fSizeHints->min_width)
            fSizeHints->max_width = 32767;
        if (fSizeHints->max_height < fSizeHints->min_height)
            fSizeHints->max_height = 32767;

        if (fSizeHints->min_height <= 0)
            fSizeHints->min_height = 1;
        if (fSizeHints->min_width <= 0)
            fSizeHints->min_width = 1;

        if (notbit(fSizeHints->flags, PWinGravity)) {
            fSizeHints->win_gravity = NorthWestGravity;
            fSizeHints->flags |= PWinGravity;
        }
        else if (fSizeHints->win_gravity < ForgetGravity)
            fSizeHints->win_gravity = ForgetGravity;
        else if (fSizeHints->win_gravity > StaticGravity)
            fSizeHints->win_gravity = StaticGravity;
    }
}

void YFrameClient::getClassHint() {
    if (!prop.wm_class)
        return;

    fClassHint.reset();
    XGetClassHint(xapp->display(), handle(), &fClassHint);
}

void YFrameClient::getTransient() {
    if (!prop.wm_transient_for)
        return;

    Window newTransientFor = None;

    if (XGetTransientForHint(xapp->display(),
                             handle(),
                             &newTransientFor))
    {
        if (newTransientFor == handle())    /* bug in fdesign */
            newTransientFor = None;
    }

    if (newTransientFor != fTransientFor) {
        if (fTransientFor)
            if (getFrame())
                getFrame()->removeAsTransient();
        fTransientFor = newTransientFor;
        if (fTransientFor)
            if (getFrame())
                getFrame()->addAsTransient();
    }
}

void YFrameClient::constrainSize(int &w, int &h, int flags)
{
    if (fSizeHints && (considerSizeHintsMaximized || !getFrame()->isMaximized())) {
        int const wMin(fSizeHints->min_width);
        int const hMin(fSizeHints->min_height);
        int const wMax(fSizeHints->max_width);
        int const hMax(fSizeHints->max_height);
        int const wBase(fSizeHints->base_width);
        int const hBase(fSizeHints->base_height);
        int const wInc(max(1, fSizeHints->width_inc));
        int const hInc(max(1, fSizeHints->height_inc));

        if (fSizeHints->flags & PAspect) { // aspect ratios
            int const xMin(fSizeHints->min_aspect.x);
            int const yMin(fSizeHints->min_aspect.y);
            int const xMax(fSizeHints->max_aspect.x);
            int const yMax(fSizeHints->max_aspect.y);

            MSG(("aspect"));
            if (flags & csKeepX) {
                MSG(("keepX"));
            }
            if (flags & csKeepY) {
                MSG(("keepY"));
            }

            // !!! fix handling of KeepX and KeepY together
            if (xMin * h > yMin * w) { // min aspect
                if (flags & csKeepX) {
                    w = clamp(w, wMin, wMax);
                    h = w * yMin / non_zero(xMin);
                    h = clamp(h, hMin, hMax);
                    w = h * xMin / non_zero(yMin);
                } else {
                    h = clamp(h, hMin, hMax);
                    w = h * xMin / non_zero(yMin);
                    w = clamp(w, wMin, wMax);
                    h = w * yMin / non_zero(xMin);
                }
            }
            if (xMax * h < yMax * w) { // max aspect
                if (flags & csKeepX) {
                    w = clamp(w, wMin, wMax);
                    h = w * yMax / non_zero(xMax);
                    h = clamp(h, hMin, hMax);
                    w = h * xMax / non_zero(yMax);
                } else {
                    h = clamp(h, hMin, hMax);
                    w = h * xMax / non_zero(yMax);
                    w = clamp(w, wMin, wMax);
                    h = w * yMax / non_zero(xMax);
                }
            }
        }

        h = clamp(h, hMin, hMax);
        w = clamp(w, wMin, wMax);

        if (flags & csRound) {
            w += wInc / 2;
            h += hInc / 2;
        }

        w -= max(0, w - wBase) % non_zero(wInc);
        h -= max(0, h - hBase) % non_zero(hInc);
    }

    if (w <= 0) w = 1;
    if (h <= 0) h = 1;
}

void YFrameClient::gravityOffsets(int &xp, int &yp) {
    static const pair<int, int> offsets[11] = {
        {  0,  0 },  /* ForgetGravity */
        { -1, -1 },  /* NorthWestGravity */
        {  0, -1 },  /* NorthGravity */
        {  1, -1 },  /* NorthEastGravity */
        { -1,  0 },  /* WestGravity */
        {  0,  0 },  /* CenterGravity */
        {  1,  0 },  /* EastGravity */
        { -1,  1 },  /* SouthWestGravity */
        {  0,  1 },  /* SouthGravity */
        {  1,  1 },  /* SouthEastGravity */
        {  0,  0 },  /* StaticGravity */
    };
    int g = hasbit(sizeHintsFlags(), PWinGravity)
          ? clamp(fSizeHints->win_gravity, 0, 10) : 0;
    xp = offsets[g].left;
    yp = offsets[g].right;
}

void YFrameClient::sendMessage(Atom msg, Time timeStamp) {
    XClientMessageEvent xev;

    memset(&xev, 0, sizeof(xev));
    xev.type = ClientMessage;
    xev.window = handle();
    xev.message_type = _XA_WM_PROTOCOLS;
    xev.format = 32;
    xev.data.l[0] = msg;
    xev.data.l[1] = timeStamp;
    xapp->send(xev, handle());
}

///extern Time lastEventTime;

bool YFrameClient::sendTakeFocus() {
    if (protocols() & wpTakeFocus) {
        sendMessage(_XA_WM_TAKE_FOCUS, xapp->getEventTime("sendTakeFocus"));
        return true;
    }
    return false;
}

bool YFrameClient::sendDelete() {
    if (protocols() & wpDeleteWindow) {
        sendMessage(_XA_WM_DELETE_WINDOW, xapp->getEventTime("sendDelete"));
        return true;
    }
    return false;
}

bool YFrameClient::sendPing() {
    bool sent = false;
    if (hasbit(protocols(), (unsigned) wpPing) && fPinging == false) {
        XClientMessageEvent xev = {};
        xev.type = ClientMessage;
        xev.window = handle();
        xev.message_type = _XA_WM_PROTOCOLS;
        xev.format = 32;
        xev.data.l[0] = (long) _XA_NET_WM_PING;
        xev.data.l[1] = xapp->getEventTime("sendPing");
        xev.data.l[2] = (long) handle();
        xev.data.l[3] = (long) this;
        xev.data.l[4] = (long) fFrame;
        xapp->send(xev, handle());
        fPinging = true;
        fPingTime = xev.data.l[1];
        fPingTimer->setTimer(3000L, this, true);
        sent = true;
    }
    return sent;
}

bool YFrameClient::handleTimer(YTimer* timer) {
    if (timer == nullptr || timer != fPingTimer) {
        return false;
    }

    fPingTimer = null;
    fPinging = false;

    if (destroyed() || getFrame() == nullptr) {
        return false;
    }

    if (killPid() == false && getFrame()->owner()) {
        getFrame()->owner()->client()->killPid();
    }

    return false;
}

bool YFrameClient::killPid() {
    long pid = 0;
    return getNetWMPid(&pid) && 0 < pid && 0 == kill(pid, SIGTERM);
}

bool YFrameClient::getNetWMPid(long *pid) {
    *pid = 0;

    if (!prop.net_wm_pid)
        return false;

    if (fPid > 0) {
        *pid = fPid;
        return true;
    }

    YProperty prop(this, _XA_NET_WM_PID, F32, 1, XA_CARDINAL);
    if (prop) {
        YTextProperty text(nullptr);
        if (XGetWMClientMachine(xapp->display(), handle(), &text)) {
            char myhost[HOST_NAME_MAX + 1] = {};
            gethostname(myhost, HOST_NAME_MAX);
            int len = (int) strnlen(myhost, HOST_NAME_MAX);
            char* theirs = (char *) text.value;
            if (strncmp(myhost, theirs, len) == 0 &&
                (theirs[len] == 0 || theirs[len] == '.'))
            {
                *pid = fPid = *prop;
            }
        }
    }

    return fPid > 0 && fPid == *pid;
}

void YFrameClient::recvPing(const XClientMessageEvent &message) {
    const long* l = message.data.l;
    if (fPinging &&
        l[0] == (long) _XA_NET_WM_PING &&
        l[1] == fPingTime &&
        l[2] == (long) handle() &&
        // l[3] == (long) this &&
        // l[4] == (long) fFrame &&
        l[0] && l[1] && l[2])
    {
        fPinging = false;
        fPingTime = xapp->getEventTime("recvPing");
        fPingTimer = null;
    }
}

void YFrameClient::setFrame(YFrameWindow *newFrame) {
    if (newFrame != getFrame()) {
        if (getFrame()) {
            frameContext.remove(handle());
        }
        else {
            clientContext.remove(handle());
        }

        fFrame = newFrame;
        if (getFrame()) {
            frameContext.save(handle(), getFrame());
        }
        else {
            clientContext.save(handle(), this);
        }
    }
}

void YFrameClient::setFrameState(FrameState state) {
    if (state == WithdrawnState) {
        if (manager->wmState() != YWindowManager::wmSHUTDOWN) {
            MSG(("deleting window properties id=%lX", handle()));
            Atom atoms[] = {
                _XA_NET_FRAME_EXTENTS, _XA_NET_WM_ALLOWED_ACTIONS,
                _XA_NET_WM_DESKTOP, _XA_NET_WM_STATE,
                _XA_NET_WM_VISIBLE_ICON_NAME, _XA_NET_WM_VISIBLE_NAME,
                _XA_WIN_LAYER, _XA_WM_STATE,
            };
            for (Atom atom : atoms)
                deleteProperty(atom);
            fSavedFrameState = InvalidFrameState;
            fWinStateHint = InvalidFrameState;
        }
    }
    else if (state != fSavedFrameState) {
        Atom iconic = (state == IconicState && getFrame()->isMinimized()
                    && minimizeToDesktop && getFrame()->getMiniIcon())
                    ? getFrame()->getMiniIcon()->iconWindow() : None;
        Atom arg[2] = { Atom(state), iconic };
        setProperty(_XA_WM_STATE, _XA_WM_STATE, arg, 2);
        fSavedFrameState = state;
    }
}

FrameState YFrameClient::getFrameState() {
    if (!prop.wm_state)
        return WithdrawnState;

    fSavedFrameState = InvalidFrameState;

    FrameState st = WithdrawnState;
    YProperty prop(this, _XA_WM_STATE, F32, 2, _XA_WM_STATE);
    if (prop) {
        fSavedFrameState = st = *prop;
    }
    return st;
}

void YFrameClient::handleUnmap(const XUnmapEvent &unmap) {
    YWindow::handleUnmap(unmap);

    MSG(("UnmapWindow"));

    bool unmanage = true;
    bool destroy = false;
    do {
        XEvent ev;
        if (XCheckTypedWindowEvent(xapp->display(), unmap.window,
                                   DestroyNotify, &ev)) {
            YWindow::handleDestroyWindow(ev.xdestroywindow);
            manager->destroyedClient(unmap.window);
            unmanage = false;
        }
        else {
            destroy = (adopted() && destroyed() == false && testDestroyed());
        }
    } while (unmanage && destroy);
    if (unmanage) {
        manager->unmanageClient(this);
    }
}

void YFrameClient::handleProperty(const XPropertyEvent &property) {
    bool new_prop = (property.state != PropertyDelete);

    switch (property.atom) {
    case XA_WM_NAME:
        if (new_prop) prop.wm_name = true;
        getNameHint();
        prop.wm_name = new_prop;
        break;

    case XA_WM_ICON_NAME:
        if (new_prop) prop.wm_icon_name = true;
        getIconNameHint();
        prop.wm_icon_name = false;
        break;

    case XA_WM_CLASS:
        prop.wm_class = new_prop;
        if (prop.wm_class) {
            ClassHint old(fClassHint);
            getClassHint();
            if (fClassHint.nonempty() && fClassHint != old) {
                YFrameWindow* frame = getFrame();
                if (frame){
                    frame->getFrameHints();
                    if (taskBarTaskGrouping) {
                        frame->removeAppStatus();
                        frame->updateAppStatus();
                    }
                }
            }
        }
        break;

    case XA_WM_HINTS:
        if (new_prop) prop.wm_hints = true;
        {
            Drawable oldPix = iconPixmapHint();
            Drawable oldMask = iconMaskHint();
            bool oldUrge = urgencyHint();
            getWMHints();
            if (oldPix != iconPixmapHint() || oldMask != iconMaskHint()) {
                if (getFrame())
                    getFrame()->updateIcon();
            }
            if (oldUrge != urgencyHint()) {
                if (getFrame())
                    getFrame()->setWmUrgency(urgencyHint());
            }
        }
        prop.wm_hints = new_prop;
        break;

    case XA_WM_NORMAL_HINTS:
        if (new_prop) prop.wm_normal_hints = true;
        if (fSizeHints) {
            XSizeHints old(*fSizeHints);
            getSizeHints();
            if (old != *fSizeHints) {
                if (getFrame())
                    getFrame()->updateMwmHints(&old);
            }
        }
        prop.wm_normal_hints = new_prop;
        break;

    case XA_WM_TRANSIENT_FOR:
        if (new_prop) prop.wm_transient_for = true;
        getTransient();
        prop.wm_transient_for = new_prop;
        break;
    default:
        if (property.atom == _XA_WM_PROTOCOLS) {
            if (new_prop) prop.wm_protocols = true;
            getProtocols(false);
            prop.wm_protocols = new_prop;
        } else if (property.atom == _XA_WM_STATE) {
            prop.wm_state = new_prop;
        } else if (property.atom == _XA_KWM_WIN_ICON) {
            if (new_prop) prop.kwm_win_icon = true;
            if (getFrame() && !prop.net_wm_icon && !prop.win_icons)
                getFrame()->updateIcon();
            prop.kwm_win_icon = new_prop;
        } else if (property.atom == _XA_WIN_ICONS) {
            if (new_prop) prop.win_icons = true;
            if (getFrame() && !prop.net_wm_icon)
                getFrame()->updateIcon();
            prop.win_icons = new_prop;
        } else if (property.atom == _XA_NET_WM_NAME) {
            if (new_prop) prop.net_wm_name = true;
            getNetWmName();
            prop.net_wm_name = new_prop;
        } else if (property.atom == _XA_NET_WM_ICON_NAME) {
            if (new_prop) prop.net_wm_icon_name = true;
            getNetWmIconName();
            prop.net_wm_icon_name = new_prop;
        } else if (property.atom == _XA_NET_WM_STRUT) {
            MSG(("change: net wm strut"));
            if (new_prop) prop.net_wm_strut = true;
            if (getFrame())
                getFrame()->updateNetWMStrut();
            prop.net_wm_strut = new_prop;
        } else if (property.atom == _XA_NET_WM_STRUT_PARTIAL) {
            MSG(("change: net wm strut partial"));
            if (new_prop) prop.net_wm_strut_partial = true;
            if (getFrame())
                getFrame()->updateNetWMStrutPartial();
            prop.net_wm_strut_partial = new_prop;
        } else if (property.atom == _XA_NET_WM_USER_TIME) {
            MSG(("change: net wm user time"));
            if (new_prop) prop.net_wm_user_time = true;
            if (getFrame())
                getFrame()->updateNetWMUserTime();
            prop.net_wm_user_time = new_prop;
        } else if (property.atom == _XA_NET_WM_USER_TIME_WINDOW) {
            MSG(("change: net wm user time window"));
            if (new_prop) prop.net_wm_user_time_window = true;
            if (getFrame())
                getFrame()->updateNetWMUserTimeWindow();
            prop.net_wm_user_time_window = new_prop;
        } else if (property.atom == _XA_NET_WM_WINDOW_OPACITY) {
            MSG(("change: net wm window opacity"));
            if (new_prop) prop.net_wm_window_opacity = true;
            if (getFrame())
                getFrame()->updateNetWMWindowOpacity();
        } else if (property.atom == _XA_NET_WM_FULLSCREEN_MONITORS) {
            // ignore - we triggered this event
            // (do i need to set a property here?)
        } else if (property.atom == _XA_NET_WM_ICON) {
            MSG(("change: net wm icon"));
            if (new_prop) prop.net_wm_icon = true;
            if (getFrame())
                getFrame()->updateIcon();
            prop.net_wm_icon = new_prop;
        } else if (property.atom == _XA_WIN_TRAY) {
            prop.win_tray = new_prop;
        } else if (property.atom == _XA_WIN_LAYER) {
            prop.win_layer = new_prop;
        } else if (property.atom == _XATOM_MWM_HINTS) {
            if (new_prop) prop.mwm_hints = true;
            getMwmHints();
            if (getFrame())
                getFrame()->updateMwmHints(fSizeHints);
            prop.mwm_hints = new_prop;
        } else if (property.atom == _XA_WM_CLIENT_LEADER) { // !!! check these
            if (new_prop) prop.wm_client_leader = true;
            getClientLeader();
            prop.wm_client_leader = new_prop;
        } else if (property.atom == _XA_SM_CLIENT_ID) {
            prop.sm_client_id = new_prop;
        } else if (property.atom == _XA_NET_WM_DESKTOP) {
            prop.net_wm_desktop = new_prop;
        } else if (property.atom == _XA_NET_WM_STATE) {
            prop.net_wm_state = new_prop;
        } else if (property.atom == _XA_NET_WM_WINDOW_TYPE) {
            // !!! do we do dynamic update? (discuss on wm-spec)
            prop.net_wm_window_type = new_prop;
        } else if (property.atom == _XA_NET_WM_PID) {
            prop.net_wm_pid = new_prop;
        } else if (property.atom == _XA_NET_WM_VISIBLE_NAME) {
        } else if (property.atom == _XA_NET_WM_VISIBLE_ICON_NAME) {
        } else if (property.atom == _XA_NET_WM_ALLOWED_ACTIONS) {
        } else if (property.atom == _XA_NET_FRAME_EXTENTS) {
        } else {
            MSG(("Unknown property changed: %s, window=0x%lX",
                 XGetAtomName(xapp->display(), property.atom), handle()));
        }
        break;
    }
}

void YFrameClient::handleColormap(const XColormapEvent &colormap) {
    if (colormap.colormap == None ||
        colormap.c_new == True ||
        colormap.state == ColormapInstalled)
    {
        setColormap(colormap.colormap);
    }
    // else if (colormap.state == ColormapUninstalled) {}
}


void YFrameClient::handleDestroyWindow(const XDestroyWindowEvent &destroyWindow) {
    //msg("DESTROY: %lX", destroyWindow.window);
    YWindow::handleDestroyWindow(destroyWindow);

    if (destroyed())
        manager->destroyedClient(destroyWindow.window);
}

#ifdef CONFIG_SHAPE
void YFrameClient::handleShapeNotify(const XShapeEvent &shape) {
    if (shapes.supported) {
        MSG(("shape event: %d %d %d:%d=%dx%d time=%ld",
             shape.shaped, shape.kind,
             shape.x, shape.y, shape.width, shape.height, shape.time));
        if (shape.kind == ShapeBounding) {
            bool const newShaped(shape.shaped);
            if (newShaped)
                fShaped = newShaped;
            if (getFrame())
                getFrame()->setShape();
            if (fShaped && !newShaped) {
                fShaped = newShaped;
                getFrame()->updateMwmHints(fSizeHints);
            }
        }
    }
}
#endif

void YFrameClient::setWindowTitle(const char *title) {
    if (fWindowTitle != title) {
        fWindowTitle = title;
        if (title) {
            XChangeProperty(xapp->display(), handle(),
                    _XA_NET_WM_VISIBLE_NAME, _XA_UTF8_STRING,
                    8, PropModeReplace,
                    (const unsigned char *) title,
                    strlen(title));
        } else {
            XDeleteProperty(xapp->display(), handle(),
                    _XA_NET_WM_VISIBLE_NAME);
        }
        if (getFrame())
            getFrame()->updateTitle();
    }
}

void YFrameClient::setIconTitle(const char *title) {
    if (fIconTitle != title) {
        fIconTitle = title;
        if (title) {
            XChangeProperty(xapp->display(), handle(),
                    _XA_NET_WM_VISIBLE_ICON_NAME, _XA_UTF8_STRING,
                    8, PropModeReplace,
                    (const unsigned char *) title,
                    strlen(title));
        } else {
            XDeleteProperty(xapp->display(), handle(),
                    _XA_NET_WM_VISIBLE_ICON_NAME);
        }
        if (getFrame())
            getFrame()->updateIconTitle();
    }
}

void YFrameClient::setColormap(Colormap cmap) {
    fColormap = cmap;
    if (getFrame() && manager->colormapWindow() == getFrame())
        manager->installColormap(cmap);
}

void YFrameClient::queryShape() {
#ifdef CONFIG_SHAPE
    fShaped = false;

    if (shapes.supported) {
        int xws, yws, xbs, ybs;
        unsigned wws, hws, wbs, hbs;
        Bool boundingShaped = False, clipShaped;

        if (XShapeQueryExtents(xapp->display(), handle(),
                               &boundingShaped, &xws, &yws, &wws, &hws,
                               &clipShaped, &xbs, &ybs, &wbs, &hbs))
        {
            fShaped = boundingShaped;
        }
    }
#endif
}

static long getMask(Atom a) {
    return a == _XA_NET_WM_STATE_ABOVE ? WinStateAbove :
           a == _XA_NET_WM_STATE_BELOW ? WinStateBelow :
           a == _XA_NET_WM_STATE_DEMANDS_ATTENTION ? WinStateUrgent :
           a == _XA_NET_WM_STATE_FOCUSED ? WinStateFocused :
           a == _XA_NET_WM_STATE_FULLSCREEN ? WinStateFullscreen :
           a == _XA_NET_WM_STATE_HIDDEN ? WinStateMinimized :
           a == _XA_NET_WM_STATE_MAXIMIZED_HORZ ? WinStateMaximizedHoriz :
           a == _XA_NET_WM_STATE_MAXIMIZED_VERT ? WinStateMaximizedVert :
           a == _XA_NET_WM_STATE_MODAL ? WinStateModal :
           a == _XA_NET_WM_STATE_SHADED ? WinStateRollup :
           a == _XA_NET_WM_STATE_SKIP_PAGER ? WinStateSkipPager :
           a == _XA_NET_WM_STATE_SKIP_TASKBAR ? WinStateSkipTaskBar :
           a == _XA_NET_WM_STATE_STICKY ? WinStateSticky :
           None;
}

void YFrameClient::setNetWMFullscreenMonitors(int top, int bottom, int left, int right) {
    Atom data[4] = { Atom(top), Atom(bottom), Atom(left), Atom(right) };
    setProperty(_XA_NET_WM_FULLSCREEN_MONITORS, XA_CARDINAL, data, 4);
}

void YFrameClient::setNetFrameExtents(int left, int right, int top, int bottom) {
    Atom data[4] = { Atom(left), Atom(right), Atom(top), Atom(bottom) };
    setProperty(_XA_NET_FRAME_EXTENTS, XA_CARDINAL, data, 4);
}

void YFrameClient::setNetWMAllowedActions(Atom *actions, int count) {
    setProperty(_XA_NET_WM_ALLOWED_ACTIONS, XA_ATOM, actions, count);
}

void YFrameClient::handleClientMessage(const XClientMessageEvent &message) {
    if (message.message_type == _XA_WM_CHANGE_STATE) {
        const long state = message.data.l[0];
        YFrameWindow* frame = getFrame();
        if (state == IconicState && frame &&
            !frame->hasState(WinStateMinimized | WinStateRollup))
        {
            frame->actionPerformed(actionMinimize, None);
        }
        else if (state == NormalState && frame && frame->isUnmapped())
        {
            frame->actionPerformed(actionRestore, None);
        }
        else if (state == WithdrawnState && frame &&
            !frame->hasState(WinStateHidden))
        {
            frame->actionPerformed(actionHide, None);
        }
    } else if (message.message_type == _XA_NET_RESTACK_WINDOW) {
        if (getFrame()) {
            long window = message.data.l[1];
            long detail = message.data.l[2];
            if (inrange<long>(detail, Above, Opposite)) {
                getFrame()->netRestackWindow(window, detail);
            }
        }
    } else if (message.message_type == _XA_NET_ACTIVE_WINDOW) {
        //printf("active window w=0x%lX\n", message.window);
        YFrameWindow* f = getFrame();
        if (f && !f->frameOption(YFrameWindow::foIgnoreActivationMessages)) {
            f->activate();
            f->wmRaise();
        }
    } else if (message.message_type == _XA_NET_CLOSE_WINDOW) {
        actionPerformed(actionClose);
    } else if (message.message_type == _XA_NET_WM_MOVERESIZE &&
               message.format == 32)
    {
        ///YFrameWindow *frame(findFrame(message.window));
        if (getFrame())
            getFrame()->startMoveSize(message.data.l[0], message.data.l[1],
                                      message.data.l[2]);
    } else if (message.message_type == _XA_NET_MOVERESIZE_WINDOW) {
        if (getFrame()) {
            long flag = message.data.l[0];
            long grav = Elvis(int(flag & 0xFF), sizeHints()->win_gravity);
            long mask = ((flag >> 8) & 0x0F);
            long xpos = (mask & 1) ? message.data.l[1] : x();
            long ypos = (mask & 2) ? message.data.l[2] : y();
            long hori = (mask & 4) ? message.data.l[3] : width();
            long vert = (mask & 8) ? message.data.l[4] : height();
            XConfigureRequestEvent conf = { ConfigureRequest, None, };
            conf.value_mask = mask;
            conf.x = xpos;
            conf.y = ypos;
            conf.width = hori;
            conf.height = vert;
            int wing = sizeHints()->win_gravity;
            sizeHints()->win_gravity = grav;
            getFrame()->configureClient(conf);
            sizeHints()->win_gravity = wing;
        }
    } else if (message.message_type == _XA_NET_WM_FULLSCREEN_MONITORS) {
        if (getFrame()) {
            const long* l = message.data.l;
            getFrame()->updateNetWMFullscreenMonitors(l[0], l[1], l[2], l[3]);
        }
    } else if (message.message_type == _XA_NET_WM_STATE) {
        long action = message.data.l[0];
        if (getFrame() && inrange(action, 0L, 2L)) {
            long one = getMask(message.data.l[1]);
            long two = getMask(message.data.l[2]);
            netStateRequest(action, (one | two) &~ WinStateFocused);
        }
    } else if (message.message_type == _XA_WM_PROTOCOLS &&
               message.data.l[0] == long(_XA_NET_WM_PING)) {
        recvPing(message);
    } else if (message.message_type == _XA_NET_WM_DESKTOP) {
        if (getFrame())
            getFrame()->setWorkspace(message.data.l[0]);
        else
            setWorkspaceHint(message.data.l[0]);
    } else if (message.message_type == _XA_WIN_LAYER) {
        long layer = message.data.l[0];
        if (inrange(layer, WinLayerDesktop, WinLayerAboveAll)) {
            if (getFrame())
                getFrame()->actionPerformed(layerActionSet[layer]);
            else
                setLayerHint(layer);
        }
    } else if (message.message_type == _XA_WIN_TRAY) {
        if (getFrame())
            getFrame()->setTrayOption(message.data.l[0]);
        else
            setWinTrayHint(message.data.l[0]);
    } else
        super::handleClientMessage(message);
}

void YFrameClient::netStateRequest(long action, long mask) {
    enum Op { Rem, Add, Tog } act = Op(action);
    long state = getFrame()->getState();
    long gain = (act == Add || act == Tog) ? (mask &~ state) : None;
    long lose = (act == Rem || act == Tog) ? (mask & state) : None;
    if (gain & WinStateUnmapped) {
        if (gain & WinStateMinimized)
            actionPerformed(actionMinimize);
        else if (gain & WinStateRollup)
            actionPerformed(actionRollup);
        else if (gain & WinStateHidden)
            actionPerformed(actionHide);
        gain &= ~(WinStateFullscreen | WinStateMaximizedBoth);
        gain &= ~(WinStateUnmapped);
        lose &= ~(WinStateUnmapped);
        state = getFrame()->getState();
    }
    if (lose & (WinStateUnmapped | WinStateMaximizedBoth)) {
        long drop = (lose & (WinStateUnmapped | WinStateMaximizedBoth));
        if (drop == (state & (WinStateUnmapped | WinStateMaximizedBoth))) {
            actionPerformed(actionRestore);
            lose &= ~drop;
            state = getFrame()->getState();
        }
    }
    if (lose & (WinStateFullscreen | WinStateMaximizedBoth)) {
        long drop = (WinStateFullscreen | WinStateMaximizedBoth);
        if (getFrame()->isUnmapped()) {
            getFrame()->setState(lose & drop, None);
        }
        else {
            if ((lose & WinStateFullscreen) && getFrame()->isFullscreen()) {
                actionPerformed(actionFullscreen);
                state = getFrame()->getState();
            }
            if ((lose & WinStateMaximizedBoth) && getFrame()->isMaximized()) {
                long keep = (state & WinStateMaximizedBoth &~ lose);
                if (keep == WinStateMaximizedVert)
                    actionPerformed(actionMaximizeVert);
                else if (keep == WinStateMaximizedHoriz)
                    actionPerformed(actionMaximizeHoriz);
                else
                    actionPerformed(actionRestore);
                state = getFrame()->getState();
            }
        }
        lose &= ~drop;
    }
    if (lose & WinStateUnmapped) {
        if (getFrame()->isMaximized() == false)
            actionPerformed(actionRestore);
        else if ((state & WinStateUnmapped) == WinStateRollup)
            actionPerformed(actionRollup);
        else if ((state & WinStateUnmapped) == WinStateMinimized)
            actionPerformed(actionMinimize);
        else if ((state & WinStateUnmapped) == WinStateHidden)
            actionPerformed(actionHide);
        else
            actionPerformed(actionShow);
        lose &= ~(WinStateUnmapped);
    }
    if (gain & (WinStateFullscreen | WinStateMaximizedBoth)) {
        if (gain & WinStateFullscreen) {
            if ( !getFrame()->isFullscreen())
                actionPerformed(actionFullscreen);
        } else {
            long maxi = (gain & WinStateMaximizedBoth);
            long have = (getFrame()->getState() & WinStateMaximizedBoth);
            long want = (maxi | have);
            if (want != have) {
                if (want == WinStateMaximizedBoth)
                    actionPerformed(actionMaximize);
                else if (want == WinStateMaximizedVert)
                    actionPerformed(actionMaximizeVert);
                else if (want == WinStateMaximizedHoriz)
                    actionPerformed(actionMaximizeHoriz);
            }
        }
        gain &= ~(WinStateFullscreen | WinStateMaximizedBoth);
    }
    if (gain & WinStateSticky) {
        if (getFrame()->isAllWorkspaces() == false) {
            actionPerformed(actionOccupyAllOrCurrent);
        }
        gain &= ~WinStateSticky;
    }
    if (lose & WinStateSticky) {
        if (getFrame()->isAllWorkspaces()) {
            actionPerformed(actionOccupyAllOrCurrent);
        }
        lose &= ~WinStateSticky;
    }
    if (gain & (WinStateAbove | WinStateBelow)) {
        if ((gain & (WinStateAbove | WinStateBelow)) == WinStateAbove) {
            actionPerformed(actionLayerOnTop);
        }
        if ((gain & (WinStateAbove | WinStateBelow)) == WinStateBelow) {
            actionPerformed(actionLayerBelow);
        }
        gain &= ~(WinStateAbove | WinStateBelow);
        lose &= ~(WinStateAbove | WinStateBelow);
    }
    if (lose & (WinStateAbove | WinStateBelow)) {
        if (lose & WinStateAbove) {
            if (getFrame()->getRequestedLayer() == WinLayerOnTop) {
                actionPerformed(actionLayerNormal);
            }
        }
        if (lose & WinStateBelow) {
            if (getFrame()->getRequestedLayer() == WinLayerBelow) {
                actionPerformed(actionLayerNormal);
            }
        }
        lose &= ~(WinStateAbove | WinStateBelow);
    }
    if (gain & WinStateUrgent) {
        getFrame()->setWmUrgency(true);
        gain &= ~WinStateUrgent;
    }
    if (lose & WinStateUrgent) {
        getFrame()->setWmUrgency(false);
        lose &= ~WinStateUrgent;
    }
    if (gain || lose) {
        getFrame()->setState(gain | lose, gain);
    }
}

void YFrameClient::actionPerformed(YAction action) {
    if (getFrame()) {
        getFrame()->actionPerformed(action, 0U);
    }
}

void YFrameClient::getNameHint() {
    if (!prop.wm_name)
        return;
    if (prop.net_wm_name)
        return;

    XTextProperty text = { nullptr, None, 0, 0 };
    XGetWMName(xapp->display(), handle(), &text);
    setWindowTitle((char *)text.value);
    XFree(text.value);
}

void YFrameClient::getNetWmName() {
    if (!prop.net_wm_name)
        return;

    XTextProperty text = { nullptr, None, 0, 0 };
    XGetTextProperty(xapp->display(), handle(), &text, _XA_NET_WM_NAME);
    setWindowTitle((char *)text.value);
    XFree(text.value);
}

void YFrameClient::getIconNameHint() {
    if (!prop.wm_icon_name)
        return;
    if (prop.net_wm_icon_name)
        return;

    XTextProperty text = { nullptr, None, 0, 0 };
    XGetWMIconName(xapp->display(), handle(), &text);
    setIconTitle((char *)text.value);
    XFree(text.value);
}

void YFrameClient::getNetWmIconName() {
    if (!prop.net_wm_icon_name)
        return;

    XTextProperty text = { nullptr, None, 0, 0 };
    XGetTextProperty(xapp->display(), handle(), &text, _XA_NET_WM_ICON_NAME);
    setIconTitle((char *)text.value);
    XFree(text.value);
}

void YFrameClient::getWMHints() {
    if (fHints) {
        XFree(fHints);
        fHints = nullptr;
    }

    if (!prop.wm_hints)
        return;

    fHints = XGetWMHints(xapp->display(), handle());
    if (!fClientLeader && windowGroupHint()) {
        fClientLeader = fHints->window_group;
    }
}

Window YFrameClient::windowGroupHint() const {
    return wmHint(WindowGroupHint) ? fHints->window_group : None;
}

Window YFrameClient::iconWindowHint() const {
    return wmHint(IconWindowHint) ? fHints->icon_window : None;
}

Pixmap YFrameClient::iconPixmapHint() const {
    return wmHint(IconPixmapHint) ? fHints->icon_pixmap : None;
}

Pixmap YFrameClient::iconMaskHint() const {
    return wmHint(IconMaskHint) ? fHints->icon_mask : None;
}

void YFrameClient::getMwmHints() {
    if (!prop.mwm_hints)
        return;

    YProperty prop(this, _XATOM_MWM_HINTS, F32,
                   PROP_MWM_HINTS_ELEMENTS, _XATOM_MWM_HINTS);
    if (prop) {
        unsigned long* dest = &fMwmHints->flags;
        for (unsigned i = 0; i < PROP_MWM_HINTS_ELEMENTS; ++i) {
            dest[i] = (i < prop.size()) ? prop.data<unsigned long>()[i] : 0;
        }
    }
    else {
        fMwmHints = null;
    }
}

void YFrameClient::setMwmHints(const MwmHints &mwm) {
    setProperty(_XATOM_MWM_HINTS, _XATOM_MWM_HINTS,
                (const Atom *)&mwm, PROP_MWM_HINTS_ELEMENTS);
    *fMwmHints = mwm;
}

long YFrameClient::mwmFunctions() {
    long functions = ~0U;

    if (fMwmHints && fMwmHints->hasFuncs()) {
        functions = fMwmHints->funcs();
    } else {
        XSizeHints *sh = sizeHints();

        if (sh) {
            bool minmax = false;
            if (sh->min_width == sh->max_width &&
                sh->min_height == sh->max_height)
            {
                functions &= ~MWM_FUNC_RESIZE;
                minmax = true;
            }
            if ((minmax && !(sh->flags & PResizeInc)) ||
                (sh->width_inc == 0 && sh->height_inc == 0))
                functions &= ~MWM_FUNC_MAXIMIZE;
        }
    }
    functions &= (MWM_FUNC_RESIZE | MWM_FUNC_MOVE |
                  MWM_FUNC_MINIMIZE | MWM_FUNC_MAXIMIZE |
                  MWM_FUNC_CLOSE);
    return functions;
}

long YFrameClient::mwmDecors() {
    long decors = ~0U;
    long func = mwmFunctions();

    if (fMwmHints && fMwmHints->hasDecor()) {
        decors = fMwmHints->decor();
    } else {
        XSizeHints *sh = sizeHints();

        if (sh) {
            bool minmax = false;
            if (sh->min_width == sh->max_width &&
                sh->min_height == sh->max_height)
            {
                decors &= ~MWM_DECOR_RESIZEH;
                minmax = true;
            }
            if ((minmax && !(sh->flags & PResizeInc)) ||
                (sh->width_inc == 0 && sh->height_inc == 0))
                decors &= ~MWM_DECOR_MAXIMIZE;
        }
    }
    decors &= (MWM_DECOR_BORDER | MWM_DECOR_RESIZEH |
               MWM_DECOR_TITLE | MWM_DECOR_MENU |
               MWM_DECOR_MINIMIZE | MWM_DECOR_MAXIMIZE);

    /// !!! add disabled buttons
    decors &=
        ~(/*((func & MWM_FUNC_RESIZE) ? 0 : MWM_DECOR_RESIZEH) |*/
          ((func & MWM_FUNC_MINIMIZE) ? 0 : MWM_DECOR_MINIMIZE) |
          ((func & MWM_FUNC_MAXIMIZE) ? 0 : MWM_DECOR_MAXIMIZE));

    return decors;
}

bool YFrameClient::getKwmIcon(long* count, Pixmap** pixmap) {
    *count = 0;
    *pixmap = None;

    if (!prop.kwm_win_icon)
        return false;

    YProperty prop(this, _XA_KWM_WIN_ICON, F32, 2, _XA_KWM_WIN_ICON);
    if (prop && prop.size() == 2) {
        *count = prop.size();
        *pixmap = prop.retrieve<Pixmap>();
        getWMHints();
        return true;
    }
    return false;
}

bool YFrameClient::getWinIcons(Atom* type, long* count, long** elem) {
    *type = None;
    *count = 0;
    *elem = nullptr;

    if (!prop.win_icons)
        return false;

    YProperty prop(this, _XA_WIN_ICONS, F32, 4096);
    if (prop.typed(_XA_WIN_ICONS) || prop.typed(XA_PIXMAP)) {
        *type = prop.type();
        *count = prop.size();
        *elem = prop.retrieve<long>();
        return true;
    }
    return false;
}

bool YFrameClient::getNetWMIcon(long* count, long** elems) {
    *count = 0;
    *elems = nullptr;
    if (prop.net_wm_icon) {
        YProperty prop(this, _XA_NET_WM_ICON, F32, 1L << 22);
        if (prop) {
            if (prop.typed(XA_CARDINAL)) {
                *count = prop.size();
                *elems = prop.retrieve<long>();
            }
            else if (testOnce("_NET_WM_ICON", int(handle()))) {
                TLOG(("Bad _NET_WM_ICON for window 0x%lx: N=%ld, F=%d, T=%s",
                     handle(), prop.size(), F32,
                     XGetAtomName(xapp->display(), prop.type())));
            }
        }
    }
    return (*elems != nullptr);
}

void YFrameClient::setWorkspaceHint(long wk) {
    setProperty(_XA_NET_WM_DESKTOP, XA_CARDINAL, wk);
}

void YFrameClient::setLayerHint(long layer) {
    setProperty(_XA_WIN_LAYER, XA_CARDINAL, layer);
}

bool YFrameClient::getLayerHint(long *layer) {
    if (!prop.win_layer)
        return false;

    YProperty prop(this, _XA_WIN_LAYER, F32, 1, XA_CARDINAL);
    if (prop && inrange(*prop, 0L, WinLayerCount - 1L)) {
        *layer = *prop;
        return true;
    }
    return false;
}

void YFrameClient::setWinTrayHint(long tray_opt) {
    setProperty(_XA_WIN_TRAY, XA_CARDINAL, tray_opt);
}

bool YFrameClient::getWinTrayHint(long* tray_opt) {
    if (!prop.win_tray)
        return false;

    YProperty prop(this, _XA_WIN_TRAY, F32, 1, XA_CARDINAL);
    if (prop && *prop < WinTrayOptionCount) {
        *tray_opt = *prop;
        return true;
    }
    return false;
}

void YFrameClient::setStateHint() {
    long state = getFrame()->getState();
    MSG(("set state 0x%8lX, saved 0x%8lX, win 0x%lx",
          state, fWinStateHint, handle()));

    if (fWinStateHint == state || destroyed()) {
        return;
    } else {
        fWinStateHint = state;
    }

    Atom a[15];
    int i = 0;

    /* the next one is kinda messy */
    if ((state & WinStateMinimized) || (state & WinStateHidden))
        a[i++] = _XA_NET_WM_STATE_HIDDEN;
    else if ((state & WinStateFocused) && !(state & WinStateRollup))
        a[i++] = _XA_NET_WM_STATE_FOCUSED;
    if (state & WinStateSkipPager)
        a[i++] = _XA_NET_WM_STATE_SKIP_PAGER;
    if (state & WinStateSkipTaskBar)
        a[i++] = _XA_NET_WM_STATE_SKIP_TASKBAR;
    if (state & WinStateSticky)
        a[i++] = _XA_NET_WM_STATE_STICKY;

    if (state & WinStateRollup)
        a[i++] = _XA_NET_WM_STATE_SHADED;
    if (state & WinStateAbove)
        a[i++] = _XA_NET_WM_STATE_ABOVE;
    if (state & WinStateBelow)
        a[i++] = _XA_NET_WM_STATE_BELOW;
    if (state & WinStateModal)
        a[i++] = _XA_NET_WM_STATE_MODAL;
    if (state & WinStateFullscreen)
        a[i++] = _XA_NET_WM_STATE_FULLSCREEN;
    if (state & WinStateMaximizedVert)
        a[i++] = _XA_NET_WM_STATE_MAXIMIZED_VERT;
    if (state & WinStateMaximizedHoriz)
        a[i++] = _XA_NET_WM_STATE_MAXIMIZED_HORZ;
    if (state & WinStateUrgent)
        a[i++] = _XA_NET_WM_STATE_DEMANDS_ATTENTION;

    setProperty(_XA_NET_WM_STATE, XA_ATOM, a, i);
}

bool YFrameClient::getNetWMStateHint(long *mask, long *state) {
    long flags = None;
    YProperty prop(this, _XA_NET_WM_STATE, F32, 32, XA_ATOM);
    for (Atom atom : prop) {
        flags |= getMask(atom);
    }
    if (hasbit(flags, WinStateMinimized)) {
        flags &= ~WinStateRollup;
    }
    if (manager->wmState() != YWindowManager::wmSTARTUP) {
        flags &= ~WinStateFocused;
    }
    *mask = flags;
    *state = flags;
    return prop.typed(XA_ATOM);
}

void YFrameClient::setWinHintsHint(long hints) {
    fWinHints = hints;
}

void YFrameClient::getClientLeader() {
    Window leader = windowGroupHint();
    if (prop.wm_client_leader) {
        YProperty prop(this, _XA_WM_CLIENT_LEADER, F32, 1, XA_WINDOW);
        if (prop)
            leader = *prop;
    }
    fClientLeader = leader;
}

void YFrameClient::getWindowRole() {
    if (!prop.wm_window_role && !prop.window_role)
        return;

    Atom atom = prop.wm_window_role ? _XA_WM_WINDOW_ROLE : _XA_WINDOW_ROLE;
    fWindowRole = YProperty(this, atom, F8, 256, XA_STRING).data<char>();
}

mstring YFrameClient::getClientId(Window leader) { /// !!! fix

    if (!prop.sm_client_id)
        return null;

    return YProperty(leader, _XA_SM_CLIENT_ID, F8, 256, XA_STRING).data<char>();
}

bool YFrameClient::getNetWMStrut(int *left, int *right, int *top, int *bottom) {

    if (prop.net_wm_strut_partial)
        return false;

    *left = 0;
    *right = 0;
    *top = 0;
    *bottom = 0;

    if (!prop.net_wm_strut)
        return false;

    YProperty prop(this, _XA_NET_WM_STRUT, F32, 4, XA_CARDINAL);
    if (prop && prop.size() == 4) {
        *left = prop[0];
        *right = prop[1];
        *top = prop[2];
        *bottom = prop[3];
        MSG(("got strut %d, %d, %d, %d", *left, *right, *top, *bottom));
        return true;
    }
    return false;
}

bool YFrameClient::getNetWMStrutPartial(int *left, int *right, int *top, int *bottom)
{
    *left   = 0;
    *right  = 0;
    *top    = 0;
    *bottom = 0;

    if (!prop.net_wm_strut_partial)
        return false;

    YProperty prop(this, _XA_NET_WM_STRUT_PARTIAL, F32, 12, XA_CARDINAL);
    if (prop && prop.size() == 12) {
        *left = prop[0];
        *right = prop[1];
        *top = prop[2];
        *bottom = prop[3];
        MSG(("strut partial %d, %d, %d, %d", *left, *right, *top, *bottom));
        return true;
    }
    return false;
}

bool YFrameClient::getNetStartupId(unsigned long &time) {
    if (!prop.net_startup_id)
        return false;

    YTextProperty id(nullptr);
    if (XGetTextProperty(xapp->display(), handle(), &id, _XA_NET_STARTUP_ID)) {
        char* str = strstr((char *)id.value, "_TIME");
        if (str) {
            time = atol(str + 5) & 0xffffffff;
            if (time == -1UL)
                time = -2UL;
            return true;
        }
    }
    return false;
}

bool YFrameClient::getNetWMUserTime(Window window, unsigned long &time) {
    if (!prop.net_wm_user_time && !prop.net_wm_user_time_window)
        return false;

    YProperty prop(this, _XA_NET_WM_USER_TIME, F32, 1, XA_CARDINAL);
    if (prop) {
        MSG(("got user time"));
        time = *prop & 0xffffffff;
        if (time == -1UL)
            time = -2UL;
        return true;
    }
    return false;
}


bool YFrameClient::getNetWMUserTimeWindow(Window &window) {
    if (!prop.net_wm_user_time_window)
        return false;

    YProperty prop(this, _XA_NET_WM_USER_TIME_WINDOW, F32, 1, XA_WINDOW);
    if (prop) {
        MSG(("got user time window"));
        window = *prop;
        return true;
    }
    return false;
}

bool YFrameClient::getNetWMWindowOpacity(long &opacity) {
    if (!prop.net_wm_window_opacity)
        return false;

    YProperty prop(this, _XA_NET_WM_WINDOW_OPACITY, F32, 1, XA_CARDINAL);
    if (prop) {
        MSG(("got window opacity"));
        opacity = *prop;
        return true;
    }
    return false;
}

bool YFrameClient::getNetWMWindowType(WindowType *window_type) {
    if (!prop.net_wm_window_type)
        return false;

    YProperty prop(this, _XA_NET_WM_WINDOW_TYPE, F32, 16);
    if (prop) {
        struct { Atom atom; WindowType wt; } types[] = {
            { _XA_NET_WM_WINDOW_TYPE_COMBO,         wtCombo },
            { _XA_NET_WM_WINDOW_TYPE_DESKTOP,       wtDesktop },
            { _XA_NET_WM_WINDOW_TYPE_DIALOG,        wtDialog },
            { _XA_NET_WM_WINDOW_TYPE_DND,           wtDND },
            { _XA_NET_WM_WINDOW_TYPE_DOCK,          wtDock },
            { _XA_NET_WM_WINDOW_TYPE_DROPDOWN_MENU, wtDropdownMenu },
            { _XA_NET_WM_WINDOW_TYPE_MENU,          wtMenu },
            { _XA_NET_WM_WINDOW_TYPE_NORMAL,        wtNormal },
            { _XA_NET_WM_WINDOW_TYPE_NOTIFICATION,  wtNotification },
            { _XA_NET_WM_WINDOW_TYPE_POPUP_MENU,    wtPopupMenu },
            { _XA_NET_WM_WINDOW_TYPE_SPLASH,        wtSplash },
            { _XA_NET_WM_WINDOW_TYPE_TOOLBAR,       wtToolbar },
            { _XA_NET_WM_WINDOW_TYPE_TOOLTIP,       wtTooltip, },
            { _XA_NET_WM_WINDOW_TYPE_UTILITY,       wtUtility },
        };
        for (Atom atom : prop) {
            for (auto type : types) {
                if (atom == type.atom) {
                    *window_type = type.wt;
                    return true;
                }
            }
        }
    }
    return false;
}

bool YFrameClient::getNetWMDesktopHint(long *workspace) {
    *workspace = 0;

    if (!prop.net_wm_desktop)
        return false;

    YProperty prop(this, _XA_NET_WM_DESKTOP, F32, 1, XA_CARDINAL);
    if (prop) {
        if (inrange<int>(int(*prop) + 1, 0, workspaceCount)) {
            *workspace = *prop;
            return true;
        }
    }
    return false;
}

void YFrameClient::getPropertiesList() {
    int count;
    Atom *p;

    memset(&prop, 0, sizeof(prop));

    p = XListProperties(xapp->display(), handle(), &count);

#define HAS(x)   ((x) = true)

    if (p) {
        for (int i = 0; i < count; i++) {
            Atom a = p[i];

            if      (a == XA_WM_HINTS) HAS(prop.wm_hints);
            else if (a == XA_WM_NORMAL_HINTS) HAS(prop.wm_normal_hints);
            else if (a == XA_WM_TRANSIENT_FOR) HAS(prop.wm_transient_for);
            else if (a == XA_WM_NAME) HAS(prop.wm_name);
            else if (a == _XA_NET_WM_NAME) HAS(prop.net_wm_name);
            else if (a == XA_WM_ICON_NAME) HAS(prop.wm_icon_name);
            else if (a == _XA_NET_WM_ICON_NAME) HAS(prop.net_wm_icon_name);
            else if (a == _XA_NET_WM_ICON) HAS(prop.net_wm_icon);
            else if (a == XA_WM_CLASS) HAS(prop.wm_class);
            else if (a == _XA_WM_PROTOCOLS) HAS(prop.wm_protocols);
            else if (a == _XA_WM_CLIENT_LEADER) HAS(prop.wm_client_leader);
            else if (a == _XA_WM_WINDOW_ROLE) HAS(prop.wm_window_role);
            else if (a == _XA_WINDOW_ROLE) HAS(prop.window_role);
            else if (a == _XA_SM_CLIENT_ID) HAS(prop.sm_client_id);
            else if (a == _XATOM_MWM_HINTS) HAS(prop.mwm_hints);
            else if (a == _XA_KWM_WIN_ICON) HAS(prop.kwm_win_icon);
            else if (a == _XA_KDE_NET_WM_SYSTEM_TRAY_WINDOW_FOR) HAS(prop.kde_net_wm_system_tray_window_for);
            else if (a == _XA_NET_WM_STRUT) HAS(prop.net_wm_strut);
            else if (a == _XA_NET_WM_STRUT_PARTIAL) HAS(prop.net_wm_strut_partial);
            else if (a == _XA_NET_WM_DESKTOP) HAS(prop.net_wm_desktop);
            else if (a == _XA_NET_WM_PID) HAS(prop.net_wm_pid);
            else if (a == _XA_NET_WM_STATE) HAS(prop.net_wm_state);
            else if (a == _XA_NET_WM_WINDOW_TYPE) HAS(prop.net_wm_window_type);
            else if (a == _XA_NET_STARTUP_ID) HAS(prop.net_startup_id);
            else if (a == _XA_NET_WM_USER_TIME) HAS(prop.net_wm_user_time);
            else if (a == _XA_NET_WM_USER_TIME_WINDOW) HAS(prop.net_wm_user_time_window);
            else if (a == _XA_NET_WM_WINDOW_OPACITY) HAS(prop.net_wm_window_opacity);
            else if (a == _XA_WIN_TRAY) HAS(prop.win_tray);
            else if (a == _XA_WIN_LAYER) HAS(prop.win_layer);
            else if (a == _XA_WIN_ICONS) HAS(prop.win_icons);
            else if (a == _XA_XEMBED_INFO) HAS(prop.xembed_info);
#ifdef DEBUG
            else {
                MSG(("unknown atom: %s", XGetAtomName(xapp->display(), a)));
            }
#endif
#undef HAS
        }
        XFree(p);
    }
}

void YFrameClient::handleGravityNotify(const XGravityEvent &gravity) {
    int ox = x(), oy = y();
    YWindow::handleGravityNotify(gravity);
    if ((gravity.x < 0 || gravity.y < 0) && ox >= 0 && oy >= 0) {
        int nx = max(0, x()), ny = max(0, y());
        MSG(("gravity notify %+d%+d -> %+d%+d -> %+d%+d",
                    ox, oy, gravity.x, gravity.y, nx, ny));
        XMoveWindow(xapp->display(), handle(), nx, ny);
    }
}

bool ClassHint::match(const char* resource) const {
    if (isEmpty(resource))
        return false;
    if (*resource != '.') {
        if (isEmpty(res_name))
            return false;
        size_t len(strlen(res_name));
        if (strncmp(res_name, resource, len))
            return false;
        if (resource[len] == 0)
            return true;
        if (resource[len] != '.')
            return false;
        resource += len;
    }
    return 0 == strcmp(1 + resource, res_class ? res_class : "");
}

char* ClassHint::resource() const {
    mstring str(res_name, ".", res_class);
    return str == "." ? nullptr : strdup(str);
}

// vim: set sw=4 ts=4 et:
