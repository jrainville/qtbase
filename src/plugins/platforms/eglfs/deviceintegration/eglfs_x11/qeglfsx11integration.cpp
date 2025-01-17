// Copyright (C) 2016 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only

#include "qeglfsx11integration.h"
#include <QThread>

#include <X11/Xlib.h>
#include <X11/Xlib-xcb.h>

/* Make no mistake: This is not a replacement for the xcb platform plugin.
   This here is barely an extremely useful tool for developing eglfs itself because
   it allows to do so without any requirements for devices or drivers. */

QT_BEGIN_NAMESPACE

class EventReader : public QThread
{
public:
    EventReader(QEglFSX11Integration *integration)
        : m_integration(integration) { }

    void run() override;

private:
    QEglFSX11Integration *m_integration;
};

static QBasicAtomicInt running;

void EventReader::run()
{
    xcb_generic_event_t *event = nullptr;
    while (running.loadRelaxed() && (event = xcb_wait_for_event(m_integration->connection()))) {
        uint response_type = event->response_type & ~0x80;
        switch (response_type) {
        case XCB_CLIENT_MESSAGE: {
            xcb_client_message_event_t *client = (xcb_client_message_event_t *) event;
            const xcb_atom_t *atoms = m_integration->atoms();
            if (client->format == 32
                && client->type == atoms[Atoms::WM_PROTOCOLS]
                && client->data.data32[0] == atoms[Atoms::WM_DELETE_WINDOW]) {
                QWindow *window = m_integration->platformWindow() ? m_integration->platformWindow()->window() : nullptr;
                if (window)
                    QWindowSystemInterface::handleCloseEvent(window);
            }
            break;
            }
        default:
            break;
        }
    }
}

void QEglFSX11Integration::sendConnectionEvent(xcb_atom_t a)
{
    xcb_client_message_event_t event;
    memset(&event, 0, sizeof(event));

    event.response_type = XCB_CLIENT_MESSAGE;
    event.format = 32;
    event.sequence = 0;
    event.window = m_connectionEventListener;
    event.type = a;

    xcb_send_event(m_connection, false, m_connectionEventListener, XCB_EVENT_MASK_NO_EVENT, (const char *)&event);
    xcb_flush(m_connection);
}

#define DISPLAY ((Display *) m_display)

void QEglFSX11Integration::platformInit()
{
    m_display = XOpenDisplay(nullptr);
    if (Q_UNLIKELY(!m_display))
        qFatal("Could not open display");

    XSetEventQueueOwner(DISPLAY, XCBOwnsEventQueue);
    m_connection = XGetXCBConnection(DISPLAY);

    running.ref();

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(m_connection));

    m_connectionEventListener = xcb_generate_id(m_connection);
    xcb_create_window(m_connection, XCB_COPY_FROM_PARENT,
                      m_connectionEventListener, it.data->root,
                      0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY,
                      it.data->root_visual, 0, nullptr);

    m_eventReader = new EventReader(this);
    m_eventReader->start();
}

void QEglFSX11Integration::platformDestroy()
{
    running.deref();

    sendConnectionEvent(XCB_ATOM_NONE);

    m_eventReader->wait();
    delete m_eventReader;
    m_eventReader = nullptr;

    XCloseDisplay(DISPLAY);
    m_display = nullptr;
    m_connection = nullptr;
}

EGLNativeDisplayType QEglFSX11Integration::platformDisplay() const
{
    return DISPLAY;
}

QSize QEglFSX11Integration::screenSize() const
{
    if (m_screenSize.isEmpty()) {
        QList<QByteArray> env = qgetenv("EGLFS_X11_SIZE").split('x');
        if (env.length() == 2) {
            m_screenSize = QSize(env.at(0).toInt(), env.at(1).toInt());
        } else {
            XWindowAttributes a;
            if (XGetWindowAttributes(DISPLAY, DefaultRootWindow(DISPLAY), &a))
                m_screenSize = QSize(a.width, a.height);
        }
    }
    return m_screenSize;
}

EGLNativeWindowType QEglFSX11Integration::createNativeWindow(QPlatformWindow *platformWindow,
                                                       const QSize &size,
                                                       const QSurfaceFormat &format)
{
    Q_UNUSED(format);

    m_platformWindow = platformWindow;

    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
    m_window = xcb_generate_id(m_connection);
    xcb_create_window(m_connection, XCB_COPY_FROM_PARENT, m_window, it.data->root,
                      0, 0, size.width(), size.height(), 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, it.data->root_visual,
                      0, nullptr);

    xcb_intern_atom_cookie_t cookies[Atoms::N_ATOMS];
    static const char *atomNames[Atoms::N_ATOMS] = {
        "_NET_WM_NAME",
        "UTF8_STRING",
        "WM_PROTOCOLS",
        "WM_DELETE_WINDOW",
        "_NET_WM_STATE",
        "_NET_WM_STATE_FULLSCREEN"
    };

    for (int i = 0; i < Atoms::N_ATOMS; ++i) {
        cookies[i] = xcb_intern_atom(m_connection, false, strlen(atomNames[i]), atomNames[i]);
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(m_connection, cookies[i], nullptr);
        m_atoms[i] = reply->atom;
        free(reply);
    }

    // Set window title
    xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_window,
                        m_atoms[Atoms::_NET_WM_NAME], m_atoms[Atoms::UTF8_STRING], 8, 5, "EGLFS");

    // Enable WM_DELETE_WINDOW
    xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_window,
                        m_atoms[Atoms::WM_PROTOCOLS], XCB_ATOM_ATOM, 32, 1, &m_atoms[Atoms::WM_DELETE_WINDOW]);

    // Go fullscreen.
    xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, m_window,
                        m_atoms[Atoms::_NET_WM_STATE], XCB_ATOM_ATOM, 32, 1, &m_atoms[Atoms::_NET_WM_STATE_FULLSCREEN]);

    xcb_map_window(m_connection, m_window);

    xcb_flush(m_connection);

    return qt_egl_cast<EGLNativeWindowType>(m_window);
}

void QEglFSX11Integration::destroyNativeWindow(EGLNativeWindowType window)
{
    xcb_destroy_window(m_connection, qt_egl_cast<xcb_window_t>(window));
}

bool QEglFSX11Integration::hasCapability(QPlatformIntegration::Capability cap) const
{
    Q_UNUSED(cap);
    return false;
}

QT_END_NAMESPACE
