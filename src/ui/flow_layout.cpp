#include "ui/flow_layout.h"

#include <QWidget>

bbq_flow_layout::bbq_flow_layout(QWidget *parent, int spacing)
    : QLayout(parent), m_spacing(spacing) {
	setContentsMargins(0, 0, 0, 0);
}

bbq_flow_layout::~bbq_flow_layout() {
	while (QLayoutItem *item = takeAt(0)) {
		delete item;
	}
}

void bbq_flow_layout::addItem(QLayoutItem *item) { m_items.append(item); }

int bbq_flow_layout::count() const { return static_cast<int>(m_items.size()); }

QLayoutItem *bbq_flow_layout::itemAt(int index) const {
	return m_items.value(index);
}

QLayoutItem *bbq_flow_layout::takeAt(int index) {
	if (index < 0 || index >= m_items.size()) {
		return nullptr;
	}

	return m_items.takeAt(index);
}

Qt::Orientations bbq_flow_layout::expandingDirections() const {
	return Qt::Orientations();
}

bool bbq_flow_layout::hasHeightForWidth() const { return true; }

int bbq_flow_layout::heightForWidth(int width) const {
	return do_layout(QRect(0, 0, width, 0), true);
}

void bbq_flow_layout::setGeometry(const QRect &rect) {
	QLayout::setGeometry(rect);
	do_layout(rect, false);
}

QSize bbq_flow_layout::sizeHint() const {
	/*
	 * ONE LINE, which is not the same as the minimum -- and the
	 * difference is the whole of a defect this got wrong once.
	 *
	 * QWidget::sizeHint defers to QLayout::totalSizeHint, and for a
	 * height-for-width layout that asks heightForWidth(sizeHint().
	 * width()). Returning minimumSize() here meant asking for the
	 * height at the width of the WIDEST SINGLE ITEM -- about 150
	 * pixels, where all thirteen controls wrap onto lines of their own
	 * and the answer is some 390 pixels tall. The pane then demanded
	 * that height at every width, so it sat mostly empty with a
	 * scrollbar while the row it contained fitted on one line.
	 *
	 * The preferred size of a wrapping row is the row unwrapped. The
	 * minimum stays the widest item, which is what lets it shrink and
	 * wrap at all.
	 */
	/*
	 * (0, 0) explicitly: a default-constructed QSize is (-1, -1), and
	 * accumulating from it made the preferred width one pixel short of
	 * the row it describes -- so the last item wrapped at the layout's
	 * OWN preferred width, which is the one width it is meant to fit
	 * in. Found by the test asserting exactly that.
	 */
	QSize size(0, 0);
	bool first = true;

	for (const QLayoutItem *item : m_items) {
		const QSize wanted = item->sizeHint();
		size.setWidth(size.width() + wanted.width() + (first ? 0 : m_spacing));
		size.setHeight(qMax(size.height(), wanted.height()));
		first = false;
	}

	const QMargins edges = contentsMargins();
	return size + QSize(edges.left() + edges.right(),
	                    edges.top() + edges.bottom());
}

QSize bbq_flow_layout::minimumSize() const {
	/*
	 * The WIDEST single item, not the sum of them.
	 *
	 * A wrapping layout can always give up horizontal room by taking
	 * another line, so its minimum width is whatever its largest item
	 * needs. Reporting the sum would make the pane demand the width the
	 * row would not fit in, which is the thing being fixed.
	 */
	QSize size;
	for (const QLayoutItem *item : m_items) {
		size = size.expandedTo(item->minimumSize());
	}

	const QMargins edges = contentsMargins();
	return size + QSize(edges.left() + edges.right(),
	                    edges.top() + edges.bottom());
}

int bbq_flow_layout::do_layout(const QRect &rect, bool measure_only) const {
	const QMargins edges = contentsMargins();
	const QRect inside = rect.adjusted(edges.left(), edges.top(),
	                                   -edges.right(), -edges.bottom());

	int x = inside.x();
	int y = inside.y();
	int line_height = 0;

	for (QLayoutItem *item : m_items) {
		const QSize wanted = item->sizeHint();
		const int next = x + wanted.width();

		/*
		 * Wrap when this item would pass the right edge, unless it is
		 * the first on its line -- an item wider than the whole pane
		 * has nowhere better to go, and moving it down for ever is an
		 * endless loop rather than a layout.
		 */
		if (next > inside.right() + 1 && line_height > 0) {
			x = inside.x();
			y += line_height + m_spacing;
			line_height = 0;
		}

		if (!measure_only) {
			item->setGeometry(QRect(QPoint(x, y), wanted));
		}

		x += wanted.width() + m_spacing;
		line_height = qMax(line_height, wanted.height());
	}

	return y + line_height - rect.y() + edges.bottom();
}
