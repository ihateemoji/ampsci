#ifndef _HART_H
#define _HART_H
#include<cmath>
#include <string>
#include <vector>
#include "ElectronOrbitals.h"
#include "PRM_parametricPotentials.h"
#include "ATI_atomInfo.h"
#include "WIG_369j.h"
#include <fstream>

namespace HF{

  const int MAX_HART_ITS=100; //Max number of Hartree iterations
  const double default_eps=1.e-6;

  int hartreeFockCore(ElectronOrbitals &wf, double eps_HF);
  int hartreeFockValence(ElectronOrbitals &wf, int na, int ka, double eps_HF);
  int hartreeCore(ElectronOrbitals &wf, double eps_hartree=default_eps);

  int formNewVdir(ElectronOrbitals &wf, std::vector<double> &vdir_new,
    bool core);

  int formVexCore(ElectronOrbitals &wf,
    std::vector< std::vector<double> > &vex);
  int formVexA(ElectronOrbitals &wf, int a, std::vector<double> &vex_a);
  int formLambdaABk(std::vector<double> &L_abk, int tja, int tjb, int la,
    int lb);

}

#endif
