#ifndef FNCAS_JIT
#error "FNCAS_JIT should be set to build crash_test.cc."
#endif

#include <vector>
#include <map>

#include "../../fncas/fncas.h"

#include "boost/random.hpp"

#include "../function.h"
#include "autogen/functions.h"

int main(int argc, char* argv[]) {
  const size_t n = 100000;
  double _x[n] = {
#include "crash_data.h"
  };
  std::vector<double> x(_x, _x+n);
  printf("data: %lf %lf ...\n", x[0], x[1]);
  enchanced_math f;
  const double golden = f.eval_double(x);
  printf("eval: %lf\n", golden);
  auto intermediate = f.eval_expression(fncas::x(f.dim()));
  const double test = intermediate.eval(x);
  printf("ieval: %lf\n", test);
  auto compiled = fncas::compile(intermediate);
  const double test2 = compiled->eval(x);
  printf("ceval: %lf\n", test2);
}
