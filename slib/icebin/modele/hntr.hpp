#ifndef ICEBIN_HNTR_HPP
#define ICEBIN_HNTR_HPP

#include <ibmisc/blitz.hpp>
#include <icebin/eigen_types.hpp>

namespace icebin {
namespace modele {

class HntrGrid {
public:
    int const im;    // Number of cells in east-west direction
    int const jm;    // Number of cells in north-south direction

    // number (fraction) of cells in east-west direction from
    // International Date Line (180) to western edge of cell IA=1
    double const offi;

    // minutes of latitude for non-polar cells on grid A
    double const dlat;

//protected:
    blitz::Array<double,1> _dxyp;

public:
    int size() const { return im * jm; }
    double dxyp(int j) const { return _dxyp(j); }

    HntrGrid(int _im, int _jm, double _offi, double _dlat);

    template<class TypeT>
    blitz::Array<TypeT, 2> Array() const
        { return blitz::Array<TypeT,2>(im,jm, blitz::fortranArray); }

    void ncio(ibmisc::NcIO &ncio, std::string const &vname);

};

/** Pre-computed overlap details needed to regrid from one lat/lon
    grid to another on the sphere. */
class Hntr {
public:
    HntrGrid const Agrid;
    HntrGrid const Bgrid;

//protected:
    // SINA(JA) = sine of latitude of northern edge of cell JA on grid A
    // SINB(JB) = sine of latitude of northern edge of cell JB on grid B
    // FMIN(IB) = fraction of cell IMIN(IB) on grid A west of cell IB
    // FMAX(IB) = fraction of cell IMAX(IB) on grid A east of cell IB
    // GMIN(JB) = fraction of cell JMIN(JB) on grid A south of cell JB
    // GMAX(JB) = fraction of cell JMAX(JB) on grid A north of cell JB
    // IMIN(IB) = western most cell of grid A that intersects cell IB
    // IMAX(IB) = eastern most cell of grid A that intersects cell IB
    // JMIN(JB) = southern most cell of grid A that intersects cell JB
    // JMAX(JB) = northern most cell of grid A that intersects cell JB

    blitz::Array<double, 1> SINA, SINB;
    blitz::Array<double, 1> FMIN, FMAX;
    blitz::Array<int,1> IMIN, IMAX;
    blitz::Array<double, 1> GMIN, GMAX;
    blitz::Array<int,1> JMIN, JMAX;

    // DATMIS = missing data value inserted in output array B when
    // cell (IB,JB) has integrated value 0 of WTA
    double DATMIS;

public:
    /** Initialize overlap data structures, get ready to re-grid.
    TODO: Reference, don't copy, these HntrGrid instances. */
    Hntr(HntrGrid const &_A, HntrGrid const &_B, double _DATMIS);

    Hntr(std::array<HntrGrid const *,2> grids, double _DATMIS)
        : Hntr(*grids[1], *grids[0], _DATMIS) {}


    /**
    HNTR4 performs a horizontal interpolation of per unit area or per
    unit mass quantities defined on grid A, calculating the quantity
    on grid B.  B grid values that cannot be calculated because the
    covering A grid boxes have WTA = 0, are set to the value DATMIS.
    The area weighted integral of the quantity is conserved.
    The 3 Real input values are expected to be Real*4.

    ** NOTE **
        All arrays use 1-based (Fortran-style) indexing!!!

    Inputs to this method must all be 1-D 1-based (Fortran-style) arrays.
    See regrid() for a method accepting "natural" 2-D 1-based arrays.

    Input: WTA = weighting array for values on the A grid
             A = per unit area or per unit mass quantity
             mean_polar: polar values are replaced by their
                 longitudinal mean.
    Output:  B = horizontally interpolated quantity on B grid
    */
    void regrid1(
        blitz::Array<double,1> const &WTA,
        blitz::Array<double,1> const &A,
        blitz::Array<double,1> &B,
        bool mean_polar = false) const;

    void matrix(
        MakeDenseEigenT::AccumT &&accum,        // The output (sparse) matrix; 0-based indexing
        blitz::Array<double,1> const &_WTA);


    /** Works with 0-based or 1-based N-dimensional arrays */
    template<int RANK>
    void regrid(
        blitz::Array<double,RANK> const &WTA,
        blitz::Array<double,RANK> const &A,
        blitz::Array<double,RANK> &B,
        bool mean_polar = false) const;

    template<int RANK>
    blitz::Array<double,RANK> regrid(
        blitz::Array<double,RANK> const &WTA,
        blitz::Array<double,RANK> const &A,
        bool mean_polar = false) const;

private:
    void partition_east_west();
    void partition_north_south();

public:

    template<class AccumT>
    void matrix(
        AccumT &&accum,        // The output (sparse) matrix; 0-based indexing
        std::function<bool(long)> const &Bindex_clip,    // OPTIONAL: Fast-filter out things in B, by their index
        blitz::Array<double,1> const &WTB);    // Weight (size) of each basis function in Bgrid

};    // class Hntr

template<class AccumT>
void Hntr::matrix(
    AccumT &&accum,        // The output (sparse) matrix; 0-based indexing
    std::function<bool(long)> const &Bindex_clip,    // OPTIONAL: Fast-filter out things in B, by their index
    blitz::Array<double,1> const &WTB)    // Weight (size) of each basis function in Bgrid
{
    // ------------------
    // Interpolate the A grid onto the B grid
    for (int JB=1; JB <= Bgrid.jm; ++JB) {
        int JAMIN = JMIN(JB);
        int JAMAX = JMAX(JB);

        for (int IB=1; IB <= Bgrid.im; ++IB) {
            int const IJB = IB + Bgrid.im * (JB-1);
            double const wtb_ijb = WTB(IJB);

            if (!Bindex_clip(IJB-1)) continue;

            int const IAMIN = IMIN(IB);
            int const IAMAX = IMAX(IB);
            for (int JA=JAMIN; JA <= JAMAX; ++JA) {
                double G = SINA(JA) - SINA(JA-1);
                if (JA==JAMIN) G -= GMIN(JB);
                if (JA==JAMAX) G -= GMAX(JB);

                for (int IAREV=IAMIN; IAREV <= IAMAX; ++IAREV) {
                    int const IA  = 1 + ((IAREV-1) % Agrid.im);
                    int const IJA = IA + Agrid.im * (JA-1);
                    double F = 1;
                    if (IAREV==IAMIN) F -= FMIN(IB);
                    if (IAREV==IAMAX) F -= FMAX(IB);

                    accum.add({IJB-1, IJA-1}, wtb_ijb*F*G);    // -1 ==> convert to 0-based indexing
                }
            }
        }
    }
}



/** Works with 0-based or 1-based N-dimensional arrays */
template<int RANK>
void Hntr::regrid(
    blitz::Array<double,RANK> const &WTA,
    blitz::Array<double,RANK> const &A,
    blitz::Array<double,RANK> &B,
    bool mean_polar) const
{
    auto B1(ibmisc::reshape1(B, 1));
    return regrid1(
        ibmisc::reshape1(WTA, 1),
        ibmisc::reshape1(A, 1),
        B1, mean_polar);
}

template<int RANK>
blitz::Array<double,RANK> Hntr::regrid(
    blitz::Array<double,RANK> const &WTA,
    blitz::Array<double,RANK> const &A,
    bool mean_polar) const
{
    blitz::Array<double,2> B(Bgrid.Array<double>());
    regrid(WTA, A, B, mean_polar);
    return B;
}


}}

#endif    // guard
