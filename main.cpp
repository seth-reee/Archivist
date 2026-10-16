#include <archive.h>
#include <archive_entry.h>

#include <QAbstractTableModel>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QUrl>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QFormLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QMainWindow>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QPixmap>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QSet>
#include <QSizePolicy>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableView>
#include <QTextBrowser>
#include <QTimer>
#include <QTemporaryDir>
#include <QToolBar>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <utility>
#include <memory>
#include <vector>

namespace {

QStringList localDropPaths(const QMimeData *mime) {
    QStringList paths;
    if (!mime || !mime->hasUrls()) return paths;
    for (const QUrl &url : mime->urls()) {
        if (!url.isLocalFile()) return {};
        const QFileInfo info(url.toLocalFile());
        if (!info.exists() || (!info.isFile() && !info.isDir())) return {};
        const QString path = info.absoluteFilePath();
        if (!paths.contains(path)) paths.append(path);
    }
    return paths;
}

class FileDropFilter : public QObject {
public:
    FileDropFilter(QWidget *target, std::function<void(const QStringList &)> receive,
                   std::function<bool()> enabled = [] { return true; })
        : QObject(target), receive_(std::move(receive)), enabled_(std::move(enabled)) {
        target->setAcceptDrops(true);
        target->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *, QEvent *event) override {
        if (event->type() != QEvent::DragEnter && event->type() != QEvent::DragMove &&
            event->type() != QEvent::Drop) return false;
        auto *drop = static_cast<QDropEvent *>(event);
        const QStringList paths = localDropPaths(drop->mimeData());
        if (!enabled_() || paths.isEmpty() || !(drop->possibleActions() & Qt::CopyAction)) {
            drop->ignore();
            return true;
        }
        drop->setDropAction(Qt::CopyAction);
        drop->accept();
        if (event->type() == QEvent::Drop) {
            // Finish handling the native drop before opening modal dialogs.
            QTimer::singleShot(0, this, [this, paths] { if (enabled_()) receive_(paths); });
        }
        return true;
    }
private:
    std::function<void(const QStringList &)> receive_;
    std::function<bool()> enabled_;
};

class ArchiveTableView : public QTableView {
public:
    std::function<void()> exportSelection;
protected:
    void startDrag(Qt::DropActions) override {
        if (exportSelection) exportSelection();
    }
};

struct Entry {
    QString path;
    qint64 size = 0;
    QDateTime modified;
    bool directory = false;
    bool regular = false;
};

class ArchiveTableModel : public QAbstractTableModel {
public:
    explicit ArchiveTableModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}

    const QList<Entry> &entries() const { return entries_; }

    void setEntries(QList<Entry> entries) {
        beginResetModel();
        entries_ = std::move(entries);
        endResetModel();
    }

    Qt::ItemFlags flags(const QModelIndex &index) const override {
        return QAbstractTableModel::flags(index) | (index.isValid() ? Qt::ItemIsDragEnabled : Qt::NoItemFlags);
    }

    int rowCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : entries_.size();
    }

    int columnCount(const QModelIndex &parent = {}) const override {
        return parent.isValid() ? 0 : 4;
    }

    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= entries_.size()) return {};
        if (role == Qt::TextAlignmentRole && index.column() == 1)
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        const Entry &entry = entries_.at(index.row());
        if (role == Qt::UserRole) {
            switch (index.column()) {
            case 0: return entry.path;
            case 1: return entry.directory ? QVariant() : QVariant(entry.size);
            case 2: return entry.modified;
            case 3: return entry.directory ? QString("Folder") : entry.regular ? QString("File") : QString("Link / other");
            default: return {};
            }
        }
        if (role != Qt::DisplayRole) return {};
        switch (index.column()) {
        case 0: return entry.path;
        case 1: return entry.directory ? QString() : QLocale().formattedDataSize(entry.size);
        case 2: return QLocale().toString(entry.modified, QLocale::ShortFormat);
        case 3: return entry.directory ? QString("Folder") : entry.regular ? QString("File") : QString("Link / other");
        default: return {};
        }
    }

    QVariant headerData(int section, Qt::Orientation orientation, int role) const override {
        if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
        static const QStringList headers = {"Name", "Size", "Modified", "Type"};
        return section >= 0 && section < headers.size() ? QVariant(headers.at(section)) : QVariant();
    }

private:
    QList<Entry> entries_;
};

using ProgressCallback = std::function<bool(qint64, const QString &)>;

class ProgressDialog : public QDialog {
public:
    ProgressDialog(QWidget *parent, const QString &title, qint64 totalBytes)
        : QDialog(parent), totalBytes_(std::max<qint64>(0, totalBytes)) {
        setWindowTitle(title);
        setWindowModality(Qt::WindowModal);
        setMinimumWidth(430);
        auto *layout = new QVBoxLayout(this);
        current_ = new QLabel("Starting…");
        current_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        layout->addWidget(current_);
        bar_ = new QProgressBar;
        bar_->setRange(0, totalBytes_ > 0 ? 1000 : 0);
        layout->addWidget(bar_);
        details_ = new QLabel("Preparing…");
        layout->addWidget(details_);
        auto *cancel = new QPushButton("Cancel");
        auto *row = new QHBoxLayout;
        row->addStretch();
        row->addWidget(cancel);
        layout->addLayout(row);
        connect(cancel, &QPushButton::clicked, this, [this, cancel] {
            canceled_ = true;
            cancel->setEnabled(false);
            current_->setText("Stopping…");
        });
        clock_.start();
        show();
        QApplication::processEvents();
    }

    bool report(qint64 bytes, const QString &path) {
        doneBytes_ += std::max<qint64>(0, bytes);
        const qint64 now = clock_.elapsed();
        if (now - lastUpdate_ >= 100 || path != lastPath_ || canceled_) {
            lastUpdate_ = now;
            lastPath_ = path;
            current_->setText(path.isEmpty() ? "Working…" : path);
            current_->setToolTip(path);
            if (totalBytes_ > 0)
                bar_->setValue(static_cast<int>(std::min<qint64>(1000, doneBytes_ * 1000 / totalBytes_)));
            const double seconds = std::max(0.001, now / 1000.0);
            const double speed = doneBytes_ / seconds;
            QString detail;
            if (totalBytes_ == 0 && doneBytes_ == 0) {
                detail = "Working  ·  " + QString::number(now / 1000) + "s elapsed";
            } else {
                detail = QLocale().formattedDataSize(doneBytes_);
                if (totalBytes_ > 0) detail += " / " + QLocale().formattedDataSize(totalBytes_);
                detail += "  ·  " + QLocale().formattedDataSize(static_cast<qint64>(speed)) + "/s";
                detail += "  ·  " + QString::number(now / 1000) + "s elapsed";
                if (totalBytes_ > doneBytes_ && speed > 0)
                    detail += "  ·  ~" + QString::number(static_cast<qint64>((totalBytes_ - doneBytes_) / speed)) + "s left";
            }
            details_->setText(detail);
            QApplication::processEvents();
        }
        return !canceled_;
    }

    void finish() {
        if (totalBytes_ > 0) bar_->setValue(1000);
        hide();
    }

protected:
    void reject() override { canceled_ = true; }

private:
    qint64 totalBytes_ = 0;
    qint64 doneBytes_ = 0;
    qint64 lastUpdate_ = -100;
    QString lastPath_;
    bool canceled_ = false;
    QElapsedTimer clock_;
    QLabel *current_ = nullptr;
    QLabel *details_ = nullptr;
    QProgressBar *bar_ = nullptr;
};

QString archiveError(archive *a) {
    return QString::fromLocal8Bit(archive_error_string(a) ? archive_error_string(a) : "Unknown archive error");
}

QString cleanEntryPath(QString path) {
    path.replace('\\', '/');
    while (path.startsWith("./")) path.remove(0, 2);
    while (path.endsWith('/') && path.size() > 1) path.chop(1);
    return path;
}

bool safeEntryPath(const QString &path) {
    if (path.isEmpty() || path.startsWith('/') || path.contains('\\') || path.contains(QChar::Null)) return false;
    const QStringList parts = path.split('/');
    if (parts.contains("..") || path.contains(':')) return false;
    return QDir::cleanPath(path) != ".";
}

bool hasSingleRootFolder(const QList<Entry> &entries) {
    QString root;
    for (const Entry &entry : entries) {
        if ((!entry.regular && !entry.directory) || !safeEntryPath(entry.path)) continue;
        const QString path = QDir::cleanPath(entry.path);
        const QString first = path.section('/', 0, 0);
        if (!path.contains('/') && !entry.directory) return false;
        if (root.isEmpty()) root = first;
        else if (root != first) return false;
    }
    return !root.isEmpty();
}

QString extractionFolderName(const QString &archivePath) {
    const QString name = QFileInfo(archivePath).fileName();
    const QString lower = name.toLower();
    for (const QString &suffix : {QString(".tar.gz"), QString(".tar.bz2"), QString(".tar.xz"), QString(".tar.zst")}) {
        if (lower.endsWith(suffix) && name.size() > suffix.size())
            return name.left(name.size() - suffix.size());
    }
    const QString base = QFileInfo(name).completeBaseName();
    return base.isEmpty() || base == "." || base == ".." ? QString("Archive") : base;
}

QString createExtractionFolder(const QString &parent, const QString &archivePath, QString *error) {
    const QString base = extractionFolderName(archivePath);
    for (int number = 1; ; ++number) {
        const QString name = number == 1 ? base : base + QString(" (%1)").arg(number);
        const QString path = QDir(parent).absoluteFilePath(name);
        const QFileInfo info(path);
        if (info.exists() || info.isSymLink()) continue;
        if (QDir(parent).mkdir(name)) return path;
        *error = "Could not create extraction folder: " + path;
        return {};
    }
}

archive *openReader(const QString &path, QString *error) {
    archive *reader = archive_read_new();
    archive_read_support_filter_all(reader);
    archive_read_support_format_all(reader);
    archive_read_support_format_raw(reader);
    if (archive_read_open_filename(reader, QFile::encodeName(path).constData(), 10240) != ARCHIVE_OK) {
        *error = archiveError(reader);
        archive_read_free(reader);
        return nullptr;
    }
    return reader;
}

bool listArchive(const QString &path, QList<Entry> *entries, QString *error) {
    archive *reader = openReader(path, error);
    if (!reader) return false;
    entries->clear();
    archive_entry *raw = nullptr;
    int result;
    while ((result = archive_read_next_header(reader, &raw)) == ARCHIVE_OK) {
        Entry item;
        item.path = cleanEntryPath(QString::fromUtf8(archive_entry_pathname(raw)));
        item.size = archive_entry_size(raw);
        item.modified = QDateTime::fromSecsSinceEpoch(archive_entry_mtime(raw));
        item.directory = archive_entry_filetype(raw) == AE_IFDIR;
        item.regular = archive_entry_filetype(raw) == AE_IFREG;
        if (!item.path.isEmpty() && item.path != ".") entries->append(item);
        archive_read_data_skip(reader);
    }
    const bool ok = result == ARCHIVE_EOF;
    if (!ok) *error = archiveError(reader);
    archive_read_free(reader);
    return ok;
}

bool checkParents(const QString &root, const QString &relative, QString *error) {
    const QStringList parts = relative.split('/');
    QString current = root;
    for (int i = 0; i < parts.size() - 1; ++i) {
        current += '/' + parts.at(i);
        QFileInfo info(current);
        if (info.isSymLink() || (info.exists() && !info.isDir())) {
            *error = "Unsafe destination path: " + current;
            return false;
        }
        if (!QDir().mkpath(current)) {
            *error = "Could not create " + current;
            return false;
        }
        if (!QDir(current).canonicalPath().startsWith(root + '/')) {
            *error = "Destination escapes the selected folder: " + current;
            return false;
        }
    }
    return true;
}

bool extractArchive(const QString &path, const QString &destination, const QSet<QString> &selected,
                    bool all, int *written, int *skipped, QString *error,
                    const ProgressCallback &progress = {}) {
    const QString root = QDir(destination).canonicalPath();
    if (root.isEmpty()) { *error = "Destination folder does not exist."; return false; }
    archive *reader = openReader(path, error);
    if (!reader) return false;
    *written = 0;
    *skipped = 0;
    archive_entry *raw = nullptr;
    int result;
    char buffer[65536];
    while ((result = archive_read_next_header(reader, &raw)) == ARCHIVE_OK) {
        const QString name = cleanEntryPath(QString::fromUtf8(archive_entry_pathname(raw)));
        bool wanted = all || selected.contains(name);
        if (!wanted) for (const QString &folder : selected) {
            if (name.startsWith(folder + '/')) { wanted = true; break; }
        }
        if (!wanted) { archive_read_data_skip(reader); continue; }
        if (progress && !progress(0, name)) { *error = "Canceled"; archive_read_free(reader); return false; }
        const auto type = archive_entry_filetype(raw);
        if (!safeEntryPath(name) || (type != AE_IFREG && type != AE_IFDIR)) {
            ++*skipped;
            archive_read_data_skip(reader);
            continue;
        }
        if (!checkParents(root, name, error)) { archive_read_free(reader); return false; }
        const QString target = root + '/' + name;
        QFileInfo info(target);
        if (info.exists() || info.isSymLink()) {
            if (type == AE_IFDIR && info.isDir() && !info.isSymLink()) continue;
            ++*skipped;
            archive_read_data_skip(reader);
            continue;
        }
        if (type == AE_IFDIR) {
            if (!QDir().mkdir(target)) { *error = "Could not create " + target; archive_read_free(reader); return false; }
            ++*written;
            continue;
        }
        QSaveFile output(target);
        if (!output.open(QIODevice::WriteOnly)) { *error = output.errorString(); archive_read_free(reader); return false; }
        la_ssize_t count;
        while ((count = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
            if (output.write(buffer, count) != count) {
                *error = output.errorString(); output.cancelWriting(); archive_read_free(reader); return false;
            }
            if (progress && !progress(count, name)) {
                *error = "Canceled"; output.cancelWriting(); archive_read_free(reader); return false;
            }
        }
        if (count < 0) { *error = archiveError(reader); output.cancelWriting(); archive_read_free(reader); return false; }
        if (!output.commit()) { *error = output.errorString(); archive_read_free(reader); return false; }
        ++*written;
    }
    const bool ok = result == ARCHIVE_EOF;
    if (!ok) *error = archiveError(reader);
    archive_read_free(reader);
    return ok;
}

enum class WriteFormat { None, Zip, Tar, Gzip, Bzip2, Xz, Zstd, SevenZip, Rar };

WriteFormat writeFormat(const QString &path) {
    const QString lower = path.toLower();
    if (lower.endsWith(".zip")) return WriteFormat::Zip;
    if (lower.endsWith(".tar.gz") || lower.endsWith(".tgz")) return WriteFormat::Gzip;
    if (lower.endsWith(".tar.bz2") || lower.endsWith(".tbz2")) return WriteFormat::Bzip2;
    if (lower.endsWith(".tar.xz") || lower.endsWith(".txz")) return WriteFormat::Xz;
    if (lower.endsWith(".tar.zst") || lower.endsWith(".tzst")) return WriteFormat::Zstd;
    if (lower.endsWith(".tar")) return WriteFormat::Tar;
    if (lower.endsWith(".7z")) return WriteFormat::SevenZip;
    if (lower.endsWith(".rar")) return WriteFormat::Rar;
    return WriteFormat::None;
}

QString formatExtension(WriteFormat format) {
    switch (format) {
    case WriteFormat::Zip: return ".zip";
    case WriteFormat::Tar: return ".tar";
    case WriteFormat::Gzip: return ".tar.gz";
    case WriteFormat::Bzip2: return ".tar.bz2";
    case WriteFormat::Xz: return ".tar.xz";
    case WriteFormat::Zstd: return ".tar.zst";
    case WriteFormat::SevenZip: return ".7z";
    case WriteFormat::Rar: return ".rar";
    default: return {};
    }
}

QString compressionSettingKey(const QString &path) {
    const QByteArray digest = QCryptographicHash::hash(QFileInfo(path).absoluteFilePath().toUtf8(), QCryptographicHash::Sha256).toHex();
    return "compression/" + QString::fromLatin1(digest);
}

int configureWriter(archive *writer, WriteFormat format, int level) {
    if (format == WriteFormat::None || format == WriteFormat::Rar) return ARCHIVE_FAILED;
    int result = format == WriteFormat::Zip ? archive_write_set_format_zip(writer)
               : format == WriteFormat::SevenZip ? archive_write_set_format_7zip(writer)
                                                 : archive_write_set_format_pax_restricted(writer);
    if (result != ARCHIVE_OK) return result;
    const char *filter = nullptr;
    if (format == WriteFormat::Gzip) { result = archive_write_add_filter_gzip(writer); filter = "gzip"; }
    else if (format == WriteFormat::Bzip2) { result = archive_write_add_filter_bzip2(writer); filter = "bzip2"; }
    else if (format == WriteFormat::Xz) { result = archive_write_add_filter_xz(writer); filter = "xz"; }
    else if (format == WriteFormat::Zstd) { result = archive_write_add_filter_zstd(writer); filter = "zstd"; }
    if (result == ARCHIVE_OK) result = archive_write_set_bytes_in_last_block(writer, 1);
    if (result != ARCHIVE_OK || level < 0 || format == WriteFormat::Tar) return result;
    const QByteArray value = QByteArray::number(level);
    if (format == WriteFormat::Zip) {
        result = archive_write_set_format_option(writer, "zip", "compression", level == 0 ? "store" : "deflate");
        if (result == ARCHIVE_OK && level > 0)
            result = archive_write_set_format_option(writer, "zip", "compression-level", value.constData());
    } else if (format == WriteFormat::SevenZip) {
        result = archive_write_set_format_option(writer, "7zip", "compression", level == 0 ? "store" : "lzma2");
        if (result == ARCHIVE_OK && level > 0)
            result = archive_write_set_format_option(writer, "7zip", "compression-level", value.constData());
    } else {
        result = archive_write_set_filter_option(writer, filter, "compression-level", value.constData());
    }
    return result;
}

int writerOpen(archive *, void *) { return ARCHIVE_OK; }
la_ssize_t writerWrite(archive *, void *client, const void *data, size_t size) {
    return static_cast<QSaveFile *>(client)->write(static_cast<const char *>(data), static_cast<qint64>(size));
}
int writerClose(archive *, void *) { return ARCHIVE_OK; }

struct ArchiveInput {
    QString source, name;
    bool directory;
};

// Snapshot inputs before opening any output or creating a staging directory.
bool collectInputs(const QStringList &paths, const QString &outputPath,
                   QList<ArchiveInput> *inputs, QString *error,
                   const ProgressCallback &progress) {
    const QString output = QFileInfo(outputPath).absoluteFilePath();
    QSet<QString> names;
    for (const QString &path : paths) {
        const QFileInfo root(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
        QStringList pending{root.absoluteFilePath()};
        while (!pending.isEmpty()) {
            const QString source = pending.takeLast();
            if (source == output) {
                if (source == root.absoluteFilePath()) {
                    *error = "An archive cannot include itself."; return false;
                }
                continue;
            }
            const QFileInfo info(source);
            if (info.isSymLink() && source != root.absoluteFilePath()) continue;
            if (info.isSymLink() || (!info.isFile() && !info.isDir()) || !info.isReadable()) {
                *error = "Could not read " + source; return false;
            }
            const QString name = root.fileName() + source.mid(root.absoluteFilePath().size());
            if (!safeEntryPath(name) || names.contains(name)) {
                *error = "Unsafe or duplicate archive path: " + name; return false;
            }
            names.insert(name);
            if (progress && !progress(0, "Reading " + name)) { *error = "Canceled"; return false; }
            inputs->append({source, name, info.isDir()});
            if (info.isDir()) {
                const auto children = QDir(source).entryInfoList(
                    QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
                for (auto it = children.crbegin(); it != children.crend(); ++it)
                    pending.append(it->absoluteFilePath());
            }
        }
    }
    return true;
}

bool appendFiles(archive *writer, const QList<ArchiveInput> &inputs, QString *error,
                 const ProgressCallback &progress) {
    char buffer[65536];
    for (const auto &input : inputs) {
        const QFileInfo info(input.source);
        if (info.isSymLink() || info.isDir() != input.directory ||
            (!info.isFile() && !info.isDir())) {
            *error = "Input changed while archiving: " + input.source; return false;
        }
        if (progress && !progress(0, input.name)) { *error = "Canceled"; return false; }
        QFile file(input.source);
        if (!input.directory && !file.open(QIODevice::ReadOnly)) {
            *error = "Could not read " + input.source; return false;
        }
        archive_entry *entry = archive_entry_new();
        const QByteArray encoded = (input.directory ? input.name + '/' : input.name).toUtf8();
        archive_entry_set_pathname(entry, encoded.constData());
        archive_entry_set_filetype(entry, input.directory ? AE_IFDIR : AE_IFREG);
        archive_entry_set_perm(entry, input.directory ? 0755 : 0644);
        archive_entry_set_size(entry, input.directory ? 0 : info.size());
        archive_entry_set_mtime(entry, info.lastModified().toSecsSinceEpoch(), 0);
        const int result = archive_write_header(writer, entry);
        archive_entry_free(entry);
        if (result != ARCHIVE_OK) { *error = archiveError(writer); return false; }
        while (!input.directory && !file.atEnd()) {
            const qint64 count = file.read(buffer, sizeof(buffer));
            if (count < 0) { *error = file.errorString(); return false; }
            if (count == 0) break;
            if (archive_write_data(writer, buffer, count) != count) { *error = archiveError(writer); return false; }
            if (progress && !progress(count, input.name)) { *error = "Canceled"; return false; }
        }
    }
    return true;
}

bool createArchive(const QString &path, WriteFormat format, int level, const QStringList &files,
                   QString *error, const ProgressCallback &progress = {}) {
    if (format == WriteFormat::None || writeFormat(path) != format) { *error = "Archive format does not match its filename."; return false; }
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) { *error = "An archive with that name already exists."; return false; }
    QList<ArchiveInput> inputs;
    if (!collectInputs(files, path, &inputs, error, progress)) return false;
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) { *error = output.errorString(); return false; }
    archive *writer = archive_write_new();
    int result = configureWriter(writer, format, level);
    if (result == ARCHIVE_OK) result = archive_write_open(writer, &output, writerOpen, writerWrite, writerClose);
    if (result != ARCHIVE_OK) {
        *error = archiveError(writer);
        archive_write_free(writer);
        output.cancelWriting();
        return false;
    }
    bool ok = appendFiles(writer, inputs, error, progress);
    if (archive_write_close(writer) != ARCHIVE_OK && ok) { *error = archiveError(writer); ok = false; }
    archive_write_free(writer);
    if (!ok) { output.cancelWriting(); return false; }
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) {
        *error = "An archive with that name already exists.";
        output.cancelWriting();
        return false;
    }
    if (!output.commit()) { *error = output.errorString(); return false; }
    return true;
}

bool saveEdits(const QString &path, const QSet<QString> &removed,
               const QHash<QString, QString> &renamed, const QStringList &added, QString *error,
               const ProgressCallback &progress = {}, int level = -1) {
    QList<ArchiveInput> inputs;
    if (!collectInputs(added, path, &inputs, error, progress)) return false;
    archive *reader = openReader(path, error);
    if (!reader) return false;
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly)) {
        *error = output.errorString(); archive_read_free(reader); return false;
    }
    archive *writer = archive_write_new();
    const WriteFormat format = writeFormat(path);
    int result = configureWriter(writer, format, level);
    if (result == ARCHIVE_OK) result = archive_write_open(writer, &output, writerOpen, writerWrite, writerClose);
    if (result != ARCHIVE_OK) { *error = archiveError(writer); archive_write_free(writer); archive_read_free(reader); output.cancelWriting(); return false; }

    bool ok = true;
    archive_entry *raw = nullptr;
    char buffer[65536];
    while ((result = archive_read_next_header(reader, &raw)) == ARCHIVE_OK) {
        const QString name = cleanEntryPath(QString::fromUtf8(archive_entry_pathname(raw)));
        if (progress && !progress(0, name)) { *error = "Canceled"; ok = false; break; }
        bool discard = removed.contains(name);
        if (!discard) for (const QString &folder : removed) {
            if (name.startsWith(folder + '/')) { discard = true; break; }
        }
        if (discard) { archive_read_data_skip(reader); continue; }
        archive_entry *copy = archive_entry_clone(raw);
        const QString newName = renamed.value(name, name);
        const QByteArray encoded = newName.toUtf8();
        archive_entry_set_pathname(copy, encoded.constData());
        if (archive_write_header(writer, copy) != ARCHIVE_OK) {
            *error = archiveError(writer); archive_entry_free(copy); ok = false; break;
        }
        archive_entry_free(copy);
        la_ssize_t count;
        while ((count = archive_read_data(reader, buffer, sizeof(buffer))) > 0) {
            if (archive_write_data(writer, buffer, count) != count) { *error = archiveError(writer); ok = false; break; }
            if (progress && !progress(count, name)) { *error = "Canceled"; ok = false; break; }
        }
        if (count < 0) { *error = archiveError(reader); ok = false; }
        if (!ok) break;
    }
    if (ok && result != ARCHIVE_EOF) { *error = archiveError(reader); ok = false; }
    if (ok) ok = appendFiles(writer, inputs, error, progress);
    if (archive_write_close(writer) != ARCHIVE_OK && ok) { *error = archiveError(writer); ok = false; }
    archive_write_free(writer);
    archive_read_free(reader);
    if (!ok) { output.cancelWriting(); return false; }
    if (!output.commit()) { *error = output.errorString(); return false; }
    return true;
}

QString rarExecutable() { return QStandardPaths::findExecutable("rar"); }

bool editableFormat(WriteFormat format) {
    return format != WriteFormat::None && (format != WriteFormat::Rar || !rarExecutable().isEmpty());
}

bool rarSafeArgument(const QString &name) {
    return safeEntryPath(name) && !name.startsWith('-') && !name.startsWith('@') &&
           !name.contains('*') && !name.contains('?');
}

bool copyForRar(const QString &source, const QString &destination, QString *error,
                const ProgressCallback &progress, const QString &message) {
    QFile input(source);
    QFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        *error = "Could not copy " + source + ": " + (input.isOpen() ? output.errorString() : input.errorString());
        return false;
    }
    char buffer[65536];
    while (!input.atEnd()) {
        const qint64 count = input.read(buffer, sizeof(buffer));
        if (count < 0 || output.write(buffer, count) != count) {
            *error = count < 0 ? input.errorString() : output.errorString();
            return false;
        }
        if (progress && !progress(0, message)) { *error = "Canceled"; return false; }
    }
    return true;
}

bool runRar(const QStringList &arguments, const QString &workingDirectory, QString *error,
            const ProgressCallback &progress) {
    const QString executable = rarExecutable();
    if (executable.isEmpty()) { *error = "The rar command line tool is not installed."; return false; }
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.setWorkingDirectory(workingDirectory);
    process.start(executable, arguments);
    if (!process.waitForStarted(5000)) { *error = process.errorString(); return false; }
    process.closeWriteChannel();
    QByteArray output;
    while (process.state() != QProcess::NotRunning) {
        process.waitForFinished(100);
        output += process.readAll();
        if (output.size() > 16384) output = output.right(16384);
        if (progress && !progress(0, "Running rar…")) {
            process.kill();
            process.waitForFinished();
            *error = "Canceled";
            return false;
        }
    }
    output += process.readAll();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        *error = QString("rar exited with code %1.\n%2").arg(process.exitCode()).arg(QString::fromLocal8Bit(output.right(3000)).trimmed());
        return false;
    }
    return true;
}

bool commitRarArchive(const QString &source, const QString &destination, bool newArchive,
                      QString *error, const ProgressCallback &progress) {
    if (!QFileInfo::exists(source)) { *error = "rar did not produce an archive."; return false; }
    if (newArchive && (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())) {
        *error = "An archive with that name already exists."; return false;
    }
    QFile input(source);
    QSaveFile output(destination);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        *error = input.isOpen() ? output.errorString() : input.errorString(); return false;
    }
    char buffer[65536];
    while (!input.atEnd()) {
        const qint64 count = input.read(buffer, sizeof(buffer));
        if (count < 0 || output.write(buffer, count) != count) {
            *error = count < 0 ? input.errorString() : output.errorString();
            output.cancelWriting(); return false;
        }
        if (progress && !progress(0, "Saving archive…")) {
            *error = "Canceled"; output.cancelWriting(); return false;
        }
    }
    if (newArchive && (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())) {
        *error = "An archive with that name already exists."; output.cancelWriting(); return false;
    }
    if (!output.commit()) { *error = output.errorString(); return false; }
    return true;
}

bool stageRarFiles(const QList<ArchiveInput> &inputs, const QString &folder, QStringList *names,
                   QString *error, const ProgressCallback &progress) {
    if (!QDir().mkdir(folder)) { *error = "Could not prepare temporary files."; return false; }
    for (const auto &input : inputs) {
        if (!rarSafeArgument(input.name)) { *error = "RAR cannot safely add this filename: " + input.name; return false; }
        const QFileInfo info(input.source);
        if (info.isSymLink() || info.isDir() != input.directory || (!info.isFile() && !info.isDir())) {
            *error = "Input changed while archiving: " + input.source; return false;
        }
        if (progress && !progress(0, "Preparing " + input.name)) { *error = "Canceled"; return false; }
        const QString target = folder + '/' + input.name;
        if (input.directory) {
            if (!QDir().mkpath(target)) { *error = "Could not stage " + input.source; return false; }
        } else if (!copyForRar(input.source, target, error, progress, "Preparing " + input.name)) return false;
        if (!input.name.contains('/')) names->append("./" + input.name);
    }
    return true;
}

bool createRarArchive(const QString &path, int level, const QStringList &files,
                      QString *error, const ProgressCallback &progress = {}) {
    if (rarExecutable().isEmpty()) { *error = "The rar command line tool is not installed."; return false; }
    if (writeFormat(path) != WriteFormat::Rar) { *error = "RAR archives need a .rar filename."; return false; }
    if (files.isEmpty()) { *error = "RAR needs at least one file when the archive is created."; return false; }
    if (QFileInfo::exists(path) || QFileInfo(path).isSymLink()) { *error = "An archive with that name already exists."; return false; }
    QList<ArchiveInput> plannedInputs;
    if (!collectInputs(files, path, &plannedInputs, error, progress)) return false;
    QTemporaryDir temp(QFileInfo(path).absolutePath() + "/.archivist-XXXXXX");
    if (!temp.isValid()) { *error = "Could not prepare a temporary archive."; return false; }
    QStringList names;
    const QString inputs = temp.path() + "/inputs";
    if (!stageRarFiles(plannedInputs, inputs, &names, error, progress)) return false;
    const QString stagedArchive = temp.path() + "/archive.rar";
    QStringList args = {"a", "-y", "-r", "-ep1", "-m" + QString::number(level < 0 ? 3 : level), stagedArchive};
    args.append(names);
    if (!runRar(args, inputs, error, progress)) return false;
    QList<Entry> check;
    if (!listArchive(stagedArchive, &check, error)) return false;
    return commitRarArchive(stagedArchive, path, true, error, progress);
}

bool saveRarEdits(const QString &path, const QList<Entry> &entries, const QSet<QString> &removed,
                  const QHash<QString, QString> &renamed, const QStringList &added, int level,
                  QString *error, const ProgressCallback &progress = {}) {
    if (rarExecutable().isEmpty()) { *error = "The rar command line tool is not installed."; return false; }
    for (const Entry &entry : entries) if (entry.directory && removed.contains(entry.path)) {
        *error = "Select files individually to delete them from a RAR archive."; return false;
    }
    QList<ArchiveInput> plannedInputs;
    if (!collectInputs(added, path, &plannedInputs, error, progress)) return false;
    QTemporaryDir temp(QFileInfo(path).absolutePath() + "/.archivist-XXXXXX");
    if (!temp.isValid()) { *error = "Could not prepare a temporary archive."; return false; }
    const QString stagedArchive = temp.path() + "/archive.rar";
    if (!copyForRar(path, stagedArchive, error, progress, "Preparing archive…")) return false;
    QStringList deletions;
    for (const Entry &entry : entries) {
        bool remove = removed.contains(entry.path);
        if (!remove) for (const QString &folder : removed) {
            if (entry.path.startsWith(folder + '/')) { remove = true; break; }
        }
        if (!remove) continue;
        if (!rarSafeArgument(entry.path)) { *error = "RAR cannot safely delete this path: " + entry.path; return false; }
        deletions.append(entry.path);
    }
    if (!deletions.isEmpty()) {
        if (deletions.size() == entries.size()) { *error = "RAR cannot keep an empty archive. Leave at least one entry."; return false; }
        QStringList args = {"d", "-y", stagedArchive};
        args.append(deletions);
        if (!runRar(args, temp.path(), error, progress)) return false;
    }
    for (auto it = renamed.cbegin(); it != renamed.cend(); ++it) {
        if (!rarSafeArgument(it.key()) || !rarSafeArgument(it.value())) {
            *error = "RAR cannot safely rename this entry."; return false;
        }
        if (!runRar({"rn", "-y", stagedArchive, it.key(), it.value()}, temp.path(), error, progress)) return false;
    }
    if (!added.isEmpty()) {
        QStringList names;
        const QString inputs = temp.path() + "/inputs";
        if (!stageRarFiles(plannedInputs, inputs, &names, error, progress)) return false;
        QStringList args = {"a", "-y", "-r", "-ep1", "-m" + QString::number(level < 0 ? 3 : level), stagedArchive};
        args.append(names);
        if (!runRar(args, inputs, error, progress)) return false;
    }
    QList<Entry> check;
    if (!listArchive(stagedArchive, &check, error)) return false;
    return commitRarArchive(stagedArchive, path, false, error, progress);
}

class NewArchiveDialog : public QDialog {
public:
    explicit NewArchiveDialog(QWidget *parent, const QString &initialFolder, const QStringList &initialFiles = {}) : QDialog(parent) {
        setWindowTitle("New archive");
        setMinimumWidth(500);
        auto *layout = new QVBoxLayout(this);
        auto *form = new QFormLayout;
        name_ = new QLineEdit("Archive");
        form->addRow("Name", name_);
        auto *folderRow = new QHBoxLayout;
        folder_ = new QLineEdit(initialFolder);
        auto *browse = new QPushButton("Browse…");
        folderRow->addWidget(folder_);
        folderRow->addWidget(browse);
        form->addRow("Save in", folderRow);
        format_ = new QComboBox;
        format_->addItem("ZIP (.zip)", static_cast<int>(WriteFormat::Zip));
        format_->addItem("TAR (.tar)", static_cast<int>(WriteFormat::Tar));
        format_->addItem("TAR + Gzip (.tar.gz)", static_cast<int>(WriteFormat::Gzip));
        format_->addItem("TAR + Bzip2 (.tar.bz2)", static_cast<int>(WriteFormat::Bzip2));
        format_->addItem("TAR + XZ (.tar.xz)", static_cast<int>(WriteFormat::Xz));
        format_->addItem("TAR + Zstandard (.tar.zst)", static_cast<int>(WriteFormat::Zstd));
        format_->addItem("7z (.7z)", static_cast<int>(WriteFormat::SevenZip));
        if (!rarExecutable().isEmpty()) format_->addItem("RAR (.rar)", static_cast<int>(WriteFormat::Rar));
        form->addRow("Format", format_);
        level_ = new QSpinBox;
        level_->setRange(0, 9);
        level_->setValue(6);
        level_->setToolTip("Higher levels usually make smaller archives but take more time.");
        form->addRow("Compression level", level_);
        layout->addLayout(form);
        levelHint_ = new QLabel;
        layout->addWidget(levelHint_);
        layout->addWidget(new QLabel("Files and folders to include (optional)"));
        files_ = new QListWidget;
        files_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        files_->setMinimumHeight(120);
        layout->addWidget(files_);
        auto *fileRow = new QHBoxLayout;
        auto *add = new QPushButton("Add files…");
        auto *addFolder = new QPushButton("Add folder…");
        auto *remove = new QPushButton("Remove selected");
        fileRow->addWidget(add);
        fileRow->addWidget(addFolder);
        fileRow->addWidget(remove);
        fileRow->addStretch();
        layout->addLayout(fileRow);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText("Create");
        layout->addWidget(buttons);
        connect(browse, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getExistingDirectory(this, "Choose archive folder", folder_->text());
            if (!path.isEmpty()) folder_->setText(path);
        });
        auto addPaths = [this](const QStringList &paths) {
            for (const QString &path : paths) {
                bool duplicate = false;
                for (int i = 0; i < files_->count(); ++i)
                    if (files_->item(i)->data(Qt::UserRole).toString() == path) { duplicate = true; break; }
                if (!duplicate) {
                    auto *item = new QListWidgetItem(QFileInfo(path).fileName(), files_);
                    item->setData(Qt::UserRole, path);
                    item->setToolTip(path);
                }
            }
        };
        new FileDropFilter(files_->viewport(), addPaths);
        addPaths(initialFiles);
        connect(add, &QPushButton::clicked, this, [this, addPaths] {
            addPaths(QFileDialog::getOpenFileNames(this, "Files to include", folder_->text()));
        });
        connect(addFolder, &QPushButton::clicked, this, [this, addPaths] {
            const QString path = QFileDialog::getExistingDirectory(this, "Folder to include", folder_->text());
            if (!path.isEmpty()) addPaths({path});
        });
        connect(remove, &QPushButton::clicked, this, [this] {
            qDeleteAll(files_->selectedItems());
        });
        connect(format_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { updateLevel(); });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { validateAndAccept(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        updateLevel();
    }

    WriteFormat format() const { return static_cast<WriteFormat>(format_->currentData().toInt()); }
    int level() const { return format() == WriteFormat::Tar ? -1 : level_->value(); }
    QString archivePath() const {
        QString base = name_->text().trimmed();
        const QString lower = base.toLower();
        for (const QString &suffix : {QString(".tar.bz2"), QString(".tar.zst"), QString(".tar.gz"),
                                      QString(".tar.xz"), QString(".zip"), QString(".tar"),
                                      QString(".7z"), QString(".rar")}) {
            if (lower.endsWith(suffix)) { base.chop(suffix.size()); break; }
        }
        return QDir(folder_->text().trimmed()).absoluteFilePath(base + formatExtension(format()));
    }
    QStringList files() const {
        QStringList paths;
        for (int i = 0; i < files_->count(); ++i) paths << files_->item(i)->data(Qt::UserRole).toString();
        return paths;
    }

private:
    void updateLevel() {
        const WriteFormat type = format();
        if (type == WriteFormat::Tar) {
            level_->setEnabled(false);
            levelHint_->setText("TAR stores files without compression.");
            return;
        }
        level_->setEnabled(true);
        if (type == WriteFormat::Bzip2 || type == WriteFormat::Zstd) level_->setRange(1, type == WriteFormat::Zstd ? 22 : 9);
        else level_->setRange(0, type == WriteFormat::Rar ? 5 : 9);
        if (type == WriteFormat::Zip || type == WriteFormat::SevenZip || type == WriteFormat::Rar)
            levelHint_->setText("0 stores files without compression; higher levels usually make smaller archives.");
        else levelHint_->setText("Lower levels are faster; higher levels usually make smaller archives.");
    }

    void validateAndAccept() {
        const QString name = name_->text().trimmed();
        if (name.isEmpty() || name == "." || name == ".." || name.contains('/') || name.contains('\\')) {
            QMessageBox::warning(this, "Invalid name", "Enter a file name without slashes."); return;
        }
        const QFileInfo folderInfo(folder_->text().trimmed());
        if (!folderInfo.isDir() || !folderInfo.isWritable()) {
            QMessageBox::warning(this, "Invalid folder", "Choose an existing writable folder."); return;
        }
        if (QFileInfo::exists(archivePath()) || QFileInfo(archivePath()).isSymLink()) {
            QMessageBox::warning(this, "Name already exists", "An archive with that name already exists."); return;
        }
        QSet<QString> names;
        if (format() == WriteFormat::Rar && files_->count() == 0) {
            QMessageBox::warning(this, "No files selected", "RAR archives need at least one file when created."); return;
        }
        for (const QString &path : files()) {
            const QFileInfo info(path);
            if (info.isSymLink() || (!info.isFile() && !info.isDir()) || !info.isReadable()) {
                QMessageBox::warning(this, "Unreadable item", "Cannot read " + path); return;
            }
            if (names.contains(info.fileName())) {
                QMessageBox::warning(this, "Duplicate file name", "The selected files include two named " + info.fileName() + "."); return;
            }
            if (format() == WriteFormat::Rar && !rarSafeArgument(info.fileName())) {
                QMessageBox::warning(this, "Unsupported filename", "RAR cannot safely add " + info.fileName() + "."); return;
            }
            names.insert(info.fileName());
        }
        accept();
    }

    QLineEdit *name_ = nullptr;
    QLineEdit *folder_ = nullptr;
    QComboBox *format_ = nullptr;
    QSpinBox *level_ = nullptr;
    QLabel *levelHint_ = nullptr;
    QListWidget *files_ = nullptr;
};

QString themeFile() {
    return QDir::homePath() + "/.local/state/omarchy/current/theme/colors.toml";
}

void applyOmarchyPalette(QApplication &app) {
    QHash<QString, QColor> colors{
        {"background", QColor("#121212")},
        {"dark_background", QColor("#121212")},
        {"lighter_background", QColor("#1e1e1e")},
        {"foreground", QColor("#bebebe")},
        {"light_foreground", QColor("#8a8a8d")},
        {"dark_foreground", QColor("#555555")},
        {"accent", QColor("#e68e0d")},
        {"selection", QColor("#333333")},
        {"muted", QColor("#333333")}
    };
    QFile file(themeFile());
    const QRegularExpression line(R"theme(^\s*([A-Za-z_]+)\s*=\s*"(#[0-9A-Fa-f]{6})")theme");
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        while (!file.atEnd()) {
            const auto match = line.match(QString::fromUtf8(file.readLine()));
            if (match.hasMatch()) colors.insert(match.captured(1), QColor(match.captured(2)));
        }
    }
    auto color = [&](const QString &key, QColor fallback) { return colors.value(key, fallback); };
    const QColor bg = color("background", QColor("#121212"));
    const QColor panel = color("lighter_background", bg.lighter(115));
    const QColor fg = color("foreground", QColor("#bebebe"));
    const QColor accent = color("accent", QColor("#e68e0d"));
    const QColor selection = color("selection", QColor("#333333"));
    const QColor muted = color("muted", QColor("#333333"));
    const QColor disabled = color("dark_foreground", QColor("#555555"));
    QPalette palette = app.palette();
    palette.setColor(QPalette::Window, bg);
    palette.setColor(QPalette::WindowText, fg);
    palette.setColor(QPalette::Base, panel);
    palette.setColor(QPalette::AlternateBase, panel);
    palette.setColor(QPalette::Text, fg);
    palette.setColor(QPalette::Button, panel);
    palette.setColor(QPalette::ButtonText, fg);
    palette.setColor(QPalette::Highlight, selection);
    palette.setColor(QPalette::HighlightedText, fg);
    palette.setColor(QPalette::ToolTipBase, panel);
    palette.setColor(QPalette::ToolTipText, fg);
    palette.setColor(QPalette::PlaceholderText, color("light_foreground", fg.darker(120)));
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    app.setPalette(palette);
    app.setStyleSheet(QString(
        "QWidget { font-size: 13px; } "
        "QPushButton, QToolButton, QLineEdit, QComboBox, QSpinBox { padding: 7px 10px; border: 1px solid %1; border-radius: 6px; } "
        "QPushButton:hover, QToolButton:hover { border-color: %2; } "
        "QPushButton:disabled, QToolButton:disabled { color: %3; } "
        "QToolBar { spacing: 4px; border: none; } "
        "QTableWidget { border: 1px solid %1; border-radius: 6px; gridline-color: %1; } "
        "QHeaderView::section { background: %4; padding: 8px; border: none; border-bottom: 1px solid %1; } "
        "QTableWidget::item { padding: 5px; } QTableWidget::item:selected { background: %5; } "
        "QMenu { border: 1px solid %1; padding: 4px; } "
        "QMenu::item { padding: 6px 24px 6px 10px; } QMenu::item:selected { background: %5; }")
        .arg(muted.name(), accent.name(), disabled.name(), panel.name(), selection.name()));
}

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("Archivist");
        setWindowIcon(QIcon(":/archivist.png"));
        resize(900, 600);
        auto *container = new QWidget;
        auto *layout = new QVBoxLayout(container);
        layout->setContentsMargins(20, 20, 20, 12);
        layout->setSpacing(12);
        auto *titleRow = new QHBoxLayout;
        auto *titleIcon = new QLabel;
        titleIcon->setPixmap(QPixmap(":/archivist.png").scaled(42, 42, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        auto *title = new QLabel("Archivist");
        QFont titleFont = title->font();
        titleFont.setBold(true);
        title->setFont(titleFont);
        auto *about = new QPushButton("About");
        titleRow->addWidget(titleIcon);
        titleRow->addWidget(title);
        titleRow->addStretch();
        titleRow->addWidget(about);
        layout->addLayout(titleRow);

        auto *bar = new QToolBar("Archive", container);
        bar->setMovable(false);
        auto *newAction = bar->addAction("New archive");
        auto *openAction = bar->addAction("Open");
        extractAction_ = bar->addAction("Extract");
        bar->addSeparator();
        addAction_ = bar->addAction("Add files");
        addFolderAction_ = bar->addAction("Add folder");
        renameAction_ = bar->addAction("Rename");
        deleteAction_ = bar->addAction("Delete");
        heading_ = new QLabel("Drop an archive to open it, or drop files and folders to compress");
        layout->addWidget(heading_);
        layout->addWidget(bar);
        auto *archiveTable = new ArchiveTableView;
        table_ = archiveTable;
        table_->setDragEnabled(true);
        table_->setDragDropMode(QAbstractItemView::DragOnly);
        archiveTable->exportSelection = [this] { dragSelection(); };
        auto receive = [this](const QStringList &paths) {
            if (!archivePath_.isEmpty()) { addPaths(paths); return; }
            QList<Entry> entries;
            QString error;
            if (paths.size() == 1 && QFileInfo(paths.first()).isFile() &&
                listArchive(paths.first(), &entries, &error)) openArchive(paths.first());
            else newArchive(paths);
        };
        auto canDrop = [this] {
            return !dragging_ && (archivePath_.isEmpty() || editableFormat(writeFormat(archivePath_)));
        };
        new FileDropFilter(table_->viewport(), receive, canDrop);
        new FileDropFilter(this, receive, canDrop);
        model_ = new ArchiveTableModel(table_);
        sortModel_ = new QSortFilterProxyModel(table_);
        sortModel_->setSourceModel(model_);
        sortModel_->setSortRole(Qt::UserRole);
        sortModel_->setSortCaseSensitivity(Qt::CaseInsensitive);
        sortModel_->setDynamicSortFilter(true);
        table_->setModel(sortModel_);
        table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        table_->verticalHeader()->hide();
        table_->horizontalHeader()->resizeSection(1, 110);
        table_->horizontalHeader()->resizeSection(2, 180);
        table_->horizontalHeader()->resizeSection(3, 95);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setAlternatingRowColors(true);
        table_->setSortingEnabled(true);
        layout->addWidget(table_);
        setCentralWidget(container);
        statusBar()->showMessage("Ready");
        connect(newAction, &QAction::triggered, this, [this] { newArchive(); });
        connect(openAction, &QAction::triggered, this, [this] { chooseArchive(); });
        connect(extractAction_, &QAction::triggered, this, [this] { extract(); });
        connect(addAction_, &QAction::triggered, this, [this] { addFiles(); });
        connect(addFolderAction_, &QAction::triggered, this, [this] { addFolder(); });
        connect(renameAction_, &QAction::triggered, this, [this] { renameEntry(); });
        connect(deleteAction_, &QAction::triggered, this, [this] { deleteEntries(); });
        connect(about, &QPushButton::clicked, this, [this] { showAbout(); });
        connect(table_->selectionModel(), &QItemSelectionModel::selectionChanged,
                this, [this] { updateActions(); });
        updateActions();
    }

    void openArchive(const QString &path) {
        QList<Entry> next;
        QString error;
        if (!listArchive(path, &next, &error)) { QMessageBox::critical(this, "Cannot open archive", error); return; }
        archivePath_ = QFileInfo(path).absoluteFilePath();
        model_->setEntries(std::move(next));
        heading_->setText(QFileInfo(archivePath_).fileName());
        setWindowTitle(QFileInfo(archivePath_).fileName() + " — Archivist");
        const QString mode = editableFormat(writeFormat(archivePath_)) ? "Editing available" : "View and extract";
        statusBar()->showMessage(QString("%1 entries · %2").arg(model_->entries().size()).arg(mode));
        updateActions();
    }

private:
    void showAbout() {
        QDialog dialog(this);
        dialog.setWindowTitle("About Archivist");
        dialog.resize(580, 420);
        auto *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(20, 20, 20, 20);
        layout->setSpacing(10);

        auto *aboutIcon = new QLabel;
        aboutIcon->setPixmap(QPixmap(":/archivist.png").scaled(72, 72, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        layout->addWidget(aboutIcon);

        auto *heading = new QLabel("Archivist " + qApp->applicationVersion());
        QFont font = heading->font();
        font.setPointSize(17);
        font.setBold(true);
        heading->setFont(font);
        layout->addWidget(heading);
        layout->addWidget(new QLabel("A lightweight archive manager for Omarchy."));

        auto *github = new QLabel("Created by seth-reee · <a href=\"https://github.com/seth-reee\">GitHub profile</a>");
        github->setOpenExternalLinks(true);
        layout->addWidget(github);
        layout->addWidget(new QLabel("MIT License · Copyright © 2026 seth-reee"));

        QFile license(":/LICENSE");
        auto *licenseText = new QTextBrowser;
        if (license.open(QIODevice::ReadOnly)) licenseText->setPlainText(QString::fromUtf8(license.readAll()));
        layout->addWidget(licenseText, 1);

        auto *close = new QPushButton("Close");
        auto *bottom = new QHBoxLayout;
        bottom->addStretch();
        bottom->addWidget(close);
        layout->addLayout(bottom);
        connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
        dialog.exec();
    }

    void newArchive(const QStringList &initialFiles = {}) {
        const QString folder = archivePath_.isEmpty() ? QDir::homePath() : QFileInfo(archivePath_).absolutePath();
        NewArchiveDialog dialog(this, folder, initialFiles);
        if (dialog.exec() != QDialog::Accepted) return;
        const QString path = dialog.archivePath();
        const QStringList files = dialog.files();
        QString error;
        bool ok;
        if (files.isEmpty()) {
            ok = dialog.format() == WriteFormat::Rar
                     ? createRarArchive(path, dialog.level(), files, &error)
                     : createArchive(path, dialog.format(), dialog.level(), files, &error);
        } else {
            qint64 total = 0;
            for (const QString &source : files) total += std::max<qint64>(0, QFileInfo(source).size());
            ProgressDialog progress(this, "Creating archive", dialog.format() == WriteFormat::Rar || std::any_of(files.cbegin(), files.cend(), [](const QString &p) { return QFileInfo(p).isDir(); }) ? 0 : total);
            const ProgressCallback callback = [&](qint64 bytes, const QString &name) { return progress.report(bytes, name); };
            ok = dialog.format() == WriteFormat::Rar
                     ? createRarArchive(path, dialog.level(), files, &error, callback)
                     : createArchive(path, dialog.format(), dialog.level(), files, &error, callback);
            progress.finish();
        }
        if (ok) {
            QSettings().setValue(compressionSettingKey(path), dialog.level());
            openArchive(path);
        } else if (error == "Canceled") {
            statusBar()->showMessage("Archive creation canceled", 5000);
        } else {
            QMessageBox::critical(this, "Could not create archive", error);
        }
    }

    void chooseArchive() {
        const QString path = QFileDialog::getOpenFileName(this, "Open archive", QDir::homePath());
        if (!path.isEmpty()) openArchive(path);
    }

    QSet<QString> selectedPaths() const {
        QSet<QString> result;
        for (const QModelIndex &index : table_->selectionModel()->selectedRows())
            result.insert(entryAt(index).path);
        return result;
    }

    const Entry &entryAt(const QModelIndex &viewIndex) const {
        return model_->entries().at(sortModel_->mapToSource(viewIndex).row());
    }

    qint64 extractionTotal(const QSet<QString> &selected) const {
        qint64 total = 0;
        for (const Entry &entry : model_->entries()) {
            if (!entry.regular || !safeEntryPath(entry.path)) continue;
            bool wanted = selected.isEmpty() || selected.contains(entry.path);
            if (!wanted) for (const QString &folder : selected) {
                if (entry.path.startsWith(folder + '/')) { wanted = true; break; }
            }
            if (wanted) total += std::max<qint64>(0, entry.size);
        }
        return total;
    }

    bool performEdit(const QSet<QString> &removed, const QHash<QString, QString> &renamed,
                     const QStringList &added, const QString &title) {
        qint64 total = 0;
        for (const Entry &entry : model_->entries()) {
            bool discard = removed.contains(entry.path);
            if (!discard) for (const QString &folder : removed) {
                if (entry.path.startsWith(folder + '/')) { discard = true; break; }
            }
            if (!discard && entry.regular) total += std::max<qint64>(0, entry.size);
        }
        for (const QString &source : added) total += std::max<qint64>(0, QFileInfo(source).size());
        const bool useRar = writeFormat(archivePath_) == WriteFormat::Rar;
        ProgressDialog progress(this, title, useRar || std::any_of(added.cbegin(), added.cend(), [](const QString &p) { return QFileInfo(p).isDir(); }) ? 0 : total);
        QString error;
        const int level = QSettings().value(compressionSettingKey(archivePath_), -1).toInt();
        const ProgressCallback callback = [&](qint64 bytes, const QString &name) { return progress.report(bytes, name); };
        const bool ok = useRar
                            ? saveRarEdits(archivePath_, model_->entries(), removed, renamed, added, level, &error, callback)
                            : saveEdits(archivePath_, removed, renamed, added, &error, callback, level);
        progress.finish();
        if (ok) openArchive(archivePath_);
        else if (error == "Canceled") statusBar()->showMessage("Canceled; archive unchanged", 5000);
        else QMessageBox::critical(this, title, error);
        return ok;
    }

    void updateActions() {
        const WriteFormat format = writeFormat(archivePath_);
        const bool editable = !archivePath_.isEmpty() && editableFormat(format);
        const QModelIndexList selected = table_->selectionModel()->selectedRows();
        const int count = selected.size();
        bool rarFolderSelected = false;
        if (format == WriteFormat::Rar) for (const QModelIndex &index : selected)
            rarFolderSelected |= entryAt(index).directory;
        extractAction_->setEnabled(!archivePath_.isEmpty());
        addAction_->setEnabled(editable);
        addFolderAction_->setEnabled(editable);
        deleteAction_->setEnabled(editable && count > 0 && !rarFolderSelected);
        deleteAction_->setToolTip(rarFolderSelected ? "Select files individually to delete them from a RAR archive." : "");
        renameAction_->setEnabled(editable && count == 1 && entryAt(selected.first()).regular);
    }

    void extract() {
        if (archivePath_.isEmpty()) { chooseArchive(); return; }
        const QSet<QString> selected = selectedPaths();
        const bool needsFolder = selected.isEmpty() && !model_->entries().isEmpty() &&
                                 !hasSingleRootFolder(model_->entries());
        QString destination = QFileDialog::getExistingDirectory(this,
            needsFolder ? "Choose where to create extracted folder" :
            selected.isEmpty() ? "Extract all to" : "Extract selected to", QFileInfo(archivePath_).absolutePath());
        if (destination.isEmpty()) return;
        int written = 0, skipped = 0;
        QString error;
        if (needsFolder) {
            destination = createExtractionFolder(destination, archivePath_, &error);
            if (destination.isEmpty()) { QMessageBox::critical(this, "Extraction stopped", error); return; }
        }
        ProgressDialog progress(this, "Extracting archive", extractionTotal(selected));
        const bool ok = extractArchive(archivePath_, destination, selected, selected.isEmpty(), &written, &skipped, &error,
                                       [&](qint64 bytes, const QString &name) { return progress.report(bytes, name); });
        progress.finish();
        if (!ok && error == "Canceled") QMessageBox::information(this, "Extraction canceled",
            QString("Stopped after extracting %1 item(s). Files already extracted remain in the destination.").arg(written));
        else if (!ok) QMessageBox::critical(this, "Extraction stopped", error);
        else QMessageBox::information(this, "Extraction complete", QString("Extracted %1 item(s) to %3. Skipped %2 unsafe or existing item(s).").arg(written).arg(skipped).arg(destination));
    }

    void dragSelection() {
        const QSet<QString> selected = selectedPaths();
        if (archivePath_.isEmpty() || selected.isEmpty() || dragging_) return;
        dragging_ = true;
        auto staging = std::make_unique<QTemporaryDir>();
        int written = 0, skipped = 0;
        QString error;
        // Wayland requires the original implicit pointer grab at start_drag.
        // Do not show a modal window or pump events before QDrag::exec(): either
        // can consume the release or transfer focus away from the source surface.
        const bool ok = staging->isValid() && extractArchive(archivePath_, staging->path(), selected, false,
            &written, &skipped, &error);
        if (!ok) {
            dragging_ = false;
            if (error != "Canceled") QMessageBox::critical(this, "Could not prepare files", error.isEmpty() ? "Could not create temporary folder." : error);
            return;
        }
        QList<QUrl> urls;
        QStringList paths = selected.values();
        std::sort(paths.begin(), paths.end());
        QStringList exported;
        for (const QString &path : paths) {
            if (!safeEntryPath(path) || !QFileInfo::exists(staging->path() + '/' + path)) continue;
            bool covered = false;
            for (const QString &parent : exported)
                if (path.startsWith(parent + '/')) { covered = true; break; }
            if (!covered) {
                exported.append(path);
                urls.append(QUrl::fromLocalFile(staging->path() + '/' + path));
            }
        }
        if (!urls.isEmpty()) {
            auto *mime = new QMimeData;
            mime->setUrls(urls);
            // File managers may read the URLs after the native drag has returned.
            dragFiles_.push_back(std::move(staging));
            QDrag drag(table_);
            drag.setMimeData(mime);
            drag.exec(Qt::CopyAction, Qt::CopyAction);
        }
        dragging_ = false;
    }

    void addFiles() {
        const QStringList paths = QFileDialog::getOpenFileNames(this, "Add files", QDir::homePath());
        addPaths(paths);
    }

    void addFolder() {
        const QString path = QFileDialog::getExistingDirectory(this, "Add folder", QDir::homePath());
        if (!path.isEmpty()) addPaths({path});
    }

    void addPaths(const QStringList &paths) {
        if (paths.isEmpty()) return;
        QSet<QString> names;
        for (const Entry &e : model_->entries()) names.insert(e.path.section('/', 0, 0));
        for (const QString &path : paths) {
            const QString name = QFileInfo(path).fileName();
            if (names.contains(name)) { QMessageBox::warning(this, "Name already exists", name + " is already in the archive."); return; }
            names.insert(name);
        }
        performEdit({}, {}, paths, "Adding files");
    }

    void renameEntry() {
        const QSet<QString> selected = selectedPaths();
        const QString oldName = *selected.cbegin();
        bool accepted = false;
        const QString newName = QInputDialog::getText(this, "Rename entry", "New path within archive:",
            QLineEdit::Normal, oldName, &accepted).trimmed();
        if (!accepted || newName == oldName) return;
        if (!safeEntryPath(newName)) { QMessageBox::warning(this, "Invalid path", "Use a relative path without '..' or ':'."); return; }
        for (const Entry &e : model_->entries()) if (e.path == newName) {
            QMessageBox::warning(this, "Name already exists", "An entry with that path already exists."); return;
        }
        performEdit({}, {{oldName, newName}}, {}, "Renaming entry");
    }

    void deleteEntries() {
        const QSet<QString> selected = selectedPaths();
        if (selected.isEmpty()) return;
        if (QMessageBox::question(this, "Delete entries", QString("Remove %1 selected entry or folder from the archive?").arg(selected.size())) != QMessageBox::Yes) return;
        performEdit(selected, {}, {}, "Deleting entries");
    }

    bool dragging_ = false;
    std::vector<std::unique_ptr<QTemporaryDir>> dragFiles_;
    QString archivePath_;
    QLabel *heading_ = nullptr;
    QTableView *table_ = nullptr;
    ArchiveTableModel *model_ = nullptr;
    QSortFilterProxyModel *sortModel_ = nullptr;
    QAction *addAction_ = nullptr;
    QAction *addFolderAction_ = nullptr;
    QAction *extractAction_ = nullptr;
    QAction *renameAction_ = nullptr;
    QAction *deleteAction_ = nullptr;
};

} // namespace

int main(int argc, char **argv) {
    if (qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("Hyprland", Qt::CaseInsensitive))
        qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");
    QApplication app(argc, argv);
    app.setOrganizationName("Archivist");
    app.setApplicationName("Archivist");
    app.setApplicationVersion(ARCHIVIST_VERSION);
    app.setWindowIcon(QIcon(":/archivist.png"));
    QSettings legacySettings("Archway", "Archway");
    QSettings settings;
    legacySettings.beginGroup("compression");
    settings.beginGroup("compression");
    for (const QString &key : legacySettings.childKeys())
        if (!settings.contains(key)) settings.setValue(key, legacySettings.value(key));
    settings.endGroup();
    legacySettings.endGroup();
    applyOmarchyPalette(app);
    QFileSystemWatcher watcher;
    auto refreshTheme = [&] {
        QTimer::singleShot(250, &app, [&] {
            applyOmarchyPalette(app);
            watcher.removePaths(watcher.files());
            watcher.removePaths(watcher.directories());
            if (QFile::exists(themeFile())) watcher.addPath(themeFile());
            const QString parent = QFileInfo(themeFile()).absolutePath();
            if (QDir(parent).exists()) watcher.addPath(parent);
            const QString current = QFileInfo(parent).absolutePath();
            if (QDir(current).exists()) watcher.addPath(current);
        });
    };
    QObject::connect(&watcher, &QFileSystemWatcher::fileChanged, &app, refreshTheme);
    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, &app, refreshTheme);
    refreshTheme();
    MainWindow window;
    window.show();
    if (argc > 1) window.openArchive(QString::fromLocal8Bit(argv[1]));
    return app.exec();
}
