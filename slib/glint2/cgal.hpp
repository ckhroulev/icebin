#pragma once

#include <CGAL/Exact_predicates_exact_constructions_kernel.h>
#include <CGAL/Boolean_set_operations_2.h>
#include <CGAL/bounding_box.h>


namespace glint2 {

/** GLINT2-specific instatiations of CGAL templates. */
namespace gc {
	typedef CGAL::Exact_predicates_exact_constructions_kernel Kernel;
	typedef Kernel::Point_2                                   Point_2;
	typedef Kernel::Iso_rectangle_2                           Iso_rectangle_2;
	typedef CGAL::Polygon_2<Kernel>                           Polygon_2;
	typedef CGAL::Polygon_with_holes_2<Kernel> Polygon_with_holes_2;
}


// =======================================================================
// A general template to compute CGAL Polygon Overlaps

/**
Computes the overlap area of two linear simple polygons. Uses BSO and then add the area of the polygon and substract
the area of its holes.
@param P The first polygon.
@param Q The second polygon.
@return The area of the overlap between P and Q.
@see acg.cs.tau.ac.il/courses/workshop/spring-2008/useful-routines.h
*/
template <class Kernel, class Container>
CGAL::Polygon_2<Kernel, Container>
poly_overlap(const CGAL::Polygon_2<Kernel, Container> &P, 
	const CGAL::Polygon_2<Kernel, Container> &Q)
{
	CGAL_precondition(P.is_simple());
	CGAL_precondition(Q.is_simple());

	// typedef typename CGAL::Polygon_2<Kernel, Container>::FT FT;
	typedef CGAL::Polygon_with_holes_2<Kernel, Container> Polygon_with_holes_2;
	// typedef std::list<Polygon_with_holes_2> Pol_list;

	std::list<Polygon_with_holes_2> overlap;
	CGAL::intersection(P, Q, std::back_inserter(overlap));

	// Look at all the polygons returned
	auto ii0 = overlap.begin();
	if (ii0 == overlap.end()) {
		// Empty list
		return gc::Polygon_2();
	}

	auto ii1(ii0); ++ii1;

	if (ii1 != overlap.end()) {
		// More than one Polygon here --- complain
		fprintf(stderr, "ERROR: Overlap expects only simple polygons.  If this is not a bug, you must upgrade your non-CGAL overlap data structures.");
		throw std::exception();
	}

	if (ii0->holes_begin() != ii0->holes_end()) {
		// Polygon has holes --- again, our format doesn't support it.
		fprintf(stderr, "ERROR: Overlap expects only simple polygons.  If this is not a bug, you must upgrade your non-CGAL overlap data structures.");
		throw std::exception();
	}

	// We have a nice, clean overlap.  Return it!
	return ii0->outer_boundary();
}

inline std::unique_ptr<gc::Polygon_2> Cell_to_Polygon_2(Cell const &cell)
{
	std::unique_ptr<gc::Polygon_2> poly(new gc::Polygon_2());

	// Copy the vertices
	for (auto vertex = cell.begin(); vertex != cell.end(); ++vertex)
		poly->push_back(gc::Point_2(vertex->x, vertex->y));

	return poly;
}


}	// namespace glint2