#pragma once

#include <cstring>
#include <vector>
#include <memory>
#include <netcdfcpp.h>
#include <blitz/array.h>
#include <boost/function.hpp>
#include <boost/bind.hpp>
#include <giss/blitz.hpp>

namespace giss {



// --------------------------------------------------------------------
// Convert template types to Numpy type_nums

template<class T>
inline NcType get_nc_type()
{
	fprintf(stderr, "get_nc_type(): Unknown type");
	throw std::exception();
}


template<> inline NcType get_nc_type<double>()
	{ return ncDouble; }

template<> inline NcType get_nc_type<int>()
	{ return ncInt; }

// --------------------------------------------------------------------
// Avoid memory leaks
inline std::unique_ptr<NcAtt> get_att(NcVar *var, NcToken attname)
{
	NcAtt *att = var->get_att(attname);
	return std::unique_ptr<NcAtt>(att);
}
// --------------------------------------------------------------------




extern NcDim *get_or_add_dim(NcFile &nc, std::string const &dim_name, long dim_size);

NcVar *get_var_safe(NcFile &nc, std::string const &var_name);

//extern std::vector<double> read_double_vector(NcFile &nc, std::string const &var_name);

//extern std::vector<int> read_int_vector(NcFile &nc, std::string const &var_name);

template<class T>
std::vector<T> read_vector(NcFile &nc, std::string const &var_name);

template<class T>
std::vector<T> read_vector(NcFile &nc, std::string const &var_name)
{
	// Read points vector
	NcVar *vpoints = nc.get_var(var_name.c_str());
	long npoints = vpoints->get_dim(0)->size();
	std::vector<T> points(npoints);
	vpoints->get(&points[0], npoints);
	return points;
}


// Compatibility functions.  Deprecated.
inline std::vector<double> read_double_vector(NcFile &nc, std::string const &var_name)
	{ return read_vector<double>(nc, var_name); }

inline std::vector<int> read_int_vector(NcFile &nc, std::string const &var_name)
	{ return read_vector<int>(nc, var_name); }

// -----------------------------------------------------------

template<class T, int rank>
blitz::Array<T,rank> read_blitz(NcFile &nc, std::string const &var_name);

template<class T, int rank>
blitz::Array<T,rank> read_blitz(NcFile &nc, std::string const &var_name)
{
	// Read points vector
	NcVar *vpoints = nc.get_var(var_name.c_str());
	int ndims = vpoints->num_dims();
	if (ndims != rank) {
		fprintf(stderr, "NetCDF variable %s has rank %d, expected rank %d\n",
			var_name.c_str(), ndims, rank);
		throw std::exception();
	}

	blitz::TinyVector<int,rank> shape(0);
	long counts[rank];
	for (int i=0; i<rank; ++i) {
		shape[i] = vpoints->get_dim(i)->size();
printf("read_blitz: shape[%d] = %d\n", i, shape[i]);
		counts[i] = shape[i];
	}

	blitz::Array<T,rank> ret(shape);
for (int i=0; i<rank; ++i) printf("read_blitz: ret.extent(%d) = %d\n", i, ret.extent(i));
	vpoints->get(ret.data(), counts);
	return ret;
}


// -----------------------------------------------------------
void netcdf_write_functions(std::vector<boost::function<void ()>> const &functions);


// -----------------------------------------------------------
/** NOTE: val is NOT a pointer to ensure the blitz::Array is copied into
the boost::bind closure.  blitz::Array does reference counting, so this
does not actually copy any data. */
template<class T, int rank>
void netcdf_write_blitz(NcVar *nc_var, blitz::Array<T, rank> const &val);

template<class T, int rank>
void netcdf_write_blitz(NcVar *nc_var, blitz::Array<T, rank> const &val)
{
	long counts[rank];
	for (int i=0; i<rank; ++i) counts[i] = val.extent(i);
	nc_var->put(val.data(), counts);
}
// ----------------------------------------------------
template<class T, int rank>
boost::function<void ()> netcdf_define(
	NcFile &nc,
	std::string const &vname,
	blitz::Array<T,rank> const &val,
	std::vector<NcDim *> const &ddims = {});

template<class T, int rank>
boost::function<void ()> netcdf_define(
	NcFile &nc,
	std::string const &vname,
	blitz::Array<T,rank> const &val,
	std::vector<NcDim *> const &ddims = {})
{

	// Type-check for unit strides
	int stride = 1;
	for (int i=rank-1; i>=0; --i) {
		if (val.stride(i) != stride) {
			fprintf(stderr, "Unexpected stride of %d in dimension %d (extent=%d) of %s (rank=%d)\n", val.stride(i), i, val.extent(i), vname.c_str(), rank);
			throw std::exception();
		}
		stride *= val.extent(i);
	}

	// Create the required dimensions
	NcDim const *dims[rank];
	long counts[rank];
	for (int i=0; i<rank; ++i) {
		if (i >= ddims.size()) {
			char dim_name[200];
			sprintf(dim_name, "%s.dim%d", vname.c_str(), i);
			dims[i] = nc.add_dim(dim_name, val.extent(i));
		} else {
			dims[i] = ddims[i];
		}
		counts[i] = val.extent(i);
	}

	// Create the variable
	NcVar *nc_var = nc.add_var(vname.c_str(), get_nc_type<T>(), rank, dims);

	// Write it out (later)
	return boost::bind(&netcdf_write_blitz<T,rank>, nc_var, val);
}
// -------------------------------------------------------------
template<class T>
inline boost::function<void ()> netcdf_define(
	NcFile &nc,
	std::string const &vname,
	std::vector<T> const &val,
	std::vector<NcDim *> const &ddims = {})
{
	blitz::Array<T,1> const bval(vector_to_blitz(val));
//		const_cast<std::vector<T>>(val)));
	return netcdf_define<T,1>(nc, vname, bval, ddims);
}


}
