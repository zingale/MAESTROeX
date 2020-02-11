
#include <Maestro.H>
#include <Maestro_F.H>

using namespace amrex;

void
Maestro::MakeVelForce (Vector<MultiFab>& vel_force_cart,
                       const Vector<std::array< MultiFab, AMREX_SPACEDIM > >& uedge_in,
                       const Vector<MultiFab>& rho,
                       const RealVector& rho0,
                       const RealVector& grav_cell,
                       const Vector<MultiFab>& w0_force_cart,
                       int do_add_utilde_force)
{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::MakeVelForce()",MakeVelForce);

    Vector<MultiFab> gradw0_cart(finest_level+1);
    Vector<MultiFab> grav_cart(finest_level+1);
    Vector<MultiFab> rho0_cart(finest_level+1);

	// constants in Fortran
	Real base_cutoff_density=0.0; 
	get_base_cutoff_density(&base_cutoff_density);
	Real buoyancy_cutoff_factor=0.0;
	get_buoyancy_cutoff_factor(&buoyancy_cutoff_factor);

    for (int lev=0; lev<=finest_level; ++lev) {

        gradw0_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        gradw0_cart[lev].setVal(0.);

        grav_cart[lev].define(grids[lev], dmap[lev], AMREX_SPACEDIM, 1);
        grav_cart[lev].setVal(0.);

        rho0_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        rho0_cart[lev].setVal(0.);

    }

    RealVector gradw0( (max_radial_level+1)*nr_fine , 0.0);
    gradw0.shrink_to_fit();

    Real* AMREX_RESTRICT p_gradw0 = gradw0.dataPtr();
    Real* AMREX_RESTRICT p_w0 = w0.dataPtr();
    const Real dr0 = dr_fine;

    if ( !(use_exact_base_state || average_base_state) ) {
        AMREX_PARALLEL_FOR_1D (gradw0.size(), i,
        {       
            p_gradw0[i] = (p_w0[i+1] - p_w0[i])/dr0;
        });
    }

    Put1dArrayOnCart(gradw0,gradw0_cart,0,0,bcs_u,0,1);
    Put1dArrayOnCart(rho0,rho0_cart,0,0,bcs_s,Rho);
    Put1dArrayOnCart(grav_cell,grav_cart,0,1,bcs_f,0);

    // Reset vel_force
    for (int lev=0; lev<=finest_level; ++lev) {
        vel_force_cart[lev].setVal(0.);
    }

    for (int lev=0; lev<=finest_level; ++lev) {

        // get references to the MultiFabs at level lev
        MultiFab& vel_force_mf = vel_force_cart[lev];
        
        // Get grid spacing
        const GpuArray<Real, AMREX_SPACEDIM> dx = geom[lev].CellSizeArray();

        // Loop over boxes (make sure mfi takes a cell-centered multifab as an argument)
#ifdef _OPENMP
#pragma omp parallel
#endif
        for ( MFIter mfi(vel_force_mf, TilingIfNotGPU()); mfi.isValid(); ++mfi ) {

            // Get Array4 inputs
            const Array4<const Real> gpi_arr = gpi[lev].array(mfi);
            const Array4<const Real> rho_arr = rho[lev].array(mfi);
            const Array4<const Real> uedge = uedge_in[lev][0].array(mfi);
            const Array4<const Real> vedge = uedge_in[lev][1].array(mfi);
#if (AMREX_SPACEDIM == 3)
            const Array4<const Real> wedge = uedge_in[lev][2].array(mfi);
#endif
            const Array4<const Real> w0_arr = w0_cart[lev].array(mfi);
            const Array4<const Real> gradw0_arr = gradw0_cart[lev].array(mfi);
            const Array4<const Real> w0_force = w0_force_cart[lev].array(mfi);
            const Array4<const Real> grav = grav_cart[lev].array(mfi);
            const Array4<const Real> rho0_arr = rho0_cart[lev].array(mfi);
            
            // output
            const Array4<Real> vel_force = vel_force_mf.array(mfi);
            
            // Get the index space of the valid region
            const Box& tileBox = mfi.tilebox();
            const Box& domainBox = geom[lev].Domain();

            // y-direction in 2D, z-direction in 3D
            const int domhi = domainBox.hiVect()[AMREX_SPACEDIM-1];
            
            // offload to GPU
            if (spherical == 0) {

                AMREX_PARALLEL_FOR_3D(tileBox, i, j, k, 
                {
                    Real rhopert = rho_arr(i,j,k) - rho0_arr(i,j,k);
                    
                    // cutoff the buoyancy term if we are outside of the star
                    if (rho_arr(i,j,k) < (buoyancy_cutoff_factor*base_cutoff_density)) {
                        rhopert = 0.0;
                    }

                    // note: if use_alt_energy_fix = T, then gphi is already
                    // weighted by beta0
                    for (int dim=0; dim<AMREX_SPACEDIM-1; ++dim) {
                        vel_force(i,j,k,dim) = -gpi_arr(i,j,k,dim) / rho_arr(i,j,k);
                    }

                    vel_force(i,j,k,AMREX_SPACEDIM-1) = (rhopert * grav(i,j,k,AMREX_SPACEDIM-1) - gpi_arr(i,j,k,AMREX_SPACEDIM-1)) / rho_arr(i,j,k) - w0_force(i,j,k,AMREX_SPACEDIM-1);

                    if (do_add_utilde_force == 1) {
                        
#if (AMREX_SPACEDIM == 2)
                        if (j <= -1) {
                            // do not modify force since dw0/dr=0
                        } else if (j >= domhi) {
                            // do not modify force since dw0/dr=0
                        } else {
                            vel_force(i,j,k,1) = vel_force(i,j,k,1)
                                - (vedge(i,j+1,k)+vedge(i,j,k))*(w0_arr(i,j+1,k,1) - w0_arr(i,j,k,1))/(2.0*dx[1]);
                        }
#else
                        
                        if (k <= -1) {
                            // do not modify force since dw0/dr=0
                        } else if (k >= domhi) {
                            // do not modify force since dw0/dr=0
                        } else {
                            vel_force(i,j,k,2) = vel_force(i,j,k,2) 
                                - (wedge(i,j,k+1)+wedge(i,j,k))*(w0_arr(i,j,k+1,2)-w0_arr(i,j,k,2)) / (2.0*dx[2]);
                        }
#endif
                    }
                });          
            } else {  // spherical
#if (AMREX_SPACEDIM == 3)
                const Array4<const Real> normal_arr = normal[lev].array(mfi);
            
                AMREX_PARALLEL_FOR_3D(tileBox, i, j, k, 
                {
                    Real rhopert = rho_arr(i,j,k) - rho0_arr(i,j,k);

                    // cutoff the buoyancy term if we are outside of the star
                    if (rho_arr(i,j,k) < (buoyancy_cutoff_factor*base_cutoff_density)) {
                        rhopert = 0.0;
                    }

                    // note: if use_alt_energy_fix = T, then gphi is already
                    // weighted by beta0
                    for (int dim=0; dim < AMREX_SPACEDIM; ++dim) {
                        vel_force(i,j,k,dim) = (rhopert*grav(i,j,k,dim) - gpi_arr(i,j,k,dim))/rho_arr(i,j,k) - w0_force(i,j,k,dim);
                    }
                    
                    if (do_add_utilde_force == 1) {
                        Real Ut_dot_er = 0.5*(uedge(i,j,k)+uedge(i+1,j  ,k  ))*normal_arr(i,j,k,0) + 
                                         0.5*(vedge(i,j,k)+vedge(i  ,j+1,k  ))*normal_arr(i,j,k,1) + 
                                         0.5*(wedge(i,j,k)+wedge(i  ,j,  k+1))*normal_arr(i,j,k,2);
                        
                        for (int dim=0; dim < AMREX_SPACEDIM; ++dim) {
                            vel_force(i,j,k,dim) -= 
                                Ut_dot_er*gradw0_arr(i,j,k)*normal_arr(i,j,k,dim);
                        }
                    }
                });
#endif
            }
        }
    }

    // average fine data onto coarser cells
    AverageDown(vel_force_cart,0,AMREX_SPACEDIM);

    // note - we need to reconsider the bcs type here
    // it matches fortran MAESTRO but is that correct?
    FillPatch(t_old, vel_force_cart, vel_force_cart, vel_force_cart, 0, 0, AMREX_SPACEDIM, 0,
              bcs_u, 1);

}


void
Maestro::ModifyScalForce(Vector<MultiFab>& scal_force,
                         const Vector<MultiFab>& state,
                         const Vector<std::array< MultiFab, AMREX_SPACEDIM > >& umac_in,
                         const RealVector& s0,
                         const RealVector& s0_edge,
                         const Vector<MultiFab>& s0_cart,
                         int comp,
                         const Vector<BCRec>& bcs,
                         int fullform)
{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::ModifyScalForce()",ModifyScalForce);

    Vector<MultiFab> s0_edge_cart(finest_level+1);

    for (int lev=0; lev<=finest_level; ++lev) {
        s0_edge_cart[lev].define(grids[lev], dmap[lev], 1, 1);
    }

    if (spherical == 0) {
        Put1dArrayOnCart(s0_edge,s0_edge_cart,0,0,bcs_f,0);
    }

    RealVector divw0;
    Vector<MultiFab> divu_cart(finest_level+1);

    if (spherical == 1) {
        divw0.resize(nr_fine);
        std::fill(divw0.begin(), divw0.end(), 0.);

        if (!use_exact_base_state) {
            for (int r=0; r<nr_fine-1; ++r) {
                Real dr = r_edge_loc[r+1] - r_edge_loc[r];
                divw0[r] = (r_edge_loc[r+1]*r_edge_loc[r+1] * w0[r+1]
                           - r_edge_loc[r]*r_edge_loc[r] * w0[r]) / (dr * r_cc_loc[r]*r_cc_loc[r]);
            }
        }

        for (int lev=0; lev<=finest_level; ++lev) {
            divu_cart[lev].define(grids[lev], dmap[lev], 1, 0);
            divu_cart[lev].setVal(0.);
        }
        Put1dArrayOnCart(divw0,divu_cart,0,0,bcs_u,0);
    }
    
    for (int lev=0; lev<=finest_level; ++lev) {

        // Get the index space and grid spacing of the domain
        const GpuArray<Real, AMREX_SPACEDIM> dx = geom[lev].CellSizeArray();

        // get references to the MultiFabs at level lev
        MultiFab& scal_force_mf = scal_force[lev];

        // loop over boxes (make sure mfi takes a cell-centered multifab as an argument)
#ifdef _OPENMP
#pragma omp parallel
#endif
        for ( MFIter mfi(scal_force_mf, TilingIfNotGPU()); mfi.isValid(); ++mfi ) {
            
            // Get Array4 inputs
            const Array4<const Real> scal = state[lev].array(mfi,comp);
            const Array4<const Real> umac = umac_in[lev][0].array(mfi);
            const Array4<const Real> vmac = umac_in[lev][1].array(mfi);
#if (AMREX_SPACEDIM == 3)
            const Array4<const Real> wmac = umac_in[lev][2].array(mfi);
#endif
            const Array4<const Real> s0_arr = s0_cart[lev].array(mfi);
            const Array4<const Real> s0_edge_arr = s0_edge_cart[lev].array(mfi);
            const Array4<const Real> w0_arr = w0_cart[lev].array(mfi);

            // output
            const Array4<Real> force = scal_force_mf.array(mfi,comp);
            
            // Get the index space of the valid region
            const Box& tileBox = mfi.tilebox();

            // offload to GPU
            if (spherical == 1) {
#if (AMREX_SPACEDIM == 3)
                // lo and hi of domain
                const Box& domainBox = geom[lev].Domain();
                const int* AMREX_RESTRICT domlo = domainBox.loVect();
                const int* AMREX_RESTRICT domhi = domainBox.hiVect();
                const Array4<const Real> divu_arr = divu_cart[lev].array(mfi);
                
                AMREX_PARALLEL_FOR_3D(tileBox, i, j, k, 
                {                   
                    // umac does not contain w0
                    Real divumac = (umac(i+1,j,k) - umac(i,j,k)) / dx[0] 
                                  +(vmac(i,j+1,k) - vmac(i,j,k)) / dx[1]
                                  +(wmac(i,j,k+1) - wmac(i,j,k)) / dx[2];
                    
                    if (fullform == 1) {
                        
                        force(i,j,k) -= scal(i,j,k)*(divumac+divu_arr(i,j,k));
                    } else {
        
                        Real s0_xlo = 0.0;
                        Real s0_xhi = 0.0;              
                        if (i < domhi[0]) {
                            s0_xhi = 0.5 * (s0_arr(i,j,k) + s0_arr(i+1,j,k));
                        } else {
                            s0_xhi = s0_arr(i,j,k);
                        }
                        if (i > domlo[0]) {
                            s0_xlo = 0.5 * (s0_arr(i,j,k) + s0_arr(i-1,j,k));
                        } else {
                            s0_xlo = s0_arr(i,j,k);
                        }
                        
                        Real s0_ylo = 0.0;
                        Real s0_yhi = 0.0;
                        if (j < domhi[1]) {
                            s0_yhi = 0.5 * (s0_arr(i,j,k) + s0_arr(i,j+1,k));
                        } else {
                            s0_yhi = s0_arr(i,j,k);
                        }
                        if (j > domlo[1]) {
                            s0_ylo = 0.5 * (s0_arr(i,j,k) + s0_arr(i,j-1,k));
                        } else {
                            s0_ylo = s0_arr(i,j,k);
                        }

                        Real s0_zlo = 0.0;
                        Real s0_zhi = 0.0;
                        if (k < domhi[2]) {
                            s0_zhi = 0.5 * (s0_arr(i,j,k) + s0_arr(i,j,k+1));
                        } else {
                            s0_zhi = s0_arr(i,j,k);
                        }
                        if (k > domlo[2]) {
                            s0_zlo = 0.5 * (s0_arr(i,j,k) + s0_arr(i,j,k-1));
                        } else {
                            s0_zlo = s0_arr(i,j,k);
                        }
                        
                        Real divs0u = (umac(i+1,j,k)*s0_xhi - umac(i,j,k)*s0_xlo)/dx[0] + 
                            (vmac(i,j+1,k)*s0_yhi - vmac(i,j,k)*s0_ylo)/dx[1] + 
                            (wmac(i,j,k+1)*s0_zhi - wmac(i,j,k)*s0_zlo)/dx[2];
                            
                        force(i,j,k) = force(i,j,k) - divs0u
                            - (scal(i,j,k)-s0_arr(i,j,k))*(divumac+divu_arr(i,j,k));
                    }
                });
#else
                Abort("ModifyScalForce: Spherical is not valid for DIM < 3");
#endif
            } else {

                AMREX_PARALLEL_FOR_3D(tileBox, i, j, k, 
                {
                    // umac does not contain w0
#if (AMREX_SPACEDIM == 2)
                    Real divu = (umac(i+1,j,k) - umac(i,j,k)) / dx[0] 
                        +(vmac(i,j+1,k) - vmac(i,j,k)) / dx[1];

                    // add w0 contribution
                    divu += (w0_arr(i,j+1,k,AMREX_SPACEDIM-1)- w0_arr(i,j,k,AMREX_SPACEDIM-1))/dx[AMREX_SPACEDIM-1];
#elif (AMREX_SPACEDIM == 3)
                    Real divu = (umac(i+1,j,k) - umac(i,j,k)) / dx[0] 
                        +(vmac(i,j+1,k) - vmac(i,j,k)) / dx[1] 
                        +(wmac(i,j,k+1) - wmac(i,j,k)) / dx[2];

                    // add w0 contribution
                    divu += (w0_arr(i,j,k+1,AMREX_SPACEDIM-1)-w0_arr(i,j,k,AMREX_SPACEDIM-1))/dx[AMREX_SPACEDIM-1];
#endif
                        
                    if (fullform == 1) {
                        force(i,j,k) -= scal(i,j,k)*divu;
                    } else {

#if (AMREX_SPACEDIM == 2)
                        Real divs0u = s0_arr(i,j,k)*(  umac(i+1,j,k) - umac(i,j,k))/dx[0] 
                            +(vmac(i,j+1,k) * s0_edge_arr(i,j+1,k) - 
                              vmac(i,j  ,k) * s0_edge_arr(i,j,k) )/ dx[1];
#elif (AMREX_SPACEDIM == 3)
                        Real divs0u = s0_arr(i,j,k)*( (umac(i+1,j,k) - umac(i,j,k))/dx[0] 
                                                      +(vmac(i,j+1,k) - vmac(i,j,k))/dx[1] ) 
                            +(wmac(i,j,k+1) * s0_edge_arr(i,j,k+1) - 
                              wmac(i,j,k  ) * s0_edge_arr(i,j,k))/ dx[2];
#endif

                        force(i,j,k) = force(i,j,k) - (scal(i,j,k)-s0_arr(i,j,k))*divu - divs0u;
                    }
                });
            }
        }
    }

    // average fine data onto coarser cells
    AverageDown(scal_force,comp,1);

    // fill ghost cells
    FillPatch(t_old, scal_force, scal_force, scal_force, comp, comp, 1, 0, bcs_f);
}

void
Maestro::MakeRhoHForce(Vector<MultiFab>& scal_force,
                       const int is_prediction,
                       const Vector<MultiFab>& thermal,
                       const Vector<std::array< MultiFab, AMREX_SPACEDIM > >& umac_cart,
                       const int add_thermal,
                       const int &which_step)

{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::MakeRhoHForce()",MakeRhoHForce);

    // if we are doing the prediction, then it only makes sense to be in
    // this routine if the quantity we are predicting is rhoh', h, or rhoh
    if (is_prediction == 1 && !(enthalpy_pred_type == predict_rhohprime ||
                                enthalpy_pred_type == predict_h ||
                                enthalpy_pred_type == predict_rhoh) ) {
        Abort("ERROR: should only call mkrhohforce when predicting rhoh', h, or rhoh");
    }

    RealVector rho0( (max_radial_level+1)*nr_fine );
    RealVector   p0( (max_radial_level+1)*nr_fine );
    RealVector grav( (max_radial_level+1)*nr_fine );
    rho0.shrink_to_fit();
    p0.shrink_to_fit();
    grav.shrink_to_fit();

    if (which_step == 1) {
        rho0 = rho0_old;
        p0 =   p0_old;
    }
    else {
        for(int i=0; i<rho0.size(); ++i) {
            rho0[i] = 0.5*(rho0_old[i]+rho0_new[i]);
            p0[i] = 0.5*(  p0_old[i]+  p0_new[i]);
        }
    }

    Vector<MultiFab> p0_cart(finest_level+1);
    Vector<MultiFab> psi_cart(finest_level+1);
    Vector<MultiFab> grav_cart(finest_level+1);
    Vector<MultiFab> rho0_cart(finest_level+1);
    Vector< std::array< MultiFab, AMREX_SPACEDIM > > p0mac(finest_level+1);

    for (int lev=0; lev<=finest_level; ++lev) {
        p0_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        psi_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        grav_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        rho0_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        AMREX_D_TERM(p0mac[lev][0].define(convert(grids[lev],nodal_flag_x), dmap[lev], 1, 1); ,
                    p0mac[lev][1].define(convert(grids[lev],nodal_flag_y), dmap[lev], 1, 1); ,
                    p0mac[lev][2].define(convert(grids[lev],nodal_flag_z), dmap[lev], 1, 1); );
        psi_cart[lev].setVal(0.);
        grav_cart[lev].setVal(0.);
        rho0_cart[lev].setVal(0.);
        p0_cart[lev].setVal(0.);
    }

    Put1dArrayOnCart(p0, p0_cart, 0, 0, bcs_f, 0);
    if (spherical == 1) {
        MakeS0mac(p0, p0mac);
    } 
    Put1dArrayOnCart(psi,psi_cart,0,0,bcs_f,0);
    Put1dArrayOnCart(rho0,rho0_cart,0,0,bcs_s,Rho);

    make_grav_cell(grav.dataPtr(),
                   rho0.dataPtr(),
                   r_cc_loc.dataPtr(),
                   r_edge_loc.dataPtr());

    Put1dArrayOnCart(grav,grav_cart,0,0,bcs_f,0);

    // constants in Fortran
    const int enthalpy_pred_type_in = enthalpy_pred_type;
    const int predict_h_const = predict_h;
    const int predict_rhoh_const = predict_rhoh;
    
    for (int lev=0; lev<=finest_level; ++lev) {

        // get references to the MultiFabs at level lev
        MultiFab& scal_force_mf = scal_force[lev];

        // Get cutoff coord
        int base_cutoff_density_coord;
        get_base_cutoff_density_coord(&lev, &base_cutoff_density_coord);
    
        // Get grid spacing
        const GpuArray<Real, AMREX_SPACEDIM> dx = geom[lev].CellSizeArray();
        
        // loop over boxes (make sure mfi takes a cell-centered multifab as an argument)
#ifdef _OPENMP
#pragma omp parallel
#endif
        for ( MFIter mfi(scal_force_mf, TilingIfNotGPU()); mfi.isValid(); ++mfi ) {

            // inputs
            const Array4<const Real> umac = umac_cart[lev][0].array(mfi);
            const Array4<const Real> vmac = umac_cart[lev][1].array(mfi);
            const Array4<const Real> p0cart = p0_cart[lev].array(mfi);
            const Array4<const Real> p0macx = p0mac[lev][0].array(mfi);
            const Array4<const Real> p0macy = p0mac[lev][1].array(mfi);
#if (AMREX_SPACEDIM == 3)
            const Array4<const Real> wmac = umac_cart[lev][2].array(mfi);
            const Array4<const Real> p0macz = p0mac[lev][2].array(mfi);
#endif
            const Array4<const Real> thermal_arr = thermal[lev].array(mfi);
            const Array4<const Real> psicart = psi_cart[lev].array(mfi);
            const Array4<const Real> gravcart = grav_cart[lev].array(mfi);
            const Array4<const Real> rho0cart = rho0_cart[lev].array(mfi);

            // output
            const Array4<Real> rhoh_force = scal_force_mf.array(mfi, RhoH);
            
            // Get the index space of the valid region
            const Box& tileBox = mfi.tilebox();
            const Box& domainBox = geom[lev].Domain();
            
            const int domhi = domainBox.hiVect()[AMREX_SPACEDIM-1];

            // if use_exact_base_state or average_base_state,
            // psi is set to dpdt in advance subroutine

			// For non-spherical, add wtilde d(p0)/dr
			// For spherical, we make u grad p = div (u p) - p div (u)
            if (spherical == 0) {

                AMREX_PARALLEL_FOR_3D (tileBox, i, j, k,
                {
                    Real gradp0 = 0.0;
                
#if (AMREX_SPACEDIM == 2)
                    if (j < base_cutoff_density_coord) {
                        gradp0 = rho0cart(i,j,k) * gravcart(i,j,k);
                    } else if (j == domhi) {
                        // NOTE: this should be zero since p0 is constant up here
                        gradp0 = ( p0cart(i,j,k) - p0cart(i,j-1,k) ) / dx[1]; 
                    } else {
                        // NOTE: this should be zero since p0 is constant up here
                        gradp0 = ( p0cart(i,j+1,k) - p0cart(i,j,k) ) / dx[1];
                    }

                    Real veladv = 0.5*(vmac(i,j,k)+vmac(i,j+1,k));
                    rhoh_force(i,j,k) = veladv * gradp0;
#else 
                    if (k < base_cutoff_density_coord) {
                        gradp0 = rho0cart(i,j,k) * gravcart(i,j,k);
                    } else if (k == domhi) {
                        // NOTE: this should be zero since p0 is constant up here
                        gradp0 = ( p0cart(i,j,k) - p0cart(i,j-1,k) ) / dx[2]; 
                    } else {
                        // NOTE: this should be zero since p0 is constant up here
                        gradp0 = ( p0cart(i,j+1,k) - p0cart(i,j,k) ) / dx[2];
                    }

                    Real veladv = 0.5*(wmac(i,j,k)+wmac(i,j,k+1));
                    rhoh_force(i,j,k) = veladv * gradp0;
#endif
                    //
                    // psi should always be in the force if we are doing the final update
                    // For prediction, it should not be in the force if we are predicting
                    // (rho h)', but should be there if we are predicting h or rhoh
                    //
                    // If use_exact_base_state or average_base_state is on, psi is instead dpdt term
                    //
                    if ((is_prediction == 1 && enthalpy_pred_type_in == predict_h_const) ||
                        (is_prediction == 1 && enthalpy_pred_type_in == predict_rhoh_const) || 
                        (is_prediction == 0)) {
                    
                        rhoh_force(i,j,k) += psicart(i,j,k);
                    }

                    if (add_thermal == 1) {
                        rhoh_force(i,j,k) += thermal_arr(i,j,k);
                    }
                });
                
            } else {
                
#if (AMREX_SPACEDIM == 3)
                AMREX_PARALLEL_FOR_3D (tileBox, i, j, k,
                {
                    Real divup = (umac(i+1,j,k) * p0macx(i+1,j,k) - umac(i,j,k) * p0macx(i,j,k)) / dx[0] + 
                        (vmac(i,j+1,k) * p0macy(i,j+1,k) - vmac(i,j,k) * p0macy(i,j,k)) / dx[1] + 
                        (wmac(i,j,k+1) * p0macz(i,j,k+1) - wmac(i,j,k) * p0macz(i,j,k)) / dx[2];

                    Real p0divu = ( (umac(i+1,j,k) - umac(i,j,k)) / dx[0] + 
                               (vmac(i,j+1,k) - vmac(i,j,k)) / dx[1] + 
                               (wmac(i,j,k+1) - wmac(i,j,k)) / dx[2] ) * p0cart(i,j,k);

                    rhoh_force(i,j,k) = divup - p0divu;

                    if ((is_prediction == 1 && enthalpy_pred_type_in == predict_h_const) ||
                        (is_prediction == 1 && enthalpy_pred_type_in == predict_rhoh_const) || 
                        (is_prediction == 0)) {
                    
                        rhoh_force(i,j,k) += psicart(i,j,k);
                    }

                    if (add_thermal == 1) {
                        rhoh_force(i,j,k) += thermal_arr(i,j,k);
                    }
                });
#endif          
            }
        }
    }

    // average down and fill ghost cells
    AverageDown(scal_force,RhoH,1);
    FillPatch(t_old,scal_force,scal_force,scal_force,RhoH,RhoH,1,0,bcs_f);
}


void
Maestro::MakeTempForce(Vector<MultiFab>& temp_force,
                       const Vector<MultiFab>& scal,
                       const Vector<MultiFab>& thermal,
                       const Vector<std::array< MultiFab, AMREX_SPACEDIM > >& umac)

{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::MakeTempForce()",MakeTempForce);

    // if we are doing the prediction, then it only makes sense to be in
    // this routine if the quantity we are predicting is rhoh', h, or rhoh
    if (!(enthalpy_pred_type == predict_T_then_rhohprime ||
          enthalpy_pred_type == predict_T_then_h ||
          enthalpy_pred_type == predict_Tprime_then_h) ) {
        Abort("ERROR: should only call mktempforce when predicting T' or T");
    }

    Vector<MultiFab> p0_cart(finest_level+1);
    Vector<MultiFab> psi_cart(finest_level+1);

    for (int lev=0; lev<=finest_level; ++lev) {
        p0_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        psi_cart[lev].define(grids[lev], dmap[lev], 1, 1);
        p0_cart[lev].setVal(0.);
        psi_cart[lev].setVal(0.);
    }

    Put1dArrayOnCart(p0_old, p0_cart, 0, 0, bcs_f, 0);
    Put1dArrayOnCart(psi,psi_cart,0,0,bcs_f,0);

    for (int lev=0; lev<=finest_level; ++lev) {

        // get references to the MultiFabs at level lev
        MultiFab& temp_force_mf = temp_force[lev];
        const MultiFab& umac_mf = umac[lev][0];
        const MultiFab& vmac_mf = umac[lev][1];
#if (AMREX_SPACEDIM == 3)
        const MultiFab& wmac_mf = umac[lev][2];
#endif
        const MultiFab& scal_mf = scal[lev];
        const MultiFab& p0cart_mf = p0_cart[lev];
        const MultiFab& thermal_mf = thermal[lev];
        const MultiFab& psi_mf = psi_cart[lev];

        // loop over boxes (make sure mfi takes a cell-centered multifab as an argument)
#ifdef _OPENMP
#pragma omp parallel
#endif
        for ( MFIter mfi(temp_force_mf, TilingIfNotGPU()); mfi.isValid(); ++mfi ) {

            // Get the index space of the valid region
            const Box& tileBox = mfi.tilebox();
            const Box& domainBox = geom[lev].Domain();

            // Get grid spacing
            const Real* dx = geom[lev].CellSize();
                
            // call fortran subroutine
            // use macros in AMReX_ArrayLim.H to pass in each FAB's data,
            // lo/hi coordinates (including ghost cells), and/or the # of components
            // We will also pass "validBox", which specifies the "valid" region.

            // if use_exact_base_state or average_base_state,
            // psi is set to dpdt in advance subroutine
#pragma gpu box(tileBox)
            mktempforce(AMREX_INT_ANYD(tileBox.loVect()),
                        AMREX_INT_ANYD(tileBox.hiVect()),
                        lev,
                        temp_force_mf[mfi].dataPtr(Temp),
                        AMREX_INT_ANYD(temp_force_mf[mfi].loVect()),
                        AMREX_INT_ANYD(temp_force_mf[mfi].hiVect()),
                        BL_TO_FORTRAN_ANYD(scal_mf[mfi]), 
                        BL_TO_FORTRAN_ANYD(umac_mf[mfi]),
                        BL_TO_FORTRAN_ANYD(vmac_mf[mfi]),
#if (AMREX_SPACEDIM == 3)
                        BL_TO_FORTRAN_ANYD(wmac_mf[mfi]),
#endif
                        BL_TO_FORTRAN_ANYD(thermal_mf[mfi]),
                        BL_TO_FORTRAN_ANYD(p0cart_mf[mfi]),
                        BL_TO_FORTRAN_ANYD(psi_mf[mfi]),
                        AMREX_REAL_ANYD(dx), 
                        AMREX_INT_ANYD(domainBox.hiVect()));
        }
    }

    // average down and fill ghost cells
    AverageDown(temp_force,Temp,1);
    FillPatch(t_old,temp_force,temp_force,temp_force,Temp,Temp,1,0,bcs_f);
}
