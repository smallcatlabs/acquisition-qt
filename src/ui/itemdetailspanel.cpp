// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2014 Ilya Zhuravlev

#include "ui/itemdetailspanel.h"
#include "ui_itemdetailspanel.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QSettings>
#include <QUrl>

#include "imagecache.h"
#include "item.h"
#include "itemconstants.h"
#include "itemlocation.h"
#include "locationinventory.h"
#include "replytimeout.h"
#include "ui/itemtooltip.h"
#include "ui/mainwindowhelpers.h"
#include "util/networkmanager.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

constexpr const char *POE_WEBCDN
    = "http://webcdn.pathofexile.com"; // Should be updated to https://web.poecdn.com ?

constexpr int CURRENT_ITEM_UPDATE_DELAY_MS = 100;

ItemDetailsPanel::ItemDetailsPanel(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::ItemDetailsPanel)
{
    ui->setupUi(this);

    ui->itemLayout->setAlignment(Qt::AlignTop);
    ui->itemLayout->setAlignment(ui->minimapLabel, Qt::AlignHCenter);
    ui->itemLayout->setAlignment(ui->nameLabel, Qt::AlignHCenter);
    ui->itemLayout->setAlignment(ui->imageLabel, Qt::AlignHCenter);
    ui->itemLayout->setAlignment(ui->locationLabel, Qt::AlignHCenter);

    ui->propertiesLabel->setStyleSheet(
        "QLabel { background-color: black; color: #7f7f7f; padding: 10px; font-size: 17px; }");
    ui->propertiesLabel->setFont(QFont("Fontin SmallCaps"));
    ui->itemNameFirstLine->setFont(QFont("Fontin SmallCaps"));
    ui->itemNameSecondLine->setFont(QFont("Fontin SmallCaps"));
    ui->itemNameFirstLine->setAlignment(Qt::AlignCenter);
    ui->itemNameSecondLine->setAlignment(Qt::AlignCenter);

    ui->itemTextTooltip->setStyleSheet(
        "QLabel { background-color: black; color: #7f7f7f; padding: 3px; }");

    ui->itemTooltipWidget->hide();
    ui->itemButtonsWidget->hide();

    m_delayed_update_current_item.setInterval(CURRENT_ITEM_UPDATE_DELAY_MS);
    m_delayed_update_current_item.setSingleShot(true);
    connect(&m_delayed_update_current_item,
            &QTimer::timeout,
            this,
            &ItemDetailsPanel::updateCurrentItem);
}

ItemDetailsPanel::~ItemDetailsPanel()
{
    delete ui;
}

void ItemDetailsPanel::initialize(QSettings &settings,
                                  NetworkManager &network_manager,
                                  ImageCache &image_cache,
                                  const LocationInventory &inventory)
{
    m_settings = &settings;
    m_network_manager = &network_manager;
    m_image_cache = &image_cache;
    m_inventory = &inventory;

    connect(ui->itemInfoTypeTabs, &QTabWidget::currentChanged, this, [=, this](int idx) {
        auto tabs = ui->itemInfoTypeTabs;
        for (int i = 0; i < tabs->count(); i++) {
            if (i != idx) {
                tabs->widget(i)->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
            }
        }
        auto widget = tabs->widget(idx);
        widget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        widget->resize(tabs->widget(idx)->minimumSizeHint());
        widget->adjustSize();
        m_settings->setValue("tooltip_tab", idx);
    });

    // Connect the Tooltip tab buttons
    connect(ui->uploadTooltipButton, &QPushButton::clicked, this, &ItemDetailsPanel::uploadToImgur);
    connect(ui->pobTooltipButton,
            &QPushButton::clicked,
            this,
            &ItemDetailsPanel::copyForPathOfBuilding);
}

void ItemDetailsPanel::loadSettings()
{
    ui->itemInfoTypeTabs->setCurrentIndex(m_settings->value("tooltip_tab").toInt());
}

void ItemDetailsPanel::showItem(std::shared_ptr<Item> item)
{
    m_item = std::move(item);
    updateCurrentItem();
}

void ItemDetailsPanel::showItemLater(std::shared_ptr<Item> item)
{
    m_item = std::move(item);
    m_delayed_update_current_item.start();
}

void ItemDetailsPanel::clear()
{
    spdlog::trace("ItemDetailsPanel::clear() entered");
    m_item = nullptr;
    // A pending details update belongs to an item that is no longer shown.
    m_delayed_update_current_item.stop();
    ui->imageLabel->hide();
    ui->minimapLabel->hide();
    ui->locationLabel->hide();
    ui->itemTooltipWidget->hide();
    ui->itemButtonsWidget->hide();

    ui->nameLabel->setText("Select an item");
    ui->nameLabel->show();

    ui->pobTooltipButton->setEnabled(false);
}

void ItemDetailsPanel::showBucket(const ItemLocation &location)
{
    spdlog::trace("ItemDetailsPanel::showBucket() entered");
    m_item = nullptr;
    // A pending item details update would overwrite the bucket's.
    m_delayed_update_current_item.stop();
    ui->imageLabel->hide();
    ui->minimapLabel->hide();
    ui->locationLabel->hide();
    ui->itemTooltipWidget->hide();
    ui->itemButtonsWidget->hide();

    ui->nameLabel->setText(location.GetHeader());
    ui->nameLabel->show();

    ui->pobTooltipButton->setEnabled(false);
}

void ItemDetailsPanel::updateLocation(const Item &item)
{
    ui->locationLabel->setText(
        item.location().GetItemHeader(m_inventory->Canonical(item.location())));
}

// The url an item's image is fetched from: a relative icon is served by the CDN.
static QString itemImageUrl(const Item &item)
{
    const QString icon = item.icon();
    if (icon.startsWith('/')) {
        return POE_WEBCDN + icon;
    }
    return icon;
}

void ItemDetailsPanel::updateCurrentItem()
{
    spdlog::trace("ItemDetailsPanel::updateCurrentItem() entered");
    if (m_item == nullptr) {
        clear();
        return;
    }

    ui->imageLabel->show();
    ui->minimapLabel->show();
    ui->locationLabel->show();
    ui->itemTooltipWidget->show();
    ui->itemButtonsWidget->show();
    ui->nameLabel->hide();

    ui->imageLabel->setText("Loading...");
    ui->imageLabel->setStyleSheet("QLabel { background-color : rgb(12, 12, 43); color: white }");
    ui->imageLabel->setFixedSize(QSize(m_item->w(), m_item->h()) * PIXELS_PER_SLOT);

    // Everything except item image now lives in itemtooltip.cpp
    // in future should move everything tooltip-related there
    UpdateItemTooltip(*m_item, ui);

    // The location line renders through the canonical inventory (S4
    // review round 1): a metadata delta renames a tab without replacing
    // the items fetched from it, so the embedded location can be stale.
    // An item on a guardian or the Wickerman says so (#418).
    updateLocation(*m_item);
    ui->pobTooltipButton->setEnabled(m_item->Wearable());

    emit imageRequested(itemImageUrl(*m_item));
}

void ItemDetailsPanel::onImageFetched(const QString &url)
{
    if (m_item) {
        if (url == itemImageUrl(*m_item)) {
            const QImage image = m_image_cache->load(url);
            if (image.isNull()) {
                ui->imageLabel->setText("No image");
            } else {
                const QPixmap pixmap = GenerateItemIcon(*m_item, image);
                ui->imageLabel->setPixmap(pixmap);
            }
        }
    }
}

void ItemDetailsPanel::uploadToImgur()
{
    ui->uploadTooltipButton->setDisabled(true);
    ui->uploadTooltipButton->setText("Uploading...");

    QPixmap pixmap(ui->itemTooltipWidget->size());
    ui->itemTooltipWidget->render(&pixmap);

    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    pixmap.save(&buffer, "PNG"); // writes pixmap into bytes in PNG format

    QNetworkRequest request(QUrl("https://api.imgur.com/3/upload/"));
    request.setRawHeader("Authorization", "Client-ID d6d2d8a0437a90f");
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setTransferTimeout(kImgurUploadTimeout);
    QByteArray image_data = "image=" + QUrl::toPercentEncoding(bytes.toBase64());
    QNetworkReply *reply = m_network_manager->post(request, image_data);
    connect(reply, &QNetworkReply::finished, this, &ItemDetailsPanel::onUploadFinished);
}

void ItemDetailsPanel::copyForPathOfBuilding()
{
    if (m_item == nullptr) {
        return;
    }
    // if category isn't wearable, including flasks, don't do anything
    if (!m_item->Wearable()) {
        spdlog::warn("{}, category: {}, should not have been exportable.",
                     m_item->PrettyName(),
                     m_item->category());
        return;
    }

    QApplication::clipboard()->setText(m_item->POBformat());
    spdlog::info("{} was copied to your clipboard in Path of Building's \"Create custom\" format.",
                 m_item->PrettyName());
}

void ItemDetailsPanel::onUploadFinished()
{
    ui->uploadTooltipButton->setDisabled(false);
    ui->uploadTooltipButton->setText("Upload to imgur");

    QNetworkReply *reply = qobject_cast<QNetworkReply *>(QObject::sender());
    const QByteArray bytes = reply->readAll();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        spdlog::error("Imgur upload failed: {}", reply->errorString());
        return;
    }

    const auto result = ParseImgurUploadReply(bytes);
    if (!result) {
        spdlog::error("Imgur upload failed: {}", result.error());
        return;
    }

    const QString url = *result;
    QApplication::clipboard()->setText(url);
    spdlog::info("Image uploaded to '{}' and the URL has been copied to your clipboard.", url);
}
