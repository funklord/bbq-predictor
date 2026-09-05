#ifndef BBQ_UI_FLOW_LAYOUT_H
#define BBQ_UI_FLOW_LAYOUT_H

#include <QLayout>
#include <QList>

/*
 * A row of controls that wraps instead of running off the edge
 * (project.md sec 16.10).
 *
 * Qt has no wrapping layout. A QHBoxLayout squeezes its items to their
 * minimum and then clips whatever is left over, and the controls it
 * clips are simply gone: the pane's horizontal scrolling is off, which
 * is deliberate, so there is nothing to scroll to them with.
 *
 * That is what the desktop row did at its own default width. Wind,
 * Steady scale, Layout and Theme were past the edge until somebody
 * widened the window, and nothing said so.
 *
 * Height depends on width here, which is unusual enough to state: ask
 * for the height of a given width rather than for a size.
 */
class bbq_flow_layout : public QLayout {
public:
	explicit bbq_flow_layout(QWidget *parent, int spacing = 6);
	~bbq_flow_layout() override;

	void addItem(QLayoutItem *item) override;
	int count() const override;
	QLayoutItem *itemAt(int index) const override;
	QLayoutItem *takeAt(int index) override;

	Qt::Orientations expandingDirections() const override;
	bool hasHeightForWidth() const override;
	int heightForWidth(int width) const override;
	void setGeometry(const QRect &rect) override;
	QSize sizeHint() const override;
	QSize minimumSize() const override;

private:
	/*
	 * One routine for both questions, because a layout that measures
	 * itself one way and places itself another reports a height nothing
	 * ends up occupying.
	 */
	int do_layout(const QRect &rect, bool measure_only) const;

	QList<QLayoutItem *> m_items;
	int m_spacing;
};

#endif
