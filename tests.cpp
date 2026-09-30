#define main archivist_application_main
#include "main.cpp"
#undef main

#include <QCoreApplication>
#include <QDebug>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
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
