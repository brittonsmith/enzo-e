// See LICENSE_CELLO file for license and copyright information

/// @file     enzo_EnzoMethodComovingExpansion.cpp
/// @author   Britton Smith (bds006@ucsd.edu)
/// @date     Wed May 24 12:25:56 PDT 2017
/// @brief    Implements comoving expansion class

#include "cello.hpp"
#include "enzo.hpp"

//----------------------------------------------------------------------

EnzoMethodComovingExpansion::EnzoMethodComovingExpansion
(
 const FieldDescr * field_descr,
 bool comoving_coordinates
) 
  : Method(),
    comoving_coordinates_(comoving_coordinates)
{
  const int ir = add_refresh(4,0,neighbor_leaf,sync_barrier,
			     enzo_sync_id_comoving_expansion);
  refresh(ir)->add_all_fields();
  if ( ! comoving_coordinates_ ) {
    WARNING
      ("EnzoMethodComovingExpansion::EnzoMethodComovingExpansion()",
       "Including \"comoving_expansion\" method but cosmology is disabled");
  }
}

//----------------------------------------------------------------------

void EnzoMethodComovingExpansion::compute ( Block * block) throw()
{
  EnzoBlock * enzo_block = static_cast<EnzoBlock*> (block);
  Field field = enzo_block->data()->field();

  /* Only do this if
     1. this is a leaf block
     2. we are using comoving coordinates
     3. baryon fields are present.
  */

  if ((block->is_leaf() &&
       comoving_coordinates_ &&
       field.field_count() > 0))
    {

      EnzoPhysicsCosmology * cosmology = (EnzoPhysicsCosmology * )
  	block->simulation()->problem()->physics("cosmology");

      ASSERT ("EnzoMethodComovingExpansion::compute()",
  	      "comoving_coordinates enabled but missing EnzoPhysicsCosmology",
  	      ! (comoving_coordinates_ && (cosmology == NULL)) );

      /* Compute adot/a at time = t-1/2dt (time-centered). */

      int has_history = ((field.num_history() > 0) &&
  			 (field.history_time(1) > 0.));

      enzo_float compute_time;
      if (has_history) {
  	compute_time = 0.5 * (enzo_block->time() +
  			      field.history_time(1));
      }
      else {
  	compute_time = enzo_block->time();
      }

      enzo_float a, dadt;
      cosmology->compute_expansion_factor (&a, &dadt, compute_time);
      enzo_float Coefficient = block->dt()*dadt/a;

      /* Determine the size of the block. */

      int mx, my, mz, m, rank;
      field.dimensions(0,&mx,&my,&mz);
      m = mx*my*mz;
      rank = enzo_block->rank();

      /* If we can, compute the pressure at the mid-point.
  	 We can, because we will always have an old baryon field now. */
      const int in = cello::index_static();
      enzo_float * pressure = new enzo_float[m];
      int rval;

      if (EnzoBlock::DualEnergyFormalism[in]) {
  	rval = enzo_block->ComputePressureDualEnergyFormalism
	  (compute_time, pressure, comoving_coordinates_);
      }
      else{
  	rval = enzo_block->ComputePressure
	  (compute_time, pressure, comoving_coordinates_);
      }
      if (rval == ENZO_FAIL) {
  	fprintf(stderr,
  		"Error in ComputePressureDualEnergyFormalism or "
		"ComputePressure.\n");
  	exit(ENZO_FAIL);
      }

      // hard-code hydromethod for PPM for now
      int HydroMethod = 0;

      // hard-code CR method to off
      int CRModel = 0;
      enzo_float * cr_field_new = NULL;
      enzo_float * cr_field_old = NULL;

      /* Get the necessary fields.
  	 field.values(<field_name>, 0) is the field at the current time.
  	 field.values(<field_name>, 1) is the field at the previous time.
      */

      int i_new = 0;
      int i_old = has_history ? 1 : 0;

      enzo_float * density_new         =
  	(enzo_float *) field.values("density", i_new);
      enzo_float * density_old         =
  	(enzo_float *) field.values("density", i_old);
      enzo_float * total_energy_new    =
  	(enzo_float *) field.values("total_energy", i_new);
      enzo_float * total_energy_old    =
  	(enzo_float *) field.values("total_energy", i_old);
      enzo_float * internal_energy_new =
  	(enzo_float *) field.values("internal_energy", i_new);
      enzo_float * internal_energy_old =
  	(enzo_float *) field.values("internal_energy", i_old);

      enzo_float * velocity_x_new = NULL;
      enzo_float * velocity_y_new = NULL;
      enzo_float * velocity_z_new = NULL;
      enzo_float * velocity_x_old = NULL;
      enzo_float * velocity_y_old = NULL;
      enzo_float * velocity_z_old = NULL;

      velocity_x_new   = (enzo_float *) field.values("velocity_x", i_new);
      velocity_x_old   = (enzo_float *) field.values("velocity_x", i_old);
      if (rank >= 2) {
  	velocity_y_new = (enzo_float *) field.values("velocity_y", i_new);
  	velocity_y_old = (enzo_float *) field.values("velocity_y", i_old);
      }
      if (rank >= 3) {
  	velocity_z_new = (enzo_float *) field.values("velocity_z", i_new);
  	velocity_z_old = (enzo_float *) field.values("velocity_z", i_old);
      }

      /* Call fortran routine to do the real work. */

      FORTRAN_NAME(expand_terms)
	(
	 &rank, &m, &EnzoBlock::DualEnergyFormalism[in], &Coefficient,
	 (int*) &HydroMethod, &EnzoBlock::Gamma[in],
	 pressure,
	 density_new, total_energy_new, internal_energy_new,
	 velocity_x_new, velocity_y_new, velocity_z_new,
	 density_old, total_energy_old, internal_energy_old,
	 velocity_x_old, velocity_y_old, velocity_z_old,
	 &CRModel, cr_field_new, cr_field_old);

    }

  block->compute_done(); 
}

//----------------------------------------------------------------------

double EnzoMethodComovingExpansion::timestep( Block * block ) const throw()
{

  enzo_float dtExpansion = ENZO_HUGE_VAL;

  if (!comoving_coordinates_)
    return (double) dtExpansion;

  EnzoPhysicsCosmology * cosmology = (EnzoPhysicsCosmology * )
    block->simulation()->problem()->physics("cosmology");

  ASSERT ("EnzoMethodComovingExpansion::timestep()",
	  "comoving_coordinates enabled but missing EnzoPhysicsCosmology",
	  ! (comoving_coordinates_ && (cosmology == NULL)) );

  EnzoBlock * enzo_block = static_cast<EnzoBlock*> (block);

  cosmology->compute_expansion_timestep(&dtExpansion,
                                        (enzo_float) enzo_block->time());

  return (double) dtExpansion;

}

//----------------------------------------------------------------------

void EnzoMethodComovingExpansion::pup (PUP::er &p)
{
  TRACEPUP;
  // NOTE: change this function whenever attributes change

  Method::pup(p);

  p | comoving_coordinates_;
}

//======================================================================