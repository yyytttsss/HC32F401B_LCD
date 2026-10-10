#include "hid_transport.hpp"
#include "iap_client.hpp"
#include "iap_protocol.hpp"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

QString style_sheet() {
    return QStringLiteral(
        "QWidget { font-family: 'Microsoft YaHei UI', 'Segoe UI', sans-serif; font-size: 13px; }"
        "QWidget#main_bg { background: #EEF1F6; }"

        "QGroupBox { background: #FFFFFF; border: 1px solid #D8DEE6; border-radius: 10px; "
        "            margin-top: 10px; padding: 18px 10px 10px 10px; "
        "            font-weight: 600; font-size: 13px; color: #3A4A5C; }"
        "QGroupBox::title { subcontrol-origin: margin; left: 14px; padding: 0 6px; }"

        "QLabel { color: #4A5568; }"
        "QLabel#field_label { color: #718096; font-size: 12px; }"
        "QLabel#field_value { color: #2D3748; font-size: 13px; font-weight: 500; "
        "                    font-family: 'Consolas','Cascadia Code',monospace; }"
        "QLabel#status_label { font-size: 14px; font-weight: 600; padding: 4px 12px; border-radius: 4px; }"
        "QLabel#batch_count { font-size: 18px; font-weight: 700; color: #2B6CB0; }"
        "QLabel#msg_label { color: #718096; font-size: 12px; }"

        "QPushButton { background: #4299E1; color: white; border: none; border-radius: 5px; "
        "              padding: 7px 18px; font-weight: 600; min-height: 20px; }"
        "QPushButton:hover { background: #3182CE; }"
        "QPushButton:pressed { background: #2C5282; }"
        "QPushButton:disabled { background: #CBD5E0; color: #A0AEC0; }"
        "QPushButton#btn_danger { background: #E53E3E; }"
        "QPushButton#btn_danger:hover { background: #C53030; }"
        "QPushButton#btn_danger:disabled { background: #FEB2B2; color: #FFFFFF; }"
        "QPushButton#btn_success { background: #38A169; }"
        "QPushButton#btn_success:hover { background: #2F855A; }"
        "QPushButton#btn_success:disabled { background: #9AE6B4; color: #FFFFFF; }"
        "QPushButton#btn_secondary { background: #EDF2F7; color: #4A5568; border: 1px solid #CBD5E0; }"
        "QPushButton#btn_secondary:hover { background: #E2E8F0; }"
        "QPushButton#btn_secondary:disabled { background: #E2E8F0; color: #A0AEC0; border: 1px solid #E2E8F0; }"

        "QProgressBar { background: #EDF2F7; border: 1px solid #E2E8F0; border-radius: 4px; "
        "               text-align: center; color: #4A5568; font-size: 12px; min-height: 16px; }"
        "QProgressBar::chunk { background: #48BB78; border-radius: 3px; }"

        "QScrollBar:vertical { background: #EDF2F7; width: 8px; border-radius: 4px; }"
        "QScrollBar::handle:vertical { background: #A0AEC0; border-radius: 4px; min-height: 20px; }"
        "QScrollBar::handle:vertical:hover { background: #718096; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
    );
}

}  // namespace

class SquareCanvas : public QWidget {
    Q_OBJECT
public:
    explicit SquareCanvas(QWidget* parent = nullptr) : QWidget(parent) {
        setMouseTracking(true);
        setAttribute(Qt::WA_OpaquePaintEvent);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setCursor(Qt::ArrowCursor);
    }

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w; }

    void set_image(const QImage& img) {
        source_ = img;
        scale_ = 1.0;
        offset_ = QPointF(0, 0);
        adjust_view();
        setCursor(source_.isNull() ? Qt::ArrowCursor : Qt::OpenHandCursor);
        update();
    }

    void set_landscape(bool landscape) {
        landscape_ = landscape;
        update();
    }

    void zoom_in() { zoom_by(1.25); }
    void zoom_out() { zoom_by(1.0 / 1.25); }

    bool has_image() const { return !source_.isNull(); }
    const QImage& image() const { return source_; }

    QRect crop_rect() const {
        const int side = std::min(width(), height());
        const int crop_w = landscape_ ? side : static_cast<int>(side * 240.0 / 320.0);
        const int crop_h = landscape_ ? static_cast<int>(side * 240.0 / 320.0) : side;
        return QRect((width() - crop_w) / 2, (height() - crop_h) / 2, crop_w, crop_h);
    }

    QImage crop_to_image(int target_w, int target_h) const {
        if (source_.isNull() || scale_ <= 0) return QImage();
        const QRect cr = crop_rect();
        const double sx = (cr.x() - offset_.x()) / scale_;
        const double sy = (cr.y() - offset_.y()) / scale_;
        const double sw = cr.width() / scale_;
        const double sh = cr.height() / scale_;
        const int ix = std::max(0, static_cast<int>(sx));
        const int iy = std::max(0, static_cast<int>(sy));
        const int iw = std::min(static_cast<int>(source_.width()) - ix, static_cast<int>(sw));
        const int ih = std::min(static_cast<int>(source_.height()) - iy, static_cast<int>(sh));
        QImage sub = source_.copy(ix, iy, iw, ih);
        return sub.scaled(target_w, target_h, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(128, 128, 128));
        if (source_.isNull()) {
            p.setPen(QColor(80, 80, 80));
            p.setFont(QFont("Microsoft YaHei UI", 14));
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("点击\"图片导入\"加载图片"));
            return;
        }
        p.translate(offset_);
        p.scale(scale_, scale_);
        p.drawImage(0, 0, source_);
        p.resetTransform();

        const QRect cr = crop_rect();
        p.setClipRegion(QRegion(rect()) - QRegion(cr));
        p.fillRect(rect(), QColor(0, 0, 0, 220));
        p.setClipping(false);

        p.setPen(QPen(QColor(255, 255, 255, 180), 2));
        p.setBrush(Qt::NoBrush);
        p.drawRect(cr);

        /* 底部提示文字 */
        const QString tip = QStringLiteral("滚轮缩放 · 拖拽移动");
        const QFont tipFont("Microsoft YaHei UI", 10);
        p.setFont(tipFont);
        const QFontMetrics fm(tipFont);
        const int tw = fm.horizontalAdvance(tip);
        const int th = fm.height();
        const int pad = 10;
        const QRect tipRect((width() - tw) / 2 - pad, height() - th - pad * 2,
                            tw + pad * 2, th + pad);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 120));
        p.drawRoundedRect(tipRect, 6, 6);
        p.setPen(QColor(255, 255, 255, 200));
        p.drawText(tipRect, Qt::AlignCenter, tip);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (source_.isNull()) return;
        last_pos_ = e->pos();
        setCursor(Qt::ClosedHandCursor);
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        if (source_.isNull()) return;
        const QPointF delta = e->pos() - last_pos_;
        last_pos_ = e->pos();
        if (e->buttons() & Qt::LeftButton) {
            offset_ += delta;
            adjust_offset();
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent* e) override {
        Q_UNUSED(e);
        if (!source_.isNull()) setCursor(Qt::OpenHandCursor);
    }

    void wheelEvent(QWheelEvent* e) override {
        if (source_.isNull()) return;
        const double factor = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
        const QPointF pos = e->posF();
        const QPointF img_pos = (pos - offset_) / scale_;
        scale_ *= factor;
        scale_ = std::max(0.05, std::min(20.0, scale_));
        offset_ = pos - img_pos * scale_;
        adjust_offset();
        update();
    }

    void resizeEvent(QResizeEvent* e) override {
        if (source_.isNull() || scale_ <= 0) {
            QWidget::resizeEvent(e);
            return;
        }
        const QSize old_size = e->oldSize();
        if (old_size.width() <= 0 || old_size.height() <= 0) {
            QWidget::resizeEvent(e);
            return;
        }
        /* 根据旧尺寸计算旧裁剪框中心 */
        const int old_side = std::min(old_size.width(), old_size.height());
        const int old_crop_w = landscape_ ? old_side : static_cast<int>(old_side * 240.0 / 320.0);
        const int old_crop_h = landscape_ ? static_cast<int>(old_side * 240.0 / 320.0) : old_side;
        const QPoint old_center((old_size.width() - old_crop_w) / 2 + old_crop_w / 2,
                                (old_size.height() - old_crop_h) / 2 + old_crop_h / 2);
        /* 旧裁剪框中心对应的图片坐标 */
        const QPointF img_center = (old_center - offset_) / scale_;
        /* 新裁剪框中心对准同一个图片点 */
        const QPoint new_center = crop_rect().center();
        offset_ = new_center - img_center * scale_;
        adjust_offset();
        QWidget::resizeEvent(e);
    }

private:
    void zoom_by(double factor) {
        if (source_.isNull()) return;
        const QPointF center = crop_rect().center();
        const QPointF img_pos = (center - offset_) / scale_;
        scale_ *= factor;
        scale_ = std::max(0.05, std::min(20.0, scale_));
        offset_ = center - img_pos * scale_;
        adjust_offset();
        update();
    }

    void adjust_view() {
        if (source_.isNull()) return;
        const QRect cr = crop_rect();
        const double sx = static_cast<double>(cr.width()) / source_.width();
        const double sy = static_cast<double>(cr.height()) / source_.height();
        scale_ = std::min(sx, sy) * 0.9;
        const double w = source_.width() * scale_;
        const double h = source_.height() * scale_;
        offset_ = QPointF(cr.x() + (cr.width() - w) / 2.0,
                          cr.y() + (cr.height() - h) / 2.0);
    }

    void adjust_offset() {
        const QRect cr = crop_rect();
        const double w = source_.width() * scale_;
        const double h = source_.height() * scale_;
        /* 约束：裁剪框必须在图片范围内。图片比裁剪框小就居中。 */
        if (w <= cr.width()) {
            offset_.setX(cr.x() + (cr.width() - w) / 2.0);
        } else {
            const double min_x = cr.right() - w + 1;
            const double max_x = cr.left();
            offset_.setX(std::min(max_x, std::max(min_x, offset_.x())));
        }
        if (h <= cr.height()) {
            offset_.setY(cr.y() + (cr.height() - h) / 2.0);
        } else {
            const double min_y = cr.bottom() - h + 1;
            const double max_y = cr.top();
            offset_.setY(std::min(max_y, std::max(min_y, offset_.y())));
        }
    }

    QImage source_;
    QPointF offset_{0, 0};
    double scale_ = 1.0;
    QPointF last_pos_;
    bool landscape_ = true;
};

class JpgToolWindow : public QWidget {
    Q_OBJECT
public:
    explicit JpgToolWindow(QWidget* parent = nullptr) : QWidget(parent) {
        setWindowTitle(QStringLiteral("JPG 图片管理工具"));
        resize(720, 780);
        setMinimumSize(640, 700);
        setObjectName("main_bg");
        setStyleSheet(style_sheet());
        build_ui();
        load_cache();
        sync_canvas_orientation();
        update_button_states();
        discovery_timer_ = new QTimer(this);
        discovery_timer_->setInterval(50);
        connect(discovery_timer_, &QTimer::timeout, this, [this] { poll_device(); });
        discovery_timer_->start();
        poll_device();
    }

    ~JpgToolWindow() {
        batch_stop_.store(true);
        if (worker_.joinable()) worker_.join();
    }

private:
    QLabel* make_label(const QString& text, const QString& obj = "field_label") {
        auto* lbl = new QLabel(text);
        lbl->setObjectName(obj);
        return lbl;
    }

    void build_ui() {
        auto* main_layout = new QVBoxLayout(this);
        main_layout->setContentsMargins(12, 12, 12, 12);
        main_layout->setSpacing(10);

        /* === 头部：设备信息 === */
        auto* header_box = new QGroupBox(QStringLiteral("  设备信息"));
        auto* header_lay = new QHBoxLayout(header_box);
        header_lay->setSpacing(20);
        header_lay->setContentsMargins(14, 20, 14, 14);

        header_lay->addWidget(make_label("设备名称"));
        name_val_ = make_label("--", "field_value");
        header_lay->addWidget(name_val_);
        header_lay->addSpacing(30);

        header_lay->addWidget(make_label("UID"));
        uid_val_ = make_label("--", "field_value");
        header_lay->addWidget(uid_val_);
        header_lay->addStretch();

        status_label_ = new QLabel(QStringLiteral("● 未连接"));
        status_label_->setObjectName("status_label");
        status_label_->setStyleSheet("color: #D69E2E; background: #FEF5E7; padding: 4px 12px; border-radius: 4px;");
        header_lay->addWidget(status_label_);

        main_layout->addWidget(header_box);

        /* === 中间：正方形图片编辑 === */
        auto* edit_box = new QGroupBox(QStringLiteral("  图片编辑"));
        auto* edit_lay = new QVBoxLayout(edit_box);
        edit_lay->setSpacing(8);
        edit_lay->setContentsMargins(10, 20, 10, 10);

        auto* edit_frame = new QFrame;
        edit_frame->setStyleSheet("QFrame { background: #808080; border: 1px solid #4A5568; border-radius: 8px; }");
        edit_frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        edit_frame->setMinimumSize(300, 300);
        auto* frame_lay = new QVBoxLayout(edit_frame);
        frame_lay->setContentsMargins(0, 0, 0, 0);
        frame_lay->setSpacing(0);
        edit_canvas_ = new SquareCanvas;
        edit_canvas_->setStyleSheet("background: transparent; border: none;");
        frame_lay->addWidget(edit_canvas_);
        edit_lay->addWidget(edit_frame, 1);

        auto* tool_row = new QHBoxLayout;
        tool_row->setSpacing(6);
        import_button_ = new QPushButton(QStringLiteral("  图片导入"));
        import_button_->setObjectName("btn_default");
        zoom_in_button_ = new QPushButton(QStringLiteral("  放大"));
        zoom_in_button_->setObjectName("btn_secondary");
        zoom_out_button_ = new QPushButton(QStringLiteral("  缩小"));
        zoom_out_button_->setObjectName("btn_secondary");
        save_button_ = new QPushButton(QStringLiteral("  保存"));
        save_button_->setObjectName("btn_secondary");
        finish_button_ = new QPushButton(QStringLiteral("  完成"));
        finish_button_->setObjectName("btn_success");
        tool_row->addWidget(import_button_);
        tool_row->addWidget(zoom_in_button_);
        tool_row->addWidget(zoom_out_button_);
        tool_row->addStretch();
        tool_row->addWidget(save_button_);
        tool_row->addWidget(finish_button_);
        edit_lay->addLayout(tool_row);

        main_layout->addWidget(edit_box, 1);

        /* === 尾部：设备操作 === */
        auto* action_box = new QGroupBox(QStringLiteral("  设备操作"));
        auto* act_lay = new QVBoxLayout(action_box);
        act_lay->setSpacing(8);
        act_lay->setContentsMargins(14, 20, 14, 14);

        auto* btn_row = new QHBoxLayout;
        btn_row->setSpacing(6);
        write_button_ = new QPushButton(QStringLiteral("  写入"));
        write_button_->setObjectName("btn_default");
        batch_button_ = new QPushButton(QStringLiteral("  批量写入"));
        batch_button_->setObjectName("btn_success");
        stop_button_ = new QPushButton(QStringLiteral("  停止"));
        stop_button_->setObjectName("btn_danger");
        stop_button_->setEnabled(false);
        btn_row->addWidget(write_button_);
        btn_row->addWidget(batch_button_);
        btn_row->addWidget(stop_button_);
        btn_row->addSpacing(60);

        batch_count_label_ = new QLabel(QStringLiteral("批量写入计数: 0"));
        batch_count_label_->setObjectName("batch_count");
        batch_count_label_->setMinimumWidth(180);
        batch_count_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        btn_row->addWidget(batch_count_label_);
        btn_row->addStretch();

        act_lay->addLayout(btn_row);

        progress_ = new QProgressBar;
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progress_->setFixedHeight(22);
        progress_->setTextVisible(true);
        act_lay->addWidget(progress_);

        msg_label_ = new QLabel(QStringLiteral("就绪"));
        msg_label_->setObjectName("msg_label");
        act_lay->addWidget(msg_label_);

        main_layout->addWidget(action_box);

        /* 信号 */
        connect(import_button_, &QPushButton::clicked, this, [this] { import_image(); });
        connect(zoom_in_button_, &QPushButton::clicked, this, [this] { edit_canvas_->zoom_in(); });
        connect(zoom_out_button_, &QPushButton::clicked, this, [this] { edit_canvas_->zoom_out(); });
        connect(save_button_, &QPushButton::clicked, this, [this] { save_image(); });
        connect(finish_button_, &QPushButton::clicked, this, [this] { finish_convert(); });
        connect(write_button_, &QPushButton::clicked, this, [this] { write_once(); });
        connect(batch_button_, &QPushButton::clicked, this, [this] { start_batch_write(); });
        connect(stop_button_, &QPushButton::clicked, this, [this] { stop_batch(); });
    }

    void set_msg(const QString& text) {
        QMetaObject::invokeMethod(this, [this, text] {
            msg_label_->setText(text);
        });
    }

    void set_status(const QString& text, const QString& fg, const QString& bg) {
        QMetaObject::invokeMethod(this, [this, text, fg, bg] {
            status_label_->setText(text);
            status_label_->setStyleSheet(
                QStringLiteral("color: %1; background: %2; padding: 4px 12px; border-radius: 4px; font-weight: 600;")
                    .arg(fg, bg));
        });
    }

    bool worker_running() const { return worker_running_.load(); }

    void poll_device() {
        if (worker_running()) return;
        if (worker_.joinable()) worker_.join();
        const bool present = !usb_upper::HidTransport::enumerate_paths().empty();
        if (present && !device_seen_) {
            set_msg("已发现设备");
            device_seen_ = true;
            info_attempted_ = false;
        }
        if (present && !info_attempted_) {
            start_info_query();
        }
        if (!present && device_seen_) {
            set_msg("设备已断开");
            device_seen_ = false;
            info_attempted_ = false;
            clear_info();
            if (!batch_mode_.load()) {
                usb_connected_ = false;
                update_button_states();
                set_status(QStringLiteral("● 未连接"), "#D69E2E", "#FEF5E7");
            }
        }
    }

    void start_info_query() {
        if (worker_running()) return;
        info_attempted_ = true;
        set_status(QStringLiteral("● 连接中"), "#4299E1", "#EBF8FF");
        set_msg("正在读取设备信息");
        worker_running_.store(true);
        worker_ = std::thread([this] {
            try {
                usb_upper::HidTransport transport;
                usb_upper::iap::Client client(&transport);
                const auto info = client.connect(1000);
                QMetaObject::invokeMethod(this, [this, info] {
                    show_info(info);
                    usb_connected_ = true;
                    update_button_states();
                    set_status(QStringLiteral("● 已连接"), "#38A169", "#F0FFF4");
                    set_msg("设备信息读取完成");
                });
            } catch (const std::exception& e) {
                set_msg(QString::fromUtf8(e.what()));
                QMetaObject::invokeMethod(this, [this] {
                    usb_connected_ = false;
                    update_button_states();
                    set_status(QStringLiteral("● 未连接"), "#D69E2E", "#FEF5E7");
                });
            }
            worker_running_.store(false);
            QMetaObject::invokeMethod(this, [this] { update_button_states(); });
        });
    }

    void show_info(const usb_upper::iap::DeviceInfo& info) {
        uid_val_->setText(QString::fromStdString(usb_upper::iap::uid_hex(info.uid)));
        name_val_->setText(QString::fromStdString(
            usb_upper::iap::format_device_name(info.model, info.hardware_version)));
        cached_info_ = info;
        sync_canvas_orientation();
    }

    void show_info_by_uid(const std::string& uid_str) {
        uid_val_->setText(QString::fromStdString(uid_str));
    }

    void clear_info() {
        uid_val_->setText("--");
        name_val_->setText("--");
        cached_info_ = {};
        sync_canvas_orientation();
    }

    QString cache_dir() const {
        return QFileInfo(QCoreApplication::applicationFilePath()).absolutePath() + "/cache";
    }

    void load_cache() {
        const QString dir = cache_dir();
        const QString img_path = dir + "/last_image.png";
        if (QFileInfo::exists(img_path)) {
            QImageReader reader(img_path);
            QImage img = reader.read();
            if (!img.isNull()) {
                edit_canvas_->set_image(img);
                source_name_ = QStringLiteral("last_image");
                set_msg("已恢复上次导入的图片");
            }
        }
        const QString enc_path = dir + "/last_jpg.bin";
        if (QFileInfo::exists(enc_path)) {
            QFile f(enc_path);
            if (f.open(QIODevice::ReadOnly)) {
                const QByteArray data = f.readAll();
                encoded_.assign(data.constData(), data.constData() + data.size());
            }
        }
        QSettings settings(
            QFileInfo(QCoreApplication::applicationFilePath()).absolutePath() + "/jpgtool.ini",
            QSettings::IniFormat);
        const int count = settings.value("batch/count", 0).toInt();
        batch_count_.store(count);
        batch_count_label_->setText(QStringLiteral("批量写入计数: %1").arg(count));
    }

    void save_image_cache(const QImage& img) {
        const QString dir = cache_dir();
        QDir().mkpath(dir);
        img.save(dir + "/last_image.png", "PNG");
    }

    void save_encoded_cache() {
        const QString dir = cache_dir();
        QDir().mkpath(dir);
        QFile f(dir + "/last_jpg.bin");
        if (f.open(QIODevice::WriteOnly)) {
            f.write(reinterpret_cast<const char*>(encoded_.data()),
                    static_cast<qint64>(encoded_.size()));
        }
    }

    void save_count(int count) {
        QSettings settings(
            QFileInfo(QCoreApplication::applicationFilePath()).absolutePath() + "/jpgtool.ini",
            QSettings::IniFormat);
        settings.setValue("batch/count", count);
    }

    void update_button_states() {
        const bool usb = usb_connected_;
        const bool working = worker_running();
        const bool batching = batch_mode_.load();
        const bool editable = usb && !working && !batching;

        import_button_->setEnabled(editable);
        zoom_in_button_->setEnabled(editable);
        zoom_out_button_->setEnabled(editable);
        save_button_->setEnabled(editable);
        finish_button_->setEnabled(editable);
        write_button_->setEnabled(editable);
        batch_button_->setEnabled(editable);
        stop_button_->setEnabled(usb && batching);
    }

    bool is_landscape() const {
        return usb_upper::iap::is_landscape(cached_info_.hardware_version);
    }

    void sync_canvas_orientation() {
        edit_canvas_->set_landscape(is_landscape());
    }

    void import_image() {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择图片"),
            QString(), QStringLiteral("图片 (*.jpg *.jpeg *.png *.bmp);;所有文件 (*)"));
        if (path.isEmpty()) return;
        QImageReader reader(path);
        reader.setAutoTransform(true);
        QImage img = reader.read();
        if (img.isNull()) {
            QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("无法加载图片"));
            return;
        }
        source_path_ = path;
        source_name_ = QFileInfo(path).completeBaseName();
        edit_canvas_->set_image(img);
        encoded_.clear();
        save_image_cache(img);
        set_msg(QStringLiteral("已导入: %1").arg(source_name_));
    }

    void save_image() {
        if (!edit_canvas_->has_image()) {
            QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先导入图片"));
            return;
        }
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存图片"),
            source_name_.isEmpty() ? QStringLiteral("image.jpg") : source_name_ + ".jpg",
            QStringLiteral("JPG (*.jpg);;PNG (*.png);;BMP (*.bmp)"));
        if (path.isEmpty()) return;
        if (edit_canvas_->image().save(path)) {
            set_msg(QStringLiteral("已保存: %1").arg(path));
        } else {
            QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("保存失败"));
        }
    }

    bool convert_image() {
        if (!edit_canvas_->has_image()) {
            QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先导入图片"));
            return false;
        }
        const int target_w = is_landscape() ? 320 : 240;
        const int target_h = is_landscape() ? 240 : 320;
        const QImage src = edit_canvas_->crop_to_image(target_w, target_h);
        QImage img = src.convertToFormat(QImage::Format_RGB888);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        if (!buffer.open(QIODevice::WriteOnly) || !img.save(&buffer, "JPG", 85)) {
            QMessageBox::warning(this, QStringLiteral("错误"), QStringLiteral("JPG 编码失败"));
            return false;
        }
        encoded_.assign(encoded.begin(), encoded.end());
        save_encoded_cache();
        set_msg(QStringLiteral("转换完成 (%1x%2, %3 B)").arg(target_w).arg(target_h).arg(encoded.size()));
        return true;
    }

    void finish_convert() {
        if (convert_image()) {
            set_msg("图片已转换，可点击\"写入\"写入设备");
        }
    }

    void write_once() {
        if (worker_running()) return;
        if (encoded_.empty()) {
            if (!convert_image()) return;
        }
        const auto data = encoded_;
        worker_running_.store(true);
        update_button_states();
        QMetaObject::invokeMethod(this, [this] { progress_->setValue(0); });
        worker_ = std::thread([this, data] {
            try {
                usb_upper::HidTransport transport;
                usb_upper::iap::Client client(&transport);
                set_msg("连接设备中...");
                const auto app_info = client.connect(1000);
                try {
                    client.enter_boot(3000);
                } catch (const std::exception&) {
                    set_msg("按已在 Boot 模式继续");
                }
                usb_upper::iap::DeviceInfo info{};
                bool found = false;
                for (unsigned retry = 0; retry < 60; ++retry) {
                    try {
                        info = client.connect(1000);
                        found = true;
                        break;
                    } catch (const std::exception&) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                if (!found) throw usb_upper::iap::ProtocolError("等待 Boot 信息查询超时");
                if (app_info.uid != info.uid) {
                    throw usb_upper::iap::ProtocolError("App 与 Boot 的设备身份不一致");
                }
                QMetaObject::invokeMethod(this, [this, info] { show_info(info); });
                const auto base = info.legacy_format
                    ? usb_upper::iap::kResBaseLegacy : usb_upper::iap::kJpgBase;
                const auto size = info.legacy_format
                    ? usb_upper::iap::kResSizeLegacy : usb_upper::iap::kJpgSize;
                if (data.size() > size) {
                    throw usb_upper::iap::ProtocolError("图片超出 JPG 区容量");
                }
                set_msg("开始写入 JPG");
                client.write_jpg(base, data, info,
                    [this](const std::string& msg) { set_msg(QString::fromStdString(msg)); },
                    [this](std::size_t done, std::size_t total) {
                        QMetaObject::invokeMethod(this, [this, done, total] {
                            progress_->setValue(static_cast<int>(done * 100 / total));
                        });
                    });
                set_msg("写入完成，返回 App...");
                try {
                    client.return_app(3000);
                } catch (const std::exception&) {}
                set_msg("写入完成，已返回 App");
            } catch (const std::exception& e) {
                set_msg(QString::fromUtf8(e.what()));
            }
            worker_running_.store(false);
            QMetaObject::invokeMethod(this, [this] { update_button_states(); });
        });
    }

    void start_batch_write() {
        if (worker_running()) return;
        if (encoded_.empty()) {
            if (!convert_image()) return;
        }
        batch_count_.store(0);
        save_count(0);
        batch_stop_.store(false);
        batch_mode_.store(true);
        batch_count_label_->setText(QStringLiteral("批量写入计数: 0"));
        set_msg("批量写入已启动，请插入设备...");
        worker_running_.store(true);
        update_button_states();
        const auto data = encoded_;

        worker_ = std::thread([this, data] {
            int count = 0;
            std::string last_written_uid;
            while (!batch_stop_.load()) {
                /* 等待设备出现 */
                set_msg(QStringLiteral("等待设备接入... (已写入 %1 台)").arg(count));
                bool device_found = false;
                while (!batch_stop_.load()) {
                    const bool present = !usb_upper::HidTransport::enumerate_paths().empty();
                    if (present) {
                        device_found = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (batch_stop_.load()) break;
                if (!device_found) continue;

                /* 设备出现后等待稳定 */
                std::this_thread::sleep_for(std::chrono::milliseconds(300));

                try {
                    usb_upper::HidTransport transport;
                    usb_upper::iap::Client client(&transport);

                    set_msg(QStringLiteral("第 %1 台设备，连接中...").arg(count + 1));
                    const auto app_info = client.connect(1000);

                    /* UID 防连续重复：和上一台相同则跳过（允许间断重复） */
                    const std::string uid_str = usb_upper::iap::uid_hex(app_info.uid);
                    if (!last_written_uid.empty() && last_written_uid == uid_str) {
                        QMetaObject::invokeMethod(this, [this, uid_str] {
                            show_info_by_uid(uid_str);
                        });
                        set_msg(QStringLiteral("设备 UID 已写入，等待更换设备..."));
                        /* 等设备断开后继续等下一台 */
                        while (!batch_stop_.load()) {
                            if (usb_upper::HidTransport::enumerate_paths().empty()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        continue;
                    }

                    try {
                        client.enter_boot(3000);
                    } catch (const std::exception&) {
                        set_msg("按已在 Boot 模式继续");
                    }
                    usb_upper::iap::DeviceInfo info{};
                    bool found = false;
                    for (unsigned retry = 0; retry < 60 && !batch_stop_.load(); ++retry) {
                        try {
                            info = client.connect(1000);
                            found = true;
                            break;
                        } catch (const std::exception&) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        }
                    }
                    if (!found) {
                        set_msg(QStringLiteral("第 %1 台设备连接失败，跳过").arg(count + 1));
                        while (!batch_stop_.load()) {
                            if (usb_upper::HidTransport::enumerate_paths().empty()) break;
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }
                        continue;
                    }
                    if (app_info.uid != info.uid) {
                        set_msg("设备身份不一致，跳过");
                        continue;
                    }
                    QMetaObject::invokeMethod(this, [this, info] { show_info(info); });
                    const auto base = info.legacy_format
                        ? usb_upper::iap::kResBaseLegacy : usb_upper::iap::kJpgBase;
                    const auto size = info.legacy_format
                        ? usb_upper::iap::kResSizeLegacy : usb_upper::iap::kJpgSize;
                    if (data.size() > size) {
                        set_msg("图片超出容量，跳过");
                        continue;
                    }
                    QMetaObject::invokeMethod(this, [this] { progress_->setValue(0); });
                    set_msg(QStringLiteral("第 %1 台设备，写入中...").arg(count + 1));
                    client.write_jpg(base, data, info,
                        [this](const std::string& msg) { set_msg(QString::fromStdString(msg)); },
                        [this](std::size_t done, std::size_t total) {
                            QMetaObject::invokeMethod(this, [this, done, total] {
                                progress_->setValue(static_cast<int>(done * 100 / total));
                            });
                        });
                    /* 写入成功，更新最后一次写入的 UID */
                    last_written_uid = uid_str;
                    ++count;
                    batch_count_.store(count);
                    save_count(count);
                    QMetaObject::invokeMethod(this, [this, count] {
                        batch_count_label_->setText(QStringLiteral("批量写入计数: %1").arg(count));
                    });
                    set_msg(QStringLiteral("第 %1 台写入完成，返回 App...").arg(count));
                    try {
                        client.return_app(3000);
                    } catch (const std::exception&) {}
                    set_msg(QStringLiteral("第 %1 台完成 (UID: %2)，请更换下一台设备")
                        .arg(count).arg(QString::fromStdString(uid_str)));
                } catch (const std::exception& e) {
                    set_msg(QStringLiteral("第 %1 台异常: %2").arg(count + 1).arg(QString::fromUtf8(e.what())));
                }

                /* 等设备断开后才继续下一台 */
                set_msg("等待设备断开后继续...");
                while (!batch_stop_.load()) {
                    if (usb_upper::HidTransport::enumerate_paths().empty()) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            batch_mode_.store(false);
            set_msg(QStringLiteral("批量写入结束，共写入 %1 台").arg(count));
            worker_running_.store(false);
            QMetaObject::invokeMethod(this, [this] {
                usb_connected_ = false;
                update_button_states();
                device_seen_ = false;
                info_attempted_ = false;
            });
        });
    }

    void stop_batch() {
        batch_stop_.store(true);
        set_msg("正在停止...");
    }

private:
    QTimer* discovery_timer_ = nullptr;
    QLabel* uid_val_ = nullptr;
    QLabel* name_val_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* msg_label_ = nullptr;
    QLabel* batch_count_label_ = nullptr;
    SquareCanvas* edit_canvas_ = nullptr;
    QProgressBar* progress_ = nullptr;

    QPushButton* import_button_ = nullptr;
    QPushButton* zoom_in_button_ = nullptr;
    QPushButton* zoom_out_button_ = nullptr;
    QPushButton* save_button_ = nullptr;
    QPushButton* finish_button_ = nullptr;
    QPushButton* write_button_ = nullptr;
    QPushButton* batch_button_ = nullptr;
    QPushButton* stop_button_ = nullptr;

    QString source_path_;
    QString source_name_;
    usb_upper::iap::Bytes encoded_;
    usb_upper::iap::DeviceInfo cached_info_{};
    std::atomic_bool worker_running_{false};
    std::atomic_bool batch_stop_{false};
    std::atomic_bool batch_mode_{false};
    std::atomic<int> batch_count_{0};
    std::thread worker_;
    bool device_seen_ = false;
    bool info_attempted_ = false;
    bool usb_connected_ = false;
};

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setStyle(QStringLiteral("Fusion"));
    JpgToolWindow window;
    window.show();
    return app.exec();
}

#include "qt_jpg_console.moc"
