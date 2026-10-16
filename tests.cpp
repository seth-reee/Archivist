#define main archivist_application_main
#include "main.cpp"
#undef main

#include <QCoreApplication>
#include <QDebug>

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QString error;
    auto require = [&](bool condition, const char *message) {
        if (!condition) qFatal("%s: %s", message, qPrintable(error));
    };
    QTemporaryDir temp;
    require(temp.isValid(), "temporary directory");
    const QString folder = temp.path() + "/Project";
    require(QDir().mkpath(folder + "/nested/empty"), "input directories");
    auto write = [&](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
    };
    write(folder + "/nested/note.txt", "hello");
    write(folder + "/.hidden", "hidden");
    const QString loose = temp.path() + "/loose.txt";
    write(loose, "loose");
    require(QFile::link(loose, folder + "/link"), "symlink fixture");
    QMimeData dropped;
    dropped.setUrls({QUrl::fromLocalFile(loose), QUrl::fromLocalFile(folder), QUrl::fromLocalFile(loose)});
    require(localDropPaths(&dropped) == QStringList({loose, folder}), "accept local file and directory drops and deduplicate");
    dropped.setUrls({QUrl::fromLocalFile(loose), QUrl("https://example.com/file")});
    require(localDropPaths(&dropped).isEmpty(), "reject mixed remote drops");
    dropped.setUrls({QUrl::fromLocalFile(temp.path() + "/missing")});
    require(localDropPaths(&dropped).isEmpty(), "reject missing files");
    QWidget dropTarget;
    QStringList received;
    new FileDropFilter(&dropTarget, [&](const QStringList &paths) { received = paths; });
    dropped.setUrls({QUrl::fromLocalFile(loose)});
    QDragEnterEvent enter(QPoint(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dropTarget, &enter);
    require(enter.isAccepted(), "widget accepts incoming file drag");
    QDropEvent drop(QPointF(5, 5), Qt::CopyAction | Qt::MoveAction, &dropped, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&dropTarget, &drop);
    require(drop.isAccepted() && drop.dropAction() == Qt::CopyAction, "file drop accepts copy only");
    QApplication::processEvents();
    require(received == QStringList({loose}), "file drop dispatches paths after native event");
    MainWindow window;
    auto *table = window.findChild<QTableView *>();
    require(table && table->viewport()->acceptDrops(), "archive table accepts external drops");
    QDragEnterEvent tableEnter(QPoint(5, 5), Qt::CopyAction, &dropped, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(table->viewport(), &tableEnter);
    require(tableEnter.isAccepted(), "archive table accepts file drag through viewport");
    const QSet<QString> expected{"Project", "Project/nested", "Project/nested/empty",
        "Project/nested/note.txt", "Project/.hidden", "loose.txt"};
    for (auto format : {WriteFormat::Zip, WriteFormat::SevenZip, WriteFormat::Tar,
                        WriteFormat::Gzip, WriteFormat::Bzip2, WriteFormat::Xz, WriteFormat::Zstd}) {
        const QString path = temp.path() + "/test" + formatExtension(format);
        require(createArchive(path, format, 3, {folder + "/", loose}, &error), "create mixed archive");
        QList<Entry> entries;
        require(listArchive(path, &entries, &error), "list mixed archive");
        QSet<QString> names;
        for (const auto &entry : entries) names.insert(entry.path);
        require(names == expected, "preserve directory structure and skip symlinks");
        const QString dest = temp.path() + "/extract" + formatExtension(format);
        require(QDir().mkpath(dest), "extraction directory");
        int written = 0, skipped = 0;
        require(extractArchive(path, dest, {}, true, &written, &skipped, &error), "extract archive");
        QFile content(dest + "/Project/nested/note.txt");
        require(content.open(QIODevice::ReadOnly) && content.readAll() == "hello", "round trip file content");
        require(QFileInfo(dest + "/Project/nested/empty").isDir(), "round trip empty folder");
    }
    const Entry rootDir{"Project", 0, {}, true, false};
    const Entry nestedFile{"Project/note.txt", 5, {}, false, true};
    const Entry looseFile{"note.txt", 5, {}, false, true};
    require(hasSingleRootFolder({rootDir, nestedFile}), "recognize explicit root folder");
    require(hasSingleRootFolder({nestedFile}), "recognize implicit root folder");
    require(hasSingleRootFolder({rootDir}), "recognize single empty root folder");
    require(!hasSingleRootFolder({looseFile}), "single loose file needs wrapper");
    require(!hasSingleRootFolder({rootDir, nestedFile, looseFile}), "mixed root items need wrapper");
    require(!hasSingleRootFolder({nestedFile, {"Other/note.txt", 5, {}, false, true}}), "multiple root folders need wrapper");
    require(extractionFolderName("/tmp/Photos.TAR.GZ") == "Photos", "strip compound archive suffix");
    require(extractionFolderName("/tmp/Photos.2026.zip") == "Photos.2026", "preserve dots in archive name");
    const QString wrapped = createExtractionFolder(temp.path(), "/tmp/test.tar.gz", &error);
    require(wrapped == temp.path() + "/test" && QFileInfo(wrapped).isDir(), "create archive-named extraction folder");
    const QString collision = createExtractionFolder(temp.path(), "/tmp/test.zip", &error);
    require(collision == temp.path() + "/test (2)", "avoid merging into existing extraction folder");
    int wrapperWritten = 0, wrapperSkipped = 0;
    require(extractArchive(temp.path() + "/test.zip", wrapped, {}, true, &wrapperWritten, &wrapperSkipped, &error), "extract mixed archive into wrapper");
    require(QFileInfo(wrapped + "/loose.txt").isFile() && QFileInfo(wrapped + "/Project/nested/note.txt").isFile(), "wrapper contains all root items");
    const QString selectedOutput = temp.path() + "/drag-selection";
    require(QDir().mkpath(selectedOutput), "drag staging directory");
    int written = 0, skipped = 0;
    require(extractArchive(temp.path() + "/test.zip", selectedOutput, {"Project/nested"}, false,
                           &written, &skipped, &error), "stage selected folder for dragging");
    require(QFileInfo(selectedOutput + "/Project/nested/note.txt").isFile(), "drag selected folder includes descendants");
    require(QFileInfo(selectedOutput + "/Project/nested/empty").isDir(), "drag preserves empty folders");
    require(!QFileInfo::exists(selectedOutput + "/loose.txt") &&
            !QFileInfo::exists(selectedOutput + "/Project/.hidden"), "drag excludes unselected entries");
    const QString zip = folder + "/inside.zip";
    require(createArchive(zip, WriteFormat::Zip, 3, {folder}, &error), "archive inside selected folder");
    QList<Entry> entries;
    require(listArchive(zip, &entries, &error), "list inside archive");
    require(entries.size() == expected.size() - 1, "exclude archive output and temporary files");
    const QString second = temp.path() + "/Second";
    require(QDir().mkpath(second), "added directory");
    require(saveEdits(zip, {}, {}, {second}, &error), "add folder to existing archive");
    entries.clear();
    require(listArchive(zip, &entries, &error), "list edited archive");
    require(std::any_of(entries.cbegin(), entries.cend(), [](const Entry &e) { return e.path == "Second" && e.directory; }), "added empty directory exists");

    QList<ArchiveInput> inputs;
    require(collectInputs({folder}, zip, &inputs, &error, {}), "snapshot RAR inputs");
    const QString staging = folder + "/staging";
    QStringList names;
    require(stageRarFiles(inputs, staging, &names, &error, {}), "stage RAR directory inside input");
    require(QFileInfo(staging + "/Project/nested/empty").isDir(), "stage empty folder");
    require(!QFileInfo::exists(staging + "/Project/staging"), "no recursive staging");
    require(!QFileInfo::exists(staging + "/Project/inside.zip"), "exclude existing output archive");
    return 0;
}
