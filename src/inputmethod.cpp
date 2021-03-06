/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2016 Martin Gräßlin <mgraesslin@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "inputmethod.h"
#include "abstract_client.h"
#include "virtualkeyboard_dbus.h"
#include "input.h"
#include "keyboard_input.h"
#include "utils.h"
#include "screens.h"
#include "wayland_server.h"
#include "workspace.h"
#include "screenlockerwatcher.h"
#include "deleted.h"

#include <KWaylandServer/display.h>
#include <KWaylandServer/keyboard_interface.h>
#include <KWaylandServer/seat_interface.h>
#include <KWaylandServer/textinput_v3_interface.h>
#include <KWaylandServer/surface_interface.h>
#include <KWaylandServer/inputmethod_v1_interface.h>

#include <KStatusNotifierItem>
#include <KLocalizedString>

#include <QDBusConnection>
#include <QDBusPendingCall>
#include <QDBusMessage>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

using namespace KWaylandServer;

namespace KWin
{

KWIN_SINGLETON_FACTORY(InputMethod)

InputMethod::InputMethod(QObject *parent)
    : QObject(parent)
{
    // this is actually too late. Other processes are started before init,
    // so might miss the availability of text input
    // but without Workspace we don't have the window listed at all
    connect(kwinApp(), &Application::workspaceCreated, this, &InputMethod::init);
}

InputMethod::~InputMethod() = default;

void InputMethod::init()
{
    connect(ScreenLockerWatcher::self(), &ScreenLockerWatcher::aboutToLock, this, &InputMethod::hide);

    if (waylandServer()) {
        m_enabled = !input()->hasAlphaNumericKeyboard();
        qCDebug(KWIN_VIRTUALKEYBOARD) << "enabled by default: " << m_enabled;
        connect(input(), &InputRedirection::hasAlphaNumericKeyboardChanged, this,
            [this] (bool set) {
                qCDebug(KWIN_VIRTUALKEYBOARD) << "AlphaNumeric Keyboard changed:" << set << "toggling virtual keyboard.";
                setEnabled(!set);
            }
        );
    }

    qCDebug(KWIN_VIRTUALKEYBOARD) << "Registering the SNI";
    m_sni = new KStatusNotifierItem(QStringLiteral("kwin-virtual-keyboard"), this);
    m_sni->setStandardActionsEnabled(false);
    m_sni->setCategory(KStatusNotifierItem::Hardware);
    m_sni->setStatus(KStatusNotifierItem::Passive);
    m_sni->setTitle(i18n("Virtual Keyboard"));
    updateSni();
    connect(m_sni, &KStatusNotifierItem::activateRequested, this,
        [this] {
            setEnabled(!m_enabled);
        }
    );
    connect(this, &InputMethod::enabledChanged, this, &InputMethod::updateSni);

    new VirtualKeyboardDBus(this);
    qCDebug(KWIN_VIRTUALKEYBOARD) << "Registering the DBus interface";
    connect(input(), &InputRedirection::keyStateChanged, this, &InputMethod::hide);

    if (waylandServer()) {
        new TextInputManagerV2Interface(waylandServer()->display());
        new TextInputManagerV3Interface(waylandServer()->display());

        connect(workspace(), &Workspace::clientAdded, this, &InputMethod::clientAdded);
        connect(waylandServer()->seat(), &SeatInterface::focusedTextInputSurfaceChanged, this, &InputMethod::handleFocusedSurfaceChanged);

        TextInputV2Interface *textInputV2 = waylandServer()->seat()->textInputV2();
        connect(textInputV2, &TextInputV2Interface::requestShowInputPanel, this, &InputMethod::show);
        connect(textInputV2, &TextInputV2Interface::requestHideInputPanel, this, &InputMethod::hide);
        connect(textInputV2, &TextInputV2Interface::surroundingTextChanged, this, &InputMethod::surroundingTextChanged);
        connect(textInputV2, &TextInputV2Interface::contentTypeChanged, this, &InputMethod::contentTypeChanged);
        connect(textInputV2, &TextInputV2Interface::stateUpdated, this, &InputMethod::textInputInterfaceV2StateUpdated);

        TextInputV3Interface *textInputV3 = waylandServer()->seat()->textInputV3();
        connect(textInputV3, &TextInputV3Interface::surroundingTextChanged, this, &InputMethod::surroundingTextChanged);
        connect(textInputV3, &TextInputV3Interface::contentTypeChanged, this, &InputMethod::contentTypeChanged);
        connect(textInputV3, &TextInputV3Interface::stateCommitted, this, &InputMethod::stateCommitted);

        if (m_enabled) {
            connect(textInputV2, &TextInputV2Interface::enabledChanged, this, &InputMethod::textInputInterfaceV2EnabledChanged);
            connect(textInputV3, &TextInputV3Interface::enabledChanged, this, &InputMethod::textInputInterfaceV3EnabledChanged);
        }
    }
}

void InputMethod::show()
{
    setActive(true);
}

void InputMethod::hide()
{
    setActive(false);
}

void InputMethod::setActive(bool active)
{
    if (m_active) {
        waylandServer()->inputMethod()->sendDeactivate();
    }

    if (active) {
        if (!m_enabled) {
            return;
        }

        waylandServer()->inputMethod()->sendActivate();
        if (m_active) {
            adoptInputMethodContext();
        }
    } else {
        updateInputPanelState();
    }

    if (m_active != active) {
        m_active = active;
        Q_EMIT activeChanged(active);
    }
}

void InputMethod::clientAdded(AbstractClient* client)
{
    if (!client->isInputMethod()) {
        return;
    }
    m_inputClient = client;
    auto refreshFrame = [this] {
        if (!m_trackedClient) {
            return;
        }

        if (m_inputClient && !m_inputClient->inputGeometry().isEmpty()) {
            m_trackedClient->setVirtualKeyboardGeometry(m_inputClient->inputGeometry());
        }
    };
    connect(client->surface(), &SurfaceInterface::inputChanged, this, refreshFrame);
    connect(client, &QObject::destroyed, this, [this] {
        if (m_trackedClient) {
            m_trackedClient->setVirtualKeyboardGeometry({});
        }
    });
    connect(m_inputClient, &AbstractClient::frameGeometryChanged, this, refreshFrame);
    // Current code have a assumption that InputMethod started by the kwin is virtual keyboard,
    // InputMethod::hide sends out a deactivate signal to input-method client, this is not desired
    // when we support input methods like ibus which can show and hide surfaces/windows as they please
    // and are not exactly Virtual keyboards.
    connect(m_inputClient, &AbstractClient::windowHidden, this, &InputMethod::hide);
    connect(m_inputClient, &AbstractClient::windowClosed, this, &InputMethod::hide);
}

void InputMethod::handleFocusedSurfaceChanged()
{
    SurfaceInterface *focusedSurface = waylandServer()->seat()->focusedTextInputSurface();
    if (focusedSurface) {
        AbstractClient *focusedClient = waylandServer()->findClient(focusedSurface);
        // Reset the old client virtual keybaord geom if necessary
        // Old and new clients could be the same if focus moves between subsurfaces
        if (m_trackedClient != focusedClient) {
            if (m_trackedClient) {
                m_trackedClient->setVirtualKeyboardGeometry(QRect());
            }
            m_trackedClient = focusedClient;
        }
        updateInputPanelState();
    } else {
        setActive(false);
    }
}

void InputMethod::surroundingTextChanged()
{
    auto t2 = waylandServer()->seat()->textInputV2();
    auto t3 = waylandServer()->seat()->textInputV3();
    auto inputContext = waylandServer()->inputMethod()->context();
    if (!inputContext) {
        return;
    }
    if (t2 && t2->isEnabled()) {
        inputContext->sendSurroundingText(t2->surroundingText(), t2->surroundingTextCursorPosition(), t2->surroundingTextSelectionAnchor());
        return;
    }
    if (t3 && t3->isEnabled()) {
        inputContext->sendSurroundingText(t3->surroundingText(), t3->surroundingTextCursorPosition(), t3->surroundingTextSelectionAnchor());
        return;
    }
}

void InputMethod::contentTypeChanged()
{
    auto t2 = waylandServer()->seat()->textInputV2();
    auto t3 = waylandServer()->seat()->textInputV3();
    auto inputContext = waylandServer()->inputMethod()->context();
    if (!inputContext) {
        return;
    }
    if (t2 && t2->isEnabled()) {
        inputContext->sendContentType(t2->contentHints(), t2->contentPurpose());
    }
    if (t3 && t3->isEnabled()) {
        inputContext->sendContentType(t3->contentHints(), t3->contentPurpose());
    }
}

void InputMethod::textInputInterfaceV2StateUpdated(quint32 serial, KWaylandServer::TextInputV2Interface::UpdateReason reason)
{
    auto t2 = waylandServer()->seat()->textInputV2();
    auto inputContext = waylandServer()->inputMethod()->context();
    if (!inputContext) {
        return;
    }
    if (!t2 || !t2->isEnabled()) {
        return;
    }
    switch (reason) {
    case KWaylandServer::TextInputV2Interface::UpdateReason::StateChange:
        inputContext->sendCommitState(serial);
        break;
    case KWaylandServer::TextInputV2Interface::UpdateReason::StateEnter:
        waylandServer()->inputMethod()->sendActivate();
        inputContext->sendCommitState(serial);
        break;
    case KWaylandServer::TextInputV2Interface::UpdateReason::StateFull:
        adoptInputMethodContext();
        inputContext->sendCommitState(serial);
        break;
    case KWaylandServer::TextInputV2Interface::UpdateReason::StateReset:
        inputContext->sendReset();
        inputContext->sendCommitState(serial);
        break;
    }
}

void InputMethod::textInputInterfaceV2EnabledChanged()
{
    auto t = waylandServer()->seat()->textInputV2();
    if (t->isEnabled()) {
        waylandServer()->inputMethod()->sendActivate();
        adoptInputMethodContext();
    } else {
        hide();
    }
}

void InputMethod::textInputInterfaceV3EnabledChanged()
{
    auto t3 = waylandServer()->seat()->textInputV3();
    if (t3->isEnabled()) {
        waylandServer()->inputMethod()->sendActivate();
    } else {
        waylandServer()->inputMethod()->sendDeactivate();
        // reset value of preedit when textinput is disabled
        preedit.text = QString();
        preedit.begin = 0;
        preedit.end = 0;
    }
    auto context = waylandServer()->inputMethod()->context();
    if (context) {
        context->sendReset();
        adoptInputMethodContext();
    }
}

void InputMethod::stateCommitted(uint32_t serial)
{
    auto inputContext = waylandServer()->inputMethod()->context();
    if (!inputContext) {
        return;
    }
    inputContext->sendCommitState(serial);
}

void InputMethod::setEnabled(bool enabled)
{
    if (m_enabled == enabled) {
        return;
    }
    m_enabled = enabled;
    emit enabledChanged(m_enabled);

    // send OSD message
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.kde.plasmashell"),
        QStringLiteral("/org/kde/osdService"),
        QStringLiteral("org.kde.osdService"),
        QStringLiteral("virtualKeyboardEnabledChanged")
    );
    msg.setArguments({enabled});
    QDBusConnection::sessionBus().asyncCall(msg);

    auto textInputV2 = waylandServer()->seat()->textInputV2();
    auto textInputV3 = waylandServer()->seat()->textInputV3();
    if (m_enabled) {
        connect(textInputV2, &TextInputV2Interface::enabledChanged, this, &InputMethod::textInputInterfaceV2EnabledChanged, Qt::UniqueConnection);
        connect(textInputV3, &TextInputV3Interface::enabledChanged, this, &InputMethod::textInputInterfaceV3EnabledChanged, Qt::UniqueConnection);
    } else {
        hide();

        disconnect(textInputV2, &TextInputV2Interface::enabledChanged, this, &InputMethod::textInputInterfaceV2EnabledChanged);
        disconnect(textInputV3, &TextInputV3Interface::enabledChanged, this, &InputMethod::textInputInterfaceV3EnabledChanged);
    }
}

static quint32 keysymToKeycode(quint32 sym)
{
    switch(sym) {
    case XKB_KEY_BackSpace:
        return KEY_BACKSPACE;
    case XKB_KEY_Return:
        return KEY_ENTER;
    case XKB_KEY_Left:
        return KEY_LEFT;
    case XKB_KEY_Right:
        return KEY_RIGHT;
    case XKB_KEY_Up:
        return KEY_UP;
    case XKB_KEY_Down:
        return KEY_DOWN;
    default:
        return KEY_UNKNOWN;
    }
}

void InputMethod::keysymReceived(quint32 serial, quint32 time, quint32 sym, bool pressed, Qt::KeyboardModifiers modifiers)
{
    Q_UNUSED(serial)
    Q_UNUSED(time)
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        if (pressed) {
            t2->keysymPressed(sym, modifiers);
        } else {
            t2->keysymReleased(sym, modifiers);
        }
        return;
    }
    auto t3 = waylandServer()->seat()->textInputV3();
    if (t3 && t3->isEnabled()) {
        KWaylandServer::KeyboardKeyState state;
        if (pressed) {
            state = KWaylandServer::KeyboardKeyState::Pressed;
        } else {
            state = KWaylandServer::KeyboardKeyState::Released;
        }
        waylandServer()->seat()->notifyKeyboardKey(keysymToKeycode(sym), state);
        return;
    }
}

void InputMethod::commitString(qint32 serial, const QString &text)
{
    Q_UNUSED(serial)
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        t2->commitString(text.toUtf8());
        t2->preEdit({}, {});
        return;
    }
    auto t3 = waylandServer()->seat()->textInputV3();
    if (t3 && t3->isEnabled()) {
        t3->commitString(text.toUtf8());
        t3->done();
        return;
    }
}

void InputMethod::deleteSurroundingText(int32_t index, uint32_t length)
{
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        t2->deleteSurroundingText(index, length);
    }
    auto t3 = waylandServer()->seat()->textInputV3();
    if (t3 && t3->isEnabled()) {
        t3->deleteSurroundingText(index, length);
    }
}

void InputMethod::setCursorPosition(qint32 index, qint32 anchor)
{
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        t2->setCursorPosition(index, anchor);
    }
}

void InputMethod::setLanguage(uint32_t serial, const QString &language)
{
    Q_UNUSED(serial)
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        t2->setLanguage(language.toUtf8());
    }
}

void InputMethod::setTextDirection(uint32_t serial, Qt::LayoutDirection direction)
{
    Q_UNUSED(serial)
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        t2->setTextDirection(direction);
    }
}

void InputMethod::setPreeditCursor(qint32 index)
{
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        t2->setPreEditCursor(index);
    }
    auto t3 = waylandServer()->seat()->textInputV3();
    if (t3 && t3->isEnabled()) {
        preedit.begin = index;
        preedit.end = index;
        t3->sendPreEditString(preedit.text, preedit.begin, preedit.end);
    }
}


void InputMethod::setPreeditString(uint32_t serial, const QString &text, const QString &commit)
{
    Q_UNUSED(serial)
    auto t2 = waylandServer()->seat()->textInputV2();
    if (t2 && t2->isEnabled()) {
        t2->preEdit(text.toUtf8(), commit.toUtf8());
    }
    auto t3 = waylandServer()->seat()->textInputV3();
    if (t3 && t3->isEnabled()) {
        preedit.text = text;
        t3->sendPreEditString(preedit.text, preedit.begin, preedit.end);
    }
}

void InputMethod::adoptInputMethodContext()
{
    auto inputContext = waylandServer()->inputMethod()->context();

    TextInputV2Interface *t2 = waylandServer()->seat()->textInputV2();
    TextInputV3Interface *t3 = waylandServer()->seat()->textInputV3();

    if (t2 && t2->isEnabled()) {
        inputContext->sendSurroundingText(t2->surroundingText(), t2->surroundingTextCursorPosition(), t2->surroundingTextSelectionAnchor());
        inputContext->sendPreferredLanguage(t2->preferredLanguage());
        inputContext->sendContentType(t2->contentHints(), t2->contentPurpose());
        connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::language, this, &InputMethod::setLanguage);
        connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::textDirection, this, &InputMethod::setTextDirection);
    }

    if (t3 && t3->isEnabled()) {
        inputContext->sendSurroundingText(t3->surroundingText(), t3->surroundingTextCursorPosition(), t3->surroundingTextSelectionAnchor());
        inputContext->sendContentType(t3->contentHints(), t3->contentPurpose());
    }

    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::keysym, this, &InputMethod::keysymReceived, Qt::UniqueConnection);
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::commitString, this, &InputMethod::commitString, Qt::UniqueConnection);
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::deleteSurroundingText, this, &InputMethod::deleteSurroundingText, Qt::UniqueConnection);
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::cursorPosition, this, &InputMethod::setCursorPosition, Qt::UniqueConnection);
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::preeditString, this, &InputMethod::setPreeditString, Qt::UniqueConnection);
    connect(inputContext, &KWaylandServer::InputMethodContextV1Interface::preeditCursor, this, &InputMethod::setPreeditCursor, Qt::UniqueConnection);
}

void InputMethod::updateSni()
{
    if (!m_sni) {
        return;
    }
    if (m_enabled) {
        m_sni->setIconByName(QStringLiteral("input-keyboard-virtual-on"));
        m_sni->setTitle(i18n("Virtual Keyboard: enabled"));
    } else {
        m_sni->setIconByName(QStringLiteral("input-keyboard-virtual-off"));
        m_sni->setTitle(i18n("Virtual Keyboard: disabled"));
    }
    m_sni->setToolTipTitle(i18n("Whether to show the virtual keyboard on demand."));
}

void InputMethod::updateInputPanelState()
{
    if (!waylandServer()) {
        return;
    }

    auto t = waylandServer()->seat()->textInputV2();

    if (!t) {
        return;
    }

    if (m_trackedClient) {
        m_trackedClient->setVirtualKeyboardGeometry(m_inputClient ? m_inputClient->inputGeometry() : QRect());
    }
    t->setInputPanelState(m_inputClient && m_inputClient->isShown(false), QRect(0, 0, 0, 0));
}

}
