#include <cmath>
#include <giss/CooVector.hpp>
#include <giss/ncutil.hpp>
#include <glint2/MatrixMaker.hpp>
#include <glint2/IceSheet_L0.hpp>
#include <giss/IndexTranslator.hpp>
#include <giss/IndexTranslator2.hpp>
#include <galahad/qpt_c.hpp>
#include <galahad/eqp_c.hpp>

namespace glint2 {

void MatrixMaker::clear()
{
	sheets.clear();
	sheets_by_id.clear();
	grid1.reset();
	mask1.reset();
	hpdefs.clear();
	// hcmax.clear();
}

void MatrixMaker::realize() {

	// ---------- Check array bounds
	long n1 = grid1->ndata();
	if (mask1.get() && mask1->extent(0) != n1) {
		fprintf(stderr, "mask1 for %s has wrong size: %d (vs %d expected)\n",
			mask1->extent(0), n1);
		throw std::exception();
	}

	// ------------- Realize the ice sheets
	for (auto sheet=sheets.begin(); sheet != sheets.end(); ++sheet)
		sheet->realize();

	// ------------- Set up HCIndex
	hc_index = HCIndex::new_HCIndex(_hptype, *this);
}

int MatrixMaker::add_ice_sheet(std::unique_ptr<IceSheet> &&sheet)
{
	if (sheet->name == "") {
		fprintf(stderr, "MatrixMaker::add_ice_sheet(): Sheet must have a name\n");
		throw std::exception();
	}

	int const index = _next_sheet_index++;
	sheet->index = index;
printf("MatrixMaker: %p.sheetno = %d\n", &*sheet, sheet->index);
	sheet->gcm = this;
	
	sheets_by_id.insert(std::make_pair(sheet->index, sheet.get()));
	sheets.insert(sheet->name, std::move(sheet));
	return index;
}

// --------------------------------------------------------------
/** NOTE: Allows for multiple ice sheets overlapping the same grid cell (as long as they do not overlap each other, which would make no physical sense). */
void MatrixMaker::fgice(giss::CooVector<int,double> &fgice1)
{

	// Accumulate areas over all ice sheets
	giss::SparseAccumulator<int,double> area1_m_hc;
	fgice1.clear();
	for (auto sheet = sheets.begin(); sheet != sheets.end(); ++sheet) {

		// Local area1_m just for this ice sheet
		giss::SparseAccumulator<int,double> area1_m;
		sheet->accum_areas(area1_m);

		// Use the local area1_m to contribute to fgice1
		giss::Proj2 proj;
		grid1->get_ll_to_xy(proj, sheet->grid2->sproj);
		for (auto ii = area1_m.begin(); ii != area1_m.end(); ++ii) {
			int const i1 = ii->first;
			double ice_covered_area = ii->second;
			Cell *cell = grid1->get_cell(i1);
			if (!cell) continue;	// Ignore cells in the halo
			double area1 = area_of_proj_polygon(*cell, proj);
			fgice1.add(i1, ice_covered_area / area1);

		}
	}
	fgice1.sort();
}

// --------------------------------------------------------------
/** Change this to a boost function later
@param trans_2_2p Tells us which columns of conserv (i2) are active,
       after masking with mask1, mask1h and mask2.
*/
static std::unique_ptr<giss::VectorSparseMatrix> remove_small_constraints(
giss::VectorSparseMatrix const &in_constraints_const,
int min_row_count)
{
	// Const cast because we don't know how to do const_iterator() right in VectorSparseMatrix
	auto in_constraints(const_cast<giss::VectorSparseMatrix &>(in_constraints_const));

	std::set<int> delete_row;		// Rows to delete
	std::set<int> delete_col;		// Cols to delete

	// Make sure there are no constraints (rows) with too few variables (columns).
	// Uses an iterative process
	std::vector<int> row_count(in_constraints.nrow);
	for (;;) {
		// Count rows
		row_count.clear(); row_count.resize(in_constraints.nrow);
		for (auto oi = in_constraints.begin(); oi != in_constraints.end(); ++oi) {
			int i2 = oi.col();

			// Loop if it's already in our delete_row and delete_col sets
			if (delete_row.find(oi.row()) != delete_row.end()) continue;
			if (delete_col.find(i2) != delete_col.end()) continue;

			++row_count[oi.row()];
		}


		// Add to our deletion set
		int num_deleted = 0;
		for (auto oi = in_constraints.begin(); oi != in_constraints.end(); ++oi) {
			int i2 = oi.col();

			// Loop if it's already in our delete_row and delete_col sets
			if (delete_row.find(oi.row()) != delete_row.end()) continue;
			if (delete_col.find(i2) != delete_col.end()) continue;

			if (row_count[oi.row()] < min_row_count) {
				++num_deleted;
				delete_row.insert(oi.row());
				delete_col.insert(i2);
			}
		}

		// Terminate if we didn't remove anything on this round
printf("num_deleted = %d\n", num_deleted);
		if (num_deleted == 0) break;
	}


	// Copy over the matrix, deleting rows and columns as planned
	std::unique_ptr<giss::VectorSparseMatrix> out_constraints(
		new giss::VectorSparseMatrix(giss::SparseDescr(in_constraints)));
	for (auto oi = in_constraints.begin(); oi != in_constraints.end(); ++oi) {
		int i2 = oi.col();

		// Loop if it's already in our delete_row and delete_col sets
		if (delete_row.find(oi.row()) != delete_row.end()) continue;
		if (delete_col.find(i2) != delete_col.end()) continue;

		out_constraints->set(oi.row(), i2, oi.val());
	}
	return out_constraints;
}
// -------------------------------------------------------------
/** Checksums an interpolation matrix, to ensure that the sume of weights
for each output grid cell is 1.  This should always be the case, no
matter what kind of interpolation is used. */
static bool checksum_interp(giss::VectorSparseMatrix &mat, std::string const &name,
double epsilon)
{
	bool ret = true;
	auto rowsums(mat.sum_per_row_map());
	for (auto ii = rowsums.begin(); ii != rowsums.end(); ++ii) {
		int row = ii->first;
		double sum = ii->second;
		if (std::abs(sum - 1.0d) > epsilon) {
			printf("rowsum != 1 at %s: %d %g\n", name.c_str(), row, sum);
			ret = false;
		}
	}
	return false;
}
// -------------------------------------------------------------
class I3XTranslator {
	HCIndex *hc_index;
	int nhc;

public:

	I3XTranslator(HCIndex *_hc_index, int _nhc) :
		hc_index(_hc_index), nhc(_nhc) {}

	int i3x_to_i3(int i3x)
	{
		int i1 = i3x / nhc;
		int k = i3x - i1 * nhc;
		return hc_index->ik_to_index(i1, k);
	}

	int i3_to_i3x(int i3)
	{
		int i1, k;
		hc_index->index_to_ik(i3, i1, k);
		int i3x = i1 * nhc + k;
	//printf("i3=%d (%d, %d) --> i3x=%d\n", i3, i1, k, i3x);
		return i3x;
	}
};
// -------------------------------------------------------------
/** @params f2 Some field on each ice grid (referenced by ID).  Do not have to be complete.
TODO: This only works on one ice sheet.  Will need to be extended
for multiple ice sheets. */
giss::CooVector<int, double>
MatrixMaker::ice_to_hp(
std::map<int, blitz::Array<double,1>> &f2s,
blitz::Array<double,1> &initial3)
{
printf("BEGIN MatrixMaker::ice_to_hp()\n");
	// =============== Set up basic vector spaces for optimization problem
	std::set<int> used1, used3;
	std::set<std::pair<int,int>> used2;

	// Used in constraints
	std::unique_ptr<giss::VectorSparseMatrix> RM0(hp_to_atm());	// 3->1
	for (auto ii = RM0->begin(); ii != RM0->end(); ++ii) {
		used1.insert(ii.row());
		int i3 = ii.col();
		used3.insert(i3);
	}

// In some cases in the past, QP optimization has not worked well
// when there are grid cells with very few entries in the
// constraints matrix.  Not an issue here now.
#if 0
	std::unique_ptr<giss::VectorSparseMatrix> RM(
		remove_small_constraints(*RM0, 2));
	RM0.reset();
#else
	std::unique_ptr<giss::VectorSparseMatrix> RM(std::move(RM0));
#endif

	giss::SparseAccumulator<int,double> area1;
	giss::MapDict<int, giss::VectorSparseMatrix> Ss;
	giss::MapDict<int, giss::VectorSparseMatrix> XMs;
	std::map<int, size_t> size2;	// Size of each ice vector space
	for (auto f2i=f2s.begin(); f2i != f2s.end(); ++f2i) {
		IceSheet *sheet = (*this)[f2i->first];

		std::unique_ptr<giss::VectorSparseMatrix> S(
			sheet->ice_to_projatm(area1));		// 2 -> 1
		if (_correct_area1) S = multiply(
			*sheet->atm_proj_correct(ProjCorrect::PROJ_TO_NATIVE),
			*S);
		for (auto ii = S->begin(); ii != S->end(); ++ii) {
			used1.insert(ii.row());
			used2.insert(std::make_pair(sheet->index, ii.col()));
		}

		std::unique_ptr<giss::VectorSparseMatrix> XM(
			sheet->hp_to_ice());				// 3 -> 2
//		checksum_interp(*XM, "XM");

		for (auto ii = XM->begin(); ii != XM->end(); ++ii) {
			used2.insert(std::make_pair(sheet->index, ii.row()));
			int i3 = ii.col();
			used3.insert(i3);
		}
printf("MatrixMaker::ice_to_hp() 4\n");

		size2[sheet->index] = sheet->n2();

		// Store away for later reference
		Ss.insert(sheet->index, std::move(S));
		XMs.insert(sheet->index, std::move(XM));
	}

	// Count the number of height points
	// (so we can set up 3x indexing scheme)
	int max_k = 0;		// Maximum height class index
	for (auto p3 = used3.begin(); p3 != used3.end(); ++p3) {
		int i1, k;
		hc_index->index_to_ik(*p3, i1, k);
		max_k = std::max(k, max_k);
	}

	// Convert from i3 to i3x (renumbered height class indices)
	I3XTranslator trans3x(&*hc_index, max_k + 1);
	std::set<int> used3x;
	for (auto p3 = used3.begin(); p3 != used3.end(); ++p3) {
		int i3x = trans3x.i3_to_i3x(*p3);
		used3x.insert(i3x);
	}

	giss::IndexTranslator trans_1_1p("trans_1_1p");
		trans_1_1p.init(n1(), used1);
	giss::IndexTranslator2 trans_2_2p("trans_2_2p");
		trans_2_2p.init(std::move(size2), used2);
	giss::IndexTranslator trans_3x_3p("trans_3x_3p");
		trans_3x_3p.init(n3(), used3x);

	int n1p = trans_1_1p.nb();
	int n2p = trans_2_2p.nb();
	int n3p = trans_3x_3p.nb();

	// Translate to new matrices
	giss::VectorSparseMatrix RMp(giss::SparseDescr(n1p, n3p));
	giss::VectorSparseMatrix Sp(giss::SparseDescr(n1p, n2p));
	giss::VectorSparseMatrix XMp(giss::SparseDescr(n2p, n3p));


printf("Translating RM\n");
	for (auto ii = RM->begin(); ii != RM->end(); ++ii) {
		int i3x = trans3x.i3_to_i3x(ii.col());
		RMp.add(
			trans_1_1p.a2b(ii.row()),
			trans_3x_3p.a2b(i3x), ii.val());
	}


	for (auto f2i=f2s.begin(); f2i != f2s.end(); ++f2i) {
		int const index = f2i->first;
		IceSheet *sheet = (*this)[index];

		giss::VectorSparseMatrix *S(Ss[index]);
		giss::VectorSparseMatrix *XM(XMs[index]);

printf("Translating S: %d\n", index);
		for (auto ii = S->begin(); ii != S->end(); ++ii) {
			Sp.add(
				trans_1_1p.a2b(ii.row()),
				trans_2_2p.a2b(std::make_pair(index, ii.col())),
				ii.val());
		}

printf("Translating XM: %d\n", index);
		for (auto ii = XM->begin(); ii != XM->end(); ++ii) {
			int i3x = trans3x.i3_to_i3x(ii.col());
			XMp.add(
				trans_2_2p.a2b(std::make_pair(index, ii.row())),
				trans_3x_3p.a2b(i3x),
				ii.val());
		}
	}

	// -------- Translate f2 -> f2p
	// Ignore elements NOT listed in the translation
	blitz::Array<double,1> f2p(n2p);
	f2p = 0;
	for (int i2p = 0; i2p < n2p; ++i2p) {
		std::pair<int,int> const &a(trans_2_2p.b2a(i2p));
		int index = a.first;
		int i2 = a.second;
		f2p(i2p) = f2s[index](i2);
	}

	// ----------- Translate area1 -> area1p
	blitz::Array<double,1> area1p_inv(n1p);
	area1p_inv = 0;
	for (auto ii = area1.begin(); ii != area1.end(); ++ii) {
		int i1 = ii->first;
		int i1p = trans_1_1p.a2b(i1);
		area1p_inv(i1p) += ii->second;
	}
	for (int i1p=0; i1p<n1p; ++i1p) {
		if (area1p_inv(i1p) != 0) area1p_inv(i1p) = 1.0d / area1p_inv(i1p);
	}

	// ---------- Divide Sp by area1p to complete the regridding matrix
	for (auto ii = Sp.begin(); ii != Sp.end(); ++ii) {
		int i1p = ii.row();
		ii.val() *= area1p_inv(i1p);
	}

	// ========================================================
	// ========================================================

 	// ---------- Allocate the QPT problem
	// m = # constraints = n1p (size of atmosphere grid)
	// n = # variabeles = n3p
	galahad::qpt_problem_c qpt(n1p, n3p, true);

	// ================ Objective Function
	// 1/2 (XM F_E - F_I)^2    where XM = (Ice->Exch)(Elev->Ice)
	// qpt%H = (XM)^T (XM),    qpt%G = f_I \cdot (XM),        qpt%f = f_I \cdot f_I

	// -------- H = 2 * XMp^T XMp
	giss::VectorSparseMatrix XMp_T(giss::SparseDescr(XMp.ncol, XMp.nrow));
	transpose(XMp, XMp_T);
	std::unique_ptr<giss::VectorSparseMatrix> H(multiply(XMp_T, XMp));	// n3xn3

	// Count items in H lower triangle
	size_t ltri = 0;
	for (auto ii = H->begin(); ii != H->end(); ++ii)
		if (ii.row() >= ii.col()) ++ltri;

	// Copy ONLY the lower triangle items to GALAHAD
	// (otherwise, GALAHAD won't work)
	qpt.alloc_H(ltri);
	giss::ZD11SparseMatrix H_zd11(qpt.H, 0);
	for (auto ii = H->begin(); ii != H->end(); ++ii) {
		if (ii.row() >= ii.col())
			H_zd11.add(ii.row(), ii.col(), 2.0d * ii.val());
	}

	// -------- Linear term of obj function
	// G = -2*f2p \cdot XMp
	for (int i=0; i < qpt.n; ++i) qpt.G[i] = 0;
	for (auto ii = XMp.begin(); ii != XMp.end(); ++ii) {
		qpt.G[ii.col()] -= 2.0d * f2p(ii.row()) * ii.val();
	}

	// --------- Constant term of objective function
	// f = f2p \cdot f2p
	qpt.f = 0;
	for (int i2p=0; i2p<n2p; ++i2p) {
		qpt.f += f2p(i2p) * f2p(i2p);
	}

	// De-allocate...
//	H.reset();
//	XMp.clear();
//	XMp_T.clear();

	// ============================ Constraints
	// RM x = Sp f2p

	// qpt.A = constraints matrix = RMp
	qpt.alloc_A(RMp.size());
	giss::ZD11SparseMatrix A_zd11(qpt.A, 0);
	copy(RMp, A_zd11);

	// qpt.C = equality constraints RHS = Sp * f2p
	for (int i=0; i<n1p; ++i) qpt.C[i] = 0;
	for (auto ii = Sp.begin(); ii != Sp.end(); ++ii) {
		int i1p = ii.row();		// Atm
		int i2p = ii.col();		// Ice
		qpt.C[i1p] -= f2p(i2p) * ii.val();
	}

	// De-allocate
	RMp.clear();

	// =========================== Initial guess at solution
	bool nanerr = false;
	for (int i3p=0; i3p<n3p; ++i3p) {
		int i3x = trans_3x_3p.b2a(i3p);
		int i3 = trans3x.i3x_to_i3(i3x);
		double val = initial3(i3);
		if (std::isnan(val)) {
			fprintf(stderr, "ERROR: ice_to_hp(), NaN in initial guess, i3=%d\n", i3);
			nanerr = true;
			qpt.X[i3p] = 0;
		} else {
			qpt.X[i3p] = val;
		}
	}

	// =========================== Verify the objective function
#if 0
	// 1/2 (XM F_E - F_I)^2    where XM = (Ice->Exch)(Elev->Ice)
	printf("objective value1 = %g\n", qpt.eval_objective(qpt.X));


	// 1/2 (XM F_E - F_I)^2    where XM = (Ice->Exch)(Elev->Ice)
	printf("objective value = %g\n", qpt.eval_objective(qpt.X));
	{
		// Initial2p = XMp * initial3p
		blitz::Array<double,1> initial2p(n2p);
		initial2p = 0;
		for (auto ii = XMp.begin(); ii != XMp.end(); ++ii) {
			int i3p = ii.col();
			int i3 = trans_3_3p.b2a(i3p);
			int i2p = ii.row();
	//		initial2p(i2p) += ii.val() * initial3(i3);	// Both give same result
			initial2p(i2p) += ii.val() * qpt.X[i3p];
		}

		// Sum it up!
		double obj = 0;
		for (int i2p=0; i2p<n2p; ++i2p) {
			double val = initial2p(i2p) - f2p(i2p);
			obj += val*val;
		}

		printf("objective value2 = %g\n", obj);
	}

	{
		// Initial2p = XMp * initial3p
		blitz::Array<double,1> initial2p(n2p);
		initial2p = 0;
		for (auto ii = XMp_T.begin(); ii != XMp_T.end(); ++ii) {
			int i3p = ii.row();
			int i3 = trans_3_3p.b2a(i3p);
			int i2p = ii.col();
	//		initial2p(i2p) += ii.val() * initial3(i3);	// Both give same result
			initial2p(i2p) += ii.val() * qpt.X[i3p];
		}

		// Sum it up!
		double obj = 0;
		for (int i2p=0; i2p<n2p; ++i2p) {
			double val = initial2p(i2p) - f2p(i2p);
			obj += val*val;
		}

		printf("objective value3 = %g\n", obj);
	}


	// 1/2 (XM F_E - F_I)^2    where XM = (Ice->Exch)(Elev->Ice)
	printf("objective value = %g\n", qpt.eval_objective(qpt.X));
	{
		int const index = 0;
		// Initial2 = XM * initial3
		auto sheet(sheets_by_id.find(index)->second);
		blitz::Array<double,1> initial2(sheet->n2());
		initial2 = 0;
		giss::VectorSparseMatrix *XM(XMs[index]);
		for (auto ii = XM->begin(); ii != XM->end(); ++ii) {
			int i3 = ii.col();
			int i2 = ii.row();
			initial2(i2) += ii.val() * initial3(i3);
		}

		// Sum it up!
		double obj = 0;
		for (int i2=0; i2<sheet->n2(); ++i2) {
			double val = initial2(i2) - f2s[index](i2);
			if (std::isnan(val)) continue;
			obj += val*val;
		}

		printf("objective value4 = %g\n", obj);
	}
#endif


	// =========================== Solve the Problem!
	double infinity = 1e20;
	eqp_solve_simple(qpt.this_f, infinity);



	// ========================================================
	// ========================================================

	// --------- Pick out the answer and convert back to standard vector space
	giss::CooVector<int, double> ret;
	for (int i3p=0; i3p<n3p; ++i3p) {
		int i3x = trans_3x_3p.b2a(i3p);
		int i3 = trans3x.i3x_to_i3(i3x);
		ret.add(i3, qpt.X[i3p]);
	}

	return ret;
}


// --------------------------------------------------------------
/** TODO: This doesn't account for spherical earth */
std::unique_ptr<giss::VectorSparseMatrix> MatrixMaker::hp_to_atm()
{
//	int n1 = grid1->ndata();
printf("BEGIN hp_to_atm() %d %d\n", n1(), n3());
	std::unique_ptr<giss::VectorSparseMatrix> ret(
		new giss::VectorSparseMatrix(
		giss::SparseDescr(n1(), n3())));

	// Compute the hp->ice and ice->hc transformations for each ice sheet
	// and combine into one hp->hc matrix for all ice sheets.
	giss::SparseAccumulator<int,double> area1_m;
	for (auto sheet = sheets.begin(); sheet != sheets.end(); ++sheet) {
		auto hp2proj(sheet->hp_to_projatm(area1_m));
		if (_correct_area1) hp2proj = multiply(
			*sheet->atm_proj_correct(ProjCorrect::PROJ_TO_NATIVE),
			*hp2proj);
		ret->append(*hp2proj);
	}

	giss::SparseAccumulator<int,double> area1_m_inv;
	divide_by(*ret, area1_m, area1_m_inv);
	ret->sum_duplicates();

printf("END hp_to_atm()\n");
	return ret;
}
// --------------------------------------------------------------
// --------------------------------------------------------------
// --------------------------------------------------------------
// ==============================================================
// Write out the parts that this class computed --- so we can test/check them

boost::function<void ()> MatrixMaker::netcdf_define(NcFile &nc, std::string const &vname) const
{
	std::vector<boost::function<void ()>> fns;
	fns.reserve(sheets.size() + 1);

printf("MatrixMaker::netcdf_define(%s) (BEGIN)\n", vname.c_str());

	// ------ Attributes
	auto one_dim = giss::get_or_add_dim(nc, "one", 1);
	NcVar *info_var = nc.add_var((vname + ".info").c_str(), ncInt, one_dim);
	info_var->add_att("hptype", _hptype.str());

	// Names of the ice sheets
	std::string sheet_names = "";
	for (auto sheet = sheets.begin(); ; ) {
		sheet_names.append(sheet->name);
		++sheet;
		if (sheet == sheets.end()) break;
		sheet_names.append(",");
	}
	info_var->add_att("sheetnames", sheet_names.c_str());
#if 0
		info_var->add_att("grid1.name", gcm->grid1->name.c_str());
		info_var->add_att("grid2.name", grid2->name.c_str());
		info_var->add_att("exgrid.name", exgrid->name.c_str());
#endif

	// Define the variables
	fns.push_back(grid1->netcdf_define(nc, vname + ".grid1"));
	if (mask1.get())
		fns.push_back(giss::netcdf_define(nc, vname + "mask1", *mask1));
	fns.push_back(giss::netcdf_define(nc, vname + ".hpdefs", hpdefs));
	for (auto sheet = sheets.begin(); sheet != sheets.end(); ++sheet) {
		fns.push_back(sheet->netcdf_define(nc, vname + "." + sheet->name));
	}


printf("MatrixMaker::netcdf_define(%s) (END)\n", vname.c_str());

	return boost::bind(&giss::netcdf_write_functions, fns);
}
// -------------------------------------------------------------
static std::vector<std::string> parse_comma_list(std::string list)
{
	std::stringstream ss(list);
	std::vector<std::string> result;

	while( ss.good() ) {
		std::string substr;
		getline( ss, substr, ',' );
		result.push_back( substr );
	}
	return result;
}

std::unique_ptr<IceSheet> read_icesheet(NcFile &nc, std::string const &vname)
{
	auto info_var = nc.get_var((vname + ".info").c_str());
	std::string stype(giss::get_att(info_var, "parameterization")->as_string(0));

	std::unique_ptr<IceSheet> sheet;
	if (stype == "L0") {
		sheet.reset(new IceSheet_L0);
	}
#if 0
	else if (stype == "L1") {
		sheet.reset(new IceSheet_L1);
	}
#endif

	sheet->read_from_netcdf(nc, vname);
	printf("read_icesheet(%s) END\n", vname.c_str());
	return sheet;

}


void MatrixMaker::read_from_netcdf(NcFile &nc, std::string const &vname)
{
	clear();

	printf("MatrixMaker::read_from_netcdf(%s) 1\n", vname.c_str());
	grid1.reset(read_grid(nc, vname + ".grid1").release());
	if (giss::get_var_safe(nc, vname + ".mask1")) {
		mask1.reset(new blitz::Array<int,1>(
		giss::read_blitz<int,1>(nc, vname + ".mask1")));
	}
	hpdefs = giss::read_vector<double>(nc, vname + ".hpdefs");

	printf("MatrixMaker::read_from_netcdf(%s) 2\n", vname.c_str());

//	grid2.reset(read_grid(nc, "grid2").release());
//	exgrid.reset(read_grid(nc, "exgrid").release());

	// Read list of ice sheets
	NcVar *info_var = nc.get_var((vname + ".info").c_str());

	std::string shptype(giss::get_att(info_var, "hptype")->as_string(0));
	_hptype = *HCIndex::Type::get_by_name(shptype.c_str());


	std::vector<std::string> sheet_names = parse_comma_list(std::string(
		giss::get_att(info_var, "sheetnames")->as_string(0)));

	for (auto sname = sheet_names.begin(); sname != sheet_names.end(); ++sname) {
		std::string var_name(vname + "." + *sname);
		printf("MatrixMaker::read_from_netcdf(%s) %s 3\n",
			vname.c_str(), var_name.c_str());
		add_ice_sheet(read_icesheet(nc, var_name));
	}

	// Remove grid cells that are not part of this domain.
	// TODO: This should be done while reading the cells in the first place.
	boost::function<bool (int)> include_cell1(domain->get_in_halo2());
	grid1->filter_cells(include_cell1);

	// Now remove cells from the exgrids and grid2s that interacted with grid1
	for (auto sheet=sheets.begin(); sheet != sheets.end(); ++sheet) {
		sheet->filter_cells1(include_cell1);
	}

}

std::unique_ptr<IceSheet> new_ice_sheet(Grid::Parameterization parameterization)
{
	switch(parameterization.index()) {
		case Grid::Parameterization::L0 : {
			IceSheet *ics = new IceSheet_L0;
			return std::unique_ptr<IceSheet>(ics);
//			return std::unique_ptr<IceSheet>(new IceSheet_L0);
		} break;
#if 0
		case Grid::Parameterization::L1 :
			return std::unique_ptr<IceSheet>(new IceSheet_L1);
		break;
#endif
		default :
			fprintf(stderr, "Unrecognized parameterization: %s\n", parameterization.str());
			throw std::exception();
	}
}


}
