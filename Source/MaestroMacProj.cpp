
#include <Maestro.H>

using namespace amrex;

// umac enters with face-centered, time-centered Utilde^* and should leave with Utilde
// macphi is the solution to the elliptic solve and 
//   enters as either zero, or the solution to the predictor MAC projection
// macrhs enters as beta0*(S-Sbar)
// beta0 is a 1d cell-centered array    
void
Maestro::MacProj (Vector<std::array< MultiFab, AMREX_SPACEDIM > >& umac,
                  Vector<MultiFab>& macphi,
                  const Vector<MultiFab>& macrhs,
                  const Vector<Real>& beta0,
                  const bool& is_predictor)
{
    // this will hold solver RHS = macrhs - div(beta0*umac)
    Vector<MultiFab> solverrhs(finest_level+1);
    for (int lev=0; lev<=finest_level; ++lev) {
        solverrhs[lev].define(grids[lev], dmap[lev], 1, 0);
    }

    // we also need beta0 at edges
    // allocate AND compute it here
    Vector<Real> beta0_edge( (max_radial_level+1)*(nr_fine+1) );
    cell_to_edge(beta0.dataPtr(),beta0_edge.dataPtr());

    // convert Utilde^* to beta0*Utilde^*
    int mult_or_div = 1;
    MultFacesByBeta0(umac,beta0,beta0_edge,mult_or_div);

    // compute the RHS for the solve, RHS = macrhs - div(beta0*umac)
    ComputeMACSolverRHS(solverrhs,macrhs,umac);

    // create a MultiFab filled with rho and 1 ghost cell.
    // if this is the predictor mac projection, use rho^n
    // if this is the corrector mac projection, use (1/2)(rho^n + rho^{n+1,*})
    Vector<MultiFab> rho(finest_level+1);
    for (int lev=0; lev<=finest_level; ++lev) {
        rho[lev].define(grids[lev], dmap[lev], 1, 1);
    }
    Real rho_time = is_predictor ? t_old : 0.5*(t_old+t_new);
    for (int lev=0; lev<=finest_level; ++lev) {
        FillPatch(lev, rho_time, rho[lev], snew, sold, Rho, 0, 1, Rho, bcs_s);
    }

    // coefficients for solver
    Vector<MultiFab> acoef(finest_level+1);
    Vector<MultiFab> bcoef(finest_level+1);
    Vector<std::array< MultiFab, AMREX_SPACEDIM > > face_bcoef(finest_level+1);
    for (int lev=0; lev<=finest_level; ++lev) {
        acoef[lev].define(grids[lev], dmap[lev], 1, 0);
        bcoef[lev].define(grids[lev], dmap[lev], 1, 1);
        AMREX_D_TERM(face_bcoef[lev][0].define(convert(grids[lev],nodal_flag_x), dmap[lev], 1, 0);,
                     face_bcoef[lev][1].define(convert(grids[lev],nodal_flag_y), dmap[lev], 1, 0);,
                     face_bcoef[lev][2].define(convert(grids[lev],nodal_flag_z), dmap[lev], 1, 0););
    }

    // set cell-centered A coefficient to zero
    for (int lev=0; lev<=finest_level; ++lev) {
        acoef[lev].setVal(0.);
    }

    // average face-centered B coefficients to 1/rho
    AvgFaceBcoeffsInv(face_bcoef,rho);
    // for (int lev=0; lev<=finest_level; ++lev) {
    //     amrex::average_cellcenter_to_face({AMREX_D_DECL(&face_bcoef[lev][0],
    //                                                     &face_bcoef[lev][1],
    //                                                     &face_bcoef[lev][2])},
    //                                         rho[lev], geom[lev]);
    // }

    // DEBUG
    VisMF::Write(face_bcoef[0][0],"a_facebcoefx");
    VisMF::Write(face_bcoef[0][1],"a_facebcoefy");
    ///////////////////////////////////////////////

    // multiply face-centered B coefficients by beta0 so they contain beta0/rho
    mult_or_div = 1;
    MultFacesByBeta0(face_bcoef,beta0,beta0_edge,mult_or_div);

    VisMF::Write(face_bcoef[0][0],"a_facebcoef1");
    VisMF::Write(face_bcoef[0][1],"a_facebcoef2");

    // 
    // Set up implicit solve using MLABecLaplacian class
    //
    LPInfo info;
    MLABecLaplacian mlabec(geom, grids, dmap, info);

    // order of stencil
    int linop_maxorder = 2;
    mlabec.setMaxOrder(linop_maxorder);

    // set boundaries for mlabec using velocity bc's
    SetMacSolverBCs(mlabec);

    for (int lev = 0; lev <= finest_level; ++lev) {
	mlabec.setLevelBC(lev, &macphi[lev]);
    }

    mlabec.setScalars(0.0, 1.0);

    for (int lev = 0; lev <= finest_level; ++lev) {
	mlabec.setACoeffs(lev, acoef[lev]);
	mlabec.setBCoeffs(lev, amrex::GetArrOfConstPtrs(face_bcoef[lev]));
    }

    // solve -div B grad phi = RHS
    //
    // create grad_phi
    Vector< std::array<MultiFab,AMREX_SPACEDIM> > mac_gradphi(finest_level+1);
    for (int lev = 0; lev <= finest_level; ++lev) 
    {
	AMREX_D_TERM(mac_gradphi[lev][0].define(convert(grids[lev],nodal_flag_x), dmap[lev], 1, 0);,
		     mac_gradphi[lev][1].define(convert(grids[lev],nodal_flag_y), dmap[lev], 1, 0);,
		     mac_gradphi[lev][2].define(convert(grids[lev],nodal_flag_z), dmap[lev], 1, 0););
    }

    // build an MLMG solver
    MLMG mac_mlmg(mlabec);

    // set solver parameters
    int max_fmg_iter = 0;
    mac_mlmg.setMaxFmgIter(max_fmg_iter);
    mac_mlmg.setVerbose(10);

    // tolerance parameters taken from original MAESTRO fortran code
    const Real eps_mac = 1.e-10;
    const Real eps_mac_max = 1.e-8;
    const Real mac_level_factor = 10.0;
    const Real mac_tol_abs = -1.e-10;
    const Real mac_tol_rel = std::min(eps_mac*pow(mac_level_factor,finest_level), eps_mac_max);

    mac_mlmg.solve(GetVecOfPtrs(macphi), GetVecOfConstPtrs(solverrhs), mac_tol_rel, mac_tol_abs);

    // DEBUG
    // Write out macphi
    VisMF::Write(macphi[0],"a_macphi");
    Abort();
    ////////////////////////////////////////////////////
    
    // update velocity, beta0 * Utilde = beta0 * Utilde^* - B grad phi
    //
    // Note: must solve linear system first before computing grad phi
    auto& gradphi_flux = mac_gradphi;
    for (int lev = 0; lev <= finest_level; ++lev) {
	mac_mlmg.getFluxes({amrex::GetArrOfPtrs(gradphi_flux[lev])});
    }

    for (int lev = 0; lev <= finest_level; ++lev) 
    {
	for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) {
	    MultiFab::Multiply(gradphi_flux[lev][idim], face_bcoef[lev][idim], 0, 0, 1, 0);
	    MultiFab::Subtract(umac[lev][idim], gradphi_flux[lev][idim], 0, 0, 1, 0);
	}
    }
    
    // convert beta0*Utilde to Utilde
    mult_or_div = 0;
    MultFacesByBeta0(umac,beta0,beta0_edge,mult_or_div);

    // fill periodic ghost cells
    for (int lev = 0; lev <= finest_level; ++lev) {
        for (int d = 0; d < AMREX_SPACEDIM; ++d) {
            umac[lev][d].FillBoundary(geom[lev].periodicity());
        }
    }

    // fill ghost cells behind physical boundaries
    FillUmacGhost(umac);

}

// multiply (or divide) face-data by beta0
void Maestro::MultFacesByBeta0 (Vector<std::array< MultiFab, AMREX_SPACEDIM > >& edge,
                                const Vector<Real>& beta0,
                                const Vector<Real>& beta0_edge,
                                const int& mult_or_div)
{

    // write an MFIter loop to convert edge -> beta0*edge OR beta0*edge -> edge
    for (int lev = 0; lev <= finest_level; ++lev) 
    {
	// get references to the MultiFabs at level lev
	MultiFab& xedge_mf = edge[lev][0];
#if (AMREX_SPACEDIM >= 2)
	MultiFab& yedge_mf = edge[lev][1];
#if (AMREX_SPACEDIM == 3)
	MultiFab& zedge_mf = edge[lev][2];
#endif
#endif

	// Must get cell-centered MultiFab boxes for MIter
	MultiFab& sold_mf = sold[lev];

	// loop over boxes
	for ( MFIter mfi(sold_mf); mfi.isValid(); ++mfi) {

	    // Get the index space of valid region
	    const Box& validBox = mfi.validbox();

	    // call fortran subroutine
	    mult_beta0(lev,ARLIM_3D(validBox.loVect()),ARLIM_3D(validBox.hiVect()), 
		       BL_TO_FORTRAN_3D(xedge_mf[mfi]), 
#if (AMREX_SPACEDIM >= 2)
		       BL_TO_FORTRAN_3D(yedge_mf[mfi]),
#if (AMREX_SPACEDIM == 3)
		       BL_TO_FORTRAN_3D(zedge_mf[mfi]),
#endif
		       beta0_edge.dataPtr(),
#endif
		       beta0.dataPtr(), mult_or_div);

	}
    }
    

}

// compute the RHS for the solve, RHS = macrhs - div(beta0*umac)
void Maestro::ComputeMACSolverRHS (Vector<MultiFab>& solverrhs,
                                   const Vector<MultiFab>& macrhs,
                                   const Vector<std::array< MultiFab, AMREX_SPACEDIM > >& umac)
{

    // Note that umac = beta0*mac
    for (int lev = 0; lev <= finest_level; ++lev) 
    {
	// get references to the MultiFabs at level lev
	MultiFab& solverrhs_mf = solverrhs[lev];
	const MultiFab& macrhs_mf = macrhs[lev];
	const MultiFab& uedge_mf = umac[lev][0];
#if (AMREX_SPACEDIM >= 2)
	const MultiFab& vedge_mf = umac[lev][1];
#if (AMREX_SPACEDIM == 3)
	const MultiFab& wedge_mf = umac[lev][2];
#endif
#endif

	// Must use cell-centered MultiFab boxes for MIter
	MultiFab& macphi_mf = sold[lev];

	// loop over boxes
	for ( MFIter mfi(macphi_mf); mfi.isValid(); ++mfi) {

	    // Get the index space of valid region
	    const Box& validBox = mfi.validbox();
	    const Real* dx = geom[lev].CellSize();

	    // call fortran subroutine
	    mac_solver_rhs(lev,ARLIM_3D(validBox.loVect()),ARLIM_3D(validBox.hiVect()), 
			   BL_TO_FORTRAN_3D(solverrhs_mf[mfi]), 
			   BL_TO_FORTRAN_3D(macrhs_mf[mfi]), 
			   BL_TO_FORTRAN_3D(uedge_mf[mfi]), 
#if (AMREX_SPACEDIM >= 2)
			   BL_TO_FORTRAN_3D(vedge_mf[mfi]),
#if (AMREX_SPACEDIM == 3)
			   BL_TO_FORTRAN_3D(wedge_mf[mfi]),
#endif
#endif
			   dx);

	}
    }
    


}

// Average bcoefs at faces using inverse of rho
void Maestro::AvgFaceBcoeffsInv(Vector<std::array< MultiFab, AMREX_SPACEDIM > >& facebcoef,
				 const Vector<MultiFab>& rhocc)
{

    // write an MFIter loop 
    for (int lev = 0; lev <= finest_level; ++lev) 
    {
	// get references to the MultiFabs at level lev
	MultiFab& xbcoef_mf = facebcoef[lev][0];
#if (AMREX_SPACEDIM >= 2)
	MultiFab& ybcoef_mf = facebcoef[lev][1];
#if (AMREX_SPACEDIM == 3)
	MultiFab& zbcoef_mf = facebcoef[lev][2];
#endif
#endif

	// Must get cell-centered MultiFab boxes for MIter
	const MultiFab& rhocc_mf = rhocc[lev];

	// loop over boxes
	for ( MFIter mfi(rhocc_mf); mfi.isValid(); ++mfi) {

	    // Get the index space of valid region
	    const Box& validBox = mfi.validbox();

	    // call fortran subroutine
	    mac_bcoef_face(lev,ARLIM_3D(validBox.loVect()),ARLIM_3D(validBox.hiVect()), 
			   BL_TO_FORTRAN_3D(xbcoef_mf[mfi]), 
#if (AMREX_SPACEDIM >= 2)
			   BL_TO_FORTRAN_3D(ybcoef_mf[mfi]),
#if (AMREX_SPACEDIM == 3)
			   BL_TO_FORTRAN_3D(zbcoef_mf[mfi]),
#endif
#endif
			   BL_TO_FORTRAN_3D(rhocc_mf[mfi]));

	}
    }
    

}

// Set boundaries for MAC velocities
void Maestro::SetMacSolverBCs(MLABecLaplacian& mlabec) 
{

    // build array of boundary conditions needed by MLABecLaplacian
    std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_lobc;
    std::array<LinOpBCType,AMREX_SPACEDIM> mlmg_hibc;

    for (int idim = 0; idim < AMREX_SPACEDIM; ++idim) 
    {
	// lo-side BCs
        if (bcs_u[0].lo(idim) == BCType::int_dir) {
            mlmg_lobc[idim] = LinOpBCType::Periodic;
        } 
	else if (bcs_u[0].lo(idim) == BCType::foextrap ||
		 bcs_u[0].lo(idim) == BCType::hoextrap) {
	    mlmg_lobc[idim] = LinOpBCType::Neumann;
	} 
	else if (bcs_u[0].lo(idim) == BCType::ext_dir) {
	    mlmg_lobc[idim] = LinOpBCType::Dirichlet;
	}
	else {
	    amrex::Abort("Invalid mlmg_lobc");
        }

	// hi-side BCs
        if (bcs_u[0].hi(idim) == BCType::int_dir) {
            mlmg_hibc[idim] = LinOpBCType::Periodic;
        } 
	else if (bcs_u[0].hi(idim) == BCType::foextrap ||
		 bcs_u[0].hi(idim) == BCType::hoextrap) {
	    mlmg_hibc[idim] = LinOpBCType::Neumann;
	} 
	else if (bcs_u[0].hi(idim) == BCType::ext_dir) {
	    mlmg_hibc[idim] = LinOpBCType::Dirichlet;
        }
	else {
	    amrex::Abort("Invalid mlmg_hibc");
        }
    }

    mlabec.setDomainBC(mlmg_lobc,mlmg_hibc);
}
