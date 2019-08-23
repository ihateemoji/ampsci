#include "DiracSpinor.hpp"
#include "Maths/Grid.hpp"
#include "Maths/NumCalc_quadIntegrate.hpp"
#include "Physics/AtomInfo.hpp"
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

//******************************************************************************
DiracSpinor::DiracSpinor(int in_n, int in_k, const Grid &rgrid, bool in_imag_g)
    : p_rgrid(&rgrid),                        //
      n(in_n), k(in_k), en(0.0),              //
      f(std::vector<double>(rgrid.ngp, 0.0)), //
      g(f),                                   //
      pinf(rgrid.ngp - 1),                    //
      imaginary_g(in_imag_g),                 //
      its(-1), eps(-1), occ_frac(0),          //
      m_twoj(AtomInfo::twoj_k(in_k)),         //
      m_l(AtomInfo::l_k(in_k)),               //
      m_parity(AtomInfo::parity_k(in_k)),     //
      m_k_index(AtomInfo::indexFromKappa(in_k)) {}

//******************************************************************************
std::string DiracSpinor::symbol(bool gnuplot) const {
  // Readable symbol (s_1/2, p_{3/2} etc.).
  // gnuplot-firndly '{}' braces optional.
  std::string ostring1 = std::to_string(n) + AtomInfo::l_symbol(m_l);
  std::string ostring2 = gnuplot ? "_{" + std::to_string(m_twoj) + "/2}"
                                 : "_" + std::to_string(m_twoj) + "/2";
  return ostring1 + ostring2;
}

std::string DiracSpinor::shortSymbol() const {
  std::string pm = (k < 0) ? "+" : "-";
  return std::to_string(n) + AtomInfo::l_symbol(m_l) + pm;
}

//******************************************************************************
double DiracSpinor::norm() const { return std::sqrt((*this) * (*this)); }

//******************************************************************************
void DiracSpinor::scale(const double factor) {
  // for (auto &f_r : f)
  //   f_r *= factor;
  // for (auto &g_r : g)
  //   g_r *= factor;
  for (std::size_t i = 0; i < pinf; ++i)
    f[i] *= factor;
  for (std::size_t i = 0; i < pinf; ++i)
    g[i] *= factor;
}

//******************************************************************************
void DiracSpinor::normalise(double norm_to) {
  double rescale_factor = norm_to / norm();
  scale(rescale_factor);
}

//******************************************************************************
std::pair<double, double> DiracSpinor::r0pinfratio() const {
  auto max_abs_compare = [](double a, double b) {
    return std::fabs(a) < std::fabs(b);
  };
  auto max_pos = std::max_element(f.begin(), f.begin() + pinf, max_abs_compare);
  auto r0_ratio = f[0] / *max_pos;
  auto pinf_ratio = f[pinf - 1] / *max_pos;
  return std::make_pair(r0_ratio, pinf_ratio);
  // nb: do i care about ratio to max? or just value?
}

//******************************************************************************
//******************************************************************************
double operator*(const DiracSpinor &lhs, const DiracSpinor &rhs) {
  // XXX This is slow??? And one of the most critial parts!
  // Change the relative sign based in Complex f or g component
  // (includes complex conjugation of lhs)
  int ffs = ((!lhs.imaginary_g) && rhs.imaginary_g) ? -1 : 1;
  int ggs = (lhs.imaginary_g && !rhs.imaginary_g) ? -1 : 1;
  // auto imax = lhs.p_rgrid->ngp; // std::min(pinf, rhs.pinf); //XXX
  auto imax = std::min(lhs.pinf, rhs.pinf); // XXX
  auto ff = NumCalc::integrate(lhs.f, rhs.f, lhs.p_rgrid->drdu, 1.0, 0, imax);
  auto gg = NumCalc::integrate(lhs.g, rhs.g, lhs.p_rgrid->drdu, 1.0, 0, imax);
  return (ffs * ff + ggs * gg) * lhs.p_rgrid->du;
}

DiracSpinor &DiracSpinor::operator+=(const DiracSpinor &rhs) {
  // auto imax = p_rgrid->ngp;
  // auto imax = std::max(pinf, rhs.pinf); // XXX
  // pinf = imax;
  auto imax = std::min(pinf, rhs.pinf); // XXX
  for (std::size_t i = 0; i < imax; i++)
    f[i] += rhs.f[i];
  for (std::size_t i = 0; i < imax; i++)
    g[i] += rhs.g[i];
  return *this;
}
DiracSpinor operator+(DiracSpinor lhs, const DiracSpinor &rhs) {
  lhs += rhs;
  return lhs;
}
DiracSpinor &DiracSpinor::operator-=(const DiracSpinor &rhs) {
  // auto imax = p_rgrid->ngp; // std::min(pinf, rhs.pinf); //XXX
  // auto imax = pinf;
  // auto imax = std::max(pinf, rhs.pinf); // XXX
  // pinf = imax;
  auto imax = std::min(pinf, rhs.pinf); // XXX
  for (std::size_t i = 0; i < imax; i++)
    f[i] -= rhs.f[i];
  for (std::size_t i = 0; i < imax; i++)
    g[i] -= rhs.g[i];
  return *this;
}
DiracSpinor operator-(DiracSpinor lhs, const DiracSpinor &rhs) {
  lhs -= rhs;
  return lhs;
}

DiracSpinor &DiracSpinor::operator*=(const double x) {
  scale(x);
  return *this;
}
DiracSpinor operator*(DiracSpinor lhs, const double x) {
  lhs *= x;
  return lhs;
}
DiracSpinor operator*(const double x, DiracSpinor rhs) {
  rhs *= x;
  return rhs;
}
DiracSpinor operator*(const std::vector<double> &v, DiracSpinor rhs) {
  // auto max = rhs.p_rgrid->ngp;
  auto max = rhs.pinf;
  for (auto i = 0ul; i < max; i++) {
    rhs.f[i] *= v[i];
    rhs.g[i] *= v[i];
  }
  return rhs;
}

DiracSpinor &DiracSpinor::operator=(const DiracSpinor &other) {
  if (this != &other) {
    en = other.en;
    f = other.f;
    g = other.g;
    pinf = other.pinf;
  }
  return *this;
}

//******************************************************************************
//******************************************************************************
// comparitor overloads:

bool operator==(const DiracSpinor &lhs, const DiracSpinor &rhs) {
  return lhs.n == rhs.n && lhs.k == rhs.k;
}

bool operator!=(const DiracSpinor &lhs, const DiracSpinor &rhs) {
  return !(lhs == rhs);
}

bool operator<(const DiracSpinor &lhs, const DiracSpinor &rhs) {
  if (lhs.n == rhs.n)
    return AtomInfo::indexFromKappa(lhs.k) < AtomInfo::indexFromKappa(rhs.k);
  return lhs.n < rhs.n;
}

bool operator>(const DiracSpinor &lhs, const DiracSpinor &rhs) {
  return rhs < lhs;
}

bool operator<=(const DiracSpinor &lhs, const DiracSpinor &rhs) {
  return !(lhs > rhs);
}

bool operator>=(const DiracSpinor &lhs, const DiracSpinor &rhs) {
  return !(lhs < rhs);
}
