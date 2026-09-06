#include "cli/options.h"

QString bbq_option_value(const QStringList &arguments, const QString &name) {
	const int index = arguments.indexOf(name);
	if (index < 0 || index + 1 >= arguments.size()) {
		return QString();
	}

	const QString value = arguments.at(index + 1);
	if (value.startsWith(QStringLiteral("--"))) {
		return QString();
	}

	return value;
}
