
#include <Maestro.H>

using namespace amrex;

void
Maestro::DensityAdvance (bool is_predictor,
                         Vector<MultiFab>& scalold,
                         Vector<MultiFab>& scalnew,
                         Vector<std::array< MultiFab, AMREX_SPACEDIM > >& sedge,
                         Vector<std::array< MultiFab, AMREX_SPACEDIM > >& sflux,
                         Vector<MultiFab>& scal_force,
                         Vector<std::array< MultiFab, AMREX_SPACEDIM > >& umac)
{
    Vector<Real> rho0_edge_old( (max_radial_level+1)*(nr_fine+1) );
    Vector<Real> rho0_edge_new( (max_radial_level+1)*(nr_fine+1) );

    if (spherical == 0) {
        // create edge-centered base state quantities.
        // Note: rho0_edge_{old,new} 
        // contains edge-centered quantities created via spatial interpolation.
        // This is to be contrasted to rho0_predicted_edge which is the half-time
        // edge state created in advect_base. 
        cell_to_edge(rho0_old.dataPtr(),rho0_edge_old.dataPtr());
        cell_to_edge(rho0_new.dataPtr(),rho0_edge_new.dataPtr());
    }

    //////////////////////////////////
    // Create source terms at time n
    //////////////////////////////////

    // source terms for X and for tracers are zero - do nothing
    for (int lev=0; lev<=finest_level; ++lev) {
        scal_force[lev].setVal(0.);
    }

    if (spherical == 1) {

    }

    // ** density source term **

    // Make source term for rho or rho' 
    if (species_pred_type == predict_rhoprime_and_X) {
        // rho' souce term
        // this is needed for pred_rhoprime_and_X
        ModifyScalForce(scal_force,umac,rho0_old,rho0_edge_old,Rho,bcs_s,0);

    }
    else if (species_pred_type == predict_rho_and_X) {
        // rho source term
        ModifyScalForce(scal_force,umac,rho0_old,rho0_edge_old,Rho,bcs_s,1);

    }

    // ** species source term **

    // for species_pred_types predict_rhoprime_and_X and
    // predict_rho_and_X, there is no force for X.

    // for predict_rhoX, we are predicting (rho X)
    // as a conservative equation, and there is no force.


    /////////////////////////////////////////////////////////////////
    // Add w0 to MAC velocities (trans velocities already have w0).
    /////////////////////////////////////////////////////////////////

    Addw0(umac,1.);
    
    /////////////////////////////////////////////////////////////////
    // Create the edge states of (rho X)' or X and rho'
    /////////////////////////////////////////////////////////////////

    if ((species_pred_type == predict_rhoprime_and_X) ||
        (species_pred_type == predict_rho_and_X)) {

        // we are predicting X to the edges, so convert the scalar
        // data to those quantities

        // convert (rho X) --> X in scalold 
	ConvertRhoXToX(scalold,true);
    }

    if (species_pred_type == predict_rhoprime_and_X) {
        // convert rho -> rho' in scalold
        //   . this is needed for predict_rhoprime_and_X
	PutInPertForm(scalold, rho0_old, Rho, true);
    }

    // predict species at the edges -- note, either X or (rho X) will be
    // predicted here, depending on species_pred_type

    int is_vel = 0; // false
    if (species_pred_type == predict_rhoprime_and_X) {

	// we are predicting X to the edges, using the advective form of
	// the prediction
	// call make_edge_scal(sold,sedge,umac,scal_force, &
	//                     dx,dt,is_vel,the_bc_level, &
	//                     spec_comp,dm+spec_comp,nspec,.false.,mla)
	MakeEdgeScal(scalold,sedge,umac,scal_force,is_vel,bcs_s,Nscal,FirstSpec,FirstSpec,NumSpec,0);
    }
    
    // predict rho or rho' at the edges (depending on species_pred_type)
    if (species_pred_type == predict_rhoprime_and_X) {
	// call make_edge_scal(sold,sedge,umac,scal_force, &
	//                     dx,dt,is_vel,the_bc_level, &
	//                     rho_comp,dm+rho_comp,1,.false.,mla)
	MakeEdgeScal(scalold,sedge,umac,scal_force,is_vel,bcs_s,Nscal,Rho,Rho,1,0);
    }

    if (species_pred_type == predict_rhoprime_and_X) {
	// convert rho' -> rho in scalold 
	PutInPertForm(scalold, rho0_old, Rho, false);
    }

    if ((species_pred_type == predict_rhoprime_and_X) ||
        (species_pred_type == predict_rho_and_X)) {
	// convert X --> (rho X) in scalold 
	ConvertRhoXToX(scalold,false);
    }


    /////////////////////////////////////////////////////////////////
    // Subtract w0 from MAC velocities.
    /////////////////////////////////////////////////////////////////

    Addw0(umac,-1.);


}
