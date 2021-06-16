#ifndef PROBLEMS_TUMOR_VASCULATURE_PLOT_HPP_
#define PROBLEMS_TUMOR_VASCULATURE_PLOT_HPP_

#include <ostream>

#include "defs.hpp"


namespace tumor::vasc {

void plot(std::ostream& os, const val_array& v);

void plot(const std::string& file, const val_array& v);

}

#endif /* PROBLEMS_TUMOR_VASCULATURE_PLOT_HPP_ */