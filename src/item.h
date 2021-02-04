/*
    SPDX-FileCopyrightText: 2021 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "scene.h"

namespace KWin
{

/**
 * The Item class is the base class for items in the scene.
 */
class KWIN_EXPORT Item : public QObject
{
    Q_OBJECT

public:
    explicit Item(Scene::Window *window, Item *parent = nullptr);
    ~Item() override;

    /**
     * Returns the x coordinate of the item.
     */
    int x() const;
    /**
     * Returns the natural x coordinate of the item.
     */
    int implicitX() const;
    /**
     * Sets the x coordinate of the item to @a x.
     */
    void setX(int x);
    /**
     * Sets the natural x coordinate of the item to @a x. Note that this function has no
     * real effect if the position of the item has been set using setX() function.
     */
    void setImplicitX(int x);
    /**
     * Resets the x coordinate of the item to the natural position.
     */
    void resetX();

    /**
     * Returns the y coordinate of this item.
     */
    int y() const;
    /**
     * Returhs the natureal y coordinate of this item.
     */
    int implicitY() const;
    /**
     * Sets the y coordinate of the item to @a y.
     */
    void setY(int y);
    /**
     * Sets the natural y coordinate of the item to @a y. Note that this function has no
     * real effect if the position of the item has been set using setY() function.
     */
    void setImplicitY(int y);
    /**
     * Resets the y coordinate of the item to the natural position.
     */
    void resetY();

    /**
     * Returns the width of the item.
     */
    int width() const;
    /**
     * Returns the natural width of this item if no width is explicitly specified.
     */
    int implicitWidth() const;
    /**
     * Sets the width of the item to @a width.
     */
    void setWidth(int width);
    /**
     * Sets the natural width of the item to @a width. Note that this function has no
     * real effect if the size has been set using setWidth() function.
     */
    void setImplicitWidth(int width);
    /**
     * Resets the width of the item to the natural width.
     */
    void resetWidth();

    /**
     * Returns the height of this item.
     */
    int height() const;
    /**
     * Returns the natural height of this item if no height is explicitly specified.
     */
    int implicitHeight() const;
    /**
     * Sets the height of the item to @a height.
     */
    void setHeight(int height);
    /**
     * Sets the natural height of the item to @a height. Note that this function has no
     * real effect is the size has been set using setHeight() function.
     */
    void setImplicitHeight(int height);
    /**
     * Resets the height of the item to the natural height.
     */
    void resetHeight();

    /**
     * Returns the natural position of this item if no x and y are explicitly set.
     */
    QPoint implicitPosition() const;
    /**
     * Sets the natural position of this item to @a point. The position of the item won't
     * be changed if an explicit position has been set.
     */
    void setImplicitPosition(const QPoint &point);
    /**
     * Returns the position of the item.
     */
    QPoint position() const;
    /**
     * Sets the position of the item to @a point.
     */
    void setPosition(const QPoint &point);

    /**
     * Returns the natural size of this item.
     */
    QSize implicitSize() const;
    /**
     * Sets the implicit size of this item to @a size. Note that this function won't have
     * any effect if an explicit size has been set.
     */
    void setImplicitSize(const QSize &size);
    /**
     * Returns the size of the item.
     */
    QSize size() const;
    /**
     * Sets the size of the item to @a size.
     */
    void setSize(const QSize &size);

    /**
     * Returns the enclosing rectangle of the item. The rect equals QRect(0, 0, width(), height()).
     */
    QRect rect() const;
    /**
     * Returns the enclosing rectangle of the item and all of its descendants.
     */
    QRect boundingRect() const;

    /**
     * Returns the visual parent of the item. Note that the visual parent differs from
     * the QObject parent.
     */
    Item *parentItem() const;
    /**
     * Sets the visual parent of the item to @a parent.
     */
    void setParentItem(Item *parent);
    /**
     * Returns the children of this item.
     */
    QList<Item *> childItems() const;

    Scene::Window *window() const;
    virtual QPoint windowPosition() const;
    QPoint rootPosition() const;

    /**
     * Maps the given @a region from the item's coordinate system to the scene's coordinate
     * system.
     */
    QRegion mapToGlobal(const QRegion &region) const;
    /**
     * Maps the given @a rect from the item's coordinate system to the scene's coordinate
     * system.
     */
    QRect mapToGlobal(const QRect &rect) const;

    /**
     * Moves this item right before the specified @a sibling in the parent's children list.
     */
    void stackBefore(Item *sibling);
    /**
     * Moves this item right after the specified @a sibling in the parent's children list.
     */
    void stackAfter(Item *sibling);
    /**
     * Restacks the child items in the specified order. Note that the specified stacking order
     * must be a permutation of childItems().
     */
    void stackChildren(const QList<Item *> &children);

    void scheduleRepaint(const QRegion &region);
    void scheduleRepaint();

Q_SIGNALS:
    /**
     * This signal is emitted when the x coordinate of this item has changed.
     */
    void xChanged();
    /**
     * This signal is emitted when the y coordinate of this item has changed.
     */
    void yChanged();
    /**
     * This signal is emitted when the width of this item has changed.
     */
    void widthChanged();
    /**
     * This signal is emitted when the height of this item has changed.
     */
    void heightChanged();

    /**
     * This signal is emitted when the rectangle that encloses this item and all of its children
     * has changed.
     */
    void boundingRectChanged();

protected:
    virtual void preprocess();
    void discardQuads();

private:
    void addChild(Item *item);
    void removeChild(Item *item);
    void updateBoundingRect();

    Scene::Window *m_window;
    QPointer<Item> m_parentItem;
    QList<Item *> m_childItems;
    QRect m_boundingRect;
    int m_x = 0;
    int m_implicitX = 0;
    int m_y = 0;
    int m_implicitY = 0;
    int m_width = 0;
    int m_implicitWidth = 0;
    int m_height = 0;
    int m_implicitHeight = 0;
    bool m_xValid = false;
    bool m_yValid = false;
    bool m_widthValid = false;
    bool m_heightValid = false;

    friend class Scene::Window;
};

} // namespace KWin
