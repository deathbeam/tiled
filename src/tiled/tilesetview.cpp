/*
 * tilesetview.cpp
 * Copyright 2008-2010, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tilesetview.h"

#include "actionmanager.h"
#include "addremovetiles.h"
#include "changetile.h"
#include "changeevents.h"
#include "changetilewangid.h"
#include "pannableviewhelper.h"
#include "preferences.h"
#include "stylehelper.h"
#include "tile.h"
#include "tileset.h"
#include "tilesetdocument.h"
#include "tilesetmodel.h"
#include "utils.h"
#include "wangoverlay.h"
#include "zoomable.h"

#include <QAbstractItemDelegate>
#include <QApplication>
#include <QCoreApplication>
#include <QGesture>
#include <QGestureEvent>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPinchGesture>
#include <QScrollBar>
#include <QUndoCommand>
#include <QWheelEvent>
#include <QtCore/qmath.h>

#include <QDebug>

using namespace Tiled;

namespace {

static void setupTilesetGridTransform(const Tileset &tileset, QTransform &transform, QRect &targetRect)
{
    if (tileset.orientation() == Tileset::Isometric) {
        const QPoint tileCenter = targetRect.center();
        targetRect.setHeight(targetRect.width());
        targetRect.moveCenter(tileCenter);

        const QSize gridSize = tileset.gridSize();

        transform.translate(tileCenter.x(), tileCenter.y());

        const auto ratio = (qreal) gridSize.height() / gridSize.width();
        const auto scaleX = 1.0 / sqrt(2.0);
        const auto scaleY = scaleX * ratio;
        transform.scale(scaleX, scaleY);

        transform.rotate(45.0);

        transform.translate(-tileCenter.x(), -tileCenter.y());
    }
}

/**
 * The delegate for drawing tile items in the tileset view.
 */
class TileDelegate : public QAbstractItemDelegate
{
public:
    TileDelegate(TilesetView *tilesetView, QObject *parent = nullptr)
        : QAbstractItemDelegate(parent)
        , mTilesetView(tilesetView)
    { }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    QSize sizeHint(const QStyleOptionViewItem &option,
                   const QModelIndex &index) const override;

private:
    void drawFilmStrip(QPainter *painter, QRect targetRect) const;
    void drawWangOverlay(QPainter *painter,
                         const Tile *tile,
                         QRect targetRect,
                         const QModelIndex &index) const;

    TilesetView *mTilesetView;
};

void TileDelegate::paint(QPainter *painter,
                         const QStyleOptionViewItem &option,
                         const QModelIndex &index) const
{
    const TilesetModel *model = static_cast<const TilesetModel*>(index.model());
    const Tile *tile = model->tileAt(index);
    if (!tile)
        return;

    const QPixmap &tileImage = tile->image();
    const int extra = mTilesetView->drawGrid() ? 1 : 0;
    const qreal zoom = mTilesetView->scale();
    const bool wrapping = mTilesetView->dynamicWrapping();

    QSize tileSize = tile->size();
    if (tileImage.isNull()) {
        Tileset *tileset = model->tileset();
        if (tileset->isCollection()) {
            tileSize = QSize(32, 32);
        } else {
            int max = std::max(tileset->tileWidth(), tileset->tileHeight());
            int min = std::min(max, 32);
            tileSize = QSize(min, min);
        }
    }

    // Compute rectangle to draw the image in: bottom- and left-aligned
    QRect targetRect = option.rect.adjusted(0, 0, -extra, -extra);

    if (wrapping) {
        qreal scale = std::min(static_cast<qreal>(targetRect.width()) / tileSize.width(),
                               static_cast<qreal>(targetRect.height()) / tileSize.height());
        tileSize *= scale;

        auto center = targetRect.center();
        targetRect.setSize(tileSize);
        targetRect.moveCenter(center);
    } else {
        tileSize *= zoom;
        targetRect.setTop(targetRect.bottom() - tileSize.height() + 1);
        targetRect.setRight(targetRect.left() + tileSize.width() - 1);
    }

    // Draw the tile image
    if (Zoomable *zoomable = mTilesetView->zoomable())
        if (zoomable->smoothTransform())
            painter->setRenderHint(QPainter::SmoothPixmapTransform);

    if (!tileImage.isNull())
        painter->drawPixmap(targetRect, tileImage, tile->imageRect());
    else
        mTilesetView->imageMissingIcon().paint(painter, targetRect, Qt::AlignBottom | Qt::AlignLeft);

    const auto highlight = option.palette.highlight();

    // Draw grid lines when in rearranging mode
    if (mTilesetView->isRelocateTiles()) {
        const qreal opacity = painter->opacity();
        painter->setOpacity(0.5);
        painter->setPen(QPen(highlight, 1, Qt::SolidLine));
        painter->drawRect(option.rect.adjusted(0, 0, -1, -1));
        painter->setOpacity(opacity);
    }

    // Overlay with film strip when animated
    if (mTilesetView->markAnimatedTiles() && tile->isAnimated())
        drawFilmStrip(painter, targetRect);

    // Overlay with highlight color when selected
    if (option.state & QStyle::State_Selected) {
        const qreal opacity = painter->opacity();
        painter->setOpacity(0.5);
        painter->fillRect(targetRect, highlight);
        painter->setOpacity(opacity);
    }

    if (mTilesetView->isEditWangSet())
        drawWangOverlay(painter, tile, targetRect, index);
}

QSize TileDelegate::sizeHint(const QStyleOptionViewItem & /* option */,
                             const QModelIndex &index) const
{
    const TilesetModel *m = static_cast<const TilesetModel*>(index.model());
    const int extra = mTilesetView->drawGrid() ? 1 : 0;
    const qreal scale = mTilesetView->scale();

    if (const Tile *tile = m->tileAt(index)) {
        if (mTilesetView->dynamicWrapping()) {
            Tileset *tileset = tile->tileset();
            return QSize(tileset->tileWidth() * scale + extra,
                         tileset->tileHeight() * scale + extra);
        }

        QSize tileSize = tile->size();

        if (tile->image().isNull()) {
            Tileset *tileset = m->tileset();
            if (tileset->isCollection()) {
                tileSize = QSize(32, 32);
            } else {
                int max = std::max(tileset->tileWidth(), tileset->tileWidth());
                int min = std::min(max, 32);
                tileSize = QSize(min, min);
            }
        }

        return QSize(tileSize.width() * scale + extra,
                     tileSize.height() * scale + extra);
    }

    return QSize(extra, extra);
}

void TileDelegate::drawFilmStrip(QPainter *painter, QRect targetRect) const
{
    painter->save();

    qreal scale = qMin(targetRect.width() / 32.0,
                       targetRect.height() / 32.0);

    painter->setClipRect(targetRect);
    painter->translate(targetRect.right(),
                       targetRect.bottom());
    painter->scale(scale, scale);
    painter->translate(-18, 3);
    painter->rotate(-45);
    painter->setOpacity(0.8);

    QRectF strip(0, 0, 32, 6);
    painter->fillRect(strip, Qt::black);

    painter->setRenderHint(QPainter::Antialiasing);
    painter->setBrush(Qt::white);
    painter->setPen(Qt::NoPen);

    QRectF hole(0, 0, strip.height() * 0.6, strip.height() * 0.6);
    qreal step = (strip.height() - hole.height()) + hole.width();
    qreal margin = (strip.height() - hole.height()) / 2;

    for (qreal x = (step - hole.width()) / 2; x < strip.right(); x += step) {
        hole.moveTo(x, margin);
        painter->drawRoundedRect(hole, 25, 25, Qt::RelativeSize);
    }

    painter->restore();
}

void TileDelegate::drawWangOverlay(QPainter *painter,
                                   const Tile *tile,
                                   QRect targetRect,
                                   const QModelIndex &index) const
{
    WangSet *wangSet = mTilesetView->wangSet();
    if (!wangSet)
        return;

    painter->save();

    QTransform transform;
    setupTilesetGridTransform(*tile->tileset(), transform, targetRect);
    painter->setTransform(transform, true);

    paintWangOverlay(painter,
                     wangSet->wangIdOfTile(tile) & wangSet->typeMask(),
                     *wangSet,
                     targetRect);

    if (mTilesetView->hoveredIndex() == index) {
        qreal opacity = painter->opacity();
        painter->setOpacity(0.5);
        paintWangOverlay(painter, mTilesetView->wangId(),
                         *wangSet,
                         targetRect,
                         WangOverlayOptions());
        painter->setOpacity(opacity);
    }

    painter->restore();
}

} // anonymous namespace

TilesetView::TilesetView(QWidget *parent)
    : QTableView(parent)
    , mZoomable(new Zoomable(this))
    , mImageMissingIcon(QStringLiteral("://images/32/image-missing.png"))
{
    setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setItemDelegate(new TileDelegate(this, this));
    setShowGrid(false);
    setTabKeyNavigation(false);
    setDropIndicatorShown(true);
    setEditTriggers(QAbstractItemView::NoEditTriggers);

    QHeaderView *hHeader = horizontalHeader();
    QHeaderView *vHeader = verticalHeader();
    hHeader->hide();
    vHeader->hide();
    hHeader->setSectionResizeMode(QHeaderView::ResizeToContents);
    vHeader->setSectionResizeMode(QHeaderView::ResizeToContents);
    hHeader->setMinimumSectionSize(1);
    vHeader->setMinimumSectionSize(1);

    // Hardcode this view on 'left to right' since it doesn't work properly
    // for 'right to left' languages.
    setLayoutDirection(Qt::LeftToRight);

    Preferences *prefs = Preferences::instance();
    mDrawGrid = prefs->showTilesetGrid();

    grabGesture(Qt::PinchGesture);

    connect(prefs, &Preferences::showTilesetGridChanged,
            this, &TilesetView::setDrawGrid);

    connect(StyleHelper::instance(), &StyleHelper::styleApplied,
            this, &TilesetView::updateBackgroundColor);

    connect(mZoomable, &Zoomable::scaleChanged, this, &TilesetView::adjustScale);

    connect(new PannableViewHelper(this), &PannableViewHelper::cursorChanged,
            this, [this] (std::optional<Qt::CursorShape> cursor) {
        if (cursor)
            viewport()->setCursor(*cursor);
        else
            viewport()->unsetCursor();
    });
}

void TilesetView::setTilesetDocument(TilesetDocument *tilesetDocument)
{
    if (mTilesetDocument)
        mTilesetDocument->disconnect(this);

    mTilesetDocument = tilesetDocument;

    if (mTilesetDocument) {
        connect(mTilesetDocument, &Document::changed, this, &TilesetView::onChange);
        connect(mTilesetDocument, &TilesetDocument::tilesAdded, this, &TilesetView::refreshColumnCount);
        connect(mTilesetDocument, &TilesetDocument::tilesRemoved, this, &TilesetView::refreshColumnCount);
        connect(mTilesetDocument, &TilesetDocument::tileImageSourceChanged, this, &TilesetView::updateAtlasSpans);
    }
}

QSize TilesetView::sizeHint() const
{
    return Utils::dpiScaled(QSize(260, 100));
}

int TilesetView::sizeHintForColumn(int column) const
{
    Q_UNUSED(column)
    const TilesetModel *model = tilesetModel();
    if (!model)
        return -1;
    if (model->tileset()->isCollection())
        return QTableView::sizeHintForColumn(column);

    const int gridSpace = mDrawGrid ? 1 : 0;
    if (dynamicWrapping())
        return model->tileset()->tileWidth() * scale() + gridSpace;

    const int tileWidth = model->tileset()->tileWidth();
    return qRound(tileWidth * scale()) + gridSpace;
}

int TilesetView::sizeHintForRow(int row) const
{
    Q_UNUSED(row)
    const TilesetModel *model = tilesetModel();
    if (!model)
        return -1;
    if (model->tileset()->isCollection())
        return QTableView::sizeHintForRow(row);

    const int gridSpace = mDrawGrid ? 1 : 0;
    if (dynamicWrapping())
        return model->tileset()->tileHeight() * scale() + gridSpace;

    const int tileHeight = model->tileset()->tileHeight();
    return qRound(tileHeight * scale()) + gridSpace;
}

qreal TilesetView::scale() const
{
    return mZoomable->scale();
}

void TilesetView::setDynamicWrapping(bool enabled)
{
    WrapBehavior behavior = enabled ? WrapDynamic : WrapFixed;
    if (mWrapBehavior == behavior)
        return;

    mWrapBehavior = behavior;
    setVerticalScrollBarPolicy(dynamicWrapping() ? Qt::ScrollBarAlwaysOn
                                                 : Qt::ScrollBarAsNeeded);
    scheduleDelayedItemsLayout();
    refreshColumnCount();
}

bool TilesetView::dynamicWrapping() const
{
    switch (mWrapBehavior) {
    case WrapDefault:
        if (tilesetModel())
            return tilesetModel()->tileset()->isCollection();
        break;
    case WrapDynamic:
        return true;
    case WrapFixed:
        return false;
    }

    return false;
}

void TilesetView::setModel(QAbstractItemModel *model)
{
    QTableView::setModel(model);

    updateAtlasSpans();
    updateBackgroundColor();
    setVerticalScrollBarPolicy(dynamicWrapping() ? Qt::ScrollBarAlwaysOn
                                                : Qt::ScrollBarAsNeeded);
    refreshColumnCount();
}

void TilesetView::setMarkAnimatedTiles(bool enabled)
{
    if (mMarkAnimatedTiles == enabled)
        return;

    mMarkAnimatedTiles = enabled;
    viewport()->update();
}

bool TilesetView::event(QEvent *event)
{
    if (event->type() == QEvent::Gesture) {
        QGestureEvent *gestureEvent = static_cast<QGestureEvent *>(event);
        if (QGesture *gesture = gestureEvent->gesture(Qt::PinchGesture))
            mZoomable->handlePinchGesture(static_cast<QPinchGesture *>(gesture));
    } else if (event->type() == QEvent::ShortcutOverride) {
        auto keyEvent = static_cast<QKeyEvent*>(event);
        if (Utils::isZoomInShortcut(keyEvent) ||
                Utils::isZoomOutShortcut(keyEvent) ||
                Utils::isResetZoomShortcut(keyEvent)) {
            event->accept();
            return true;
        }
    }

    return QTableView::event(event);
}

void TilesetView::keyPressEvent(QKeyEvent *event)
{
    if (Utils::isZoomInShortcut(event)) {
        mZoomable->zoomIn();
        return;
    }
    if (Utils::isZoomOutShortcut(event)) {
        mZoomable->zoomOut();
        return;
    }
    if (Utils::isResetZoomShortcut(event)) {
        mZoomable->resetZoom();
        return;
    }

    // TODO: These shortcuts only work while the TilesetView is focused. It
    // would be preferable if they could be used more globally.
    if (mEditWangSet && mWangBehavior == AssignWholeId && !(event->modifiers() & Qt::ControlModifier)) {
        WangId transformedWangId = mWangId;

        if (event->key() == Qt::Key_Z) {
            if (event->modifiers() & Qt::ShiftModifier)
                transformedWangId.rotate(-1);
            else
                transformedWangId.rotate(1);
        } else if (event->key() == Qt::Key_X) {
            transformedWangId.flipHorizontally();
        } else if (event->key() == Qt::Key_Y) {
            transformedWangId.flipVertically();
        }

        if (mWangId != transformedWangId) {
            setWangId(transformedWangId);
            emit currentWangIdChanged(mWangId);
            return;
        }
    }

    // Ignore space, because we'd like to use it for panning
    if (event->key() == Qt::Key_Space) {
        event->ignore();
        return;
    }

    return QTableView::keyPressEvent(event);
}

void TilesetView::setRelocateTiles(bool enabled)
{
    if (mRelocateTiles == enabled)
        return;

    mRelocateTiles = enabled;

    if (TilesetModel *m = tilesetModel()) {
        m->setRelocating(enabled);
        if (m->tileset()->isAtlas()) {
            enabled = false;
        }
    }

    if (enabled)
        setDragDropMode(QTableView::InternalMove);
    else
        setDragDropMode(QTableView::NoDragDrop);

    setMouseTracking(true);
    viewport()->update();
}

void TilesetView::setEditWangSet(bool enabled)
{
    if (mEditWangSet == enabled)
        return;

    mEditWangSet = enabled;
    setMouseTracking(true);
    viewport()->update();
}

void TilesetView::setWangSet(WangSet *wangSet)
{
    if (mWangSet == wangSet)
        return;

    mWangSet = wangSet;

    if (mEditWangSet)
        viewport()->update();
}

/**
 * Sets the WangId and changes WangBehavior to WholeId.
 */
void TilesetView::setWangId(WangId wangId)
{
    mWangId = wangId;
    mWangBehavior = AssignWholeId;

    if (mEditWangSet && hoveredIndex().isValid())
        update(hoveredIndex());
}

/**
 * Sets the wangColor, and changes WangBehavior depending on the type of the
 * WangSet.
 */
void TilesetView::setWangColor(int color)
{
    mWangColorIndex = color;
    mWangBehavior = AssignHoveredIndex;
}

QIcon TilesetView::imageMissingIcon() const
{
    return QIcon::fromTheme(QLatin1String("image-missing"), mImageMissingIcon);
}

void TilesetView::mousePressEvent(QMouseEvent *event)
{
    if (mEditWangSet) {
        if (event->button() == Qt::LeftButton)
            applyWangId();
        return;
    }

    QTableView::mousePressEvent(event);
}

void TilesetView::mouseMoveEvent(QMouseEvent *event)
{
    if (mEditWangSet) {
        if (!mWangSet)
            return;

        const QPoint pos = event->pos();
        const QModelIndex hoveredIndex = indexAt(pos);
        const QModelIndex previousHoveredIndex = mHoveredIndex;
        mHoveredIndex = hoveredIndex;

        WangId wangId;

        if (mWangBehavior == AssignWholeId) {
            wangId = mWangId;
        } else {
            QRect tileRect = visualRect(mHoveredIndex);
            QTransform transform;
            setupTilesetGridTransform(*tilesetDocument()->tileset(), transform, tileRect);

            const auto mappedPos = transform.inverted().map(pos);
            QPoint tileLocalPos = mappedPos - tileRect.topLeft();
            QPointF tileLocalPosF((qreal) tileLocalPos.x() / tileRect.width(),
                                  (qreal) tileLocalPos.y() / tileRect.height());

            const int x = qBound(0, qFloor(tileLocalPosF.x() * 3), 2);
            const int y = qBound(0, qFloor(tileLocalPosF.y() * 3), 2);
            WangId::Index index = WangId::indexByGrid(x, y);

            if (index != WangId::NumIndexes) {  // center is dead zone
                switch (mWangSet->type()) {
                case WangSet::Edge:
                    tileLocalPosF -= QPointF(0.5, 0.5);

                    if (tileLocalPosF.x() < tileLocalPosF.y()) {
                        if (tileLocalPosF.x() > -tileLocalPosF.y())
                            index = WangId::Bottom;
                        else
                            index = WangId::Left;
                    } else {
                        if (tileLocalPosF.x() > -tileLocalPosF.y())
                            index = WangId::Right;
                        else
                            index = WangId::Top;
                    }
                    break;
                case WangSet::Corner:
                    if (tileLocalPosF.x() > 0.5) {
                        if (tileLocalPosF.y() > 0.5)
                            index = WangId::BottomRight;
                        else
                            index = WangId::TopRight;
                    } else {
                        if (tileLocalPosF.y() > 0.5)
                            index = WangId::BottomLeft;
                        else
                            index = WangId::TopLeft;
                    }
                    break;
                case WangSet::Mixed:
                    break;
                }

                wangId.setIndexColor(index, mWangColorIndex ? mWangColorIndex
                                                            : WangId::INDEX_MASK);
            }
        }

        if (previousHoveredIndex != mHoveredIndex || wangId != mWangId) {
            mWangId = wangId;

            if (previousHoveredIndex.isValid())
                update(previousHoveredIndex);
            if (mHoveredIndex.isValid())
                update(mHoveredIndex);
        }

        if (event->buttons() & Qt::LeftButton)
            applyWangId();

        return;
    }

    QTableView::mouseMoveEvent(event);
}

void TilesetView::mouseReleaseEvent(QMouseEvent *event)
{
    if (mEditWangSet) {
        if (event->button() == Qt::LeftButton)
            finishWangIdChange();
        return;
    }

    if (mRelocateTiles && tilesetModel() && tilesetModel()->tileset()->isAtlas()) {
        handleAtlasMouseReleaseEvent(event);
        return;
    }

    QTableView::mouseReleaseEvent(event);
}

void TilesetView::leaveEvent(QEvent *event)
{
    if (mHoveredIndex.isValid()) {
        const QModelIndex previousHoveredIndex = mHoveredIndex;
        mHoveredIndex = QModelIndex();
        update(previousHoveredIndex);
    }

    QTableView::leaveEvent(event);
}

/**
 * Override to support zooming in and out using the mouse wheel, as well as to
 * make the scrolling speed independent of Ctrl modifier and zoom level.
 */
void TilesetView::wheelEvent(QWheelEvent *event)
{
    auto hor = horizontalScrollBar();
    auto ver = verticalScrollBar();

    bool wheelZoomsByDefault = !dynamicWrapping() && Preferences::instance()->wheelZoomsByDefault();
    bool control = event->modifiers() & Qt::ControlModifier;

    if ((wheelZoomsByDefault != control) && event->angleDelta().y()) {

        const QPointF &viewportPos = event->position();
        const QPointF contentPos(viewportPos.x() + hor->value(),
                                 viewportPos.y() + ver->value());

        QPointF relativeContentPos;

        const QSize oldContentSize = viewportSizeHint();
        if (!oldContentSize.isEmpty()) {
            relativeContentPos = QPointF(contentPos.x() / oldContentSize.width(),
                                         contentPos.y() / oldContentSize.height());
        }

        mZoomable->handleWheelDelta(event->angleDelta().y());

        executeDelayedItemsLayout();

        const QSize newContentSizeHint = viewportSizeHint();
        const QPointF newContentPos(relativeContentPos.x() * newContentSizeHint.width(),
                                    relativeContentPos.y() * newContentSizeHint.height());

        hor->setValue(newContentPos.x() - viewportPos.x());
        ver->setValue(newContentPos.y() - viewportPos.y());
        return;
    }

    QPoint delta = event->pixelDelta();
    if (delta.isNull())
        delta = Utils::dpiScaled(event->angleDelta());

    if (delta.x())
        hor->setValue(hor->value() - delta.x());
    if (delta.y())
        ver->setValue(ver->value() - delta.y());
}

/**
 * Allow changing tile properties through a context menu.
 */
void TilesetView::contextMenuEvent(QContextMenuEvent *event)
{
    const QModelIndex index = indexAt(event->pos());
    const TilesetModel *model = tilesetModel();
    if (!model)
        return;

    if (mRelocateTiles) {
        return;
    }

    Tile *tile = model->tileAt(index);

    QMenu menu;

    if (tile) {
        if (mEditWangSet) {
            selectionModel()->setCurrentIndex(index,
                                              QItemSelectionModel::SelectCurrent |
                                              QItemSelectionModel::Clear);

            if (mWangSet) {
                QAction *setImage = menu.addAction(tr("Use as Terrain Set Image"));
                connect(setImage, &QAction::triggered, this, &TilesetView::selectWangSetImage);
            }
            if (mWangBehavior != AssignWholeId && mWangColorIndex) {
                QAction *setImage = menu.addAction(tr("Use as Terrain Image"));
                connect(setImage, &QAction::triggered, this, &TilesetView::selectWangColorImage);
            }
            menu.addSeparator();
        }

        QUrl imageSource = tile->imageSource();
        if (imageSource.isEmpty())
            imageSource = tile->tileset()->imageSource();

        if (!imageSource.isEmpty()) {
            const QString localFile = imageSource.toLocalFile();
            if (!localFile.isEmpty()) {
                Utils::addOpenContainingFolderAction(menu, localFile);
                Utils::addOpenWithSystemEditorAction(menu, localFile);
                menu.addSeparator();
            }
        }

        if (mTilesetDocument) {
            const QIcon propIcon(QStringLiteral(":images/16/document-properties.png"));
            QAction *tileProperties = menu.addAction(propIcon,
                                                     tr("Tile &Properties..."));
            Utils::setThemeIcon(tileProperties, "document-properties");
            connect(tileProperties, &QAction::triggered, this, &TilesetView::editTileProperties);
        } else {
            // Assuming we're used in the MapEditor

            // Enable "swap" if there are exactly 2 tiles selected
            const bool exactlyTwoTilesSelected =
                    (selectionModel()->selectedIndexes().size() == 2);

            QAction *swapTilesAction = menu.addAction(tr("&Swap Tiles"));
            swapTilesAction->setEnabled(exactlyTwoTilesSelected);
            connect(swapTilesAction, &QAction::triggered, this, &TilesetView::swapTiles);
        }

        menu.addSeparator();
    }

    QAction *toggleGrid = menu.addAction(tr("Show &Grid"));
    toggleGrid->setCheckable(true);
    toggleGrid->setChecked(mDrawGrid);

    Preferences *prefs = Preferences::instance();
    connect(toggleGrid, &QAction::toggled,
            prefs, &Preferences::setShowTilesetGrid);

    QAction *selectAllTiles = menu.addAction(tr("Select &All Tiles"));
    connect(selectAllTiles, &QAction::triggered, this, &QAbstractItemView::selectAll);

    ActionManager::applyMenuExtensions(&menu, MenuIds::tilesetViewTiles);

    menu.exec(event->globalPos());
}

void TilesetView::resizeEvent(QResizeEvent *event)
{
    QTableView::resizeEvent(event);
    refreshColumnCount();
}

void TilesetView::onChange(const ChangeEvent &change)
{
    switch (change.type) {
    case ChangeEvent::DocumentReloaded:
        refreshColumnCount();
        break;
    case ChangeEvent::WangSetChanged: {
        auto &wangSetChange = static_cast<const WangSetChangeEvent&>(change);
        if (mEditWangSet && wangSetChange.wangSet == mWangSet &&
                (wangSetChange.property == WangSetChangeEvent::TypeProperty)) {
            viewport()->update();
        }
        break;
    }
    default:
        break;
    }
}

void TilesetView::selectWangSetImage()
{
    if (Tile *tile = currentTile())
        emit wangSetImageSelected(tile);
}

void TilesetView::selectWangColorImage()
{
    if (Tile *tile = currentTile())
        emit wangColorImageSelected(tile, mWangColorIndex);
}

void TilesetView::editTileProperties()
{
    Q_ASSERT(mTilesetDocument);

    Tile *tile = currentTile();
    if (!tile)
        return;

    mTilesetDocument->setCurrentObject(tile);
    emit mTilesetDocument->editCurrentObject();
}

void TilesetView::swapTiles()
{
    const QModelIndexList selectedIndexes = selectionModel()->selectedIndexes();
    if (selectedIndexes.size() != 2)
        return;

    const TilesetModel *model = tilesetModel();
    Tile *tile1 = model->tileAt(selectedIndexes[0]);
    Tile *tile2 = model->tileAt(selectedIndexes[1]);

    if (!tile1 || !tile2)
        return;

    emit swapTilesRequested(tile1, tile2);
}

void TilesetView::setDrawGrid(bool drawGrid)
{
    mDrawGrid = drawGrid;
    scheduleDelayedItemsLayout();
    refreshColumnCount();
}

void TilesetView::adjustScale()
{
    scheduleDelayedItemsLayout();
    refreshColumnCount();
}

void TilesetView::refreshColumnCount()
{
    if (!tilesetModel())
        return;

    if (!dynamicWrapping()) {
        tilesetModel()->setColumnCountOverride(0);
        return;
    }

    const QSize maxSize = maximumViewportSize();
    const int gridSpace = mDrawGrid ? 1 : 0;
    const int tileWidth = tilesetModel()->tileset()->tileWidth();
    const int scaledTileSize = std::max<int>(tileWidth * scale(), 1) + gridSpace;
    const int columnCount = std::max(maxSize.width() / scaledTileSize, 1);
    tilesetModel()->setColumnCountOverride(columnCount);
}

void TilesetView::applyWangId()
{
    if (!mHoveredIndex.isValid() || !mWangSet)
        return;

    Tile *tile = tilesetModel()->tileAt(mHoveredIndex);
    if (!tile)
        return;

    WangId previousWangId = mWangSet->wangIdOfTile(tile);
    WangId newWangId = previousWangId;

    if (mWangBehavior == AssignWholeId) {
        newWangId = mWangId;
    } else {
        for (int i = 0; i < WangId::NumIndexes; ++i) {
            if (mWangId.indexColor(i))
                newWangId.setIndexColor(i, mWangColorIndex);
        }
    }

    if (newWangId == previousWangId)
        return;

    bool wasUnused = !mWangSet->wangIdIsUsed(newWangId);

    QUndoCommand *command = new ChangeTileWangId(mTilesetDocument, mWangSet, tile, newWangId);
    mTilesetDocument->undoStack()->push(command);
    mWangIdChanged = true;

    if (!mWangSet->wangIdIsUsed(previousWangId))
        emit wangIdUsedChanged(previousWangId);

    if (wasUnused)
        emit wangIdUsedChanged(newWangId);
}

void TilesetView::finishWangIdChange()
{
    if (!mWangIdChanged)
        return;

    mTilesetDocument->undoStack()->push(new ChangeTileWangId);
    mWangIdChanged = false;
}

Tile *TilesetView::currentTile() const
{
    const TilesetModel *model = tilesetModel();
    return model ? model->tileAt(currentIndex()) : nullptr;
}

void TilesetView::updateBackgroundColor()
{
    QColor base = QApplication::palette().dark().color();

    if (TilesetModel *model = tilesetModel()) {
        Tileset *tileset = model->tileset();
        if (tileset->backgroundColor().isValid())
            base = tileset->backgroundColor();
    }

    QPalette p = palette();
    p.setColor(QPalette::Base, base);
    setPalette(p);
}

void TilesetView::updateAtlasSpans()
{
    if (TilesetModel *tilesetModel = qobject_cast<TilesetModel*>(model())) {
        if (!tilesetModel->tileset()->isAtlas())
            return;

        clearSpans();

        // Set up spans for atlas tiles
        for (Tile *tile : tilesetModel->tileset()->tiles()) {
            QModelIndex index = tilesetModel->tileIndex(tile);
            if (index.isValid()) {
                QSize span = tilesetModel->tileSpanSize(index);
                if (span != QSize(1, 1)) {
                    setSpan(index.row(), index.column(),
                           span.height(), span.width());
                }
            }
        }
    }
}

void TilesetView::handleAtlasMouseReleaseEvent(QMouseEvent *event)
{
    TilesetModel *m = tilesetModel();
    const QModelIndexList selectedIndexes = selectionModel()->selectedIndexes();
    if (selectedIndexes.isEmpty())
        return;

    if (event->button() == Qt::LeftButton) {
        // Find the bounding rectangle of selection
        int minRow = INT_MAX, maxRow = 0;
        int minCol = INT_MAX, maxCol = 0;
        for (const QModelIndex &index : selectedIndexes) {
            minRow = std::min(minRow, index.row());
            maxRow = std::max(maxRow, index.row());
            minCol = std::min(minCol, index.column());
            maxCol = std::max(maxCol, index.column());
        }

        mergeSpan(minRow, maxRow, minCol, maxCol);
    } else if (event->button() == Qt::RightButton) {
        QModelIndex index = selectedIndexes.first();
        QPoint viewPos = event->pos();

        if (Tile *spanTile = m->findSpanningTile(index)) {
            QRect itemRect = visualRect(index);
            const int tileWidth = m->tileset()->tileWidth() * scale();
            const int tileHeight = m->tileset()->tileHeight() * scale();
            const int col = (viewPos.x() - itemRect.left()) / tileWidth;
            const int row = (viewPos.y() - itemRect.top()) / tileHeight;
            splitSpan(spanTile, row, col);
        } else if (Tile *tile = m->tileAt(index)) {
            mTilesetDocument->undoStack()->push(new RemoveTiles(mTilesetDocument, QList<Tile*>() << tile));
        }
    } else {
        return;
    }

    selectionModel()->clear();
    updateAtlasSpans();
}

void TilesetView::mergeSpan(int minRow, int maxRow, int minCol, int maxCol)
{
    TilesetModel *m = tilesetModel();
    Tileset *tileset = m->tileset();

    // Work in grid coordinates
    const QPoint topLeft(minCol, minRow);
    const QSize spanSize(maxCol - minCol + 1, maxRow - minRow + 1);

    // Get the tile at the top-left position, if any
    QModelIndex topLeftIndex = m->index(minRow, minCol);
    Tile *targetTile = m->tileAt(topLeftIndex);

    // Convert to pixel coordinates only for the final imageRect
    const int tileWidth = tileset->tileWidth();
    const int tileHeight = tileset->tileHeight();
    const int spacing = tileset->tileSpacing();
    const int margin = tileset->margin();

    QRect mergeRect(margin + topLeft.x() * (tileWidth + spacing),
                   margin + topLeft.y() * (tileHeight + spacing),
                   spanSize.width() * tileWidth,
                   spanSize.height() * tileHeight);

    // Collect all tiles within the merge rectangle except the target tile
    QList<Tile*> tilesToRemove;
    for (Tile *tile : tileset->tiles()) {
        if (tile != targetTile && mergeRect.contains(tile->imageRect().topLeft())) {
            tilesToRemove.append(tile);
        }
    }

    // Remove the tiles that will be merged into the target
    if (!tilesToRemove.isEmpty()) {
        mTilesetDocument->undoStack()->beginMacro(tr("Merge Tiles"));
        mTilesetDocument->undoStack()->push(new RemoveTiles(mTilesetDocument, tilesToRemove));
    }

    if (targetTile) {
        // Extend the existing tile's span
        mTilesetDocument->undoStack()->push(new ChangeTileImageRect(mTilesetDocument,
                                          QList<Tile*>() << targetTile,
                                          QVector<QRect>() << mergeRect));
    } else {
        // Create a new spanning tile
        const int tileId = tileset->generateTileId(minCol, minRow);
        Tile *newTile = new Tile(tileId, tileset);
        newTile->setImageRect(mergeRect);
        mTilesetDocument->undoStack()->push(new AddTiles(mTilesetDocument, QList<Tile*>() << newTile));
    }

    if (!tilesToRemove.isEmpty())
        mTilesetDocument->undoStack()->endMacro();
}

void TilesetView::splitSpan(Tile *spanTile, int relativeRow, int relativeCol)
{
    TilesetModel *m = tilesetModel();
    Tileset *tileset = m->tileset();

    // Get base grid position
    QModelIndex tilePos = m->tileIndex(spanTile);
    const QPoint basePos(tilePos.column(), tilePos.row());

    // Calculate span in grid coordinates
    QSize span = m->tileSpanSize(tilePos);
    if (span.width() <= 0 || span.height() <= 0 ||
        relativeCol < 0 || relativeCol >= span.width() ||
        relativeRow < 0 || relativeRow >= span.height())
        return;

    // Calculate closest edge
    float relColRatio = (float)relativeCol / span.width();
    float relRowRatio = (float)relativeRow / span.height();
    float distToLeft = relColRatio;
    float distToRight = 1.0f - relColRatio;
    float distToTop = relRowRatio;
    float distToBottom = 1.0f - relRowRatio;
    float minDist = std::min({distToLeft, distToRight, distToTop, distToBottom});

    // Calculate split in grid coordinates
    QRect newSpanGridRect, splitGridRect;

    if (minDist == distToLeft) {
        // Keep right side as span
        newSpanGridRect = QRect(basePos.x() + relativeCol + 1, basePos.y(),
                              span.width() - relativeCol - 1, span.height());
        splitGridRect = QRect(basePos.x(), basePos.y(),
                            relativeCol + 1, span.height());
    } else if (minDist == distToRight) {
        // Keep left side as span
        newSpanGridRect = QRect(basePos.x(), basePos.y(),
                              relativeCol, span.height());
        splitGridRect = QRect(basePos.x() + relativeCol, basePos.y(),
                            span.width() - relativeCol, span.height());
    } else if (minDist == distToTop) {
        // Keep bottom as span
        newSpanGridRect = QRect(basePos.x(), basePos.y() + relativeRow + 1,
                              span.width(), span.height() - relativeRow - 1);
        splitGridRect = QRect(basePos.x(), basePos.y(),
                            span.width(), relativeRow + 1);
    } else {
        // Keep top as span
        newSpanGridRect = QRect(basePos.x(), basePos.y(),
                              span.width(), relativeRow);
        splitGridRect = QRect(basePos.x(), basePos.y() + relativeRow,
                            span.width(), span.height() - relativeRow);
    }

    // Convert to pixel coordinates
    const int tileWidth = tileset->tileWidth();
    const int tileHeight = tileset->tileHeight();
    const int spacing = tileset->tileSpacing();
    const int margin = tileset->margin();

    auto toPixelRect = [=](const QRect &gridRect) {
        return QRect(margin + gridRect.x() * (tileWidth + spacing),
                    margin + gridRect.y() * (tileHeight + spacing),
                    gridRect.width() * tileWidth,
                    gridRect.height() * tileHeight);
    };

    QRect newSpanRect = toPixelRect(newSpanGridRect);
    QRect splitRect = toPixelRect(splitGridRect);

    mTilesetDocument->undoStack()->beginMacro(tr("Split Tiles"));

    // Delete all tiles in the new span area
    QList<Tile*> tilesToRemove;
    for (Tile *tile : tileset->tiles()) {
        if (tile == spanTile)
            continue;
        if (newSpanRect.contains(tile->imageRect().topLeft())) {
            tilesToRemove.append(tile);
        }
    }
    if (!tilesToRemove.isEmpty())
        mTilesetDocument->undoStack()->push(new RemoveTiles(mTilesetDocument, tilesToRemove));

    const QPoint spanTilePos = spanTile->imageRect().topLeft();
    if (splitRect.contains(spanTilePos)) {
        // Resize original tile to single tile
        QRect newSpanTileRect(spanTilePos.x(), spanTilePos.y(), tileWidth, tileHeight);
        mTilesetDocument->undoStack()->push(new ChangeTileImageRect(mTilesetDocument,
                                                                  QList<Tile*>() << spanTile,
                                                                  QVector<QRect>() << newSpanTileRect));

        // Create new span in the remaining area
        if (!newSpanGridRect.isEmpty()) {
            const int tileId = tileset->generateTileId(newSpanGridRect.x(), newSpanGridRect.y());
            Tile *newSpan = new Tile(tileId, tileset);
            newSpan->setImageRect(newSpanRect);
            mTilesetDocument->undoStack()->push(new AddTiles(mTilesetDocument, QList<Tile*>() << newSpan));
        }
    } else {
        // Adjust span to new size
        mTilesetDocument->undoStack()->push(new ChangeTileImageRect(mTilesetDocument,
                                                                  QList<Tile*>() << spanTile,
                                                                  QVector<QRect>() << newSpanRect));
    }

    // Create individual tiles for split area
    QList<Tile*> newTiles;
    for (int r = 0; r < splitGridRect.height(); ++r) {
        for (int c = 0; c < splitGridRect.width(); ++c) {
            const int gridX = splitGridRect.x() + c;
            const int gridY = splitGridRect.y() + r;
            const int tileId = tileset->generateTileId(gridX, gridY);

            // Skip the original tile position
            if (tileId == spanTile->id())
                continue;

            Tile *newTile = new Tile(tileId, tileset);
            newTile->setImageRect(toPixelRect(QRect(gridX, gridY, 1, 1)));
            newTiles.append(newTile);
        }
    }

    if (!newTiles.isEmpty())
        mTilesetDocument->undoStack()->push(new AddTiles(mTilesetDocument, newTiles));

    mTilesetDocument->undoStack()->endMacro();
}

#include "moc_tilesetview.cpp"
