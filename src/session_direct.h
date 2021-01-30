/*
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "session.h"

#include <QSocketNotifier>

namespace KWin
{

class DirectSession : public Session
{
    Q_OBJECT

public:
    static DirectSession *create(QObject *parent = nullptr);
    ~DirectSession() override;

    bool isActive() const override;
    QString seat() const override;
    uint terminal() const override;
    int openRestricted(const QString &fileName) override;
    void closeRestricted(int fileDescriptor) override;
    void switchTo(uint terminal) override;

private:
    explicit DirectSession(QObject *parent = nullptr);

    bool setupTerminal();
    void restoreTerminal();
    void updateActive(bool active);

    QSocketNotifier *m_signalNotifier = nullptr;
    QString m_seat;
    uint m_terminal = 0;
    int m_ttyFd = -1;
    int m_keyboardMode = 0;
    bool m_isActive = false;
};

} // namespace KWin
