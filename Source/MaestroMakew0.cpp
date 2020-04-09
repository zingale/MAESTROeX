#include <Maestro.H>
#include <Maestro_F.H>

using namespace amrex;

void 
Maestro::Makew0(const BaseState<Real>& w0_old, 
                BaseState<Real>& w0_force, 
                const BaseState<Real>& Sbar_in, 
                const BaseState<Real>& rho0_old_in,
                const BaseState<Real>& rho0_new_in,
                const BaseState<Real>& p0_old_in,
                const BaseState<Real>& p0_new_in,
                const BaseState<Real>& gamma1bar_old_in,
                const BaseState<Real>& gamma1bar_new_in,
                const BaseState<Real>& p0_minus_peosbar, 
                const Real dt_in, const Real dtold_in, 
                const bool is_predictor) 
{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::Makew0()", Makew0);

    w0_force.setVal(0.);

    const int max_lev = max_radial_level+1;
    
    if (!spherical) {
        if (do_planar_invsq_grav || do_2d_planar_octant) {
            Makew0PlanarVarg(w0_old, w0_force, Sbar_in, 
                             rho0_old_in, rho0_new_in,
                             p0_old_in, p0_new_in, 
                             gamma1bar_old_in, gamma1bar_new_in, 
                             p0_minus_peosbar, 
                             dt_in, dtold_in);
        } else {
            Makew0Planar(w0_old, w0_force, Sbar_in, 
                         rho0_old_in, rho0_new_in,
                         p0_old_in, p0_new_in, 
                         gamma1bar_old_in, gamma1bar_new_in, 
                         p0_minus_peosbar, 
                         dt_in, dtold_in,
                         is_predictor);
        }
    } else {
        if (use_exact_base_state) {
            Makew0SphrIrreg(w0_old, w0_force, Sbar_in, 
                            rho0_old_in, rho0_new_in,
                            p0_old_in, p0_new_in, 
                            gamma1bar_old_in, gamma1bar_new_in, 
                            p0_minus_peosbar, 
                            dt_in, dtold_in);
        } else {
            Makew0Sphr(w0_old, w0_force, Sbar_in, 
                       rho0_old_in, rho0_new_in,
                       p0_old_in, p0_new_in, 
                       gamma1bar_old_in, gamma1bar_new_in, 
                       p0_minus_peosbar, 
                       dt_in, dtold_in);
        }
    }

    if (maestro_verbose >= 2) {
        for (auto n = 0; n <= finest_radial_level; ++n) {
            Real max_w0 = 0.0;
            for (auto r = r_start_coord(n,1); r <= r_end_coord(n,1)+1; ++r) {
                max_w0 = max(max_w0, fabs(w0(n,r)));
            }
            Print() << "... max CFL of w0: " << max_w0 * dt_in / dr(n) << std::endl;
        }
        Print() << std::endl;
    }
}

void 
Maestro::Makew0Planar(const BaseState<Real>& w0_old, 
                      BaseState<Real>& w0_force, 
                      const BaseState<Real>& Sbar_in, 
                      const BaseState<Real>& rho0_old_in,
                      const BaseState<Real>& rho0_new_in,
                      const BaseState<Real>& p0_old_in,
                      const BaseState<Real>& p0_new_in,
                      const BaseState<Real>& gamma1bar_old_in,
                      const BaseState<Real>& gamma1bar_new_in,
                      const BaseState<Real>& p0_minus_peosbar,  
                      const Real dt_in, const Real dtold_in, 
                      const bool is_predictor) 
{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::Makew0Planar()", Makew0Planar);

    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // Multilevel Outline
    //
    // Compute w0 at level 1 only
    // Initialize new w0 at bottom of coarse base array to 0.0.
    // do n=1,finest_radial_level
    //   Compute w0 on edges at level n
    //   Obtain the starting value of w0 from the coarser grid
    //   if n>1, compare the difference between w0 at top of level n to the
    //           corresponding point on level n-1
    //   do i=n-1,1,-1
    //     Restrict w0 from level n to level i
    //     Offset the w0 on level i above the top of level n
    //   }
    // }
    //!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!

    w0.setVal(0.0);

    const int max_lev = max_radial_level+1;

    // local variables 
    BaseState<Real> psi_planar(nr_fine);

    const auto& etarho_cc_p = etarho_cc;
    auto& w0_p = w0;

    const Real dt_loc = dt;
    const Real grav_const_loc = grav_const;
    const Real dpdt_factor_loc = dpdt_factor;

    // Compute w0 on edges at level n
    for (auto n = 0; n <= max_radial_level; ++n) {

        psi_planar.setVal(0.0);
        int base_cutoff_density_coord_loc = base_cutoff_density_coord(n);

        const Real dr_lev = dr(n);

        for (auto j = 1; j <= numdisjointchunks(n); ++j) {

            if (n == 0) {
                // Initialize new w0 at bottom of coarse base array to 0.0.
                w0(0,0) = 0.0;
            } else {
                // Obtain the starting value of w0 from the coarser grid
                w0(n,r_start_coord(n,j)) = w0(n-1,r_start_coord(n,j)/2);
            }

            // compute psi for level n
            int lo = r_start_coord(n,j); 
            int hi = r_end_coord(n,j);
            AMREX_PARALLEL_FOR_1D(hi-lo+1, k, {
                int r = k + lo;
                if (r < base_cutoff_density_coord_loc) {
                    psi_planar(r) = etarho_cc_p(n,r) * fabs(grav_const_loc);
                }
            });

            for (auto r = r_start_coord(n,j)+1; 
                r <= r_end_coord(n,j)+1; ++r) {

                Real gamma1bar_p0_avg = (gamma1bar_old_in(n,r-1)
                    + gamma1bar_new_in(n,r-1)) *
                    (p0_old_in(n,r-1) + 
                    p0_new_in(n,r-1))/4.0;

                Real delta_chi_w0 = 0.0;

                if (r < base_cutoff_density_coord_loc) {
                    if (is_predictor) {
                        delta_chi_w0 = dpdt_factor_loc * 
                            p0_minus_peosbar(n,r-1) / 
                            (gamma1bar_old_in(n,r-1)*
                            p0_old_in(n,r-1)*dt_loc);
                    } else {
                        delta_chi_w0 += dpdt_factor_loc *
                            p0_minus_peosbar(n,r-1) / 
                            (gamma1bar_new_in(n,r-1)*
                            p0_new_in(n,r-1)*dt_loc);
                    }
                } 

                w0(n,r) = w0(n,r-1) + Sbar_in(n,r-1) * dr_lev
                    - psi_planar[r-1] / gamma1bar_p0_avg * dr_lev
                    - delta_chi_w0 * dr_lev;
            }

            if (n > 0) {
                // Compare the difference between w0 at top of level n to
                // the corresponding point on level n-1
                Real offset = w0(n,r_end_coord(n,j)+1)
                    - w0(n-1,(r_end_coord(n,j)+1)/2);

                for (auto i = n-1; i >= 0; --i) {

                    int refrat = round(pow(2, n-i));

                    // Restrict w0 from level n to level i
                    for (auto r = r_start_coord(n,j); r <= r_end_coord(n,j)+1; ++r) {
                        if (r % refrat == 0) {
                            w0(n,r/refrat) = w0(n,r);
                        }
                    }

                    // Offset the w0 on level i above the top of level n
                    // for (auto r = (r_end_coord(n,j)+1)/refrat+1; r <= nr(i); ++r) {
                    lo = (r_end_coord(n,j)+1)/refrat+1; 
                    hi = nr(i);
                    AMREX_PARALLEL_FOR_1D(hi-lo+1, k, {
                        int r = k + lo;
                        w0_p(i,r) += offset;
                    });
                }
            }
        }
    }

    // zero w0 where there is no corresponding full state array
    for (auto n = 1; n <= max_radial_level; ++n) {
        for (auto j = 1; j <= numdisjointchunks(n); ++j) {
            if (j == numdisjointchunks(n)) {
                // for (auto r = r_end_coord(n,j)+2; 
                //      r <= nr(n); ++r) {
                const int lo = r_end_coord(n,j)+2; 
                const int hi = nr(n);
                AMREX_PARALLEL_FOR_1D(hi-lo+1, k, {
                    int r = k + lo;
                    w0_p(n,r) = 0.0;
                });
            } else {
                // for (auto r = r_end_coord(n,j)+2; 
                //      r <= r_start_coord(n,j+1)-1; ++r) {
                const int lo = r_end_coord(n,j)+2; 
                const int hi = r_start_coord(n,j+1)-1;
                AMREX_PARALLEL_FOR_1D(hi-lo+1, k, {
                    int r = k + lo;
                    w0_p(n,r) = 0.0;
                });
            }
        }
    }

    RestrictBase(w0, false);
    FillGhostBase(w0, false);

    for (auto n = 0; n <= max_radial_level; ++n) {
        for (auto j = 1; j <= numdisjointchunks(n); ++j) {

            // Compute the forcing term in the base state velocity
            // equation, - 1/rho0 grad pi0
            const Real dt_avg = 0.5 * (dt_in + dtold_in);
            const Real dr_lev = dr(n);

            // for (auto r = r_start_coord(n,j); 
            //      r <= r_end_coord(n,j); ++r) {
            const int lo = r_start_coord(n,j); 
            const int hi = r_end_coord(n,j);
            AMREX_PARALLEL_FOR_1D(hi-lo+1, k, {
                int r = k + lo;

                Real w0_old_cen = 0.5 * (w0_old(n,r) + 
                    w0_old(n,r+1));
                Real w0_new_cen = 0.5 * (w0_p(n,r) + 
                    w0_p(n,r+1));
                Real w0_avg = 0.5 * (dt_in * w0_old_cen + dtold_in *  w0_new_cen) / dt_avg;
                Real div_avg = 0.5 * (dt_in *(w0_old(n,r+1)
                    - w0_old(n,r)) + dtold_in * (w0_p(n,r+1) 
                    - w0_p(n,r))) / dt_avg;
                w0_force(n,r) = (w0_new_cen - w0_old_cen)/dt_avg 
                    + w0_avg*div_avg/dr_lev;
            });
        }
    }

    RestrictBase(w0_force, true);
    FillGhostBase(w0_force, true);
}

void 
Maestro::Makew0PlanarVarg(const BaseState<Real>& w0_old, 
                          BaseState<Real>& w0_force, 
                          const BaseState<Real>& Sbar_in, 
                          const BaseState<Real>& rho0_old_in,
                          const BaseState<Real>& rho0_new_in,
                          const BaseState<Real>& p0_old_in,
                          const BaseState<Real>& p0_new_in,
                          const BaseState<Real>& gamma1bar_old_in,
                          const BaseState<Real>& gamma1bar_new_in,
                          const BaseState<Real>& p0_minus_peosbar,  
                          const Real dt_in, const Real dtold_in) 
{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::Makew0PlanarVarg()",Makew0PlanarVarg);

    int fine_base_density_cutoff_coord = base_cutoff_density_coord(finest_radial_level);

    const int max_lev = max_radial_level+1;
    const int nr_finest = nr(finest_radial_level);
    const Real dr_finest = dr(finest_radial_level);
    const Real dpdt_factor_loc = dpdt_factor;

    auto& w0_p = w0;
    const auto r_edge_loc_p = r_edge_loc_b;

    // The planar 1/r**2 gravity constraint equation is solved
    // by calling the tridiagonal solver, just like spherical.
    // This is accomplished by putting all the requisite data
    // on the finest basestate grid, solving for w0, and then
    // restricting w0 back down to the coarse grid.

    // 1) allocate the finely-gridded temporary basestate arrays
    BaseState<Real> w0_fine(nr_finest+1);
    BaseState<Real> w0bar_fine(nr_finest+1);
    BaseState<Real> deltaw0_fine(nr_finest+1);
    BaseState<Real> p0_old_fine(nr_finest);
    BaseState<Real> p0_new_fine(nr_finest);
    BaseState<Real> p0_nph_fine(nr_finest);
    BaseState<Real> rho0_old_fine(nr_finest);
    BaseState<Real> rho0_new_fine(nr_finest);
    BaseState<Real> rho0_nph_fine(nr_finest);
    BaseState<Real> gamma1bar_old_fine(nr_finest);
    BaseState<Real> gamma1bar_new_fine(nr_finest);
    BaseState<Real> gamma1bar_nph_fine(nr_finest);
    BaseState<Real> p0_minus_peosbar_fine(nr_finest);
    BaseState<Real> etarho_cc_fine(nr_finest);
    BaseState<Real> Sbar_in_fine(nr_finest);
    BaseState<Real> grav_edge_fine(nr_finest+1);

    // 2) copy the data into the temp, uniformly-gridded basestate arrays.
    ProlongBasetoUniform(p0_old_in, p0_old_fine);
    ProlongBasetoUniform(p0_new_in, p0_new_fine);
    ProlongBasetoUniform(rho0_old_in, rho0_old_fine);
    ProlongBasetoUniform(rho0_new_in, rho0_new_fine);
    ProlongBasetoUniform(gamma1bar_old_in, gamma1bar_old_fine);
    ProlongBasetoUniform(gamma1bar_new_in, gamma1bar_new_fine);
    ProlongBasetoUniform(p0_minus_peosbar, p0_minus_peosbar_fine);
    ProlongBasetoUniform(etarho_cc, etarho_cc_fine);
    ProlongBasetoUniform(Sbar_in, Sbar_in_fine);

    // create time-centered base-state quantities
    // for (auto r = 0; r < nr_finest; ++r) {
    AMREX_PARALLEL_FOR_1D(nr_finest, r, {
        p0_nph_fine(r) = 0.5*(p0_old_fine(r)+ p0_new_fine(r));
        rho0_nph_fine(r) = 0.5*(rho0_old_fine(r) + rho0_new_fine(r));
        gamma1bar_nph_fine(r) = 0.5*(gamma1bar_old_fine(r) + gamma1bar_new_fine(r));
    });


    // 3) solve to w0bar -- here we just take into account the Sbar and
    //    volume discrepancy terms
    // lower boundary condition
    w0bar_fine[0] = 0.0;

    // for (auto r = 1; r <= nr_finest; ++r) {
    int lo = 1; 
    int hi = nr_finest;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        Real gamma1bar_p0_avg = gamma1bar_nph_fine(r-1) * p0_nph_fine(r-1);

        Real volume_discrepancy = (r-1 < fine_base_density_cutoff_coord) ? 
            dpdt_factor_loc * p0_minus_peosbar_fine(r-1)/dt_in : 0.0;

        w0bar_fine(r) =  w0bar_fine(r-1) + 
            Sbar_in_fine(r-1) * dr_finest 
            - (volume_discrepancy / gamma1bar_p0_avg ) * dr_finest;
    });

    // 4) get the edge-centered gravity on the uniformly-gridded
    // basestate arrays
    Abort("make_w0.f90: need to write make_grav_edge_uniform");
    //    call make_grav_edge_uniform(grav_edge_fine, rho0_nph_fine)


    // 5) solve for delta w0
    deltaw0_fine.setVal(0.0);

    // this takes the form of a tri-diagonal matrix:
    // A_j (dw_0)_{j-3/2} +
    // B_j (dw_0)_{j-1/2} +
    // C_j (dw_0)_{j+1/2} = F_j

    BaseState<Real> A(nr_finest+1);
    BaseState<Real> B(nr_finest+1);
    BaseState<Real> C(nr_finest+1);
    BaseState<Real> u(nr_finest+1);
    BaseState<Real> F(nr_finest+1);

    A.setVal(0.0);
    B.setVal(0.0);
    C.setVal(0.0);
    F.setVal(0.0);
    u.setVal(0.0);

    // for (auto r = 1; r <= fine_base_density_cutoff_coord; ++r) {
    lo = 1; 
    hi = fine_base_density_cutoff_coord;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        A(r) = gamma1bar_nph_fine(r-1) * p0_nph_fine(r-1);
        A(r) /= dr_finest*dr_finest;

        Real dpdr = (p0_nph_fine(r)-p0_nph_fine(r-1))/dr_finest;

        B(r) = -(gamma1bar_nph_fine(r-1) * p0_nph_fine(r-1) + 
            gamma1bar_nph_fine(r) * p0_nph_fine(r)) 
            / (dr_finest*dr_finest);
        B(r) -= 2.0 * dpdr / (r_edge_loc_p(finest_radial_level,r));

        C(r) = gamma1bar_nph_fine(r) * p0_nph_fine(r);
        C(r) /= dr_finest*dr_finest;

        F(r) = 2.0 * dpdr * w0bar_fine(r) / 
            r_edge_loc_p(finest_radial_level,r) -
            grav_edge_fine(r) * (etarho_cc_fine(r) - etarho_cc_fine(r-1)) / 
            dr_finest;
    });

    // Lower boundary
    A[0] = 0.0;
    B[0] = 1.0;
    C[0] = 0.0;
    F[0] = 0.0;

    // Upper boundary
    A[fine_base_density_cutoff_coord+1] = -1.0;
    B[fine_base_density_cutoff_coord+1] = 1.0;
    C[fine_base_density_cutoff_coord+1] = 0.0;
    F[fine_base_density_cutoff_coord+1] = 0.0;

    // need to synchronize gpu values with updated host values
    Gpu::synchronize();
    
    // Call the tridiagonal solver
    Tridiag(A, B, C, F, u, fine_base_density_cutoff_coord+2);

    // for (auto r = 1; r <= fine_base_density_cutoff_coord+1; ++r) {
    lo = 1; 
    hi = fine_base_density_cutoff_coord+1;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        deltaw0_fine(r) = u(r);
    });

    // for (auto r = fine_base_density_cutoff_coord+2; r <= nr_finest; ++r) {
    lo = fine_base_density_cutoff_coord+2; 
    hi = nr_finest;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        deltaw0_fine(r) = deltaw0_fine(fine_base_density_cutoff_coord+1);
    });

    // 6) compute w0 = w0bar + deltaw0
    // for (auto r = 0; r < w0_fine.size(); ++r) {
    AMREX_PARALLEL_FOR_1D(nr_finest+1, r, {
        w0_fine(r) = w0bar_fine(r) + deltaw0_fine(r);
        w0_p(finest_radial_level,r) = w0_fine(r);
    });

    // 7) fill the multilevel w0 array from the uniformly-gridded w0 we
    // just solved for.  Here, we make the coarse edge underneath equal
    // to the fine edge value.
    for (auto n = finest_radial_level; n >= 1; --n) {
        for (auto r = 0; r <= nr(n); n+=2) {
            w0(n-1,r/2) = w0(n,r);
        }
    }

    // 8) zero w0 where there is no corresponding full state array
    for (auto n = 1; n <= finest_radial_level; ++n) {
        for (auto j = 1; j <= numdisjointchunks(n); ++j) {
            if (j == numdisjointchunks(n)) {
                // for (auto r = r_end_coord(n,j)+2; r <= nr(n); ++r) {
                lo = r_end_coord(n,j)+2; 
                hi = nr(n);
                AMREX_PARALLEL_FOR_1D(hi-lo+1, k, {
                    int r = k + lo;
                    w0_p(n,r) = 0.0;
                });
            } else {
                // for (auto r = r_end_coord(n,j)+2; r < r_start_coord(n,j+1); ++r) {
                lo = r_end_coord(n,j)+2; 
                hi = r_start_coord(n,j+1);
                AMREX_PARALLEL_FOR_1D(hi-lo, k, {
                    int r = k + lo;
                    w0_p(n,r) = 0.0;
                });
            }
        }
    }

    RestrictBase(w0, false);
    FillGhostBase(w0, false);

    // compute the forcing terms
    for (auto n = 0; n <= finest_radial_level; ++n) {
        for (auto j = 1; j <= numdisjointchunks(n); ++j) {

            // Compute the forcing term in the base state velocity
            // equation, - 1/rho0 grad pi0
            const Real dt_avg = 0.5 * (dt_in + dtold_in);
            const Real dr_lev = dr(n);

            // for (auto r = r_start_coord(n,j); r <=r_end_coord(n,j); ++r) {
            lo = r_start_coord(n,j); 
            hi = r_end_coord(n,j);
            AMREX_PARALLEL_FOR_1D(hi-lo+1, k, {
                int r = k + lo;
                Real w0_old_cen = 0.5 * (w0_old(n,r) + w0_old(n,r+1));
                Real w0_new_cen = 0.5 * (w0_p(n,r) + w0_p(n,r+1));
                Real w0_avg = 0.5 * (dt_in * w0_old_cen + dtold_in *  w0_new_cen) / dt_avg;
                Real div_avg = 0.5 * (dt_in * (w0_old(n,r+1)-w0_old(n,r)) + 
                    dtold_in * (w0_p(n,r+1)-w0_p(n,r))) / dt_avg;
                w0_force(n,r) = (w0_new_cen-w0_old_cen)/dt_avg + w0_avg*div_avg/dr_lev;
            });
        }
    }

    RestrictBase(w0_force, true);
    FillGhostBase(w0_force, true);
}

void 
Maestro::Makew0Sphr(const BaseState<Real>& w0_old, 
                    BaseState<Real>& w0_force, 
                    const BaseState<Real>& Sbar_in, 
                    const BaseState<Real>& rho0_old_in,
                    const BaseState<Real>& rho0_new_in,
                    const BaseState<Real>& p0_old_in,
                    const BaseState<Real>& p0_new_in,
                    const BaseState<Real>& gamma1bar_old_in,
                    const BaseState<Real>& gamma1bar_new_in,
                    const BaseState<Real>& p0_minus_peosbar,  
                    const Real dt_in, const Real dtold_in) 
{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::Makew0Sphr()", Makew0Sphr);

    // local variables 
    const int max_lev = max_radial_level + 1;
    BaseState<Real> gamma1bar_nph(nr_fine);
    BaseState<Real> p0_nph(nr_fine);
    BaseState<Real> A(nr_fine+1);
    BaseState<Real> B(nr_fine+1);
    BaseState<Real> C(nr_fine+1);
    BaseState<Real> u(nr_fine+1);
    BaseState<Real> F(nr_fine+1);
    BaseState<Real> w0_from_Sbar(nr_fine+1);
    BaseState<Real> rho0_nph(max_lev,nr_fine);
    BaseState<Real> grav_edge(max_lev, nr_fine+1);

    const auto& p0_old_p = p0_old_in;
    const auto& p0_new_p = p0_new_in;
    const auto& r_cc_loc_p = r_cc_loc_b;
    const auto& r_edge_loc_p = r_edge_loc_b;
    const auto& etarho_cc_p = etarho_cc;
    const auto& etarho_ec_p = etarho_ec;
    auto& w0_p = w0;

    Real base_cutoff_dens = 0.0;
    get_base_cutoff_density(&base_cutoff_dens);
    int base_cutoff_density_coord_loc = base_cutoff_density_coord(0);

    const Real dr0 = dr(0);
    const Real dpdt_factor_loc = dpdt_factor;

    // create time-centered base-state quantities
    // for (auto r = 0; r < nr_fine; ++r) {
    AMREX_PARALLEL_FOR_1D(nr_fine, r, {
        p0_nph(r) = 0.5*(p0_old_p(0,r)+ p0_new_p(0,r));
        rho0_nph(0,r) = 0.5*(rho0_old_in(0,r) + rho0_new_in(0,r));
        gamma1bar_nph(r) = 0.5*(gamma1bar_old_in(0,r) + gamma1bar_new_in(0,r));
    });

    // NOTE: We first solve for the w0 resulting only from Sbar,
    //      w0_from_sbar by integrating d/dr (r^2 w0_from_sbar) =
    //      (r^2 Sbar).  Then we will solve for the update, delta w0.
    w0_from_Sbar(0) = 0.0;

    for (auto r = 1; r <= nr_fine; ++r) {
        Real volume_discrepancy = rho0_old_in(0,r-1) > base_cutoff_dens ? 
            dpdt_factor_loc * p0_minus_peosbar(0,r-1)/dt_in : 0.0;

        w0_from_Sbar(r) = w0_from_Sbar(r-1) + 
            dr0 * Sbar_in(0,r-1) * r_cc_loc_b(0,r-1)*r_cc_loc_b(0,r-1);
        if (volume_discrepancy != 0.0) {
            w0_from_Sbar(r) -= dr0 * volume_discrepancy * r_cc_loc_b(0,r-1)*r_cc_loc_b(0,r-1) 
            / (gamma1bar_nph(r-1)*p0_nph(r-1));
        }
    }

    // for (auto r = 1; r <= nr_fine; ++r) {
    int lo = 1; 
    int hi = nr_fine;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        w0_from_Sbar(r) /= (r_edge_loc_p(0,r)*r_edge_loc_p(0,r));
    });

    // make the edge-centered gravity
    MakeGravEdge(grav_edge, rho0_nph);

    // NOTE:  now we solve for the remainder, (r^2 * delta w0)
    // this takes the form of a tri-diagonal matrix:
    // A_j (r^2 dw_0)_{j-3/2} +
    // B_j (r^2 dw_0)_{j-1/2} +
    // C_j (r^2 dw_0)_{j+1/2} = F_j
    A.setVal(0.0);
    B.setVal(0.0);
    C.setVal(0.0);
    F.setVal(0.0);
    u.setVal(0.0);

    // Note that we are solving for (r^2 delta w0), not just w0.

    int max_cutoff = min(base_cutoff_density_coord_loc, nr_fine-1);
    
    // for (auto r = 1; r <= max_cutoff; ++r) {
    lo = 1; 
    hi = max_cutoff;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        A(r) = gamma1bar_nph(r-1) * p0_nph(r-1) / (r_cc_loc_p(0,r-1)*r_cc_loc_p(0,r-1));
        A(r) /= dr0*dr0;

        B(r) = -( gamma1bar_nph(r-1) * p0_nph(r-1) / (r_cc_loc_p(0,r-1)*r_cc_loc_p(0,r-1))
                + gamma1bar_nph(r) * p0_nph(r) / (r_cc_loc_p(0,r)*r_cc_loc_p(0,r)) ) 
                / (dr0*dr0);

        Real dpdr = (p0_nph(r) - p0_nph(r-1)) / dr0;

        B(r) -= 4.0 * dpdr / (r_edge_loc_p(0,r)*r_edge_loc_p(0,r)*r_edge_loc_p(0,r));

        C(r) = gamma1bar_nph(r) * p0_nph(r) / (r_cc_loc_p(0,r)*r_cc_loc_p(0,r));
        C(r) /= dr0*dr0;

        F(r) = 4.0 * dpdr * w0_from_Sbar(r) / r_edge_loc_p(0,r) - 
                grav_edge(0,r) * (r_cc_loc_p(0,r)*r_cc_loc_p(0,r) * etarho_cc_p(0,r) - 
                r_cc_loc_p(0,r-1)*r_cc_loc_p(0,r-1) * etarho_cc_p(0,r-1)) / 
                (dr0 * r_edge_loc_p(0,r)*r_edge_loc_p(0,r)) - 
                4.0 * M_PI * Gconst * 0.5 * 
                (rho0_nph(0,r) + rho0_nph(0,r-1)) * etarho_ec_p(0,r);
    });

    // Lower boundary
    A[0] = 0.0;
    B[0] = 1.0;
    C[0] = 0.0;
    F[0] = 0.0;

    // Upper boundary
    A[max_cutoff+1] = -1.0;
    B[max_cutoff+1] = 1.0;
    C[max_cutoff+1] = 0.0;
    F[max_cutoff+1] = 0.0;

    // need to synchronize gpu values with updated host values
    Gpu::synchronize();
    
    // Call the tridiagonal solver
    Tridiag(A, B, C, F, u, max_cutoff+2);

    w0(0,0) = w0_from_Sbar(0);

    // for (auto r = 1; r <= max_cutoff+1; ++r) {
    lo = 1; 
    hi = max_cutoff+1;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        w0_p(0,r) = u(r) / (r_edge_loc_p(0,r)*r_edge_loc_p(0,r)) + w0_from_Sbar(r);
    });

    // for (auto r = max_cutoff+2; r <= nr_fine; ++r) {
    lo = max_cutoff+2; 
    hi = nr_fine;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        w0_p(0,r) = w0_p(0,max_cutoff+1) * r_edge_loc_p(0,max_cutoff+1)*r_edge_loc_p(0,max_cutoff+1)/(r_edge_loc_p(0,r)*r_edge_loc_p(0,r));
    });

    // Compute the forcing term in the base state velocity equation, - 1/rho0 grad pi0
    const Real dt_avg = 0.5 * (dt_in + dtold_in);

    // for (auto r = 0; r < nr_fine; ++r) {
    AMREX_PARALLEL_FOR_1D(nr_fine, r, {
        Real w0_old_cen = 0.5 * (w0_old(0,r) + w0_old(0,r+1));
        Real w0_new_cen = 0.5 * (w0_p(0,r) + w0_p(0,r+1));
        Real w0_avg = 0.5 * (dt_in *  w0_old_cen + dtold_in *  w0_new_cen) / dt_avg;
        Real div_avg = 0.5 * (dt_in * (w0_old(0,r+1)-w0_old(0,r)) + dtold_in * (w0_p(0,r+1)-w0_p(0,r))) / dt_avg;
        w0_force(0,r) = (w0_new_cen-w0_old_cen) / dt_avg + w0_avg * div_avg / dr0;
    });
}

void 
Maestro::Makew0SphrIrreg(const BaseState<Real>& w0_old, 
                        BaseState<Real>& w0_force, 
                        const BaseState<Real>& Sbar_in, 
                        const BaseState<Real>& rho0_old_in,
                        const BaseState<Real>& rho0_new_in,
                        const BaseState<Real>& p0_old_in,
                        const BaseState<Real>& p0_new_in,
                        const BaseState<Real>& gamma1bar_old_in,
                        const BaseState<Real>& gamma1bar_new_in,
                        const BaseState<Real>& p0_minus_peosbar,  
                        const Real dt_in, const Real dtold_in) 
{
    // timer for profiling
    BL_PROFILE_VAR("Maestro::Makew0SphrIrreg()",Makew0SphrIrreg);

    // local variables 
    const int max_lev = max_radial_level+1;
    BaseState<Real> gamma1bar_nph(nr_fine);
    BaseState<Real> p0_nph(nr_fine);
    BaseState<Real> A(nr_fine+1);
    BaseState<Real> B(nr_fine+1);
    BaseState<Real> C(nr_fine+1);
    BaseState<Real> u(nr_fine+1);
    BaseState<Real> F(nr_fine+1);
    BaseState<Real> w0_from_Sbar(nr_fine+1);
    BaseState<Real> rho0_nph(max_lev,nr_fine);
    BaseState<Real> grav_edge(max_lev,nr_fine+1);

    const auto& p0_old_p = p0_old_in;
    const auto& p0_new_p = p0_new_in;
    const auto& r_cc_loc_p = r_cc_loc_b;
    const auto& r_edge_loc_p = r_edge_loc_b;
    const auto& etarho_cc_p = etarho_cc;
    const auto& etarho_ec_p = etarho_ec;
    auto& w0_p = w0;

    Real base_cutoff_dens = 0.0;
    get_base_cutoff_density(&base_cutoff_dens);
    int base_cutoff_density_coord_loc = base_cutoff_density_coord(0);
    const Real dpdt_factor_loc = dpdt_factor;

    // create time-centered base-state quantities
    // for (auto r = 0; r < nr_fine; ++r) {
    AMREX_PARALLEL_FOR_1D(nr_fine, r, {
        p0_nph(r) = 0.5*(p0_old_p(0,r) + p0_new_p(0,r));
        rho0_nph(r) = 0.5*(rho0_old_in(0,r) + rho0_new_in(0,r));
        gamma1bar_nph(r) = 0.5*(gamma1bar_old_in(0,r) + gamma1bar_new_in(0,r));
    });

    // NOTE: We first solve for the w0 resulting only from Sbar,
    //      w0_from_sbar by integrating d/dr (r^2 w0_from_sbar) =
    //      (r^2 Sbar).  Then we will solve for the update, delta w0.
    w0_from_Sbar(0) = 0.0;

    for (auto r = 1; r <= nr_fine; ++r) {
        Real volume_discrepancy = rho0_old_in(0,r-1) > base_cutoff_dens ? 
            dpdt_factor_loc * p0_minus_peosbar(0,r-1)/dt_in : 0.0;

        Real dr1 = r_edge_loc_b(0,r) - r_edge_loc_b(0,r-1);
        w0_from_Sbar(r) = w0_from_Sbar(r-1) + 
            dr1 * Sbar_in(0,r-1) * r_cc_loc_b(0,r-1)*r_cc_loc_b(0,r-1) - 
            dr1* volume_discrepancy * r_cc_loc_b(0,r-1)*r_cc_loc_b(0,r-1) 
            / (gamma1bar_nph(r-1)*p0_nph(r-1));
    }

    for (auto r = 1; r <= nr_fine; ++r) {
        w0_from_Sbar(r) /= (r_edge_loc_b(0,r)*r_edge_loc_b(0,r));
    }

    // make the edge-centered gravity
    MakeGravEdge(grav_edge, rho0_nph);

    // NOTE:  now we solve for the remainder, (r^2 * delta w0)
    // this takes the form of a tri-diagonal matrix:
    // A_j (r^2 dw_0)_{j-3/2} +
    // B_j (r^2 dw_0)_{j-1/2} +
    // C_j (r^2 dw_0)_{j+1/2} = F_j
    A.setVal(0.0);
    B.setVal(0.0);
    C.setVal(0.0);
    u.setVal(0.0);
    F.setVal(0.0);

    // Note that we are solving for (r^2 delta w0), not just w0.
    int max_cutoff = base_cutoff_density_coord_loc;
    
    // for (auto r = 1; r <= max_cutoff; ++r) {
    int lo = 1; 
    int hi = max_cutoff;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        Real dr1 = r_edge_loc_p(0,r) - r_edge_loc_p(0,r-1);
        Real dr2 = r_edge_loc_p(0,r+1) - r_edge_loc_p(0,r);
        Real dr3 = r_cc_loc_p(0,r) - r_cc_loc_p(0,r-1);

        A(r) = gamma1bar_nph(r-1) * p0_nph(r-1) / (r_cc_loc_p(0,r-1)*r_cc_loc_p(0,r-1));
        A(r) /= dr1*dr3;

        B(r) = -( gamma1bar_nph(r-1) * p0_nph(r-1) / (r_cc_loc_p(0,r-1)*r_cc_loc_p(0,r-1)*dr1) 
                + gamma1bar_nph(r) * p0_nph(r) / (r_cc_loc_p(0,r)*r_cc_loc_p(0,r)*dr2) ) 
                / dr3;

        Real dpdr = (p0_nph(r) - p0_nph(r-1)) / dr3;

        B(r) -= 4.0 * dpdr / (r_edge_loc_p(0,r)*r_edge_loc_p(0,r)*r_edge_loc_p(0,r));

        C(r) = gamma1bar_nph(r) * p0_nph(r) / (r_cc_loc_p(0,r)*r_cc_loc_p(0,r));
        C(r) /= dr2*dr3;

        F(r) = 4.0 * dpdr * w0_from_Sbar(r) / r_edge_loc_p(0,r) - 
                grav_edge(0,r) * (r_cc_loc_p(0,r)*r_cc_loc_p(0,r) * etarho_cc_p(0,r) - 
                r_cc_loc_p(0,r-1)*r_cc_loc_p(0,r-1) * etarho_cc_p(0,r-1)) / 
                (dr3 * r_edge_loc_p(0,r)*r_edge_loc_p(0,r)) - 
                4.0 * M_PI * Gconst * 0.5 * 
                (rho0_nph(0,r) + rho0_nph(0,r-1)) * etarho_ec_p(0,r);
    });

    // Lower boundary
    A[0] = 0.0;
    B[0] = 1.0;
    C[0] = 0.0;
    F[0] = 0.0;

    // Upper boundary
    A[max_cutoff+1] = -1.0;
    B[max_cutoff+1] = 1.0;
    C[max_cutoff+1] = 0.0;
    F[max_cutoff+1] = 0.0;
    
    // need to synchronize gpu values with updated host values
    Gpu::synchronize();
    
    // Call the tridiagonal solver
    Tridiag(A, B, C, F, u, max_cutoff+2);

    w0(0,0) = w0_from_Sbar(0);

    // for (auto r = 1; r <= max_cutoff+1; ++r) {
    lo = 1; 
    hi = max_cutoff+1;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        w0_p(0,r) = u(r) / (r_edge_loc_p(0,r)*r_edge_loc_p(0,r)) + w0_from_Sbar(r);
    });

    // for (auto r = max_cutoff+2; r <= nr_fine; ++r) {
    lo = max_cutoff+2; 
    hi = nr_fine;
    AMREX_PARALLEL_FOR_1D(hi-lo+1, j, {
        int r = j + lo;
        w0_p(0,r) = w0_p(0,max_cutoff+1) * r_edge_loc_p(0,max_cutoff+1)*r_edge_loc_p(0,max_cutoff+1)/(r_edge_loc_p(0,r)*r_edge_loc_p(0,r));
    });

    // Compute the forcing term in the base state velocity equation, - 1/rho0 grad pi0
    const Real dt_avg = 0.5 * (dt_in + dtold_in);

    // for (auto r = 0; r < nr_fine; ++r) {
    AMREX_PARALLEL_FOR_1D(nr_fine, r, {
        Real dr1 = r_edge_loc_p(0,r) - r_edge_loc_p(0,r-1);
        Real w0_old_cen = 0.5 * (w0_old(0,r) + w0_old(0,r+1));
        Real w0_new_cen = 0.5 * (w0_p(0,r) + w0_p(0,r+1));
        Real w0_avg = 0.5 * (dt_in *  w0_old_cen + dtold_in *  w0_new_cen) / dt_avg;
        Real div_avg = 0.5 * (dt_in * (w0_old(0,r+1)-w0_old(0,r)) + dtold_in * (w0_p(0,r+1)-w0_p(0,r))) / dt_avg;
        w0_force(0,r) = (w0_new_cen-w0_old_cen) / dt_avg + w0_avg * div_avg / dr1;
    });
}


void
Maestro::Tridiag(const BaseState<Real>& a, const BaseState<Real>& b, 
                 const BaseState<Real>& c, const BaseState<Real>& r, 
                 BaseState<Real>& u, const int n)
{
    BaseState<Real> gam(n);

    if (b[0] == 0) Abort("tridiag: CANT HAVE B[0] = 0.0");

    Real bet = b[0];
    u[0] = r[0] / bet;

    for (auto j = 1; j < n; j++) {
        gam[j] = c[j-1] / bet;
        bet = b[j] - a[j] * gam[j];
        if (bet == 0) Abort("tridiag: TRIDIAG FAILED");
        u[j] = (r[j] - a[j] * u[j-1]) / bet;
    }

    for (auto j = n-2; j >= 0; --j) {
        u[j] -= gam[j+1] * u[j+1];
    }
}

void
Maestro::ProlongBasetoUniform(const RealVector& base_ml, 
                              RealVector& base_fine)

{
    // the mask array will keep track of whether we've filled in data
    // in a corresponding radial bin.  .false. indicates that we've
    // already output there.
    IntVector imask_fine(nr_fine);
    std::fill(imask_fine.begin(), imask_fine.end(), 1);

    // r1 is the factor between the current level grid spacing and the
    // FINEST level
    int r1 = 1;

    const int max_lev = max_radial_level+1;

    for (auto n = finest_radial_level; n >= 0; --n) {
        for (auto j = 1; j < numdisjointchunks(n); ++j) {
            for (auto r = r_start_coord(n,j); r <= r_end_coord(n,j); ++r) {
                // sum up mask to see if there are any elements set to true 
                if (std::accumulate(imask_fine.begin()+r*r1-1, imask_fine.begin()+(r+1)*r1-1, 0) > 0) {
                    for (auto i = r*r1-1; i < (r+1)*r1-1; ++r) {
                        base_fine[i] = base_ml[n+max_lev*r];
                        imask_fine[i] = 0;
                    }
                }
            }
        }
        // update r1 for the next coarsest level -- assume a jump by
        // factor of 2
        r1 *= 2;
    }
    
    // check to make sure that no mask values are still true
    if (std::accumulate(imask_fine.begin(), imask_fine.end(), 0) > 0) {
        Abort("ERROR: unfilled cells in prolong_base_to_uniform");
    }
}


void
Maestro::ProlongBasetoUniform(const RealVector& base_ml, 
                              BaseState<Real>& base_fine)

{
    // the mask array will keep track of whether we've filled in data
    // in a corresponding radial bin.  .false. indicates that we've
    // already output there.
    IntVector imask_fine(nr_fine);
    std::fill(imask_fine.begin(), imask_fine.end(), 1);

    // r1 is the factor between the current level grid spacing and the
    // FINEST level
    int r1 = 1;

    const int max_lev = max_radial_level+1;

    for (auto n = finest_radial_level; n >= 0; --n) {
        for (auto j = 1; j < numdisjointchunks(n); ++j) {
            for (auto r = r_start_coord(n,j); r <= r_end_coord(n,j); ++r) {
                // sum up mask to see if there are any elements set to true 
                if (std::accumulate(imask_fine.begin()+r*r1-1, imask_fine.begin()+(r+1)*r1-1, 0) > 0) {
                    for (auto i = r*r1-1; i < (r+1)*r1-1; ++r) {
                        base_fine[i] = base_ml[n+max_lev*r];
                        imask_fine[i] = 0;
                    }
                }
            }
        }
        // update r1 for the next coarsest level -- assume a jump by
        // factor of 2
        r1 *= 2;
    }
    
    // check to make sure that no mask values are still true
    if (std::accumulate(imask_fine.begin(), imask_fine.end(), 0) > 0) {
        Abort("ERROR: unfilled cells in prolong_base_to_uniform");
    }
}

void
Maestro::ProlongBasetoUniform(const BaseState<Real>& base_ml, 
                              BaseState<Real>& base_fine)

{
    // the mask array will keep track of whether we've filled in data
    // in a corresponding radial bin.  .false. indicates that we've
    // already output there.
    IntVector imask_fine(nr_fine);
    std::fill(imask_fine.begin(), imask_fine.end(), 1);

    // r1 is the factor between the current level grid spacing and the
    // FINEST level
    int r1 = 1;

    const int max_lev = max_radial_level+1;

    for (auto n = finest_radial_level; n >= 0; --n) {
        for (auto j = 1; j < numdisjointchunks(n); ++j) {
            for (auto r = r_start_coord(n,j); r <= r_end_coord(n,j); ++r) {
                // sum up mask to see if there are any elements set to true 
                if (std::accumulate(imask_fine.begin()+r*r1-1, imask_fine.begin()+(r+1)*r1-1, 0) > 0) {
                    for (auto i = r*r1-1; i < (r+1)*r1-1; ++r) {
                        base_fine[i] = base_ml(n,r);
                        imask_fine[i] = 0;
                    }
                }
            }
        }
        // update r1 for the next coarsest level -- assume a jump by
        // factor of 2
        r1 *= 2;
    }
    
    // check to make sure that no mask values are still true
    if (std::accumulate(imask_fine.begin(), imask_fine.end(), 0) > 0) {
        Abort("ERROR: unfilled cells in prolong_base_to_uniform");
    }
}