// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2015 Ilya Zhuravlev

#include "ui/itemtooltip.h"

#include <QImage>
#include <QPainter>

#include <map>
#include <vector>

#include "ui_mainwindow.h"

#include "influence.h"
#include "item.h"
#include "itemconstants.h"
#include "ui/itemtooltiptext.h"
#include "util/spdlog_qt.h" // IWYU pragma: keep

constexpr int LINKH_HEIGHT = 16;
constexpr int LINKH_WIDTH = 38;
constexpr int LINKV_HEIGHT = LINKH_WIDTH;
constexpr int LINKV_WIDTH = LINKH_HEIGHT;

constexpr int HEADER_SINGLELINE_WIDTH = 29;
constexpr int HEADER_SINGLELINE_HEIGHT = 34;
constexpr int HEADER_DOUBLELINE_WIDTH = 44;
constexpr int HEADER_DOUBLELINE_HEIGHT = 54;

constexpr QSize HEADER_SINGLELINE_SIZE(HEADER_SINGLELINE_WIDTH, HEADER_SINGLELINE_HEIGHT);
constexpr QSize HEADER_DOUBLELINE_SIZE(HEADER_DOUBLELINE_WIDTH, HEADER_DOUBLELINE_HEIGHT);
constexpr QSize HEADER_OVERLAY_SIZE(27, 27);

class IMAGES
{
private:
    IMAGES() = default;

public:
    static const IMAGES &instance()
    {
        static IMAGES images;
        return images;
    }

    const QImage getSocketImage(const ItemSocket &socket) const
    {
        switch (socket.kind) {
        case SocketKind::Red:
            return socket_s;
        case SocketKind::Green:
            return socket_d;
        case SocketKind::Blue:
            return socket_i;
        case SocketKind::White:
            return socket_g;
        case SocketKind::Abyss:
            return socket_a;
        case SocketKind::Resonator:
            // There is no resonator socket art; draw nothing rather than
            // another kind's.
            return {};
        case SocketKind::Unknown:
            spdlog::error("ItemTooltip: unrecognised socket attribute");
            return {};
        }
        return {};
    }

    const QImage getSymbol(Item::INFLUENCE_TYPES influence) const
    {
        const auto found = symbols.find(influence);
        return found == symbols.end() ? QImage() : found->second;
    }

    const QImage socket_a{":/sockets/Socket_A.png"};
    const QImage socket_d{":/sockets/Socket_D.png"};
    const QImage socket_g{":/sockets/Socket_G.png"};
    const QImage socket_i{":/sockets/Socket_I.png"};
    const QImage socket_s{":/sockets/Socket_S.png"};
    const QImage link_h{":/sockets/Socket_Link_Horizontal.png"};
    const QImage link_v{":/sockets/Socket_Link_Vertical.png"};
    const std::map<Item::INFLUENCE_TYPES, QImage> symbols = [] {
        std::map<Item::INFLUENCE_TYPES, QImage> images;
        for (const auto influence : {Item::SHAPER,
                                     Item::ELDER,
                                     Item::CRUSADER,
                                     Item::REDEEMER,
                                     Item::HUNTER,
                                     Item::WARLORD,
                                     Item::SYNTHESISED,
                                     Item::FRACTURED,
                                     Item::SEARING_EXARCH,
                                     Item::EATER_OF_WORLDS}) {
            images.emplace(influence, QImage(InfluenceSymbolPath(influence)));
        }
        return images;
    }();
};

ItemFrameStyle ItemFrameStyleFor(int frame_type, bool singleline)
{
    // Header and separator art exists for White, Magic, Rare, Unique, Gem
    // and Currency (headers for Rare and Unique also in a single-line form).
    // A frame type without art of its own borrows the closest.
    struct Style
    {
        const char *header;
        const char *separator;
        const char *color;
    };
    static constexpr Style normal{"White", "White", "#c8c8c8"};
    Style style = normal;
    switch (frame_type) {
    case FrameType::FRAME_TYPE_NORMAL:
        break;
    case FrameType::FRAME_TYPE_MAGIC:
        style = {"Magic", "Magic", "#88f"};
        break;
    case FrameType::FRAME_TYPE_RARE:
        style = {"Rare", "Rare", "#ff7"};
        break;
    case FrameType::FRAME_TYPE_UNIQUE:
        style = {"Unique", "Unique", "#af6025"};
        break;
    case FrameType::FRAME_TYPE_GEM:
        style = {"Gem", "Gem", "#1ba29b"};
        break;
    case FrameType::FRAME_TYPE_CURRENCY:
        style = {"Currency", "Currency", "#aa9e82"};
        break;
    case FrameType::FRAME_TYPE_DIVINATION_CARD:
        // No divination card header art: borrow Currency's.
        style = {"Currency", "Currency", "#0ebaff"};
        break;
    case FrameType::FRAME_TYPE_QUEST_ITEM:
        // No quest art in the resources: borrow Gem's. (assets/separators
        // has SeparatorQuest.png, but it is not compiled in.)
        style = {"Gem", "Gem", "#4ae63a"};
        break;
    case FrameType::FRAME_TYPE_PROPHECY:
        // No prophecy header or separator art: borrow Magic's.
        style = {"Magic", "Magic", "#b54bff"};
        break;
    case FrameType::FRAME_TYPE_FOIL:
    case FrameType::FRAME_TYPE_SUPPORTER_FOIL:
        // Foil (relic) uniques: the Unique art in the relic colour.
        style = {"Unique", "Unique", "#82ad6a"};
        break;
    case FrameType::FRAME_TYPE_NECROPOLIS:
    case FrameType::FRAME_TYPE_GOLD:
    case FrameType::FRAME_TYPE_BREACH_SKILL:
    default:
        // Art and colour not known here: drawn as a normal item.
        break;
    }
    const QString header(style.header);
    const bool has_single_line_art = (header == "Rare") || (header == "Unique");
    const QString suffix = (singleline && has_single_line_art) ? "SingleLine" : "";
    return {":/headers/" + header + suffix, style.separator, style.color};
}

QString InfluenceBackgroundFor(const Item &item)
{
    // An item with both influences keeps the Elder art, as before.
    const char *art = item.hasInfluence(Item::ELDER)    ? "Elder"
                      : item.hasInfluence(Item::SHAPER) ? "Shaper"
                                                        : nullptr;
    if (!art) {
        return {};
    }
    const int width = item.w();
    const int height = item.h();
    // Art exists for widths 1-2 and heights 1-4, except 1x2.
    if (width < 1 || width > 2 || height < 1 || height > 4 || (width == 1 && height == 2)) {
        return {};
    }
    return QString(":/backgrounds/%1Background_%2x%3.png").arg(art).arg(width).arg(height);
}

static void UpdateMinimap(const Item &item, Ui::MainWindow *ui)
{
    QPixmap pixmap(MINIMAP_SIZE, MINIMAP_SIZE);
    pixmap.fill(QColor("transparent"));

    QPainter painter(&pixmap);
    painter.setBrush(QBrush(QColor(0x0c, 0x0b, 0x0b)));
    painter.drawRect(0, 0, MINIMAP_SIZE, MINIMAP_SIZE);
    const ItemLocation &location = item.location();
    painter.setBrush(QBrush(location.socketed() ? Qt::blue : Qt::red));
    QRectF rect = location.GetRect();
    painter.drawRect(rect);

    ui->minimapLabel->setPixmap(pixmap);
}

void GenerateItemHeaderSide(QLabel *itemHeader,
                            bool leftNotRight,
                            QString header_path_prefix,
                            bool singleline,
                            Item::INFLUENCE_TYPES base)
{
    static const auto &images = IMAGES::instance();

    QImage header(header_path_prefix + (leftNotRight ? "Left.png" : "Right.png"));
    QSize header_size = singleline ? HEADER_SINGLELINE_SIZE : HEADER_DOUBLELINE_SIZE;
    header = header.scaled(header_size);

    QPixmap header_pixmap(header_size);
    header_pixmap.fill(Qt::transparent);
    QPainter header_painter(&header_pixmap);
    header_painter.drawImage(0, 0, header);

    QImage overlay_image;

    if (base != Item::NONE) {
        overlay_image = images.getSymbol(base);
        overlay_image = overlay_image.scaled(HEADER_OVERLAY_SIZE);
        int overlay_x = singleline ? (leftNotRight ? 2 : 1) : (leftNotRight ? 2 : 15);
        int overlay_y = (int) (0.5 * (header.height() - overlay_image.height()));
        header_painter.drawImage(overlay_x, overlay_y, overlay_image);
    }

    itemHeader->setFixedSize(header_size);
    itemHeader->setPixmap(header_pixmap);
}

void UpdateItemTooltip(const Item &item, Ui::MainWindow *ui)
{
    const bool singleline = item.name().isEmpty();
    const ItemFrameStyle style = ItemFrameStyleFor(item.frameType(), singleline);
    const QString &key = style.separator_key;

    ui->propertiesLabel->setText(GenerateItemInfo(item, key, true));
    ui->itemTextTooltip->setText(GenerateItemInfo(item, key, false));
    UpdateMinimap(item, ui);

    if (singleline) {
        ui->itemNameFirstLine->hide();
        ui->itemNameSecondLine->setAlignment(Qt::AlignCenter);
        ui->itemNameContainerWidget->setFixedSize(16777215, HEADER_SINGLELINE_HEIGHT);
    } else {
        ui->itemNameFirstLine->show();
        ui->itemNameFirstLine->setAlignment(Qt::AlignBottom | Qt::AlignHCenter);
        ui->itemNameSecondLine->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
        ui->itemNameContainerWidget->setFixedSize(16777215, HEADER_DOUBLELINE_HEIGHT);
    }
    const QString &header_path_prefix = style.header_prefix;

    GenerateItemHeaderSide(ui->itemHeaderLeft,
                           true,
                           header_path_prefix,
                           singleline,
                           item.influenceLeft());
    GenerateItemHeaderSide(ui->itemHeaderRight,
                           false,
                           header_path_prefix,
                           singleline,
                           item.influenceRight());

    ui->itemNameContainerWidget->setStyleSheet(
        ("border-radius: 0px; border: 0px; border-image: url(" + header_path_prefix
         + "Middle.png);"));

    ui->itemNameFirstLine->setText(item.name());
    ui->itemNameSecondLine->setText(item.typeLine());

    const QString &color = style.color;
    const QString css
        = "border-image: none; background-color: transparent; font-size: 20px; color: " + color;
    ui->itemNameFirstLine->setStyleSheet(css);
    ui->itemNameSecondLine->setStyleSheet(css);
}

QPixmap GenerateItemSockets(const int width,
                            const int height,
                            const std::vector<ItemSocket> &sockets)
{
    static const auto &images = IMAGES::instance();

    QPixmap pixmap(width * PIXELS_PER_SLOT,
                   height
                       * PIXELS_PER_SLOT); // This will ensure we have enough room to draw the slots
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);

    int socket_rows = 0;
    int socket_columns = 0;
    ItemSocket prev = {255, SocketKind::Unknown};
    int i = 0;

    if (sockets.size() == 0) {
        // Do nothing
    } else if (sockets.size() == 1) {
        auto &socket = sockets.front();
        QImage socket_image = images.getSocketImage(socket);
        painter.drawImage(0, PIXELS_PER_SLOT * i, socket_image);
        socket_rows = 1;
        socket_columns = 1;
    } else {
        for (auto &socket : sockets) {
            const auto &link_h = images.link_h;
            const auto &link_v = images.link_v;
            bool link = socket.group == prev.group;
            QImage socket_image = images.getSocketImage(socket);
            if (width == 1) {
                painter.drawImage(0, PIXELS_PER_SLOT * i, socket_image);
                if (link) {
                    painter.drawImage(16, PIXELS_PER_SLOT * i - 19, link_v);
                }
                socket_columns = 1;
                socket_rows = i + 1;
            } else /* w == 2 */ {
                int row = i / 2;
                int column = i % 2;
                if (row % 2 == 1)
                    column = 1 - column;
                socket_columns = qMax(column + 1, socket_columns);
                socket_rows = qMax(row + 1, socket_rows);
                painter.drawImage(PIXELS_PER_SLOT * column, PIXELS_PER_SLOT * row, socket_image);
                if (link) {
                    switch (i) {
                    case 1:
                    case 3:
                    case 5:
                        // horizontal link
                        painter.drawImage(PIXELS_PER_SLOT - LINKH_WIDTH / 2,
                                          row * PIXELS_PER_SLOT + PIXELS_PER_SLOT / 2
                                              - LINKH_HEIGHT / 2,
                                          link_h);
                        break;
                    case 2:
                        painter.drawImage(PIXELS_PER_SLOT * 1.5 - LINKV_WIDTH / 2,
                                          row * PIXELS_PER_SLOT - LINKV_HEIGHT / 2,
                                          link_v);
                        break;
                    case 4:
                        painter.drawImage(PIXELS_PER_SLOT / 2 - LINKV_WIDTH / 2,
                                          row * PIXELS_PER_SLOT - LINKV_HEIGHT / 2,
                                          link_v);
                        break;
                    default:
                        spdlog::error("No idea how to draw link for socket {}", i);
                        break;
                    }
                }
            }

            prev = socket;
            ++i;
        }
    }

    return pixmap.copy(0, 0, PIXELS_PER_SLOT * socket_columns, PIXELS_PER_SLOT * socket_rows);
}

QPixmap GenerateItemIcon(const Item &item, const QImage &image)
{
    const int height = item.h();
    const int width = item.w();

    QPixmap layered(image.width(), image.height());
    layered.fill(Qt::transparent);
    QPainter layered_painter(&layered);

    // No background is drawn where no art exists (1x2, or a size wider or
    // taller than the art).
    const QString background = InfluenceBackgroundFor(item);
    if (!background.isEmpty()) {
        layered_painter.drawImage(0, 0, QImage(background));
    }

    layered_painter.drawImage(0, 0, image);

    if (item.text_sockets().size() > 0) {
        QPixmap sockets = GenerateItemSockets(width, height, item.text_sockets());

        layered_painter.drawPixmap((int) (0.5 * (image.width() - sockets.width())),
                                   (int) (0.5 * (image.height() - sockets.height())),
                                   sockets); // Center sockets on overall image
    }

    return layered;
}
