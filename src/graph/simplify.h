#ifndef BBQ_SIMPLIFY_H
#define BBQ_SIMPLIFY_H

#include <QPolygonF>

/*
 * Drop vertices a stroked polyline does not need (project.md sec 16.91).
 *
 * The temperature curve carries one point per column, so a plot 800
 * pixels wide hands the stroker eight hundred segments and eight hundred
 * joins -- and it is stroked TWICE, once for the halo and once for the
 * ink, the halo with round joins that cost an arc apiece. Measured on a
 * one-day view of real weather, that pair was 70% of the whole paint.
 *
 * Almost none of those vertices say anything. A weather trace sampled
 * per pixel is smooth by the time it is drawn, so most points sit on the
 * line between their neighbours to well under a pixel.
 *
 * Douglas-Peucker, which gives the guarantee that makes this safe to do
 * behind the reader's back: EVERY dropped point lies within `tolerance`
 * of the polyline that is kept. Not "usually" and not "on this data" --
 * the algorithm's recursion is exactly the statement that no point
 * exceeds it, and bbq_simplify_keeps_every_point_within_tolerance holds
 * it to that on inputs chosen to be hostile.
 *
 * The first attempt here measured each point against the segment from
 * the last KEPT point to the point AFTER it, which is a different and
 * much weaker test: the reference line moves along with the point being
 * judged, so accumulated drift never registers and a smooth arc reduces
 * to nothing. It took a 429-point curve to 7 and would have drawn a
 * polygon where the weather was. A simplifier that cannot state its
 * error bound is not a simplifier.
 *
 * Endpoints are always kept, so a caller that has already split its
 * curve into runs keeps every break exactly where it was.
 */
QPolygonF bbq_simplify_polyline(const QPolygonF &line, double tolerance);

#endif
