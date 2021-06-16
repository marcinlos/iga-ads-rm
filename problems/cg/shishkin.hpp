#ifndef PROBLEMS_CG_SHISHKIN_HPP
#define PROBLEMS_CG_SHISHKIN_HPP

#include <cmath>

#include "ads/bspline/bspline.hpp"
#include "ads/util.hpp"


inline double shishkin_const(int n, double eps) {
    return std::log(n) / std::log(2) * eps;
}

ads::bspline::basis create_basis(double a, double b, int p, int elements, int repeated_nodes, bool adapt, double d);

#endif // PROBLEMS_CG_SHISHKIN_HPP