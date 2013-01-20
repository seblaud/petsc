
#include <petsc-private/vecimpl.h>      /*I "petscvec.h" I*/

#undef __FUNCT__
#define __FUNCT__ "VecCreate"
/*@
  VecCreate - Creates an empty vector object. The type can then be set with VecSetType(),
  or VecSetFromOptions().

   If you never  call VecSetType() or VecSetFromOptions() it will generate an
   error when you try to use the vector.

  Collective on MPI_Comm

  Input Parameter:
. comm - The communicator for the vector object

  Output Parameter:
. vec  - The vector object

  Level: beginner

.keywords: vector, create
.seealso: VecSetType(), VecSetSizes(), VecCreateMPIWithArray(), VecCreateMPI(), VecDuplicate(),
          VecDuplicateVecs(), VecCreateGhost(), VecCreateSeq(), VecPlaceArray()
@*/
PetscErrorCode  VecCreate(MPI_Comm comm, Vec *vec)
{
  Vec            v;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  PetscValidPointer(vec,2);
  *vec = PETSC_NULL;
#if !defined(PETSC_USE_DYNAMIC_LIBRARIES)
  ierr = VecInitializePackage(PETSC_NULL);CHKERRQ(ierr);
#endif

  ierr = PetscHeaderCreate(v, _p_Vec, struct _VecOps, VEC_CLASSID, -1, "Vec", "Vector", "Vec", comm, VecDestroy, VecView);CHKERRQ(ierr);
  ierr = PetscMemzero(v->ops, sizeof(struct _VecOps));CHKERRQ(ierr);

  ierr            = PetscLayoutCreate(comm,&v->map);CHKERRQ(ierr);
  v->array_gotten = PETSC_FALSE;
  v->petscnative  = PETSC_FALSE;

  *vec = v;
  PetscFunctionReturn(0);
}

