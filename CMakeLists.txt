cmake_minimum_required(VERSION 3.7)
project (GLINT2)
enable_language(Fortran)
set(CMAKE_CXX_STANDARD 14)

if (NOT (${CMAKE_VERSION} VERSION_LESS "3.12"))
  # Policy CMP0074 was introduced in 3.12. See "cmake --help-policies" for details.
  # use PackageName_ROOT variables
  cmake_policy(SET CMP0074 NEW)
endif()

list (APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include (setup_rpath)
message("TRANSITIVE $ENV{SPACK_TRANSITIVE_INCLUDE_PATH}")
include_directories($ENV{SPACK_TRANSITIVE_INCLUDE_PATH})

# ------- Process Configure Options

if (NOT DEFINED BUILD_PYTHON)
    set(BUILD_PYTHON NO)
endif()

if (NOT DEFINED BUILD_DOCS)
    set(BUILD_DOCS NO)
endif()
# Doesn't build docs anyway

if (NOT DEFINED BUILD_GRIDGEN)
    set(BUILD_GRIDGEN YES)
endif()
if (BUILD_GRIDGEN)
    find_package(CGAL REQUIRED)
    find_package(GMP REQUIRED)
    find_package(MPFR REQUIRED)

    # tools in gridgen/ use TCLAP
    find_package(TCLAP REQUIRED)
    include_directories(${TCLAP_INCLUDE_DIR})

    list(APPEND EXTERNAL_LIBS ${CGAL_LIBRARY} ${GMP_LIBRARY} ${MPFR_LIBRARIES})
    list(APPEND Boost_COMPONENTS thread system program_options mpi serialization )
    include_directories(${CGAL_INCLUDE_DIRS} ${GMP_INCLUDE_DIR} ${MPFR_INCLUDES})
    add_definitions(-DBUILD_GRIDGEN)
endif()

macro(icebin_find_petsc)
  find_package(PkgConfig REQUIRED)

  message(STATUS
    "Looking for PETSc (PETSC_DIR='$ENV{PETSC_DIR}', PETSC_ARCH='$ENV{PETSC_ARCH}')...")
  list(APPEND CMAKE_PREFIX_PATH $ENV{PETSC_DIR}/$ENV{PETSC_ARCH})

  pkg_search_module(PETSC IMPORTED_TARGET "PETSc>=3.7.0")

  if (NOT PETSC_FOUND)
    # If PETSc was configured with --prefix=... then PETSC_ARCH is irrelevant and we
    # should ignore it.
    message(STATUS
      "Looking for PETSc (PETSC_DIR=$ENV{PETSC_DIR} ignoring PETSC_ARCH...)")
    list(APPEND CMAKE_PREFIX_PATH $ENV{PETSC_DIR})
    pkg_search_module(PETSC REQUIRED IMPORTED_TARGET "PETSc>=3.7.0")
  endif()

  if (PETSC_FOUND)
    message(STATUS "Found PETSc: ${PETSC_PREFIX} (found version \"${PETSC_VERSION}\")")
  endif()
endmacro()


if (NOT DEFINED BUILD_COUPLER)
    set(BUILD_COUPLER YES)
endif()
if (BUILD_COUPLER)
    find_package(MPI REQUIRED)
    include_directories(${MPI_CXX_INCLUDE_PATH})
    list(APPEND EXTERNAL_LIBS ${MPI_CXX_LIBRARIES})

    if (NOT DEFINED USE_PISM)
        set(USE_PISM NO)
    endif()
    if(USE_PISM)
        find_package(PkgConfig REQUIRED)

        pkg_search_module(PISM REQUIRED IMPORTED_TARGET pismicebin)
        if (PISM_FOUND)
          message(STATUS
            "Found PISM-IceBin: ${PISM_PREFIX} (found version \"${PISM_VERSION}\")")
        endif()

        icebin_find_petsc()
        list(APPEND EXTERNAL_LIBS PkgConfig::PETSC PkgConfig::PISM)

        add_definitions(-DUSE_PISM)
    endif()
    add_definitions(-DBUILD_COUPLER)
endif()

if (NOT DEFINED BUILD_MODELE)
    set(BUILD_MODELE NO)
endif()
if (BUILD_MODELE)
    add_definitions(-DBUILD_MODELE)
    list(APPEND Boost_COMPONENTS regex program_options)
endif()

if (NOT DEFINED BUILD_DOCS)
    set(BUILD_DOCS YES)
endif()


find_package(Boost REQUIRED COMPONENTS ${Boost_COMPONENTS} filesystem date_time)
#find_package(Boost REQUIRED thread system log filesystem date_time)
include_directories(${Boost_INCLUDE_DIRS})
list(APPEND EXTERNAL_LIBS ${Boost_LIBRARIES})

# -------- Find Dependencies

# --- Everytrace
find_package(Everytrace REQUIRED)
add_definitions(-DUSE_EVERYTRACE)
include_directories(${EVERYTRACE_INCLUDE_DIR})
list(APPEND EXTERNAL_LIBS ${EVERYTRACE_LIBRARY})

# --- Blitz
find_package(Blitz REQUIRED)
add_definitions(-DUSE_BLITZ)
include_directories(${BLITZ_INCLUDE_DIR})
list(APPEND EXTERNAL_LIBS ${BLITZ_LIBRARY})

# --- NetCDF-CXX4
find_package(NetCDF_CXX4 REQUIRED)
add_definitions(-DUSE_NETCDF)
include_directories(${NETCDF_CXX4_INCLUDE_DIR})
list(APPEND EXTERNAL_LIBS ${NETCDF_CXX4_LIBRARY})

# --- IBMisc
find_package(Ibmisc REQUIRED)
list(APPEND EXTERNAL_LIBS ${IBMISC_LIBRARY})
include_directories(${IBMISC_INCLUDE_DIR})

find_package(ZLIB REQUIRED)
list(APPEND EXTERNAL_LIBS ${ZLIB_LIBRARIES})
include_directories(${ZLIB_INCLUDE_DIRS})

# --- Proj.4
find_package(PROJ4 REQUIRED)
add_definitions(-DUSE_PROJ4)
include_directories(${PROJ4_INCLUDES})
list(APPEND EXTERNAL_LIBS ${PROJ4_LIBRARIES})

# --- Eigen
find_package(Eigen3 REQUIRED)
include_directories(${EIGEN3_INCLUDE_DIR})

# -------- Process subdirectories of the build
add_subdirectory(slib)
if (BUILD_MODELE)
    add_subdirectory(modele)    # Don't need this right now, but it is current
endif()
add_subdirectory(tests)
if (BUILD_GRIDGEN)
    add_subdirectory(gridgen)
endif()

if(${BUILD_PYTHON})
    add_subdirectory(pylib)
endif()

