// The options of compilers other than nvcc that clang's table reads
// otherwise or not at all, one entry per spelling:
//
//   OWN_OPTION(SPELLING, KIND, CLANG, FAMILIES)
//
// SPELLING is written with its prefix, and a `/` one takes cl's `-` too;
// KIND is how it takes its value (cli::OptionKind); CLANG the clang option
// it reads as, INVALID for one a command leaves out; FAMILIES the families
// that write it. nvcc's options are a table of their own (nvcc_options.inc).

// A file of each unit the build writes, left out as clang's own outputs are
// (is_discarded_option): GCC's module mapper and dependency scan output,
// its SARIF and profile notes, Apple's index unit output.
OWN_OPTION("-fmodule-mapper=", Joined, INVALID, all)
OWN_OPTION("-fdeps-format=", Joined, INVALID, all)
OWN_OPTION("-fdeps-file=", Joined, INVALID, all)
OWN_OPTION("-fdeps-target=", Joined, INVALID, all)
OWN_OPTION("-fdiagnostics-add-output=", Joined, INVALID, all)
OWN_OPTION("-fdiagnostics-set-output=", Joined, INVALID, all)
OWN_OPTION("-fprofile-note=", Joined, INVALID, all)
OWN_OPTION("-aux-info", Separate, INVALID, all)
OWN_OPTION("-index-unit-output-path", Separate, INVALID, all)

// cl's C++ modules: what a unit is (an interface of any suffix, an internal
// partition), the module interfaces it writes and where it finds those
// built before it. clice reads what a unit is from its text and builds its
// modules itself, as it does with GCC's mapper.
OWN_OPTION("/interface", Flag, INVALID, msvc)
OWN_OPTION("/internalPartition", Flag, INVALID, msvc)
OWN_OPTION("/ifcOutput", JoinedOrSeparate, INVALID, msvc)
OWN_OPTION("/ifcSearchDir", JoinedOrSeparate, INVALID, msvc)
OWN_OPTION("/ifcMap", JoinedOrSeparate, INVALID, msvc)
OWN_OPTION("/stdIfcDir", JoinedOrSeparate, INVALID, msvc)
OWN_OPTION("/scanDependencies", JoinedOrSeparate, INVALID, msvc)

// Intel's OpenMP, whose _OPENMP clang's -fopenmp defines alike. icx in cl
// mode ignores the GCC-style spellings.
OWN_OPTION("-qopenmp", Flag, fopenmp, intel)
OWN_OPTION("-fiopenmp", Flag, fopenmp, intel)
OWN_OPTION("-qno-openmp", Flag, fno_openmp, intel)
OWN_OPTION("-qopenmp-simd", Flag, fopenmp_simd, intel)
OWN_OPTION("/Qiopenmp", Flag, fopenmp, intel_cl)
OWN_OPTION("/Qopenmp", Flag, fopenmp, intel_cl)
OWN_OPTION("/Qopenmp-", Flag, fno_openmp, intel_cl)
OWN_OPTION("/Qopenmp-simd", Flag, fopenmp_simd, intel_cl)

// icx's language standard in cl mode, which CMake writes with either
// separator: any of the C and C++ standards -std= names.
OWN_OPTION("/Qstd:", Joined, std_EQ, intel_cl)
OWN_OPTION("/Qstd=", Joined, std_EQ, intel_cl)

OWN_OPTION("/fsycl", Flag, fsycl, intel_cl)
