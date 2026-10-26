// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Spike
// Author: Spike

#include <QtFileMgrFileView.h>
#include <QtFileMgrFlatModel.h>
#include <QtFileMgrWidget.h>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QSaveFile>
#include <QSplitter>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>
#include <QDebug>

#include <functional>
#include <memory>

namespace {

constexpr qint64 kPreviewBytes = 64 * 1024;

QByteArray welcomeText()
{
    return QStringLiteral(
        "QtFileMgr embedded example\n"
        "==========================\n\n"
        "This text is displayed by the host application's QPlainTextEdit.\n"
        "QtFileMgr supplies navigation, selection and activation signals.\n\n"
        "UTF-8 sample: 你好，Qt!\n\n"
        "Open documents/guide.txt to try directory navigation.\n"
        "large.txt demonstrates the 64 KiB preview limit.\n"
        "binary.bin demonstrates a file that this text-only host rejects.\n")
        .toUtf8();
}

bool writeSample(const QString &path, const QByteArray &bytes)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(bytes) != bytes.size() || !file.commit()) {
        qWarning("[EmbeddedExample] fixture write failed (path=\"%s\", bytes=%lld, error=\"%s\")",
                 qPrintable(path), static_cast<long long>(bytes.size()),
                 qPrintable(file.errorString()));
        return false;
    }
    return true;
}

bool createSamples(const QString &root)
{
    const QDir directory(root);
    if (!directory.mkpath(QStringLiteral("documents"))) {
        qWarning("[EmbeddedExample] fixture mkdir failed (root=\"%s\", child=\"documents\")",
                 qPrintable(root));
        return false;
    }
    return writeSample(directory.filePath(QStringLiteral("README.txt")), welcomeText())
        && writeSample(directory.filePath(QStringLiteral("notes.txt")),
                       QByteArrayLiteral("Host applications decide what opening a file means.\n"))
        && writeSample(directory.filePath(QStringLiteral("documents/guide.txt")),
                       QByteArrayLiteral("Navigation updates the host's current-location label.\n"))
        && writeSample(directory.filePath(QStringLiteral("binary.bin")), QByteArray("\0\1\2\3", 4))
        && writeSample(directory.filePath(QStringLiteral("large.txt")),
                       QByteArray(static_cast<int>(kPreviewBytes + 1024), 'A'));
}

// The host owns its normal Qt widgets through the QObject parent tree. The
// manager copies config; no caller-owned IO or temporary config pointer is kept.
class EmbeddedWindow final : public QMainWindow {
public:
    explicit EmbeddedWindow(const QString &root, bool temporarySamples)
    {
        setWindowTitle(QStringLiteral("QtFileMgr · Embedded host example"));
        resize(1240, 760);

        auto *splitter = new QSplitter(Qt::Horizontal, this);
        splitter->setChildrenCollapsible(false);
        setCentralWidget(splitter);

        QtFileMgrConfig config;
        config.rootPath = root;
        config.allowedPaths = QStringList{root}; // Navigation scope, not a sandbox.
        config.showDevices = false;
        config.defaultViewMode = QtFileMgrFileView::DetailsView;
        config.rememberMode = false;
        config.placesPanel.width = 164;
        config.places = QList<PlaceEntry>{
            {QStringLiteral("Example folder"), root, QStringLiteral("folder")}};
        // This host demonstrates browsing/preview. These are UI restrictions;
        // a security boundary must be implemented by the host/backend itself.
        config.allowCreate = false;
        config.allowDelete = false;
        config.allowRename = false;
        config.allowDragDrop = false;
        config.toolbar.items.settingsButton = false;
        m_manager = new QtFileMgrWidget(config, splitter);

        auto *hostPanel = new QWidget(splitter);
        auto *layout = new QVBoxLayout(hostPanel);
        layout->setContentsMargins(24, 24, 24, 20);
        layout->setSpacing(12);
        auto *heading = new QLabel(QStringLiteral("Host application"), hostPanel);
        QFont headingFont = heading->font();
        if (headingFont.pointSizeF() > 0)
            headingFont.setPointSizeF(headingFont.pointSizeF() + 3);
        else if (headingFont.pixelSize() > 0)
            headingFont.setPixelSize(headingFont.pixelSize() + 4);
        else
            qWarning("[EmbeddedExample] heading font has no explicit size; keeping inherited font (family=\"%s\")",
                     qPrintable(headingFont.family()));
        headingFont.setBold(true);
        heading->setFont(headingFont);
        layout->addWidget(heading);

        auto *intro = new QLabel(QStringLiteral(
            "The file manager is embedded on the left.\n"
            "Select files, navigate folders, or press Enter to preview a file here."), hostPanel);
        intro->setWordWrap(true);
        layout->addWidget(intro);
        m_location = addLabel(layout, hostPanel);
        m_selection = addLabel(layout, hostPanel);
        m_activation = addLabel(layout, hostPanel);
        m_previewInfo = addLabel(layout, hostPanel);

        m_preview = new QPlainTextEdit(hostPanel);
        m_preview->setObjectName(QStringLiteral("hostTextPreview"));
        m_preview->setReadOnly(true);
        m_preview->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        m_preview->setPlaceholderText(QStringLiteral("Double-click a file or press Enter to preview UTF-8 text."));
        layout->addWidget(m_preview, 1);
        splitter->setSizes(QList<int>{760, 480});
        splitter->setStretchFactor(0, 3);
        splitter->setStretchFactor(1, 2);

        // The host handles each signal independently. Activation never launches
        // an external application: this host chooses a bounded text preview.
        connect(m_manager, &QtFileMgrWidget::urlChanged, this, [this](const QUrl &url) {
            ++m_locationEvents;
            showLocation(url);
        });
        connect(m_manager, &QtFileMgrWidget::filesSelected, this,
                [this](const QStringList &paths) {
            ++m_selectionEvents;
            m_selected = paths;
            m_selection->setText(QStringLiteral("Selection: %1 item(s)").arg(paths.size()));
        });
        connect(m_manager, &QtFileMgrWidget::fileActivated, this,
                [this](const QString &path) {
            ++m_activationEvents;
            m_activated = path;
            m_activation->setText(QStringLiteral("Activated: %1").arg(QFileInfo(path).fileName()));
            previewText(path);
        });
        showLocation(m_manager->url());
        m_selection->setText(QStringLiteral("Selection: 0 items"));
        m_activation->setText(QStringLiteral("Activated: none"));
        m_previewInfo->setText(QStringLiteral("UTF-8 text preview · maximum 64 KiB"));
        statusBar()->showMessage(temporarySamples
            ? QStringLiteral("Temporary sample folder — removed when this example exits")
            : QStringLiteral("Browsing an existing folder — no sample files are created here"));
    }

    QtFileMgrWidget *manager() const { return m_manager; }
    int locationEvents() const { return m_locationEvents; }
    int selectionEvents() const { return m_selectionEvents; }
    int activationEvents() const { return m_activationEvents; }
    QUrl displayedUrl() const { return m_displayedUrl; }
    QStringList selectedPaths() const { return m_selected; }
    QString activatedPath() const { return m_activated; }
    QString previewContents() const { return m_preview->toPlainText(); }
    bool previewSucceeded() const { return m_previewSucceeded; }
    bool previewTruncated() const { return m_previewTruncated; }
    bool previewIsReadOnly() const { return m_preview->isReadOnly(); }

    void previewText(const QString &path)
    {
        m_previewSucceeded = false;
        m_previewTruncated = false;
        const QFileInfo info(path);
        if (!info.isFile()) {
            previewError(path, QStringLiteral("Only existing regular files can be previewed."));
            return;
        }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            previewError(path, QStringLiteral("Cannot open file: %1").arg(file.errorString()));
            return;
        }
        QByteArray bytes = file.read(kPreviewBytes + 1);
        if (file.error() != QFileDevice::NoError) {
            previewError(path, QStringLiteral("Cannot read file: %1").arg(file.errorString()));
            return;
        }
        m_previewTruncated = bytes.size() > kPreviewBytes;
        if (m_previewTruncated) {
            qWarning("[EmbeddedExample] preview truncated (source=\"%s\", limit=%lld, fileBytes=%lld)",
                     qPrintable(path), static_cast<long long>(kPreviewBytes),
                     static_cast<long long>(info.size()));
            bytes.truncate(static_cast<int>(kPreviewBytes));
        }
        if (bytes.contains('\0')) {
            previewError(path, QStringLiteral("This text-only example does not preview NUL-containing data."));
            return;
        }
        // This example deliberately assumes UTF-8. A production host should
        // define its own encoding/content policy. Truncation can split a code
        // point; QString::fromUtf8 represents invalid input with replacement.
        const QString text = QString::fromUtf8(bytes.constData(), bytes.size());
        if (text.contains(QChar::ReplacementCharacter)) {
            qWarning("[EmbeddedExample] UTF-8 preview contains replacement characters (source=\"%s\", bytes=%lld, truncated=%d)",
                     qPrintable(path), static_cast<long long>(bytes.size()),
                     m_previewTruncated ? 1 : 0);
        }
        m_preview->setPlainText(text);
        m_previewSucceeded = true;
        m_previewInfo->setText(QStringLiteral("UTF-8 · %1 bytes shown%2")
            .arg(bytes.size())
            .arg(m_previewTruncated ? QStringLiteral(" · truncated at 64 KiB") : QString()));
    }

private:
    static QLabel *addLabel(QVBoxLayout *layout, QWidget *parent)
    {
        auto *label = new QLabel(parent);
        label->setWordWrap(true);
        label->setMinimumWidth(0);
        label->setTextFormat(Qt::PlainText);
        layout->addWidget(label);
        return label;
    }

    void showLocation(const QUrl &url)
    {
        m_displayedUrl = url;
        m_location->setText(QStringLiteral("Location: %1").arg(url.toDisplayString(QUrl::RemovePassword)));
    }

    void previewError(const QString &path, const QString &error)
    {
        qWarning("[EmbeddedExample] preview rejected (source=\"%s\", reason=\"%s\")",
                 qPrintable(path), qPrintable(error));
        m_previewInfo->setText(QStringLiteral("Preview unavailable"));
        m_preview->setPlainText(QStringLiteral("%1\n\nSource: %2").arg(error, path));
    }

    QtFileMgrWidget *m_manager = nullptr;
    QLabel *m_location = nullptr;
    QLabel *m_selection = nullptr;
    QLabel *m_activation = nullptr;
    QLabel *m_previewInfo = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QUrl m_displayedUrl;
    QStringList m_selected;
    QString m_activated;
    int m_locationEvents = 0;
    int m_selectionEvents = 0;
    int m_activationEvents = 0;
    bool m_previewSucceeded = false;
    bool m_previewTruncated = false;
};

bool waitForListing(EmbeddedWindow &window, const QString &expectedDirectory)
{
    auto ready = [&]() {
        return window.manager()->url() == QUrl::fromLocalFile(expectedDirectory)
            && !window.manager()->fileView()->flatModel()->isLoading();
    };
    if (ready())
        return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (ready())
            loop.quit();
    });
    QTimer::singleShot(8000, &loop, &QEventLoop::quit);
    poll.start();
    loop.exec();
    if (!ready()) {
        qWarning("[EmbeddedExample] listing wait failed (expected=\"%s\", actual=\"%s\", loading=%d, timeoutMs=8000)",
                 qPrintable(expectedDirectory), qPrintable(window.manager()->url().toString()),
                 window.manager()->fileView()->flatModel()->isLoading() ? 1 : 0);
        return false;
    }
    return true;
}

void activateSelection(EmbeddedWindow &window)
{
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(window.manager()->fileView(), &enter);
}

bool runSmokeTest(EmbeddedWindow &window, const QString &root)
{
    int failures = 0;
    auto check = [&](bool passed, const QString &description) {
        QTextStream(stdout) << (passed ? "PASS: " : "FAIL: ") << description << '\n';
        if (!passed) {
            ++failures;
            qWarning("[EmbeddedExample] smoke check failed (root=\"%s\", check=\"%s\")",
                     qPrintable(root), qPrintable(description));
        }
    };
    const QDir fixture(root);
    if (!waitForListing(window, root))
        return false;
    auto *view = window.manager()->fileView();
    QWidget *messageBar = view->findChild<QWidget *>(QStringLiteral("fileViewMessageBar"));
    check(messageBar && messageBar->isHidden(),
          QStringLiteral("configured root initializes without a rejected-home warning banner"));
    const int selectionBefore = window.selectionEvents();
    view->selectNewFiles(QStringList{QStringLiteral("README.txt")});
    check(window.selectionEvents() > selectionBefore
              && window.selectedPaths() == QStringList{fixture.filePath(QStringLiteral("README.txt"))},
          QStringLiteral("filesSelected updates host selection state"));
    const int activationBefore = window.activationEvents();
    activateSelection(window);
    check(window.activationEvents() == activationBefore + 1
              && window.activatedPath() == fixture.filePath(QStringLiteral("README.txt"))
              && window.previewSucceeded()
              && window.previewContents() == QString::fromUtf8(welcomeText())
              && window.previewIsReadOnly(),
          QStringLiteral("Enter emits fileActivated and the host renders read-only UTF-8 text"));

    const int locationBefore = window.locationEvents();
    view->selectNewFiles(QStringList{QStringLiteral("documents")});
    activateSelection(window);
    const QString documents = fixture.filePath(QStringLiteral("documents"));
    check(waitForListing(window, documents)
              && window.locationEvents() > locationBefore
              && window.displayedUrl() == QUrl::fromLocalFile(documents),
          QStringLiteral("folder activation emits urlChanged and updates the host location"));
    window.manager()->setUrl(QUrl::fromLocalFile(root));
    if (!waitForListing(window, root))
        return false;

    view->selectNewFiles(QStringList{QStringLiteral("large.txt")});
    activateSelection(window);
    check(window.previewSucceeded() && window.previewTruncated()
              && window.previewContents().toUtf8().size() == kPreviewBytes,
          QStringLiteral("host preview caps a large text file at 64 KiB"));
    view->selectNewFiles(QStringList{QStringLiteral("binary.bin")});
    activateSelection(window);
    check(!window.previewSucceeded() && window.previewContents().contains(QStringLiteral("NUL-containing")),
          QStringLiteral("host rejects binary content with an explicit reason"));
    const QString missing = fixture.filePath(QStringLiteral("missing.txt"));
    window.previewText(missing);
    check(!window.previewSucceeded() && window.previewContents().contains(missing),
          QStringLiteral("host reports missing-file preview context"));

    view->selectNewFiles(QStringList{QStringLiteral("README.txt")});
    activateSelection(window);
    check(window.previewSucceeded() && !window.previewTruncated(),
          QStringLiteral("valid activation recovers the preview after an error"));
    QTextStream(stdout) << "RESULT: embedded smoke " << (failures == 0 ? "PASS" : "FAIL") << '\n';
    return failures == 0;
}

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("qtfilemgr-example-embedded"));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Embed QtFileMgr in a host window with a bounded UTF-8 text preview."));
    parser.addHelpOption();
    const QCommandLineOption directoryOption(QStringLiteral("directory"),
        QStringLiteral("Browse an existing directory instead of temporary samples."), QStringLiteral("path"));
    const QCommandLineOption smokeOption(QStringLiteral("smoke-test"),
        QStringLiteral("Verify signal-driven integration using temporary samples, then exit."));
    const QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
        QStringLiteral("Save the window as a PNG at this path, then exit."), QStringLiteral("path"));
    parser.addOptions({directoryOption, smokeOption, screenshotOption});
    parser.process(app);
    if (parser.isSet(smokeOption) && parser.isSet(directoryOption)) {
        qWarning("[EmbeddedExample] --smoke-test requires temporary samples; --directory was supplied (directory=\"%s\")",
                 qPrintable(parser.value(directoryOption)));
        return 2;
    }

    // Declare the fixture before the window, so its files outlive all widgets.
    std::unique_ptr<QTemporaryDir> samples;
    QString root;
    if (parser.isSet(directoryOption)) {
        const QFileInfo requested(parser.value(directoryOption));
        root = requested.canonicalFilePath();
        if (!requested.isDir() || !requested.isReadable() || root.isEmpty()) {
            qWarning("[EmbeddedExample] invalid directory (requested=\"%s\", canonical=\"%s\", isDirectory=%d, readable=%d)",
                     qPrintable(parser.value(directoryOption)), qPrintable(root),
                     requested.isDir() ? 1 : 0, requested.isReadable() ? 1 : 0);
            return 2;
        }
    } else {
        samples.reset(new QTemporaryDir(QDir::tempPath() + QStringLiteral("/qtfilemgr-embedded-XXXXXX")));
        if (!samples->isValid()) {
            qWarning("[EmbeddedExample] cannot create sample directory (tempRoot=\"%s\", error=\"%s\")",
                     qPrintable(QDir::tempPath()), qPrintable(samples->errorString()));
            return 2;
        }
        root = samples->path();
        if (!createSamples(root))
            return 2;
    }

    EmbeddedWindow window(root, samples != nullptr);
    window.show();
    if (parser.isSet(smokeOption) || parser.isSet(screenshotOption)) {
        QTimer::singleShot(0, &window, [&]() {
            bool ok = parser.isSet(smokeOption)
                ? runSmokeTest(window, root) : waitForListing(window, root);
            if (!parser.isSet(smokeOption) && samples && ok) {
                window.manager()->fileView()->selectNewFiles(QStringList{QStringLiteral("README.txt")});
                activateSelection(window);
            }
            if (parser.isSet(screenshotOption)) {
                QApplication::processEvents(QEventLoop::AllEvents);
                const QString output = parser.value(screenshotOption);
                const bool saved = !output.isEmpty() && window.grab().save(output, "PNG");
                if (!saved) {
                    qWarning("[EmbeddedExample] screenshot failed (output=\"%s\", window=%dx%d)",
                             qPrintable(output), window.width(), window.height());
                }
                ok = saved && ok;
            }
            app.exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
