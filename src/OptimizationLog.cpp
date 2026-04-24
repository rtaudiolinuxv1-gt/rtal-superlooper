#include "OptimizationLog.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

namespace {
QString resolveLogPath()
{
    const QDir currentDir = QDir::current();
    const QString currentPath = currentDir.filePath(QStringLiteral("optimization log"));
    if (QFileInfo::exists(currentPath)) {
        return currentPath;
    }

    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString appPath = appDir.filePath(QStringLiteral("optimization log"));
    if (QFileInfo::exists(appPath)) {
        return appPath;
    }

    const QString parentPath = appDir.absoluteFilePath(QStringLiteral("../optimization log"));
    if (QFileInfo(parentPath).dir().exists()) {
        return QFileInfo(parentPath).absoluteFilePath();
    }

    return currentPath;
}

int nextEntryNumber(const QString& contents)
{
    int count = 0;
    for (const QChar ch : contents) {
        if (ch == QLatin1Char('\n')) {
            continue;
        }
    }

    const QStringList lines = contents.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const int dotIndex = line.indexOf(QLatin1Char('.'));
        bool ok = false;
        line.left(dotIndex).trimmed().toInt(&ok);
        if (ok) {
            count = std::max(count, line.left(dotIndex).trimmed().toInt());
        }
    }
    return count + 1;
}
}

QString OptimizationLog::filePath()
{
    return resolveLogPath();
}

void OptimizationLog::appendEntry(
    const QString& subsystem,
    const QString& summary,
    const QString& impact,
    const QString& risk,
    bool implemented,
    bool sideBySidePossible)
{
    QFile file(resolveLogPath());
    QString existing;
    if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        existing = QString::fromUtf8(file.readAll());
        file.close();
    }

    const int entryNumber = nextEntryNumber(existing);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    if (!existing.isEmpty() && !existing.endsWith(QLatin1Char('\n'))) {
        stream << '\n';
    }
    stream << entryNumber << ". [" << subsystem << "] "
           << summary
           << " | impact=" << impact
           << " | risk=" << risk
           << " | implemented=" << (implemented ? "yes" : "no")
           << " | side-by-side=" << (sideBySidePossible ? "yes" : "no")
           << '\n';
}

QString OptimizationLog::readAll()
{
    QFile file(resolveLogPath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return QStringLiteral("No optimization log entries yet.");
    }
    return QString::fromUtf8(file.readAll());
}
