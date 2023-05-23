/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022, Matthew Costa <ucosty@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Tab.h"
#include "BrowserWindow.h"
#include "ConsoleWidget.h"
#include "InspectorWidget.h"
#include "Settings.h"
#include "Utilities.h"
#include <Browser/History.h>
#include <LibGfx/ImageFormats/BMPWriter.h>
#include <QClipboard>
#include <QCoreApplication>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPoint>
#include <QResizeEvent>
#include <QSvgRenderer>

extern DeprecatedString s_serenity_resource_root;
extern Browser::Settings* s_settings;

static QIcon render_svg_icon_with_theme_colors(QString name, QPalette const& palette)
{
    auto path = QString(":/Icons/%1.svg").arg(name);

    QSize icon_size(16, 16);

    QIcon icon;

    auto render = [&](QColor color) -> QPixmap {
        QImage image(icon_size, QImage::Format_ARGB32);
        image.fill(Qt::transparent);

        QPainter painter(&image);
        QSvgRenderer renderer(path);
        renderer.render(&painter);
        painter.setBrush(color);
        painter.setCompositionMode(QPainter::CompositionMode_SourceAtop);
        painter.fillRect(image.rect(), color);
        return QPixmap::fromImage(image);
    };

    icon.addPixmap(render(palette.color(QPalette::ColorGroup::Normal, QPalette::ColorRole::ButtonText)), QIcon::Mode::Normal);
    icon.addPixmap(render(palette.color(QPalette::ColorGroup::Disabled, QPalette::ColorRole::ButtonText)), QIcon::Mode::Disabled);

    return icon;
}

Tab::Tab(BrowserWindow* window, StringView webdriver_content_ipc_path, WebView::EnableCallgrindProfiling enable_callgrind_profiling)
    : QWidget(window)
    , m_window(window)
{
    m_layout = new QBoxLayout(QBoxLayout::Direction::TopToBottom, this);
    m_layout->setSpacing(0);
    m_layout->setContentsMargins(0, 0, 0, 0);

    m_view = new WebContentView(webdriver_content_ipc_path, enable_callgrind_profiling);
    m_toolbar = new QToolBar(this);
    m_location_edit = new LocationEdit(this);
    m_reset_zoom_button = new QToolButton(m_toolbar);

    m_hover_label = new QLabel(this);
    m_hover_label->hide();
    m_hover_label->setFrameShape(QFrame::Shape::Box);
    m_hover_label->setAutoFillBackground(true);

    auto* focus_location_editor_action = new QAction("Edit Location");
    focus_location_editor_action->setShortcut(QKeySequence("Ctrl+L"));
    addAction(focus_location_editor_action);

    m_layout->addWidget(m_toolbar);
    m_layout->addWidget(m_view);

    rerender_toolbar_icons();

    m_toolbar->addAction(&m_window->go_back_action());
    m_toolbar->addAction(&m_window->go_forward_action());
    m_toolbar->addAction(&m_window->reload_action());
    m_toolbar->addWidget(m_location_edit);
    m_reset_zoom_button->setToolTip("Reset zoom level");
    m_reset_zoom_button_action = m_toolbar->addWidget(m_reset_zoom_button);
    m_reset_zoom_button_action->setVisible(false);

    QObject::connect(m_reset_zoom_button, &QAbstractButton::clicked, [this] {
        view().reset_zoom();
        update_reset_zoom_button();
    });

    view().on_activate_tab = [this] {
        m_window->activate_tab(tab_index());
    };

    view().on_close = [this] {
        m_window->close_tab(tab_index());
    };

    view().on_link_hover = [this](auto const& url) {
        m_hover_label->setText(qstring_from_ak_deprecated_string(url.to_deprecated_string()));
        update_hover_label();
        m_hover_label->show();
    };

    view().on_link_unhover = [this]() {
        m_hover_label->hide();
    };

    view().on_back_button = [this] {
        back();
    };

    view().on_forward_button = [this] {
        forward();
    };

    view().on_load_start = [this](const URL& url, bool is_redirect) {
        // If we are loading due to a redirect, we replace the current history entry
        // with the loaded URL
        if (is_redirect) {
            m_history.replace_current(url, m_title.toUtf8().data());
        }

        m_location_edit->setText(url.to_deprecated_string().characters());
        m_location_edit->setCursorPosition(0);

        // Don't add to history if back or forward is pressed
        if (!m_is_history_navigation) {
            m_history.push(url, m_title.toUtf8().data());
        }
        m_is_history_navigation = false;

        m_window->go_back_action().setEnabled(m_history.can_go_back());
        m_window->go_forward_action().setEnabled(m_history.can_go_forward());

        if (m_inspector_widget)
            m_inspector_widget->clear_dom_json();

        if (m_console_widget)
            m_console_widget->reset();
    };

    view().on_load_finish = [this](auto&) {
        if (m_inspector_widget != nullptr && m_inspector_widget->isVisible()) {
            view().inspect_dom_tree();
            view().inspect_accessibility_tree();
        }
    };

    QObject::connect(m_location_edit, &QLineEdit::returnPressed, this, &Tab::location_edit_return_pressed);

    view().on_title_change = [this](auto const& title) {
        m_title = qstring_from_ak_deprecated_string(title);
        m_history.update_title(title);

        emit title_changed(tab_index(), m_title);
    };

    view().on_favicon_change = [this](auto const& bitmap) {
        auto qimage = QImage(bitmap.scanline_u8(0), bitmap.width(), bitmap.height(), QImage::Format_ARGB32);
        if (qimage.isNull())
            return;
        auto qpixmap = QPixmap::fromImage(qimage);
        if (qpixmap.isNull())
            return;
        emit favicon_changed(tab_index(), QIcon(qpixmap));
    };

    QObject::connect(focus_location_editor_action, &QAction::triggered, this, &Tab::focus_location_editor);

    view().on_get_source = [this](auto const& url, auto const& source) {
        auto* text_edit = new QPlainTextEdit(this);
        text_edit->setWindowFlags(Qt::Window);
        text_edit->setFont(QFontDatabase::systemFont(QFontDatabase::SystemFont::FixedFont));
        text_edit->resize(800, 600);
        text_edit->setWindowTitle(qstring_from_ak_deprecated_string(url.to_deprecated_string()));
        text_edit->setPlainText(qstring_from_ak_deprecated_string(source));
        text_edit->show();
    };

    view().on_navigate_back = [this]() {
        back();
    };

    view().on_navigate_forward = [this]() {
        forward();
    };

    view().on_refresh = [this]() {
        reload();
    };

    view().on_restore_window = [this]() {
        m_window->showNormal();
    };

    view().on_reposition_window = [this](auto const& position) {
        m_window->move(position.x(), position.y());
        return Gfx::IntPoint { m_window->x(), m_window->y() };
    };

    view().on_resize_window = [this](auto const& size) {
        m_window->resize(size.width(), size.height());
        return Gfx::IntSize { m_window->width(), m_window->height() };
    };

    view().on_maximize_window = [this]() {
        m_window->showMaximized();
        return Gfx::IntRect { m_window->x(), m_window->y(), m_window->width(), m_window->height() };
    };

    view().on_minimize_window = [this]() {
        m_window->showMinimized();
        return Gfx::IntRect { m_window->x(), m_window->y(), m_window->width(), m_window->height() };
    };

    view().on_fullscreen_window = [this]() {
        m_window->showFullScreen();
        return Gfx::IntRect { m_window->x(), m_window->y(), m_window->width(), m_window->height() };
    };

    view().on_get_dom_tree = [this](auto& dom_tree) {
        if (m_inspector_widget)
            m_inspector_widget->set_dom_json(dom_tree);
    };

    view().on_get_accessibility_tree = [this](auto& accessibility_tree) {
        if (m_inspector_widget)
            m_inspector_widget->set_accessibility_json(accessibility_tree);
    };

    view().on_js_console_new_message = [this](auto message_index) {
        if (m_console_widget)
            m_console_widget->notify_about_new_console_message(message_index);
    };

    view().on_get_js_console_messages = [this](auto start_index, auto& message_types, auto& messages) {
        if (m_console_widget)
            m_console_widget->handle_console_messages(start_index, message_types, messages);
    };

    auto* take_visible_screenshot_action = new QAction("Take &Visible Screenshot", this);
    take_visible_screenshot_action->setIcon(QIcon(QString("%1/res/icons/16x16/filetype-image.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(take_visible_screenshot_action, &QAction::triggered, this, [this]() {
        if (auto result = view().take_screenshot(WebView::ViewImplementation::ScreenshotType::Visible); result.is_error()) {
            auto error = String::formatted("{}", result.error()).release_value_but_fixme_should_propagate_errors();
            QMessageBox::warning(this, "Ladybird", qstring_from_ak_string(error));
        }
    });

    auto* take_full_screenshot_action = new QAction("Take &Full Screenshot", this);
    take_full_screenshot_action->setIcon(QIcon(QString("%1/res/icons/16x16/filetype-image.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(take_full_screenshot_action, &QAction::triggered, this, [this]() {
        if (auto result = view().take_screenshot(WebView::ViewImplementation::ScreenshotType::Full); result.is_error()) {
            auto error = String::formatted("{}", result.error()).release_value_but_fixme_should_propagate_errors();
            QMessageBox::warning(this, "Ladybird", qstring_from_ak_string(error));
        }
    });

    m_page_context_menu = make<QMenu>("Context menu", this);
    m_page_context_menu->addAction(&m_window->go_back_action());
    m_page_context_menu->addAction(&m_window->go_forward_action());
    m_page_context_menu->addAction(&m_window->reload_action());
    m_page_context_menu->addSeparator();
    m_page_context_menu->addAction(&m_window->copy_selection_action());
    m_page_context_menu->addAction(&m_window->select_all_action());
    m_page_context_menu->addSeparator();
    m_page_context_menu->addAction(take_visible_screenshot_action);
    m_page_context_menu->addAction(take_full_screenshot_action);
    m_page_context_menu->addSeparator();
    m_page_context_menu->addAction(&m_window->view_source_action());
    m_page_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_context_menu_request = [this](auto widget_position) {
        auto screen_position = view().mapToGlobal(QPoint { widget_position.x(), widget_position.y() });
        m_page_context_menu->exec(screen_position);
    };

    auto* open_link_action = new QAction("&Open", this);
    open_link_action->setIcon(QIcon(QString("%1/res/icons/16x16/go-forward.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(open_link_action, &QAction::triggered, this, [this]() {
        open_link(m_link_context_menu_url);
    });

    auto* open_link_in_new_tab_action = new QAction("&Open in New &Tab", this);
    open_link_in_new_tab_action->setIcon(QIcon(QString("%1/res/icons/16x16/new-tab.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(open_link_in_new_tab_action, &QAction::triggered, this, [this]() {
        open_link_in_new_tab(m_link_context_menu_url);
    });

    auto* copy_url_action = new QAction("Copy &URL", this);
    copy_url_action->setIcon(QIcon(QString("%1/res/icons/16x16/edit-copy.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(copy_url_action, &QAction::triggered, this, [this]() {
        copy_link_url(m_link_context_menu_url);
    });

    m_link_context_menu = make<QMenu>("Link context menu", this);
    m_link_context_menu->addAction(open_link_action);
    m_link_context_menu->addAction(open_link_in_new_tab_action);
    m_link_context_menu->addSeparator();
    m_link_context_menu->addAction(copy_url_action);
    m_link_context_menu->addSeparator();
    m_link_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_link_context_menu_request = [this](auto const& url, auto widget_position) {
        m_link_context_menu_url = url;

        auto screen_position = view().mapToGlobal(QPoint { widget_position.x(), widget_position.y() });
        m_link_context_menu->exec(screen_position);
    };

    auto* open_image_action = new QAction("&Open Image", this);
    open_image_action->setIcon(QIcon(QString("%1/res/icons/16x16/filetype-image.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(open_image_action, &QAction::triggered, this, [this]() {
        open_link(m_image_context_menu_url);
    });

    auto* open_image_in_new_tab_action = new QAction("&Open Image in New &Tab", this);
    open_image_in_new_tab_action->setIcon(QIcon(QString("%1/res/icons/16x16/new-tab.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(open_image_in_new_tab_action, &QAction::triggered, this, [this]() {
        open_link_in_new_tab(m_image_context_menu_url);
    });

    auto* copy_image_action = new QAction("&Copy Image", this);
    copy_image_action->setIcon(QIcon(QString("%1/res/icons/16x16/edit-copy.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(copy_image_action, &QAction::triggered, this, [this]() {
        auto* bitmap = m_image_context_menu_bitmap.bitmap();
        if (bitmap == nullptr)
            return;

        auto data = Gfx::BMPWriter::encode(*bitmap);
        if (data.is_error())
            return;

        auto image = QImage::fromData(data.value().data(), data.value().size(), "BMP");
        if (image.isNull())
            return;

        auto* clipboard = QGuiApplication::clipboard();
        clipboard->setImage(image);
    });

    auto* copy_image_url_action = new QAction("Copy Image &URL", this);
    copy_image_url_action->setIcon(QIcon(QString("%1/res/icons/16x16/edit-copy.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(copy_image_url_action, &QAction::triggered, this, [this]() {
        copy_link_url(m_image_context_menu_url);
    });

    m_image_context_menu = make<QMenu>("Image context menu", this);
    m_image_context_menu->addAction(open_image_action);
    m_image_context_menu->addAction(open_image_in_new_tab_action);
    m_image_context_menu->addSeparator();
    m_image_context_menu->addAction(copy_image_action);
    m_image_context_menu->addAction(copy_image_url_action);
    m_image_context_menu->addSeparator();
    m_image_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_image_context_menu_request = [this](auto& image_url, auto widget_position, Gfx::ShareableBitmap const& shareable_bitmap) {
        m_image_context_menu_url = image_url;
        m_image_context_menu_bitmap = shareable_bitmap;

        auto screen_position = view().mapToGlobal(QPoint { widget_position.x(), widget_position.y() });
        m_image_context_menu->exec(screen_position);
    };

    m_video_context_menu_play_icon = make<QIcon>(QString("%1/res/icons/16x16/play.png").arg(s_serenity_resource_root.characters()));
    m_video_context_menu_pause_icon = make<QIcon>(QString("%1/res/icons/16x16/pause.png").arg(s_serenity_resource_root.characters()));

    m_video_context_menu_play_pause_action = make<QAction>("&Play", this);
    m_video_context_menu_play_pause_action->setIcon(*m_video_context_menu_play_icon);
    QObject::connect(m_video_context_menu_play_pause_action, &QAction::triggered, this, [this]() {
        view().toggle_video_play_state();
    });

    m_video_context_menu_controls_action = make<QAction>("Show &Controls", this);
    m_video_context_menu_controls_action->setCheckable(true);
    QObject::connect(m_video_context_menu_controls_action, &QAction::triggered, this, [this]() {
        view().toggle_video_controls_state();
    });

    m_video_context_menu_loop_action = make<QAction>("&Loop Video", this);
    m_video_context_menu_loop_action->setCheckable(true);
    QObject::connect(m_video_context_menu_loop_action, &QAction::triggered, this, [this]() {
        view().toggle_video_loop_state();
    });

    auto* open_video_action = new QAction("&Open Video", this);
    open_video_action->setIcon(QIcon(QString("%1/res/icons/16x16/filetype-video.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(open_video_action, &QAction::triggered, this, [this]() {
        open_link(m_video_context_menu_url);
    });

    auto* open_video_in_new_tab_action = new QAction("Open Video in New &Tab", this);
    open_video_in_new_tab_action->setIcon(QIcon(QString("%1/res/icons/16x16/new-tab.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(open_video_in_new_tab_action, &QAction::triggered, this, [this]() {
        open_link_in_new_tab(m_video_context_menu_url);
    });

    auto* copy_video_url_action = new QAction("Copy Video &URL", this);
    copy_video_url_action->setIcon(QIcon(QString("%1/res/icons/16x16/edit-copy.png").arg(s_serenity_resource_root.characters())));
    QObject::connect(copy_video_url_action, &QAction::triggered, this, [this]() {
        copy_link_url(m_video_context_menu_url);
    });

    m_video_context_menu = make<QMenu>("Video context menu", this);
    m_video_context_menu->addAction(m_video_context_menu_play_pause_action);
    m_video_context_menu->addAction(m_video_context_menu_controls_action);
    m_video_context_menu->addAction(m_video_context_menu_loop_action);
    m_video_context_menu->addSeparator();
    m_video_context_menu->addAction(open_video_action);
    m_video_context_menu->addAction(open_video_in_new_tab_action);
    m_video_context_menu->addSeparator();
    m_video_context_menu->addAction(copy_video_url_action);
    m_video_context_menu->addSeparator();
    m_video_context_menu->addAction(&m_window->inspect_dom_node_action());

    view().on_video_context_menu_request = [this](auto const& video_url, auto widget_position, bool is_playing, bool has_user_agent_controls, bool is_looping) {
        m_video_context_menu_url = video_url;

        if (is_playing) {
            m_video_context_menu_play_pause_action->setIcon(*m_video_context_menu_play_icon);
            m_video_context_menu_play_pause_action->setText("&Play");
        } else {
            m_video_context_menu_play_pause_action->setIcon(*m_video_context_menu_pause_icon);
            m_video_context_menu_play_pause_action->setText("&Pause");
        }

        m_video_context_menu_controls_action->setChecked(has_user_agent_controls);
        m_video_context_menu_loop_action->setChecked(is_looping);

        auto screen_position = view().mapToGlobal(QPoint { widget_position.x(), widget_position.y() });
        m_video_context_menu->exec(screen_position);
    };
}

Tab::~Tab()
{
    close_sub_widgets();
}

void Tab::update_reset_zoom_button()
{
    auto zoom_level = view().zoom_level();
    if (zoom_level != 1.0f) {
        auto zoom_level_text = MUST(String::formatted("{}%", round_to<int>(zoom_level * 100)));
        m_reset_zoom_button->setText(qstring_from_ak_string(zoom_level_text));
        m_reset_zoom_button_action->setVisible(true);
    } else {
        m_reset_zoom_button_action->setVisible(false);
    }
}

void Tab::focus_location_editor()
{
    m_location_edit->setFocus();
    m_location_edit->selectAll();
}

void Tab::navigate(QString url, LoadType load_type)
{
    if (!url.startsWith("http://", Qt::CaseInsensitive) && !url.startsWith("https://", Qt::CaseInsensitive) && !url.startsWith("file://", Qt::CaseInsensitive) && !url.startsWith("about:", Qt::CaseInsensitive))
        url = "https://" + url;
    m_is_history_navigation = (load_type == LoadType::HistoryNavigation);
    view().load(ak_deprecated_string_from_qstring(url));
}

void Tab::back()
{
    if (!m_history.can_go_back())
        return;

    m_is_history_navigation = true;
    m_history.go_back();
    view().load(m_history.current().url.to_deprecated_string());
}

void Tab::forward()
{
    if (!m_history.can_go_forward())
        return;

    m_is_history_navigation = true;
    m_history.go_forward();
    view().load(m_history.current().url.to_deprecated_string());
}

void Tab::reload()
{
    m_is_history_navigation = true;
    view().load(m_history.current().url.to_deprecated_string());
}

void Tab::open_link(URL const& url)
{
    view().on_link_click(url, "", 0);
}

void Tab::open_link_in_new_tab(URL const& url)
{
    view().on_link_click(url, "_blank", 0);
}

void Tab::copy_link_url(URL const& url)
{
    auto* clipboard = QGuiApplication::clipboard();
    clipboard->setText(qstring_from_ak_deprecated_string(url.to_deprecated_string()));
}

void Tab::location_edit_return_pressed()
{
    navigate(m_location_edit->text());
}

int Tab::tab_index()
{
    return m_window->tab_index(this);
}

void Tab::debug_request(DeprecatedString const& request, DeprecatedString const& argument)
{
    if (request == "dump-history")
        m_history.dump();
    else
        m_view->debug_request(request, argument);
}

void Tab::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (m_hover_label->isVisible())
        update_hover_label();
}

void Tab::update_hover_label()
{
    m_hover_label->resize(QFontMetrics(m_hover_label->font()).boundingRect(m_hover_label->text()).adjusted(-4, -2, 4, 2).size());
    m_hover_label->move(6, height() - m_hover_label->height() - 8);
    m_hover_label->raise();
}

bool Tab::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange) {
        rerender_toolbar_icons();
        return QWidget::event(event);
    }

    return QWidget::event(event);
}

void Tab::rerender_toolbar_icons()
{
    m_window->go_back_action().setIcon(render_svg_icon_with_theme_colors("back", palette()));
    m_window->go_forward_action().setIcon(render_svg_icon_with_theme_colors("forward", palette()));
    m_window->reload_action().setIcon(render_svg_icon_with_theme_colors("reload", palette()));
}

void Tab::show_inspector_window(InspectorTarget inspector_target)
{
    bool inspector_previously_loaded = m_inspector_widget != nullptr;

    if (!m_inspector_widget) {
        m_inspector_widget = new Ladybird::InspectorWidget;
        m_inspector_widget->setWindowTitle("Inspector");
        m_inspector_widget->resize(640, 480);
        m_inspector_widget->on_close = [this] {
            view().clear_inspected_dom_node();
        };

        m_inspector_widget->on_dom_node_inspected = [&](auto id, auto pseudo_element) {
            return view().inspect_dom_node(id, pseudo_element);
        };
    }

    if (!inspector_previously_loaded || !m_inspector_widget->dom_loaded()) {
        view().inspect_dom_tree();
        view().inspect_accessibility_tree();
    }

    m_inspector_widget->show();

    if (inspector_target == InspectorTarget::HoveredElement) {
        auto hovered_node = view().get_hovered_node_id();
        m_inspector_widget->set_selection({ hovered_node });
    } else {
        m_inspector_widget->select_default_node();
    }
}

void Tab::show_console_window()
{
    if (!m_console_widget) {
        m_console_widget = new Ladybird::ConsoleWidget;
        m_console_widget->setWindowTitle("JS Console");
        m_console_widget->resize(640, 480);
        m_console_widget->on_js_input = [this](auto js_source) {
            view().js_console_input(js_source);
        };
        m_console_widget->on_request_messages = [this](i32 start_index) {
            view().js_console_request_messages(start_index);
        };
    }

    m_console_widget->show();
}

void Tab::close_sub_widgets()
{
    auto close_widget_window = [](auto* widget) {
        if (widget)
            widget->close();
    };

    close_widget_window(m_console_widget);
    close_widget_window(m_inspector_widget);
}
