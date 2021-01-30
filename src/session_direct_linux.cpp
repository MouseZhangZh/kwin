/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "session_direct.h"
#include "utils.h"

#include <QScopeGuard>

#include <cerrno>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef KDSKBMUTE
#define KDSKBMUTE 0x4B51
#endif

namespace KWin
{

DirectSession *DirectSession::create(QObject *parent)
{
    DirectSession *session = new DirectSession(parent);
    if (session->setupTerminal()) {
        return session;
    }

    delete session;
    return nullptr;
}

DirectSession::DirectSession(QObject *parent)
    : Session(parent)
{
    const QString seat = qEnvironmentVariable("XDG_SEAT");
    if (!seat.isEmpty()) {
        m_seat = seat;
    } else {
        m_seat = QStringLiteral("seat0");
    }
}

DirectSession::~DirectSession()
{
    if (m_ttyFd == -1) {
        return;
    }
    restoreTerminal();
    close(m_ttyFd);
    close(m_signalNotifier->socket());
}

bool DirectSession::setupTerminal()
{
    if (m_seat != QStringLiteral("seat0")) {
        qCDebug(KWIN_CORE) << "Skipping VT initialization";
        return true;
    }

    const char *ttyPath = "/dev/tty";

    int fd = open(ttyPath, O_RDWR | O_CLOEXEC);
    if (fd == -1) {
        qCWarning(KWIN_CORE, "Cannot open %s: %s", ttyPath, strerror(errno));
        return false;
    }
    auto cleanup = qScopeGuard([&fd]() { close(fd); });

    vt_stat virtualTerminalStat;
    if (ioctl(fd, VT_GETSTATE, &virtualTerminalStat)) {
        qCWarning(KWIN_CORE) << "Failed to get current tty number";
        return false;
    }

    m_terminal = virtualTerminalStat.v_active;

    int kdMode;
    if (ioctl(fd, KDGETMODE, &kdMode)) {
        qCWarning(KWIN_CORE, "Failed to get the keyboard mode: %s", strerror(errno));
        return false;
    }
    if (kdMode != KD_TEXT) {
        qCWarning(KWIN_CORE) << "tty is already in graphics mode";
    }

    ioctl(fd, VT_ACTIVATE, m_terminal);
    ioctl(fd, VT_WAITACTIVE, m_terminal);

    if (ioctl(fd, KDGKBMODE, &m_keyboardMode)) {
        qCWarning(KWIN_CORE, "Failed to read keyboard mode: %s", strerror(errno));
        return false;
    }

    if (ioctl(fd, KDSKBMUTE, 1) && ioctl(fd, KDSKBMODE, K_OFF)) {
        qCWarning(KWIN_CORE, "Failed to set K_OFF keyboard mode: %s", strerror(errno));
        return false;
    }

    if (ioctl(fd, KDSETMODE, KD_GRAPHICS)) {
        qCWarning(KWIN_CORE, "Failed to set graphics mode on tty: %s", strerror(errno));
        return false;
    }

    vt_mode virtualTerminalMode = {};
    virtualTerminalMode.mode = VT_PROCESS;
    virtualTerminalMode.relsig = SIGUSR1;
    virtualTerminalMode.acqsig = SIGUSR2;

    if (ioctl(fd, VT_SETMODE, &virtualTerminalMode)) {
        qCWarning(KWIN_CORE, "Failed to take control of vt: %s", strerror(errno));
        return false;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);

    if (sigprocmask(SIG_BLOCK, &mask, nullptr) == -1) {
        qCWarning(KWIN_CORE) << "Failed to block acquire and release tty signals";
        return false;
    }

    const int signalFd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signalFd == -1) {
        qCWarning(KWIN_CORE, "Failed to create signalfd for vt handler");
        return false;
    }
    m_signalNotifier = new QSocketNotifier(signalFd, QSocketNotifier::Read, this);
    connect(m_signalNotifier, &QSocketNotifier::activated, this, [this]() {
        while (true) {
            signalfd_siginfo info;

            const ssize_t readCount = read(m_signalNotifier->socket(), &info, sizeof(info));
            if (readCount != sizeof(info)) {
                break;
            }

            switch (info.ssi_signo) {
            case SIGUSR1:
                qCDebug(KWIN_CORE) << "Releasing virtual terminal" << m_terminal;
                updateActive(false);
                ioctl(m_ttyFd, VT_RELDISP, 1);
                break;
            case SIGUSR2:
                qCDebug(KWIN_CORE) << "Acquiring virtual terminal" << m_terminal;
                updateActive(true);
                ioctl(m_ttyFd, VT_RELDISP, VT_ACKACQ);
                break;
            }
        }
    });

    m_isActive = true;
    m_ttyFd = fd;
    cleanup.dismiss();

    return true;
}

void DirectSession::restoreTerminal()
{
    vt_mode virtualTerminalMode = {};

    if (ioctl(m_ttyFd, KDSKBMUTE, 0) && ioctl(m_ttyFd, KDSKBMODE, m_keyboardMode)) {
        qCWarning(KWIN_CORE, "Failed to restore keyboard mode: %s", strerror(errno));
    }

    if (ioctl(m_ttyFd, KDSETMODE, KD_TEXT)) {
        qCWarning(KWIN_CORE, "Failed to set KD_TEXT mode on tty: %s", strerror(errno));
    }

    virtualTerminalMode.mode = VT_AUTO;
    if (ioctl(m_ttyFd, VT_SETMODE, &virtualTerminalMode)) {
        qCWarning(KWIN_CORE, "Failed to reset VT handling: %s", strerror(errno));
    }
}

bool DirectSession::isActive() const
{
    return m_isActive;
}

QString DirectSession::seat() const
{
    return m_seat;
}

uint DirectSession::terminal() const
{
    return m_terminal;
}

int DirectSession::openRestricted(const QString &fileName)
{
    return open(fileName.toUtf8().constData(), O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
}

void DirectSession::closeRestricted(int fileDescriptor)
{
    close(fileDescriptor);
}

void DirectSession::switchTo(uint terminal)
{
    if (m_seat != QStringLiteral("seat0")) {
        return;
    }
    ioctl(m_ttyFd, VT_ACTIVATE, terminal);
}

void DirectSession::updateActive(bool active)
{
    if (m_isActive == active) {
        return;
    }
    m_isActive = active;
    emit activeChanged(active);
}

} // namespace KWin
