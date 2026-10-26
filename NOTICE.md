# Source and dependency notices

QtFileMgr is authored and maintained by Spike. Its project code and documentation
are distributed under the [MIT license](LICENSE). Spike has confirmed the right
to release this code. Dolphin/KIO references describe interaction and API design
inspiration; they do not indicate copied or adapted upstream source. QtFileMgr
is an independent project and is not affiliated with KDE or Qt. The project
license does not relicense third-party dependencies.

The built-in toolbar symbols are drawn by the project using QPainter. File and
place icons are requested from the installed desktop theme or Qt standard icons;
no desktop icon theme is bundled in this source distribution. Screenshots show
Qt's standard icons and isolated demonstration files. A binary distributor who
bundles an icon theme must retain that theme's own notices.

Dependencies are installed separately; their code is not included in this source
archive:

| Dependency | Use | Licensing reference |
| --- | --- | --- |
| Qt Core, Gui, Widgets | UI and filesystem support | Qt's commercial or applicable open-source terms; see the exact Qt distribution's license files and [Qt's open-source licensing information](https://www.qt.io/development/open-source-lgpl-obligations). |
| libssh (optional) | SSH/SFTP backend | LGPL; see the installed version's COPYING and [libssh's license explanation](https://www.libssh.org/features/). |
| CMake, compiler, Python | Build/test tooling | Used as tools, not bundled runtime code. |
| OpenSSH, libnss-wrapper | Opt-in local integration fixture | Test-only tools, installed separately under their respective licenses. |

MIT applies to QtFileMgr's own code. It does not remove the licensing and notice
requirements of Qt, libssh or other libraries included in a binary package.
This first source release does not bundle third-party runtime binaries.
