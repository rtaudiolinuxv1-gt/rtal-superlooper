#pragma once

#include <QString>

namespace OptimizationLog {

[[nodiscard]] QString filePath();
void appendEntry(const QString& subsystem, const QString& summary, const QString& impact, const QString& risk, bool implemented, bool sideBySidePossible);
[[nodiscard]] QString readAll();

}
