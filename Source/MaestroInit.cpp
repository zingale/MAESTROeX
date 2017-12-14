
#include <Maestro.H>
#include <AMReX_VisMF.H>
using namespace amrex;


// initialize AMR data
// perform initial projection
// perform divu iters
// perform initial (pressure) iterations
void
Maestro::Init ()
{
    Print() << "Calling Init()" << endl;

    // fill in multifab and base state data
    InitData();

    VisMF::Write(snew[0],"a_snew");

    if (spherical == 1) {
        // FIXME
        // MakeNormal();
    }

    // make gravity
    make_grav_cell(grav_cell.dataPtr(),
                   rho0_new.dataPtr(),
                   r_cc_loc.dataPtr(),
                   r_edge_loc.dataPtr());

    // compute gamma1bar
    MakeGamma1bar(snew,gamma1bar_new,p0_new);

    // compute beta0
    make_beta0(beta0_new.dataPtr(),
               rho0_new.dataPtr(),
               p0_new.dataPtr(),
               gamma1bar_new.dataPtr(),
               grav_cell.dataPtr());

    // initial projection
    if (do_initial_projection) {
        Print() << "Doing initial projection" << endl;
        InitProj();
    }

    // compute initial time step
    FirstDt();

    // divu iters
    for (int i=1; i<=init_divu_iter; ++i) {
        Print() << "Doing initial divu iteration #" << i << endl;
        DivuIter();
    }

    // initial (pressure) iters
    for (int i=1; i<= init_iter; ++i) {
        Print() << "Doing initial pressure iteration #" << i << endl;
        InitIter();
    }
}

// fill in multifab and base state data
void
Maestro::InitData ()
{
    Print() << "Calling InitData()" << endl;

    // read in model file and fill in s0_init and p0_init for all levels
    init_base_state(s0_init.dataPtr(),p0_init.dataPtr(),rho0_new.dataPtr(),
                    rhoh0_new.dataPtr(),p0_new.dataPtr(),tempbar.dataPtr(),max_level+1);

    // calls AmrCore::InitFromScratch(), which calls a MakeNewGrids() function 
    // that repeatedly calls Maestro::MakeNewLevelFromScratch() to build and initialize
    InitFromScratch(t_new);

    // set finest_radial_level in fortran
    // compute numdisjointchunks, r_start_coord, r_end_coord
    init_multilevel(finest_level);

    // synchronize levels
    AverageDown(snew,0,Nscal);
    AverageDown(unew,0,AMREX_SPACEDIM);

    // free memory in s0_init and p0_init by swapping it
    // with an empty vector that will go out of scope
    Vector<Real> s0_swap, p0_swap;
    std::swap(s0_swap,s0_init);
    std::swap(p0_swap,p0_init);

    if (fix_base_state) {
        // compute cutoff coordinates
        compute_cutoff_coords(rho0_new.dataPtr());
        make_grav_cell(grav_cell.dataPtr(),
                       rho0_new.dataPtr(),
                       r_cc_loc.dataPtr(),
                       r_edge_loc.dataPtr());
    }
    else {
        if (do_smallscale) {
            // first compute cutoff coordinates using initial density profile
            compute_cutoff_coords(rho0_new.dataPtr());
            // set rho0_new = rhoh0_new = 0.
            std::fill(rho0_new.begin(),  rho0_new.end(),  0.);
            std::fill(rhoh0_new.begin(), rhoh0_new.end(), 0.);
        }
        else {
            // set rho0 to be the average
            Average(snew,rho0_new,Rho);
            compute_cutoff_coords(rho0_new.dataPtr());

            // compute gravity
            make_grav_cell(grav_cell.dataPtr(),
                           rho0_new.dataPtr(),
                           r_cc_loc.dataPtr(),
                           r_edge_loc.dataPtr());

            // compute p0 with HSE
            enforce_HSE(rho0_new.dataPtr(),
                        p0_new.dataPtr(),
                        grav_cell.dataPtr(),
                        r_edge_loc.dataPtr());

            // call eos with r,p as input to recompute T,h
            TfromRhoP(snew,p0_new,1);

            // set rhoh0 to be the average
            Average(snew,rhoh0_new,RhoH);
        }
    }

    if (plot_int > 0) {
        Print() << "\nWriting plotfile 0 after initialization" << endl;
        WritePlotFile(0);
    }
}

// During initialization of a simulation, Maestro::InitData() calls 
// AmrCore::InitFromScratch(), which calls 
// a MakeNewGrids() function that repeatedly calls this function to build 
// and initialize finer levels.  This function creates a new fine
// level that did not exist before by interpolating from the coarser level
// overrides the pure virtual function in AmrCore
void Maestro::MakeNewLevelFromScratch (int lev, Real time, const BoxArray& ba,
				       const DistributionMapping& dm)
{
    sold    [lev].define(ba, dm,          Nscal, 0);
    snew    [lev].define(ba, dm,          Nscal, 0);
    uold    [lev].define(ba, dm, AMREX_SPACEDIM, 0);
    unew    [lev].define(ba, dm, AMREX_SPACEDIM, 0);
    S_cc_old[lev].define(ba, dm,              1, 0);
    S_cc_new[lev].define(ba, dm,              1, 0);
    gpi     [lev].define(ba, dm, AMREX_SPACEDIM, 0);
    dSdt    [lev].define(ba, dm,              1, 0);
    if (spherical == 1) {
        normal[lev].define(ba, dm, 1, 1);
    }

    if (lev > 0 && do_reflux) {
        flux_reg_s[lev].reset(new FluxRegister(ba, dm, refRatio(lev-1), lev, Nscal));
        flux_reg_u[lev].reset(new FluxRegister(ba, dm, refRatio(lev-1), lev, AMREX_SPACEDIM));
    }

    const Real* dx = geom[lev].CellSize();
    Real cur_time = t_new;

    MultiFab& scal = snew[lev];
    MultiFab& vel = unew[lev];

    // Loop over boxes (make sure mfi takes a cell-centered multifab as an argument)
    for (MFIter mfi(scal); mfi.isValid(); ++mfi)
    {
        const Box& box = mfi.validbox();
        const int* lo  = box.loVect();
        const int* hi  = box.hiVect();

        initdata(lev, cur_time, ARLIM_3D(lo), ARLIM_3D(hi),
                 BL_TO_FORTRAN_FAB(scal[mfi]), 
                 BL_TO_FORTRAN_FAB(vel[mfi]), 
                 s0_init.dataPtr(), p0_init.dataPtr(),
                 ZFILL(dx));
    }
}


void Maestro::InitProj ()
{

    Vector<MultiFab>         S_cc(finest_level+1);
    Vector<MultiFab> rho_omegadot(finest_level+1);
    Vector<MultiFab>      thermal(finest_level+1);
    Vector<MultiFab>     rho_Hnuc(finest_level+1);
    Vector<MultiFab>     rho_Hext(finest_level+1);
    Vector<MultiFab>      rhohalf(finest_level+1);
    // nodal
    Vector<MultiFab>     nodalrhs(finest_level+1);

    Vector<Real> Sbar( (max_radial_level+1)*nr_fine );

    for (int lev=0; lev<=finest_level; ++lev) {
        S_cc        [lev].define(grids[lev], dmap[lev],       1, 0);
        rho_omegadot[lev].define(grids[lev], dmap[lev], NumSpec, 0);
        thermal     [lev].define(grids[lev], dmap[lev],       1, 0);
        rho_Hnuc    [lev].define(grids[lev], dmap[lev],       1, 0);
        rho_Hext    [lev].define(grids[lev], dmap[lev],       1, 0);
        rhohalf     [lev].define(grids[lev], dmap[lev],       1, 1);
        // nodal
        nodalrhs[lev].define    (convert(grids[lev],nodal_flag), dmap[lev], 1, 0);

        // we don't have a legit timestep yet, so we set rho_omegadot,
        // rho_Hnuc, and rho_Hext to 0 
        rho_omegadot[lev].setVal(0.);
        rho_Hnuc[lev].setVal(0.);
        rho_Hext[lev].setVal(0.);

        // initial projection does not use density weighting
        rhohalf[lev].setVal(1.);
    }

    // compute thermal diffusion
    if (use_thermal_diffusion) {
        Abort("InitProj: use_thermal_diffusion not implemented");
    }
    else {
        for (int lev=0; lev<=finest_level; ++lev) {
            thermal[lev].setVal(0.);
        }
    }

    // compute S at cell-centers
    Make_S_cc(S_cc,snew,rho_omegadot,rho_Hnuc,rho_Hext,thermal);

    // average S into Sbar
    Average(S_cc,Sbar,0);

    // make the nodal rhs for projection
    Make_NodalRHS(S_cc,nodalrhs,Sbar,beta0_new);

    // perform a nodal projection
    NodalProj(initial_projection_comp,nodalrhs,rhohalf);

}


void Maestro::DivuIter ()
{

    Vector<MultiFab>        stemp(finest_level+1);
    Vector<MultiFab>     rho_Hext(finest_level+1);
    Vector<MultiFab> rho_omegadot(finest_level+1);
    Vector<MultiFab>     rho_Hnuc(finest_level+1);
    Vector<MultiFab>      thermal(finest_level+1);

    for (int lev=0; lev<=finest_level; ++lev) {
        stemp       [lev].define(grids[lev], dmap[lev],   Nscal, 0);
        rho_Hext    [lev].define(grids[lev], dmap[lev],       1, 0);
        rho_omegadot[lev].define(grids[lev], dmap[lev], NumSpec, 0);
        rho_Hnuc    [lev].define(grids[lev], dmap[lev],       1, 0);
        thermal     [lev].define(grids[lev], dmap[lev],       1, 0);
    }

    React(snew,stemp,rho_Hext,rho_omegadot,rho_Hnuc,p0_new,0.5*dt);

    VisMF::Write(rho_omegadot[0],"a_rod");

    if (use_thermal_diffusion) {
        Abort("DivuIter: use_thermal_diffusion not implemented");
    }
    else {
        for (int lev=0; lev<=finest_level; ++lev) {
            thermal[lev].setVal(0.);
        }        
    }


}


void Maestro::InitIter ()
{
    dtold = dt;
}
