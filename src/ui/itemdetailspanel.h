// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#pragma once

#include <QTimer>
#include <QWidget>

#include <memory>

class QSettings;

class ImageCache;
class Item;
class ItemLocation;
class LocationInventory;
class NetworkManager;

namespace Ui {
    class ItemDetailsPanel;
}

// The item details beside the item list: the current item's tooltip and
// text, its image, its location line and minimap, and the Upload to imgur
// and Copy for Path of Building buttons. The window decides what is current
// and tells the panel: an item, a bucket header, or nothing. The panel owns
// everything it draws from that, and keeps the item it was last told for
// its deferred drawing, the fetched image and the buttons.
class ItemDetailsPanel : public QWidget
{
    Q_OBJECT
public:
    explicit ItemDetailsPanel(QWidget *parent = nullptr);
    ~ItemDetailsPanel();

    // The services the panel reads from. Called once, before it is shown
    // anything.
    void initialize(QSettings &settings,
                    NetworkManager &network_manager,
                    ImageCache &image_cache,
                    const LocationInventory &inventory);
    // Opens the details on the tab the settings remember.
    void loadSettings();

    // Shows `item`'s details now; a null item shows that nothing is selected.
    void showItem(std::shared_ptr<Item> item);
    // Shows `item`'s details once the selection has settled: a run of
    // selections draws only the last.
    void showItemLater(std::shared_ptr<Item> item);
    // Shows a bucket header's name in place of item details.
    void showBucket(const ItemLocation &location);
    // Shows that nothing is selected.
    void clear();
    // Draws `item`'s location line again, from the canonical metadata of its
    // tab, which a refresh can rename without replacing the item.
    void updateLocation(const Item &item);

signals:
    // The image of the item shown is wanted from the image cache.
    void imageRequested(const QString &url);

public slots:
    // The image cache has finished fetching `url`.
    void onImageFetched(const QString &url);

private:
    void updateCurrentItem();
    void uploadToImgur();
    void copyForPathOfBuilding();
    void onUploadFinished();

    Ui::ItemDetailsPanel *ui;
    QSettings *m_settings{nullptr};
    NetworkManager *m_network_manager{nullptr};
    ImageCache *m_image_cache{nullptr};
    const LocationInventory *m_inventory{nullptr};
    // The item the window last showed, or null for a bucket or nothing.
    std::shared_ptr<Item> m_item;
    QTimer m_delayed_update_current_item;
};
