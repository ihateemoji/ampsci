#include "MBPT/CorrelationPotential.hpp"
#include "Angular/Angular_tables.hpp"
#include "Coulomb/Coulomb.hpp"
#include "Coulomb/YkTable.hpp"
#include "DiracODE/DiracODE.hpp"
#include "HF/HartreeFock.hpp"
#include "IO/FRW_fileReadWrite.hpp"
#include "IO/SafeProfiler.hpp"
#include "MBPT/GreenMatrix.hpp"
#include "Maths/Grid.hpp"
#include "Maths/Interpolator.hpp"
#include "Maths/LinAlg_MatrixVector.hpp"
#include "Maths/NumCalc_quadIntegrate.hpp"
#include "Physics/AtomData.hpp"
#include "Wavefunction/DiracSpinor.hpp"
#include "Wavefunction/Wavefunction.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <vector>

//******************************************************************************
// Helper function:
static inline int find_max_tj(const std::vector<DiracSpinor> &core,
                              const std::vector<DiracSpinor> &excited) {
  if (core.empty() || excited.empty())
    return 0;
  // returns maximum value of 2*j in {core,excited}
  auto maxtj1 =
      std::max_element(core.cbegin(), core.cend(), DiracSpinor::comp_j)->twoj();
  auto maxtj2 =
      std::max_element(excited.cbegin(), excited.cend(), DiracSpinor::comp_j)
          ->twoj();
  return std::max(maxtj1, maxtj2);
}
//******************************************************************************

namespace MBPT {

//******************************************************************************
//******************************************************************************
CorrelationPotential::CorrelationPotential(
    const Grid &gr, const std::vector<DiracSpinor> &core,
    const std::vector<DiracSpinor> &excited, const int in_stride,
    const std::vector<double> &en_list, const std::string &in_fname,
    const HF::HartreeFock *const in_hf)
    : p_gr(&gr),
      m_core(core),
      m_excited(excited),
      m_yec(&gr, &m_excited, &m_core),
      m_maxk(find_max_tj(core, excited)),
      m_6j(m_maxk, m_maxk),
      stride(std::size_t(in_stride)),
      p_hf(in_hf) {
  auto sp = IO::Profile::safeProfiler(__func__);

  std::cout << "\nCorrelation potential (Sigma)\n";

  std::cout << "(Including FF";
  if (include_G)
    std::cout << ", FG/GF, and GG";
  std::cout << ")\n";

  const auto fname = in_fname == "" ? "" : in_fname + ".Sigma";
  if (fname != "" && IO::FRW::file_exists(fname)) {
    read_write(fname, IO::FRW::read);
  } else if (!en_list.empty()) {
    setup_subGrid();
    form_Sigma(en_list, fname);
  }
}

//******************************************************************************
void CorrelationPotential::setup_subGrid() {
  // Form the "Sigma sub-grid"
  const auto &rvec = p_gr->r;
  // const double rmin = m_core.empty() ? 1.0e-4 : m_core.front().r0();
  // const double rmax = m_core.empty() ? 30.0 : m_core.front().rinf();
  const double rmin = 1.0e-4;
  const double rmax = 30.0;
  // const double rmin = 1.0e-6;
  // const double rmax = 120.0;

  imin = 0;
  for (auto i = 0ul; i < rvec.size(); i += stride) {
    auto r = rvec[i];
    if (r < rmin) {
      imin++;
      continue;
    }
    if (r > rmax)
      break;
    r_stride.push_back(r);
  }

  stride_points = r_stride.size();
  printf(
      "Sigma sub-grid: r=(%.1e, %.1f)aB with %i points. [i0=%i, stride=%i]\n",
      r_stride.front(), r_stride.back(), int(stride_points), int(imin),
      int(stride));
}

//******************************************************************************
GMatrix CorrelationPotential::G_single(const DiracSpinor &ket,
                                       const DiracSpinor &bra,
                                       const double f) const {
  GMatrix Gmat(stride_points, include_G);
  addto_G(&Gmat, ket, bra, f);
  return Gmat;
}

//******************************************************************************
void CorrelationPotential::addto_G(GMatrix *Gmat, const DiracSpinor &ket,
                                   const DiracSpinor &bra,
                                   const double f) const {
  auto sp = IO::Profile::safeProfiler(__func__);
  // Adds (f)*|ket><bra| to G matrix
  // G_ij = f * Q_i * W_j
  // Q = Q(1) = ket, W = W(2) = bra
  // Takes sub-grid into account; ket,bra are on full grid, G on sub-grid
  for (auto i = 0ul; i < stride_points; ++i) {
    const auto si = std::size_t((imin + i) * stride);
    for (auto j = 0ul; j < stride_points; ++j) {
      const auto sj = std::size_t((imin + j) * stride);
      Gmat->ff[i][j] += f * ket.f[si] * bra.f[sj];
      if constexpr (include_G) {
        Gmat->fg[i][j] += f * ket.f[si] * bra.g[sj];
        Gmat->gf[i][j] += f * ket.g[si] * bra.f[sj];
        Gmat->gg[i][j] += f * ket.g[si] * bra.g[sj];
      }
    } // j
  }   // i
}

//******************************************************************************
DiracSpinor CorrelationPotential::Sigma_G_Fv(const GMatrix &Gmat,
                                             const DiracSpinor &Fv) const {
  auto sp = IO::Profile::safeProfiler(__func__);
  // lambda is fitting factor (just scales Sigma|v>)
  // Sigma|v> = int G(r1,r2)*v(r2) dr2
  // (S|v>)_i = sum_j G_ij v_j drdu_j du
  // nb: G is on sub-grid, |v> and S|v> on full-grid. Use interpolation

  const auto ki = std::size_t(Fv.k_index());
  const auto lambda = ki >= m_lambda_kappa.size() ? 1.0 : m_lambda_kappa[ki];

  const auto &gr = *(Fv.p_rgrid);
  auto SigmaFv = DiracSpinor(0, Fv.k, gr);
  std::vector<double> f(r_stride.size());
  std::vector<double> g;
  if constexpr (include_G) {
    g.resize(r_stride.size());
  }
  for (auto i = 0ul; i < stride_points; ++i) {
    const auto si = std::size_t(i);
    for (auto j = 0ul; j < stride_points; ++j) {
      const auto sj = std::size_t((imin + j) * stride);
      const auto dr = gr.drdu[sj] * gr.du * double(stride);
      f[si] += Gmat.ff[i][j] * Fv.f[sj] * dr * lambda;

      if constexpr (include_G) {
        f[si] += Gmat.fg[i][j] * Fv.g[sj] * dr * lambda;
        g[si] += Gmat.gf[i][j] * Fv.f[sj] * dr * lambda;
        g[si] += Gmat.gg[i][j] * Fv.g[sj] * dr * lambda;
      }
    }
  }
  // Interpolate from sub-grid to full grid
  SigmaFv.f = Interpolator::interpolate(r_stride, f, gr.r);
  if constexpr (include_G) {
    SigmaFv.g = Interpolator::interpolate(r_stride, g, gr.r);
  }

  return SigmaFv;
}

//******************************************************************************
void CorrelationPotential::form_Sigma(const std::vector<double> &en_list,
                                      const std::string &fname) {
  auto sp = IO::Profile::safeProfiler(__func__);

  Sigma_kappa.resize(en_list.size(), {stride_points, include_G});

  if (m_core.empty() || m_excited.empty()) {
    std::cerr << "\nERROR 162 in form_Sigma: No basis! Sigma will just be 0!\n";
    return;
  }

  std::cout << "Forming correlation potential for:\n";
  for (auto ki = 0ul; ki < en_list.size(); ki++) {
    const auto kappa = Angular::kappaFromIndex(int(ki));

    // if v.kappa > basis, then Ck angular factor won't exist!
    auto tj = Angular::twojFromIndex(int(ki));
    if (tj > m_yec.Ck().max_tj())
      continue;

    printf(" k=%2i %6s at en=%8.5f.. ", kappa,
           AtomData::kappa_symbol(kappa).c_str(), en_list[ki]);
    std::cout << std::flush;
    fill_Sigma_k_Gold(&Sigma_kappa[ki], kappa, en_list[ki]);
    // find lowest excited state, output <v|S|v> energy shift:
    auto find_kappa = [=](const auto &a) { return a.k == kappa; };
    const auto vk =
        std::find_if(cbegin(m_excited), cend(m_excited), find_kappa);
    if (vk != cend(m_excited))
      std::cout << "de=" << *vk * Sigma2Fv(*vk);
    std::cout << "\n";
  }

  // write to disk
  if (fname != "")
    read_write(fname, IO::FRW::write);
}

//******************************************************************************
DiracSpinor CorrelationPotential::Sigma2Fv(const DiracSpinor &v) const {
  auto sp = IO::Profile::safeProfiler(__func__);
  // Find correct G matrix (corresponds to kappa_v), return Sigma|v>
  // If Sigma_kappa doesn't exist, returns |0>
  auto kappa_index = std::size_t(Angular::indexFromKappa(v.k));
  if (kappa_index >= Sigma_kappa.size())
    return 0.0 * v;
  return Sigma_G_Fv(Sigma_kappa[kappa_index], v);
}

//******************************************************************************
void CorrelationPotential::fill_Sigma_k_Gold(GMatrix *Gmat, const int kappa,
                                             const double en) {
  auto sp = IO::Profile::safeProfiler(__func__);

  // Four second-order diagrams:
  // Diagram (a):
  // |Q^k_amn><Q^k_amn| / de_amn / [k][j]
  // Diagram (b) (exchange):
  // |Q^k_amn><P^k_amn| / de_amn / [k][j]
  // Diagram (c):
  // |Q^k_nba><Q^k_nba| / de_nba / [k][j]
  // Diagram (d) (exchange):
  // |Q^k_nba><P^k_nba| / de_nba / [k][j]
  // where:
  // All indeces are summed over,
  // a & b are core states, n & m are virtual excited states,
  // k is multipolarity [Coloulmb expansion]
  // de_xyz = e_v + e_x - e_y - e_z

  const auto &Ck = m_yec.Ck();

  // Just for safety, should already be zero (unless re-calcing G)
  Gmat->ff.zero();
  Gmat->fg.zero();
  Gmat->gf.zero();
  Gmat->gg.zero();

  if (m_core.empty())
    return;
  const auto &gr = *(m_core.front().p_rgrid);

  // auto fk = [&](int k) { return 1.0; };

  // Note: get_yk_ab() must only be called with k for which y^k_ab exists!
  // Therefore, must use the k_minmax() function provided
#pragma omp parallel for
  for (auto ia = 0ul; ia < m_core.size(); ia++) {
    const auto &a = m_core[ia];
    GMatrix G_a(stride_points, include_G);
    auto Qkv = DiracSpinor(0, kappa, gr); // re-use to reduce alloc'ns
    auto Pkv = DiracSpinor(0, kappa, gr); // re-use to reduce alloc'ns
    for (const auto &n : m_excited) {
      const auto [kmin_nb, kmax_nb] = m_yec.k_minmax(n, a);
      const auto max_k = std::min(m_maxk, kmax_nb);
      for (int k = kmin_nb; k <= max_k; ++k) {
        if (Ck(k, a.k, n.k) == 0)
          continue;
        const auto f_kkjj = (2 * k + 1) * (Angular::twoj_k(kappa) + 1);
        const auto &yknb = m_yec(k, n, a);

        // Diagrams (a) [direct] and (b) [exchange]
        for (const auto &m : m_excited) {
          if (Ck(k, kappa, m.k) == 0)
            continue;
          Coulomb::Qkv_bcd(&Qkv, a, m, n, k, yknb, Ck);
          Coulomb::Pkv_bcd(&Pkv, a, m, n, k, m_yec(m, a), Ck, m_6j);
          const auto dele = en + a.en - m.en - n.en;
          const auto factor = 1.0 / (f_kkjj * dele);
          addto_G(&G_a, Qkv, Qkv + Pkv, factor);
        } // m

        // Diagrams (c) [direct] and (d) [exchange]
        for (const auto &b : m_core) {
          if (Ck(k, kappa, b.k) == 0)
            continue;
          Coulomb::Qkv_bcd(&Qkv, n, b, a, k, yknb, Ck);
          Coulomb::Pkv_bcd(&Pkv, n, b, a, k, m_yec(n, b), Ck, m_6j);
          const auto dele = en + n.en - b.en - a.en;
          const auto factor = 1.0 / (f_kkjj * dele);
          addto_G(&G_a, Qkv, Qkv + Pkv, factor);
        } // b

      } // k
    }   // n
#pragma omp critical(sumG)
    { *Gmat += G_a; }
  } // a
}

//******************************************************************************
double CorrelationPotential::Sigma2vw(const DiracSpinor &v,
                                      const DiracSpinor &w) const {
  // Calculates <Fv|Sigma|Fw> from scratch, at Fv energy [full grid + fg+gg]
  if (v.k != w.k)
    return 0.0;

  const auto &Ck = m_yec.Ck();

  // if v.kappa > basis, then Ck angular factor won't exist!
  if (v.twoj() > Ck.max_tj())
    return 0.0;

  std::vector<double> delta_a(m_core.size());
#pragma omp parallel for
  for (auto ia = 0ul; ia < m_core.size(); ia++) {
    const auto &a = m_core[ia];
    auto &del_a = delta_a[ia];
    for (const auto &n : m_excited) {
      const auto [kmin_nb, kmax_nb] = m_yec.k_minmax(n, a);
      const auto max_k = std::min(m_maxk, kmax_nb);
      for (int k = kmin_nb; k <= max_k; ++k) {
        if (Ck(k, a.k, n.k) == 0)
          continue;
        const auto f_kkjj = (2 * k + 1) * v.twojp1();
        const auto &yknb = m_yec.get_yk_ab(k, n, a);

        // Diagrams (a) [direct] and (b) [exchange]
        for (const auto &m : m_excited) {
          const auto Qkv = Coulomb::Qk_abcd(v, a, m, n, k, yknb, Ck);
          if (Qkv == 0.0)
            continue;
          const auto Qkw =
              (&v == &w) ? Qkv : Coulomb::Qk_abcd(w, a, m, n, k, yknb, Ck);
          const auto &ybm = m_yec.get_y_ab(m, a);
          const auto Pkw = Coulomb::Pk_abcd(w, a, m, n, k, ybm, Ck, m_6j);
          const auto dele = v.en + a.en - m.en - n.en;
          del_a += ((1.0 / dele / f_kkjj) * (Qkw + Pkw)) * Qkv;
        } // m

        // Diagrams (c) [direct] and (d) [exchange]
        for (const auto &b : m_core) {
          const auto Qkv = Coulomb::Qk_abcd(v, n, b, a, k, yknb, Ck);
          if (Qkv == 0.0)
            continue;
          const auto Qkw =
              (&v == &w) ? Qkv : Coulomb::Qk_abcd(w, n, b, a, k, yknb, Ck);
          const auto &yna = m_yec.get_y_ab(n, b);
          const auto Pkw = Coulomb::Pk_abcd(w, n, b, a, k, yna, Ck, m_6j);
          const auto dele = v.en + n.en - b.en - a.en;
          del_a += ((1.0 / dele / f_kkjj) * (Qkw + Pkw)) * Qkv;
        } // b

      } // k
    }   // n
  }     // a

  return std::accumulate(delta_a.cbegin(), delta_a.cend(), 0.0);
}

//******************************************************************************
void CorrelationPotential::read_write(const std::string &fname,
                                      IO::FRW::RoW rw) {
  auto rw_str = rw == IO::FRW::write ? "Writing to " : "Reading from ";
  std::cout << rw_str << "Sigma file: " << fname << " ... " << std::flush;

  std::fstream iofs;
  IO::FRW::open_binary(iofs, fname, rw);

  // // write/read some grid parameters - just to check
  {
    double r0 = rw == IO::FRW::write ? p_gr->r0 : 0;
    double rmax = rw == IO::FRW::write ? p_gr->rmax : 0;
    double b = rw == IO::FRW::write ? p_gr->b : 0;
    std::size_t pts = rw == IO::FRW::write ? p_gr->num_points : 0;
    rw_binary(iofs, rw, r0, rmax, b, pts);
    if (rw == IO::FRW::read) {
      const bool grid_ok = std::abs((r0 - p_gr->r0) / r0) < 1.0e-6 &&
                           std::abs(rmax - p_gr->rmax) < 0.001 &&
                           (b - p_gr->b) < 0.001 && pts == p_gr->num_points;
      if (!grid_ok) {
        std::cerr << "\nFAIL 335 in read_write Sigma: Grid mismatch\n"
                  << "Have in file:\n"
                  << r0 << ", " << rmax << " w/ N=" << pts << ", b=" << b
                  << ", but expected:\n"
                  << p_gr->r0 << ", " << p_gr->rmax
                  << " w/ N=" << p_gr->num_points << ", b=" << p_gr->b << "\n";
        std::abort(); // abort?
      }
    }
  }

  // Sub-grid:
  rw_binary(iofs, rw, stride_points, imin, stride);
  if (rw == IO::FRW::read) {
    r_stride.resize(std::size_t(stride_points));
  }
  for (auto i = 0ul; i < r_stride.size(); ++i) {
    rw_binary(iofs, rw, r_stride[i]);
  }

  // Number of kappas (number of Sigma/G matrices)
  std::size_t num_kappas = rw == IO::FRW::write ? Sigma_kappa.size() : 0;
  rw_binary(iofs, rw, num_kappas);
  if (rw == IO::FRW::read) {
    Sigma_kappa.resize(num_kappas, {stride_points, include_G});
  }

  // Check if include FG/GG written. Note: doesn't matter if mis-match?!
  auto incl_g = rw == IO::FRW::write ? include_G : 0;
  rw_binary(iofs, rw, incl_g);

  // Read/Write G matrices
  for (auto &Gk : Sigma_kappa) {
    for (auto i = 0ul; i < stride_points; ++i) {
      for (auto j = 0ul; j < stride_points; ++j) {
        rw_binary(iofs, rw, Gk.ff[i][j]);
        if (incl_g) {
          rw_binary(iofs, rw, Gk.fg[i][j]);
          rw_binary(iofs, rw, Gk.gf[i][j]);
          rw_binary(iofs, rw, Gk.gg[i][j]);
        }
      }
    }
  }
  std::cout << "... done.\n";
  printf(
      "Sigma sub-grid: r=(%.1e, %.1f)aB with %i points. [i0=%i, stride=%i]\n",
      r_stride.front(), r_stride.back(), int(stride_points), int(imin),
      int(stride));
}

//******************************************************************************
void CorrelationPotential::print_scaling() const {
  if (!m_lambda_kappa.empty()) {
    std::cout << "Scaled Sigma, with: lambda_kappa = ";
    for (const auto &l : m_lambda_kappa) {
      std::cout << l << ", ";
    }
    std::cout << "\n";
  }
}

//******************************************************************************
//******************************************************************************

//******************************************************************************
GMatrix CorrelationPotential::Green_core(int kappa, double en) const {
  // G_core = \sum_a |a><a|/(e-ea), for all a with a.k=k
  GMatrix Gcore(stride_points, include_G);

  // loop over HF core, not Sigma core (only excited!)
  const auto &core = p_hf->get_core();
  for (const auto &a : core) {
    if (a.k == kappa)
      addto_G(&Gcore, a, a, 1.0 / (en - a.en));
  }
  return Gcore;
}

//******************************************************************************
GMatrix CorrelationPotential::Green_hf(int kappa, double en) const {
  DiracSpinor x0(0, kappa, *p_gr);
  DiracSpinor xI(0, kappa, *p_gr);

  const auto &Hmag = p_hf->get_Hrad_mag(x0.l());
  const auto alpha = p_hf->m_alpha;

  const auto Nc = p_hf->num_core_electrons();
  const auto eta = -0.5 / Nc;
  auto eta_vd = p_hf->get_vdir();
  NumCalc::scaleVec(eta_vd, eta);
  auto vl = p_hf->get_vlocal(x0.l());
  NumCalc::add_to_vector(vl, eta_vd);

  DiracODE::regularAtOrigin(x0, en, vl, Hmag, alpha);
  DiracODE::regularAtInfinity(xI, en, vl, Hmag, alpha);

  const auto pp = std::size_t(0.65 * double(xI.pinf));
  const auto w = -1.0 * (xI.f[pp] * x0.g[pp] - x0.f[pp] * xI.g[pp]) / alpha;
  // not sure why -ve sign.. is sign even defined by DiracODE?

  const auto g0 = MakeGreensG(x0, xI, w);

  GMatrix Ident(stride_points, include_G);
  Ident.make_identity();
  const auto Vx = Make_Vx(kappa, eta_vd);

  return g0 * ((Ident - g0 * Vx).inverse());
}

//******************************************************************************
GMatrix CorrelationPotential::polarisation(int k_a, int k_alpha,
                                           double omega) const {
  GMatrix pi(stride_points, include_G);
  for (const auto &a : m_core) {
    if (a.k != k_a)
      continue;
    // Non-allocating version for ga_ex!
    auto g_alpha_ex =
        Green_hf(k_alpha, a.en - omega) - Green_core(k_alpha, a.en - omega) +
        Green_hf(k_alpha, a.en + omega) - Green_core(k_alpha, a.en + omega);
    auto ketbra_a = G_single(a, a, 1.0);
    g_alpha_ex.mult_elements_by(ketbra_a);
    pi += g_alpha_ex;
  }
  return pi;
}

//******************************************************************************
// ComplexGMatrix ComplexG(const GMatrix &Gre, double om_imag) const {
//   //
// }

//******************************************************************************
GMatrix CorrelationPotential::MakeGreensG(const DiracSpinor &x0,
                                          const DiracSpinor &xI,
                                          const double w) const {
  auto sp = IO::Profile::safeProfiler(__func__);
  // Takes sub-grid into account; ket,bra are on full grid, G on sub-grid
  // G(r1,r2) = x0(rmin)*xI(imax)/w
  GMatrix g0I(stride_points, include_G);
  // XXX Take advantage of symmetry!?
  for (auto i = 0ul; i < stride_points; ++i) {
    const auto si = imin + i * stride;
    for (auto j = 0ul; j < stride_points; ++j) {
      const auto sj = imin + j * stride;
      const auto irmin = std::min(sj, si);
      const auto irmax = std::max(sj, si);
      g0I.ff[i][j] = x0.f[irmin] * xI.f[irmax] / w;
      if constexpr (include_G) {
        g0I.fg[i][j] = x0.f[irmin] * xI.g[irmax] / w;
        g0I.gf[i][j] = x0.g[irmin] * xI.f[irmax] / w;
        g0I.gg[i][j] = x0.g[irmin] * xI.g[irmax] / w;
      }
    } // j
  }   // i
  return g0I;
}

//******************************************************************************
GMatrix CorrelationPotential::Make_Vx(int kappa,
                                      const std::vector<double> vx) const {
  auto sp = IO::Profile::safeProfiler(__func__);

  const auto tj = Angular::twoj_k(kappa);

  GMatrix Vx(stride_points, include_G);

  for (auto i = 0ul; i < stride_points; ++i) {
    const auto si = imin + i * stride;
    const auto dri = p_gr->drdu[si] * p_gr->du * double(stride);
    for (auto j = 0ul; j < stride_points; ++j) {
      const auto sj = imin + j * stride;
      const auto drj = p_gr->drdu[sj] * p_gr->du * double(stride);
      const auto irmin = std::min(sj, si);
      const auto irmax = std::max(sj, si);
      const auto rmin = p_gr->r[irmin];
      const auto rmax = p_gr->r[irmax];

      const auto q_factor = rmin / rmax;

      for (const auto &a : m_core) {
        const auto kmax = (a.twoj() + tj) / 2;
        auto q = 1.0 / rmin; // (1.0 / rmax) / (rmin / rmax), for k=-1
        for (int k = 0; k <= kmax; ++k) {
          q *= q_factor; // = rmin^k/rmax^k+1 //XXX check!

          const auto c1 = Angular::Ck_kk(k, kappa, a.k);
          if (c1 == 0)
            continue;
          const auto c = -1.0 * c1 * c1 / (tj + 1);

          Vx.ff[i][j] += c * a.f[si] * a.f[sj] * q * dri * drj;
          if constexpr (include_G) {
            Vx.fg[i][j] += c * a.f[si] * a.g[sj] * q * dri * drj;
            Vx.gf[i][j] += c * a.g[si] * a.f[sj] * q * dri * drj;
            Vx.gg[i][j] += c * a.g[si] * a.g[sj] * q * dri * drj;
          }
        }
      }
    } // j
    // subtract of "effective/approx" exchange term (eta*v_dir)
    Vx.ff[i][i] -= vx[si] * dri * dri;
    if constexpr (include_G) {
      Vx.fg[i][i] -= vx[si] * dri * dri;
      Vx.gf[i][i] -= vx[si] * dri * dri;
      Vx.gg[i][i] -= vx[si] * dri * dri;
    }
  }

  return Vx;
}

} // namespace MBPT
