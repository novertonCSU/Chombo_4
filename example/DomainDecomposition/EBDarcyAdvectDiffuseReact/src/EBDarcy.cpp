
#include "EBDarcy.H"
#include "EBParabolicIntegrators.H"
#include "Chombo_ParmParse.H"
#include "Chombo_NamespaceHeader.H"
#include "DebugFunctions.H"
using Proto::Var;
/*******/
EBDarcy::
EBDarcy(shared_ptr<EBEncyclopedia<2, Real> >   & a_brit,
      shared_ptr<GeometryService<2> >        & a_geoserv,
      const DisjointBoxLayout                & a_grids,
      const Box                              & a_domain,
      const Real                             & a_dx,
      const Real                             & a_viscosity,
      const IntVect                          & a_nghost,
      ParabolicSolverType                      a_solver,
      EBIBC                                    a_ibc)
{
  CH_TIME("EBDarcy::define");
  m_brit                = a_brit;
  m_geoserv             = a_geoserv;
  m_grids               = a_grids;
  m_domain              = a_domain;
  m_dx                  = a_dx;
  m_nghost              = a_nghost;
  m_viscosity           = a_viscosity;

  m_graphs = m_geoserv->getGraphs(m_domain);
  m_exchangeCopier.exchangeDefine(m_grids, m_nghost*IntVect::Unit);
  m_copyCopier.define(m_grids, m_grids, m_nghost*IntVect::Unit);
  
  m_velo     = shared_ptr<EBLevelBoxData<CELL, DIM> >
    (new EBLevelBoxData<CELL, DIM>(m_grids, m_nghost, m_graphs));
  m_sour     = shared_ptr<EBLevelBoxData<CELL, DIM> >
    (new EBLevelBoxData<CELL, DIM>(m_grids, m_nghost, m_graphs));
  m_divuu    = shared_ptr<EBLevelBoxData<CELL, DIM> >
    (new EBLevelBoxData<CELL, DIM>(m_grids, m_nghost, m_graphs));
  m_gphi     = shared_ptr<EBLevelBoxData<CELL, DIM> >
    (new EBLevelBoxData<CELL, DIM>(m_grids, m_nghost, m_graphs));
  m_scal     = shared_ptr<EBLevelBoxData<CELL, 1  > >
    (new EBLevelBoxData<CELL, 1  >(m_grids, m_nghost, m_graphs));

  string stenname = StencilNames::Poisson2;
  string bcname;
  if(a_viscosity == 0)
  {
    bcname = StencilNames::Neumann;
    m_eulerCalc = true;
  }
  else
  {
    bcname = StencilNames::Dirichlet;
    m_eulerCalc = false;
  }

  auto cell_dict = m_brit->m_cellToCell;
  if(!m_eulerCalc)
  {
    Real alpha = 1; Real beta = 1; //these get reset before solve
    string helmnames[2*DIM];
    a_ibc.helmholtzStencilStrings(helmnames);
    m_helmholtz = shared_ptr<EBMultigrid> 
      (new EBMultigrid(cell_dict, m_geoserv, alpha, beta, m_dx, m_grids,  
                       stenname, helmnames, bcname, m_domain, m_nghost));

    if(a_solver == BackwardEuler)
    {
      m_heatSolver = shared_ptr<BaseEBParabolic>
        (new EBBackwardEuler(m_helmholtz, m_geoserv, m_grids, m_domain, m_nghost));
    }
    else if (a_solver == CrankNicolson)
    {
      m_heatSolver = shared_ptr<BaseEBParabolic>
        (new EBCrankNicolson(m_helmholtz, m_geoserv, m_grids, m_domain, m_nghost));
    }
    else if (a_solver == TGA)
    {
      m_heatSolver = shared_ptr<BaseEBParabolic>
        (new EBTGA(m_helmholtz, m_geoserv, m_grids, m_domain, m_nghost));
    }
    else
    {
      MayDay::Error("unaccounted-for solver type");
    }
  }
  m_advectOp = shared_ptr<EBAdvection>
    (new EBAdvection(m_brit, m_geoserv, m_velo, m_grids, m_domain, m_dx, a_ibc, m_nghost));
  
  m_ccProj  = shared_ptr<EBCCProjector>
    (new EBCCProjector(m_brit, m_geoserv, m_grids, m_domain, m_dx, m_nghost, a_ibc));
  m_macProj = m_ccProj->m_macprojector;

  m_bcgAdvect = shared_ptr<BCGVelAdvect>
    (new BCGVelAdvect(m_macProj, m_helmholtz, m_brit, m_geoserv, m_velo,
                      m_grids, m_domain, m_dx, m_viscosity, a_ibc, m_nghost, m_eulerCalc));


}
/*******/ 
void 
EBDarcy::
run(unsigned int a_max_step,
    Real         a_max_time,
    Real         a_cfl,
    Real         a_fixedDt,
    Real         a_tol,
    unsigned int a_numIterPres,
    unsigned int a_maxIter,
    int          a_outputInterval,
    Real         a_coveredVal)
{
  CH_TIME("EBDarcy::run");
  bool usingFixedDt = (a_fixedDt > 0);
  bool doFileOutput = (a_outputInterval > 0);
  Real dt;
  //project the resulting field
  pout() << "projecting initial velocity"<< endl;
  auto & velo =  (*m_velo );
  auto & gphi =  (*m_gphi );

  m_ccProj->project(velo, gphi, a_tol, a_maxIter);
  velo.exchange(m_exchangeCopier);

  //get the time step
  if(usingFixedDt)
  {
    dt = a_fixedDt;
  }
  else
  {
    dt = computeDt(a_cfl);
  }

  pout() << "getting initial pressure using fixed point iteration " << endl;
  initializeVelocity(dt, a_tol, a_numIterPres, a_maxIter);

  if(doFileOutput)
  {
    outputToFile(0, a_coveredVal, dt, 0.0);
  }

  m_time = 0;
  m_step = 0;
  while((m_step < a_max_step) && (m_time < a_max_time))
  {
    pout() << "step = " << m_step << ", time = " << m_time << " dt = " << dt << endl;

    pout() << "advancing velocity and pressure fields " << endl;
    advanceVelocityAndPressure(dt, a_tol, a_maxIter);

    pout() << "advancing passive scalar" << endl;
    advanceScalar(dt);
    m_step++;
    m_time += dt;
    if(!usingFixedDt)
    {
      dt = computeDt(a_cfl);
    }
    if((doFileOutput) && (m_step % a_outputInterval == 0))
    {
      outputToFile(m_step, a_coveredVal, dt, m_time);
    }
  }
}
/*******/ 
Real
EBDarcy::
computeDt(Real a_cfl) const
{
  CH_TIME("EBDarcy::computeDt");
  Real dtval;
  Real dtCFL = 999999999.;
    Real maxvel = 0;
    for(int idir = 0; idir < DIM; idir++)
    {
      maxvel = std::max(maxvel, m_velo->maxNorm(idir));
    }
    dtCFL = a_cfl*m_dx/maxvel;
    if(maxvel > 1.0e-16)
    {
      dtval = dtCFL;
      pout() << "maxvel = " << maxvel << ", dx = " << m_dx << ", dt = " << dtval << endl;
    }    
    else
    {
      pout() << "velocity seems to be zero--setting dt to dx" << endl;
      dtval = m_dx;
    }

  ParmParse pp;
  Real dtStokes = m_dx*m_dx/m_viscosity;
  bool use_stokes_dt = false;
  pp.query("use_stokes_dt", use_stokes_dt);
  if(use_stokes_dt && dtCFL > dtStokes)
  {
    dtval = dtStokes;
    pout() << "Using Stokes dt = " << dtval << endl;
  }
  return dtval;
}
/*******/ 
PROTO_KERNEL_START 
void EulerAdvanceF(Var<Real, DIM>    a_velo,
                   Var<Real, DIM>    a_divuu,
                   Var<Real, DIM>    a_gradp,
                   Real              a_dt)
{
  for(unsigned int idir = 0; idir < DIM; idir++)
  {
    a_velo(idir) -= a_dt*(a_divuu(idir) + a_gradp(idir));
  }
}
PROTO_KERNEL_END(EulerAdvanceF, EulerAdvance)

/*******/ 
void
EBDarcy::
advanceVelocityEuler(Real a_dt)
{
  CH_TIME("EBDarcy::advanceVelocityEuler");
  auto & velo =  (*m_velo );
  auto & udel =  (*m_divuu);
  auto & gphi =  (*m_gphi );

  DataIterator dit = m_grids.dataIterator();
  for(unsigned int ibox = 0; ibox < dit.size(); ibox++)
  {
    unsigned long long int numflopspt = 3*DIM;
    Bx grbx = ProtoCh::getProtoBox(m_grids[dit[ibox]]);
    auto & velofab =  velo[dit[ibox]];
    auto & udelfab =  udel[dit[ibox]];
    auto & gphifab =  gphi[dit[ibox]];  

    ebforallInPlace(numflopspt, "EulerAdvance", EulerAdvance, grbx,  
                    velofab, udelfab, gphifab, a_dt);
  }
}
/*******/ 
PROTO_KERNEL_START 
void ParabolicRHSF(Var<Real, 1>    a_rhs,
                   Var<Real, 1>    a_divuu,
                   Var<Real, 1>    a_gradp)
{
  a_rhs(0) = -a_divuu(0) - a_gradp(0);
}
PROTO_KERNEL_END(ParabolicRHSF, ParabolicRHS)

/*******/ 
void
EBDarcy::
advanceVelocityNavierStokes(Real a_dt,                          
                            Real a_tol,                         
                            unsigned int a_maxIter)
{
  CH_TIME("EBDarcy::advanceVelocityNavierStokes");
  auto & sour  = *m_sour ;
  auto & velo  = *m_velo ;
  auto & divuu = *m_divuu;
  auto & gphi  = *m_gphi ;
  DataIterator dit = m_grids.dataIterator();
  for(unsigned int idir = 0; idir < DIM; idir++)
  {
    EBLevelBoxData<CELL, 1> scalRHS, scalVelo, scalUdelu, scalGphi;
    scalRHS.define<DIM>(  sour ,idir, m_graphs);
    scalVelo.define<DIM>( velo ,idir, m_graphs);
    scalUdelu.define<DIM>(divuu,idir, m_graphs);
    scalGphi.define<DIM>( gphi ,idir, m_graphs);

    //set source of parabolic advance to -(gphi + udelu)
    for(unsigned int ibox = 0; ibox < dit.size(); ibox++)
    {
      unsigned long long int numflopspt = 2;
      Bx grbx = ProtoCh::getProtoBox(m_grids[dit[ibox]]);
      auto & sour =    scalRHS[dit[ibox]];
      auto & udel =  scalUdelu[dit[ibox]];
      auto & gphi =  scalGphi[ dit[ibox]];  

      ebforallInPlace(numflopspt, "ParabolicRHS", ParabolicRHS, grbx, sour, udel, gphi);
    }
    pout() << "calling heat solver for variable " << idir << endl;
    //advance the parabolic equation
    m_heatSolver->advanceOneStep(scalVelo, scalRHS, m_viscosity, a_dt, a_tol, a_maxIter);
  }
}
/*******/ 
PROTO_KERNEL_START 
void AddDtGphiToVeloF(Var<Real, DIM>    a_velo,
                      Var<Real, DIM>    a_gradp,
                      Real              a_dt)
{
  for(int idir = 0; idir < DIM; idir++)
  {
    a_velo(idir) += a_dt*(a_gradp(idir));
  }
}
PROTO_KERNEL_END(AddDtGphiToVeloF, AddDtGphiToVelo)
/*******/ 
PROTO_KERNEL_START 
void DivideOutDtF(Var<Real, DIM>    a_gradp,
                  Real              a_dt)
{
  for(int idir = 0; idir < DIM; idir++)
  {
    a_gradp(idir) /= a_dt;
  }
}
PROTO_KERNEL_END(DivideOutDtF, DivideOutDt)
/*******/ 
void
EBDarcy::
projectVelocityAndCorrectPressure(Real a_dt,
                                  Real a_tol,                         
                                  unsigned int a_maxIter)
{
  CH_TIME("EBDarcy::projectVelocityAndCorrectPressure");
  auto & velo =  (*m_velo);
  auto & gphi =  (*m_gphi);
  //make w = v + dt*gphi
  DataIterator dit = m_grids.dataIterator();
  for(unsigned int ibox = 0; ibox < dit.size(); ibox++)
  {
    unsigned long long int numflopspt = 2*DIM;
    Bx grbx = ProtoCh::getProtoBox(m_grids[dit[ibox]]);
    auto & velofab =  velo[dit[ibox]];
    auto & gphifab =  gphi[dit[ibox]];  

    ebforallInPlace(numflopspt, "AddDtGphiToVelo", AddDtGphiToVelo, grbx, velofab, gphifab, a_dt);
  }

  //project the resulting field
  pout() << "cc projecting vel + gphi*dt" << endl;
  m_ccProj->project(velo, gphi, a_tol, a_maxIter);

  //the resulting pressure  is = dt * gphi so we need to divide out the dt
  for(unsigned int ibox = 0; ibox < dit.size(); ibox++)
  {
    unsigned long long int numflopspt = 2*DIM;
    Bx grbx = ProtoCh::getProtoBox(m_grids[dit[ibox]]);
    auto & gphifab =  gphi[dit[ibox]];  

    ebforallInPlace(numflopspt, "DivideOutDt", DivideOutDt, grbx, gphifab, a_dt);
  }
}
/*******/ 
void
EBDarcy::
advanceVelocityAndPressure(Real a_dt,                          
                           Real a_tol,                         
                           unsigned int a_maxIter)
{
  CH_TIME("EBDarcy::advanceVelocityAndPressure");
  //get udelu
  getAdvectiveDerivative(a_dt, a_tol, a_maxIter);

  if(m_eulerCalc)
  {
    advanceVelocityEuler(a_dt);
  }
  else
  {
    advanceVelocityNavierStokes(a_dt, a_tol, a_maxIter);
  }
  projectVelocityAndCorrectPressure(a_dt, a_tol, a_maxIter);
}
/*******/ 
void
EBDarcy::
advanceScalar(Real a_dt)
              
{
  static int ideb = 0;
  auto& scal = *m_scal;
  CH_TIME("EBDarcy::advanceScalar");
  scal.exchange(m_exchangeCopier);
  m_advectOp->advance(scal, a_dt);
  scal.exchange(m_exchangeCopier);
  ideb++;
}
/*******/ 
void
EBDarcy::
outputToFile(unsigned int a_step, Real a_coveredval, Real a_dt, Real a_time) const
{
  CH_TIME("EBDarcy::outputToFile");
  const EBLevelBoxData<CELL, 1> & kappa = m_advectOp->m_kappa;
  string filescal = string("scal.") + std::to_string(a_step) + string(".hdf5");
  string filevelo = string("velo.") + std::to_string(a_step) + string(".hdf5");
  string filegphi = string("gphi.") + std::to_string(a_step) + string(".hdf5");
  writeEBLevelHDF5<1>(  filescal,  *m_scal, kappa, m_domain, m_graphs, a_coveredval, m_dx, a_dt, a_time);
  writeEBLevelHDF5<DIM>(filevelo,  *m_velo, kappa, m_domain, m_graphs, a_coveredval, m_dx, a_dt, a_time);
  writeEBLevelHDF5<DIM>(filegphi,  *m_gphi, kappa, m_domain, m_graphs, a_coveredval, m_dx, a_dt, a_time);
}
/*******/ 
#include "Chombo_NamespaceFooter.H"
