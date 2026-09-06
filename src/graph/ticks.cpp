#include "graph/ticks.h"

bbq_tick_choice bbq_ticks_for(qint64 span_s, int wanted) {
	const qint64 ladder[] = {
		60, 5 * 60, 15 * 60, 30 * 60,
		3600, 3 * 3600, 6 * 3600, 12 * 3600,
		24 * 3600, 2 * 24 * 3600, 7 * 24 * 3600, 14 * 24 * 3600,
		30 * 24 * 3600, 91 * 24 * 3600, 365 * 24 * 3600,
	};

	bbq_tick_choice chosen;
	chosen.step_s = ladder[sizeof(ladder) / sizeof(ladder[0]) - 1];

	for (qint64 candidate : ladder) {
		if (span_s / candidate <= wanted) {
			chosen.step_s = candidate;
			break;
		}
	}

	/*
	 * The label follows the STEP, not the span, and that distinction was
	 * paid for by looking at the running window.
	 *
	 * Choosing it from the span put a date-only format against a
	 * twelve-hour step at around four days, so the axis read "Tue 11,
	 * Tue 11, Wed 12, Wed 12" -- every label printed twice, each one
	 * naming a day but pointing at noon or midnight without saying
	 * which. A label has to distinguish its tick from the next tick, and
	 * only the step knows how far away that is.
	 */
	if (chosen.step_s < 6 * 3600) {
		chosen.format = QStringLiteral("HH:mm");
	} else if (chosen.step_s < 24 * 3600) {
		chosen.format = QStringLiteral("ddd HH:mm");
	} else if (chosen.step_s < 30 * 24 * 3600) {
		chosen.format = QStringLiteral("ddd d");
	} else if (chosen.step_s < 365 * 24 * 3600) {
		chosen.format = QStringLiteral("d MMM");
	} else {
		chosen.format = QStringLiteral("MMM yy");
	}

	return chosen;
}
