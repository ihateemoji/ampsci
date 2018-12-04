#pragma once
#include "ElectronOrbitals.h"
#include <vector>
/*
Calculates self-consistent Hartree-Fock potential, including exchange.
Solves all core and valence states.

//XXX Have option to give a list of valence states!
// Can solve them to some degree in parallel
//Requires re-writing the valence part (a little)

*/
class HartreeFock{

  public:

    HartreeFock(ElectronOrbitals &wf, double eps_HF = 1.e-8);

    void solveValence(ElectronOrbitals &wf, int n, int kappa);

    double calculateCoreEnergy(const ElectronOrbitals &wf);

  private:

    double m_eps_HF = 1.e-8;

    const int MAX_HART_ITS = 64;

    int m_ngp;
    int m_num_core_states;
    std::vector<int> twoj_list;
    std::vector<int> kappa_index_list;
    int m_max_kappa_index_so_far;

    //The "localised"/approximate HF potential:
    std::vector< std::vector<double> > vex;

    //Store underlying arrays. These are 'double private'
    std::vector<std::vector<std::vector<std::vector<double> > > > m_arr_v_abk_r;
    std::vector<std::vector<std::vector<double> > > m_arr_Lambda_nmk;

  private:

    void hartree_fock_core(ElectronOrbitals &wf);
    void starting_approx_core(ElectronOrbitals &wf);

    void form_core_Lambda_abk(const std::vector<int> &kappa);
    void extend_Lambda_abk(int kappa_a);
    double get_Lambda_abk(int a, int b, int k) const;

    void initialise_m_arr_v_abk_r_core();
    void extend_m_arr_v_abk_r_valence(int kappa_a);

    int index_from_kappa(int ka) const;
    int twoj_from_index(int i) const;
    int kappa_from_index(int i); //XXX
    int l_from_index(int i) const;

    void form_vabk_core(const ElectronOrbitals &wf);
    void form_vbb0(const ElectronOrbitals &wf);
    void calculate_v_abk(const ElectronOrbitals &wf, int a, int b, int k,
      std::vector<double> & vabk);
    std::vector<double>& get_v_aa0(int a);
    std::vector<std::vector<double> >& get_v_abk(int a, int b);

    void form_vabk_valence(const ElectronOrbitals &wf, int w);

    void form_vdir(std::vector<double> &vdir, const ElectronOrbitals &wf,
      bool re_scale=false);

    void form_approx_vex_core(std::vector<std::vector<double> > &vex,
      const ElectronOrbitals &wf);

    void form_approx_vex_a(int a, std::vector<double> &vex_a,
      const ElectronOrbitals &wf);


};
