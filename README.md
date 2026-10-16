# <img src="archivist.png" width="48" height="48" alt=""> Archivist

Archivist is a lightweight archive manager for Omarchy. It follows your current theme and gives you a simple place to open, extract, and edit archives.

## What it does

- Opens archives and extracts everything or just the files you select.
- Creates and edits ZIP, 7z, TAR, TAR.GZ, TAR.BZ2, TAR.XZ, and TAR.ZST archives.
- Creates and edits RAR archives when the `rar` command is installed. Readable RAR archives can still be opened and extracted without it.
- Shows progress, speed, and time remaining during extraction and most edits.

## Install

Download the latest Omarchy package from [Releases](https://github.com/seth-reee/archivist/releases), then install it from your Downloads folder:

```bash
sudo pacman -U ./archivist-*.pkg.tar.zst
```

The package recipe supports Omarchy on x86_64 and ARM64, using Arch Linux and Arch Linux ARM respectively. Install the package whose filename matches your machine's architecture. To build from the tagged release, install `base-devel`, `cmake`, `ninja`, `qt6-base`, `libarchive`, and `pkgconf`, then run `makepkg -s` in `packaging/`.

**ARM64 status:** The ARM64 package built and launched headlessly under QEMU, but remains untested on a real ARM64 Omarchy desktop.

Open **Archivist** from the application menu.

## Use

Choose **New archive** to pick a name, location, format, and compression level. Add files and folders with their separate picker buttons; both appear in the same archive, and folders include their nested contents. You can also start with an empty archive. Choose **Open** to browse an existing archive, then use **Extract** or the edit buttons in the toolbar.

Drag files and folders into an empty Archivist window to start a new archive, or into an open editable archive to add them. You can also drop them into the file list in **New archive**. Dropping a single readable archive into an empty window opens it.

Select entries and drag them into your file manager to extract copies, including the contents of selected folders. Files are prepared before the drag starts; large selections may take some time. Temporary extracted copies remain available until Archivist closes, so allow the file manager to finish copying before closing it. Dragging never removes entries from the archive.

When extracting the whole archive, Archivist creates a folder named after the archive if its contents are not already inside one top-level folder. If that destination name already exists, it uses a numbered folder. Extracting selected entries keeps the destination you choose.

Archivist skips unsafe archive paths and does not overwrite existing extracted files. Password protected and split archives are not supported yet.

## Build from source

Requires CMake, Qt 6 Widgets, and libarchive.

```bash
cmake -S . -B build
cmake --build build
./build/archivist
```

## Automated packages

GitHub Actions builds native x86_64 and ARM64 Arch packages on pushes to `master`, pull requests, and manual runs of **Arch Linux packages**. Download the `archivist-x86_64` or `archivist-aarch64` artifact from a successful Actions run. Each includes a `.pkg.tar.zst` package, its source archive, a pinned local-source PKGBUILD, and checksums. Development versions include the commit count and hash.

To prepare a release, update the version in `CMakeLists.txt` and `packaging/PKGBUILD`, update the release recipe's source checksum, and push a matching `vVERSION` tag. After both builds pass, Actions creates a **draft** GitHub release with both packages. Review and publish the draft when ready; existing releases are not replaced. The tagged source must contain this workflow.

Both architectures run the archive and widget tests, verify package metadata and ELF architecture, and perform an offscreen startup check. These checks do not replace testing on a real desktop.

## License

Archivist is made by [seth-reee](https://github.com/seth-reee) and is available under the [MIT License](LICENSE).
