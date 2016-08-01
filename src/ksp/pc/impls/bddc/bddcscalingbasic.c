#include <../src/ksp/pc/impls/bddc/bddc.h>
#include <../src/ksp/pc/impls/bddc/bddcprivate.h>
#include <petscblaslapack.h>

/* prototypes for deluxe functions */
static PetscErrorCode PCBDDCScalingCreate_Deluxe(PC);
static PetscErrorCode PCBDDCScalingDestroy_Deluxe(PC);
static PetscErrorCode PCBDDCScalingSetUp_Deluxe(PC);
static PetscErrorCode PCBDDCScalingSetUp_Deluxe_Private(PC);
static PetscErrorCode PCBDDCScalingReset_Deluxe_Solvers(PCBDDCDeluxeScaling);

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingExtension_Basic"
static PetscErrorCode PCBDDCScalingExtension_Basic(PC pc, Vec local_interface_vector, Vec global_vector)
{
  PC_IS*         pcis = (PC_IS*)pc->data;
  PC_BDDC*       pcbddc = (PC_BDDC*)pc->data;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  /* Apply partition of unity */
  ierr = VecPointwiseMult(pcbddc->work_scaling,pcis->D,local_interface_vector);CHKERRQ(ierr);
  ierr = VecSet(global_vector,0.0);CHKERRQ(ierr);
  ierr = VecScatterBegin(pcis->global_to_B,pcbddc->work_scaling,global_vector,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
  ierr = VecScatterEnd(pcis->global_to_B,pcbddc->work_scaling,global_vector,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingExtension_Deluxe"
static PetscErrorCode PCBDDCScalingExtension_Deluxe(PC pc, Vec x, Vec y)
{
  PC_IS*              pcis=(PC_IS*)pc->data;
  PC_BDDC*            pcbddc=(PC_BDDC*)pc->data;
  PCBDDCDeluxeScaling deluxe_ctx = pcbddc->deluxe_ctx;
  PetscErrorCode      ierr;

  PetscFunctionBegin;
  ierr = VecSet(pcbddc->work_scaling,0.0);CHKERRQ(ierr);
  ierr = VecSet(y,0.0);CHKERRQ(ierr);
  if (deluxe_ctx->n_simple) { /* scale deluxe vertices using diagonal scaling */
    PetscInt          i;
    const PetscScalar *array_x,*array_D;
    PetscScalar       *array;
    ierr = VecGetArrayRead(x,&array_x);CHKERRQ(ierr);
    ierr = VecGetArrayRead(pcis->D,&array_D);CHKERRQ(ierr);
    ierr = VecGetArray(pcbddc->work_scaling,&array);CHKERRQ(ierr);
    for (i=0;i<deluxe_ctx->n_simple;i++) {
      array[deluxe_ctx->idx_simple_B[i]] = array_x[deluxe_ctx->idx_simple_B[i]]*array_D[deluxe_ctx->idx_simple_B[i]];
    }
    ierr = VecRestoreArray(pcbddc->work_scaling,&array);CHKERRQ(ierr);
    ierr = VecRestoreArrayRead(pcis->D,&array_D);CHKERRQ(ierr);
    ierr = VecRestoreArrayRead(x,&array_x);CHKERRQ(ierr);
  }
  /* sequential part : all problems and Schur applications collapsed into a single matrix vector multiplication or a matvec and a solve */
  if (deluxe_ctx->seq_mat) {
    PetscInt i;
    for (i=0;i<deluxe_ctx->seq_n;i++) {
      if (deluxe_ctx->change) {
        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],x,deluxe_ctx->seq_work2[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],x,deluxe_ctx->seq_work2[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
        if (deluxe_ctx->change_with_qr) {
          Mat change;

          ierr = KSPGetOperators(deluxe_ctx->change[i],&change,NULL);CHKERRQ(ierr);
          ierr = MatMultTranspose(change,deluxe_ctx->seq_work2[i],deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
        } else {
          ierr = KSPSolve(deluxe_ctx->change[i],deluxe_ctx->seq_work2[i],deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
        }
      } else {
        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],x,deluxe_ctx->seq_work1[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],x,deluxe_ctx->seq_work1[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
      }
      ierr = MatMultTranspose(deluxe_ctx->seq_mat[i],deluxe_ctx->seq_work1[i],deluxe_ctx->seq_work2[i]);CHKERRQ(ierr);
      if (deluxe_ctx->seq_mat_inv_sum[i]) {
        PetscScalar *x;

        ierr = VecGetArray(deluxe_ctx->seq_work2[i],&x);CHKERRQ(ierr);
        ierr = VecPlaceArray(deluxe_ctx->seq_work1[i],x);CHKERRQ(ierr);
        ierr = VecRestoreArray(deluxe_ctx->seq_work2[i],&x);CHKERRQ(ierr);
        ierr = MatSolveTranspose(deluxe_ctx->seq_mat_inv_sum[i],deluxe_ctx->seq_work1[i],deluxe_ctx->seq_work2[i]);CHKERRQ(ierr);
        ierr = VecResetArray(deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
      }
      if (deluxe_ctx->change) {
        Mat change;

        ierr = KSPGetOperators(deluxe_ctx->change[i],&change,NULL);CHKERRQ(ierr);
        ierr = MatMult(change,deluxe_ctx->seq_work2[i],deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work1[i],pcbddc->work_scaling,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work1[i],pcbddc->work_scaling,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
      } else {
        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work2[i],pcbddc->work_scaling,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work2[i],pcbddc->work_scaling,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
      }
    }
  }
  /* put local boundary part in global vector */
  ierr = VecScatterBegin(pcis->global_to_B,pcbddc->work_scaling,y,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
  ierr = VecScatterEnd(pcis->global_to_B,pcbddc->work_scaling,y,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingExtension"
PetscErrorCode PCBDDCScalingExtension(PC pc, Vec local_interface_vector, Vec global_vector)
{
  PC_BDDC        *pcbddc=(PC_BDDC*)pc->data;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  PetscValidHeaderSpecific(pc,PC_CLASSID,1);
  PetscValidHeaderSpecific(local_interface_vector,VEC_CLASSID,2);
  PetscValidHeaderSpecific(global_vector,VEC_CLASSID,3);
  if (local_interface_vector == pcbddc->work_scaling) SETERRQ(PETSC_COMM_SELF,PETSC_ERR_SUP,"Local vector cannot be pcbddc->work_scaling!\n");
  ierr = PetscUseMethod(pc,"PCBDDCScalingExtension_C",(PC,Vec,Vec),(pc,local_interface_vector,global_vector));CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingRestriction_Basic"
static PetscErrorCode PCBDDCScalingRestriction_Basic(PC pc, Vec global_vector, Vec local_interface_vector)
{
  PetscErrorCode ierr;
  PC_IS          *pcis = (PC_IS*)pc->data;

  PetscFunctionBegin;
  ierr = VecScatterBegin(pcis->global_to_B,global_vector,local_interface_vector,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
  ierr = VecScatterEnd(pcis->global_to_B,global_vector,local_interface_vector,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
  /* Apply partition of unity */
  ierr = VecPointwiseMult(local_interface_vector,pcis->D,local_interface_vector);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingRestriction_Deluxe"
static PetscErrorCode PCBDDCScalingRestriction_Deluxe(PC pc, Vec x, Vec y)
{
  PC_IS*              pcis=(PC_IS*)pc->data;
  PC_BDDC*            pcbddc=(PC_BDDC*)pc->data;
  PCBDDCDeluxeScaling deluxe_ctx = pcbddc->deluxe_ctx;
  PetscErrorCode      ierr;

  PetscFunctionBegin;
  /* get local boundary part of global vector */
  ierr = VecScatterBegin(pcis->global_to_B,x,y,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
  ierr = VecScatterEnd(pcis->global_to_B,x,y,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
  if (deluxe_ctx->n_simple) { /* scale deluxe vertices using diagonal scaling */
    PetscInt          i;
    PetscScalar       *array_y;
    const PetscScalar *array_D;
    ierr = VecGetArray(y,&array_y);CHKERRQ(ierr);
    ierr = VecGetArrayRead(pcis->D,&array_D);CHKERRQ(ierr);
    for (i=0;i<deluxe_ctx->n_simple;i++) {
      array_y[deluxe_ctx->idx_simple_B[i]] *= array_D[deluxe_ctx->idx_simple_B[i]];
    }
    ierr = VecRestoreArrayRead(pcis->D,&array_D);CHKERRQ(ierr);
    ierr = VecRestoreArray(y,&array_y);CHKERRQ(ierr);
  }
  /* sequential part : all problems and Schur applications collapsed into a single matrix vector multiplication and ksp solution */
  if (deluxe_ctx->seq_mat) {
    PetscInt i;
    for (i=0;i<deluxe_ctx->seq_n;i++) {
      if (deluxe_ctx->change) {
        Mat change;

        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],y,deluxe_ctx->seq_work2[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],y,deluxe_ctx->seq_work2[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
        ierr = KSPGetOperators(deluxe_ctx->change[i],&change,NULL);CHKERRQ(ierr);
        ierr = MatMultTranspose(change,deluxe_ctx->seq_work2[i],deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
      } else {
        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],y,deluxe_ctx->seq_work1[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],y,deluxe_ctx->seq_work1[i],INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
      }
      if (deluxe_ctx->seq_mat_inv_sum[i]) {
        PetscScalar *x;

        ierr = VecGetArray(deluxe_ctx->seq_work1[i],&x);CHKERRQ(ierr);
        ierr = VecPlaceArray(deluxe_ctx->seq_work2[i],x);CHKERRQ(ierr);
        ierr = VecRestoreArray(deluxe_ctx->seq_work1[i],&x);CHKERRQ(ierr);
        ierr = MatSolve(deluxe_ctx->seq_mat_inv_sum[i],deluxe_ctx->seq_work1[i],deluxe_ctx->seq_work2[i]);CHKERRQ(ierr);
        ierr = VecResetArray(deluxe_ctx->seq_work2[i]);CHKERRQ(ierr);
      }
      ierr = MatMult(deluxe_ctx->seq_mat[i],deluxe_ctx->seq_work1[i],deluxe_ctx->seq_work2[i]);CHKERRQ(ierr);
      if (deluxe_ctx->change) {
        if (deluxe_ctx->change_with_qr) {
          Mat change;

          ierr = KSPGetOperators(deluxe_ctx->change[i],&change,NULL);CHKERRQ(ierr);
          ierr = MatMult(change,deluxe_ctx->seq_work2[i],deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
        } else {
          ierr = KSPSolveTranspose(deluxe_ctx->change[i],deluxe_ctx->seq_work2[i],deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
        }
        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work1[i],y,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work1[i],y,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
      } else {
        ierr = VecScatterBegin(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work2[i],y,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
        ierr = VecScatterEnd(deluxe_ctx->seq_scctx[i],deluxe_ctx->seq_work2[i],y,INSERT_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
      }
    }
  }
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingRestriction"
PetscErrorCode PCBDDCScalingRestriction(PC pc, Vec global_vector, Vec local_interface_vector)
{
  PC_BDDC        *pcbddc=(PC_BDDC*)pc->data;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  PetscValidHeaderSpecific(pc,PC_CLASSID,1);
  PetscValidHeaderSpecific(global_vector,VEC_CLASSID,2);
  PetscValidHeaderSpecific(local_interface_vector,VEC_CLASSID,3);
  if (local_interface_vector == pcbddc->work_scaling) SETERRQ(PETSC_COMM_SELF,PETSC_ERR_SUP,"Local vector cannot be pcbddc->work_scaling!\n");
  ierr = PetscUseMethod(pc,"PCBDDCScalingRestriction_C",(PC,Vec,Vec),(pc,global_vector,local_interface_vector));CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingSetUp"
PetscErrorCode PCBDDCScalingSetUp(PC pc)
{
  PC_IS*         pcis=(PC_IS*)pc->data;
  PC_BDDC*       pcbddc=(PC_BDDC*)pc->data;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  PetscValidHeaderSpecific(pc,PC_CLASSID,1);
  /* create work vector for the operator */
  ierr = VecDestroy(&pcbddc->work_scaling);CHKERRQ(ierr);
  ierr = VecDuplicate(pcis->vec1_B,&pcbddc->work_scaling);CHKERRQ(ierr);
  /* always rebuild pcis->D */
  if (pcis->use_stiffness_scaling) {
    ierr = MatGetDiagonal(pcbddc->local_mat,pcis->vec1_N);CHKERRQ(ierr);
    ierr = VecScatterBegin(pcis->N_to_B,pcis->vec1_N,pcis->D,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
    ierr = VecScatterEnd(pcis->N_to_B,pcis->vec1_N,pcis->D,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
  }
  ierr = VecCopy(pcis->D,pcis->vec1_B);CHKERRQ(ierr);
  ierr = VecSet(pcis->vec1_global,0.0);CHKERRQ(ierr);
  ierr = VecScatterBegin(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
  ierr = VecScatterEnd(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
  ierr = VecScatterBegin(pcis->global_to_B,pcis->vec1_global,pcis->vec1_B,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
  ierr = VecScatterEnd(pcis->global_to_B,pcis->vec1_global,pcis->vec1_B,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
  ierr = VecPointwiseDivide(pcis->D,pcis->D,pcis->vec1_B);CHKERRQ(ierr);
  /* now setup */
  if (pcbddc->use_deluxe_scaling) {
    if (!pcbddc->deluxe_ctx) {
      ierr = PCBDDCScalingCreate_Deluxe(pc);CHKERRQ(ierr);
    }
    ierr = PCBDDCScalingSetUp_Deluxe(pc);CHKERRQ(ierr);
    ierr = PetscObjectComposeFunction((PetscObject)pc,"PCBDDCScalingRestriction_C",PCBDDCScalingRestriction_Deluxe);CHKERRQ(ierr);
    ierr = PetscObjectComposeFunction((PetscObject)pc,"PCBDDCScalingExtension_C",PCBDDCScalingExtension_Deluxe);CHKERRQ(ierr);
  } else {
    ierr = PetscObjectComposeFunction((PetscObject)pc,"PCBDDCScalingRestriction_C",PCBDDCScalingRestriction_Basic);CHKERRQ(ierr);
    ierr = PetscObjectComposeFunction((PetscObject)pc,"PCBDDCScalingExtension_C",PCBDDCScalingExtension_Basic);CHKERRQ(ierr);
  }

  /* test */
  if (pcbddc->dbg_flag) {
    Mat         B0_B = NULL;
    Vec         B0_Bv = NULL, B0_Bv2 = NULL;
    Vec         vec2_global;
    PetscViewer viewer = pcbddc->dbg_viewer;
    PetscReal   error;

    /* extension -> from local to parallel */
    ierr = VecSet(pcis->vec1_global,0.0);CHKERRQ(ierr);
    ierr = VecSetRandom(pcis->vec1_B,NULL);CHKERRQ(ierr);
    ierr = VecScatterBegin(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
    ierr = VecScatterEnd(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
    ierr = VecDuplicate(pcis->vec1_global,&vec2_global);CHKERRQ(ierr);
    ierr = VecCopy(pcis->vec1_global,vec2_global);CHKERRQ(ierr);
    ierr = VecScatterBegin(pcis->global_to_B,pcis->vec1_global,pcis->vec1_B,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
    ierr = VecScatterEnd(pcis->global_to_B,pcis->vec1_global,pcis->vec1_B,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
    if (pcbddc->benign_n) {
      IS is_dummy;

      ierr = ISCreateStride(PETSC_COMM_SELF,pcbddc->benign_n,0,1,&is_dummy);CHKERRQ(ierr);
      ierr = MatGetSubMatrix(pcbddc->benign_B0,is_dummy,pcis->is_B_local,MAT_INITIAL_MATRIX,&B0_B);CHKERRQ(ierr);
      ierr = ISDestroy(&is_dummy);CHKERRQ(ierr);
      ierr = MatCreateVecs(B0_B,NULL,&B0_Bv);CHKERRQ(ierr);
      ierr = VecDuplicate(B0_Bv,&B0_Bv2);CHKERRQ(ierr);
      ierr = MatMult(B0_B,pcis->vec1_B,B0_Bv);CHKERRQ(ierr);
    }
    ierr = PCBDDCScalingExtension(pc,pcis->vec1_B,pcis->vec1_global);CHKERRQ(ierr);
    if (pcbddc->benign_saddle_point) {
      PetscReal errorl = 0.;
      ierr = VecScatterBegin(pcis->global_to_B,pcis->vec1_global,pcis->vec1_B,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
      ierr = VecScatterEnd(pcis->global_to_B,pcis->vec1_global,pcis->vec1_B,INSERT_VALUES,SCATTER_FORWARD);CHKERRQ(ierr);
      if (pcbddc->benign_n) {
        ierr = MatMult(B0_B,pcis->vec1_B,B0_Bv2);CHKERRQ(ierr);
        ierr = VecAXPY(B0_Bv,-1.0,B0_Bv2);CHKERRQ(ierr);
        ierr = VecNorm(B0_Bv,NORM_INFINITY,&errorl);CHKERRQ(ierr);
      }
      ierr = MPI_Allreduce(&errorl,&error,1,MPIU_REAL,MPI_SUM,PetscObjectComm((PetscObject)pc));CHKERRQ(ierr);
      ierr = PetscViewerASCIIPrintf(viewer,"Error benign extension %1.14e\n",error);CHKERRQ(ierr);
    }
    ierr = VecAXPY(pcis->vec1_global,-1.0,vec2_global);CHKERRQ(ierr);
    ierr = VecNorm(pcis->vec1_global,NORM_INFINITY,&error);CHKERRQ(ierr);
    ierr = PetscViewerASCIIPrintf(viewer,"Error scaling extension %1.14e\n",error);CHKERRQ(ierr);
    ierr = VecDestroy(&vec2_global);CHKERRQ(ierr);

    /* restriction -> from parallel to local */
    ierr = VecSet(pcis->vec1_global,0.0);CHKERRQ(ierr);
    ierr = VecSetRandom(pcis->vec1_B,NULL);CHKERRQ(ierr);
    ierr = VecScatterBegin(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
    ierr = VecScatterEnd(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
    ierr = PCBDDCScalingRestriction(pc,pcis->vec1_global,pcis->vec1_B);CHKERRQ(ierr);
    ierr = VecScale(pcis->vec1_B,-1.0);CHKERRQ(ierr);
    ierr = VecScatterBegin(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
    ierr = VecScatterEnd(pcis->global_to_B,pcis->vec1_B,pcis->vec1_global,ADD_VALUES,SCATTER_REVERSE);CHKERRQ(ierr);
    ierr = VecNorm(pcis->vec1_global,NORM_INFINITY,&error);CHKERRQ(ierr);
    ierr = PetscViewerASCIIPrintf(viewer,"Error scaling restriction %1.14e\n",error);CHKERRQ(ierr);
    ierr = MatDestroy(&B0_B);CHKERRQ(ierr);
    ierr = VecDestroy(&B0_Bv);CHKERRQ(ierr);
    ierr = VecDestroy(&B0_Bv2);CHKERRQ(ierr);
  }
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingDestroy"
PetscErrorCode PCBDDCScalingDestroy(PC pc)
{
  PC_BDDC* pcbddc=(PC_BDDC*)pc->data;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  if (pcbddc->deluxe_ctx) {
    ierr = PCBDDCScalingDestroy_Deluxe(pc);CHKERRQ(ierr);
  }
  ierr = VecDestroy(&pcbddc->work_scaling);CHKERRQ(ierr);
  /* remove functions */
  ierr = PetscObjectComposeFunction((PetscObject)pc,"PCBDDCScalingRestriction_C",NULL);CHKERRQ(ierr);
  ierr = PetscObjectComposeFunction((PetscObject)pc,"PCBDDCScalingExtension_C",NULL);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingCreate_Deluxe"
static PetscErrorCode PCBDDCScalingCreate_Deluxe(PC pc)
{
  PC_BDDC*            pcbddc=(PC_BDDC*)pc->data;
  PCBDDCDeluxeScaling deluxe_ctx;
  PetscErrorCode      ierr;

  PetscFunctionBegin;
  ierr = PetscNew(&deluxe_ctx);CHKERRQ(ierr);
  pcbddc->deluxe_ctx = deluxe_ctx;
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingDestroy_Deluxe"
static PetscErrorCode PCBDDCScalingDestroy_Deluxe(PC pc)
{
  PC_BDDC*            pcbddc=(PC_BDDC*)pc->data;
  PetscErrorCode      ierr;

  PetscFunctionBegin;
  ierr = PCBDDCScalingReset_Deluxe_Solvers(pcbddc->deluxe_ctx);CHKERRQ(ierr);
  ierr = PetscFree(pcbddc->deluxe_ctx);CHKERRQ(ierr);
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingReset_Deluxe_Solvers"
static PetscErrorCode PCBDDCScalingReset_Deluxe_Solvers(PCBDDCDeluxeScaling deluxe_ctx)
{
  PetscInt       i;
  PetscErrorCode ierr;

  PetscFunctionBegin;
  ierr = PetscFree(deluxe_ctx->idx_simple_B);CHKERRQ(ierr);
  deluxe_ctx->n_simple = 0;
  for (i=0;i<deluxe_ctx->seq_n;i++) {
    ierr = VecScatterDestroy(&deluxe_ctx->seq_scctx[i]);CHKERRQ(ierr);
    ierr = VecDestroy(&deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
    ierr = VecDestroy(&deluxe_ctx->seq_work2[i]);CHKERRQ(ierr);
    ierr = MatDestroy(&deluxe_ctx->seq_mat[i]);CHKERRQ(ierr);
    ierr = MatDestroy(&deluxe_ctx->seq_mat_inv_sum[i]);CHKERRQ(ierr);
  }
  ierr = PetscFree5(deluxe_ctx->seq_scctx,deluxe_ctx->seq_work1,deluxe_ctx->seq_work2,deluxe_ctx->seq_mat,deluxe_ctx->seq_mat_inv_sum);CHKERRQ(ierr);
  ierr = PetscFree(deluxe_ctx->workspace);CHKERRQ(ierr);
  deluxe_ctx->seq_n = 0;
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingSetUp_Deluxe"
static PetscErrorCode PCBDDCScalingSetUp_Deluxe(PC pc)
{
  PC_IS               *pcis=(PC_IS*)pc->data;
  PC_BDDC             *pcbddc=(PC_BDDC*)pc->data;
  PCBDDCDeluxeScaling deluxe_ctx=pcbddc->deluxe_ctx;
  PCBDDCSubSchurs     sub_schurs=pcbddc->sub_schurs;
  PetscErrorCode      ierr;

  PetscFunctionBegin;
  /* reset data structures if the topology has changed */
  if (pcbddc->recompute_topography) {
    ierr = PCBDDCScalingReset_Deluxe_Solvers(deluxe_ctx);CHKERRQ(ierr);
  }

  /* Compute data structures to solve sequential problems */
  ierr = PCBDDCScalingSetUp_Deluxe_Private(pc);CHKERRQ(ierr);

  /* diagonal scaling on interface dofs not contained in cc */
  if (sub_schurs->is_vertices || sub_schurs->is_dir) {
    PetscInt n_com,n_dir;
    n_com = 0;
    if (sub_schurs->is_vertices) {
      ierr = ISGetLocalSize(sub_schurs->is_vertices,&n_com);CHKERRQ(ierr);
    }
    n_dir = 0;
    if (sub_schurs->is_dir) {
      ierr = ISGetLocalSize(sub_schurs->is_dir,&n_dir);CHKERRQ(ierr);
    }
    if (!deluxe_ctx->n_simple) {
      deluxe_ctx->n_simple = n_dir + n_com;
      ierr = PetscMalloc1(deluxe_ctx->n_simple,&deluxe_ctx->idx_simple_B);CHKERRQ(ierr);
      if (sub_schurs->is_vertices) {
        PetscInt       nmap;
        const PetscInt *idxs;

        ierr = ISGetIndices(sub_schurs->is_vertices,&idxs);CHKERRQ(ierr);
        ierr = ISGlobalToLocalMappingApply(pcis->BtoNmap,IS_GTOLM_DROP,n_com,idxs,&nmap,deluxe_ctx->idx_simple_B);CHKERRQ(ierr);
        if (nmap != n_com) SETERRQ2(PETSC_COMM_SELF,PETSC_ERR_PLIB,"Error when mapping simply scaled dofs (is_vertices)! %d != %d",nmap,n_com);
        ierr = ISRestoreIndices(sub_schurs->is_vertices,&idxs);CHKERRQ(ierr);
      }
      if (sub_schurs->is_dir) {
        PetscInt       nmap;
        const PetscInt *idxs;

        ierr = ISGetIndices(sub_schurs->is_dir,&idxs);CHKERRQ(ierr);
        ierr = ISGlobalToLocalMappingApply(pcis->BtoNmap,IS_GTOLM_DROP,n_dir,idxs,&nmap,deluxe_ctx->idx_simple_B+n_com);CHKERRQ(ierr);
        if (nmap != n_dir) SETERRQ2(PETSC_COMM_SELF,PETSC_ERR_PLIB,"Error when mapping simply scaled dofs (sub_schurs->is_dir)! %d != %d",nmap,n_dir);
        ierr = ISRestoreIndices(sub_schurs->is_dir,&idxs);CHKERRQ(ierr);
      }
      ierr = PetscSortInt(deluxe_ctx->n_simple,deluxe_ctx->idx_simple_B);CHKERRQ(ierr);
    } else {
      if (deluxe_ctx->n_simple != n_dir + n_com) SETERRQ2(PETSC_COMM_SELF,PETSC_ERR_PLIB,"Number of simply scaled dofs %d is different from the previous one computed %d",n_dir + n_com,deluxe_ctx->n_simple);
    }
  } else {
    deluxe_ctx->n_simple = 0;
    deluxe_ctx->idx_simple_B = 0;
  }
  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PCBDDCScalingSetUp_Deluxe_Private"
static PetscErrorCode PCBDDCScalingSetUp_Deluxe_Private(PC pc)
{
  PC_BDDC                *pcbddc=(PC_BDDC*)pc->data;
  PCBDDCDeluxeScaling    deluxe_ctx=pcbddc->deluxe_ctx;
  PCBDDCSubSchurs        sub_schurs = pcbddc->sub_schurs;
  PetscScalar            *matdata,*matdata2;
  PetscInt               i,max_subset_size,cum,cum2;
  const PetscInt         *idxs;
  PetscBool              newsetup = PETSC_FALSE;
  PetscErrorCode         ierr;

  PetscFunctionBegin;
  if (!sub_schurs->n_subs) {
    PetscFunctionReturn(0);
  }

  /* Allocate arrays for subproblems */
  if (!deluxe_ctx->seq_n) {
    deluxe_ctx->seq_n = sub_schurs->n_subs;
    ierr = PetscCalloc5(deluxe_ctx->seq_n,&deluxe_ctx->seq_scctx,deluxe_ctx->seq_n,&deluxe_ctx->seq_work1,deluxe_ctx->seq_n,&deluxe_ctx->seq_work2,deluxe_ctx->seq_n,&deluxe_ctx->seq_mat,deluxe_ctx->seq_n,&deluxe_ctx->seq_mat_inv_sum);CHKERRQ(ierr);
    newsetup = PETSC_TRUE;
  } else if (deluxe_ctx->seq_n != sub_schurs->n_subs) {
    SETERRQ2(PETSC_COMM_SELF,PETSC_ERR_PLIB,"Number of deluxe subproblems %d is different from the sub_schurs %d",deluxe_ctx->seq_n,sub_schurs->n_subs);
  }
  deluxe_ctx->change_with_qr = sub_schurs->change_with_qr;

  /* Create objects for deluxe */
  max_subset_size = 0;
  for (i=0;i<sub_schurs->n_subs;i++) {
    PetscInt subset_size;
    ierr = ISGetLocalSize(sub_schurs->is_subs[i],&subset_size);CHKERRQ(ierr);
    max_subset_size = PetscMax(subset_size,max_subset_size);
  }
  if (newsetup) {
    ierr = PetscMalloc1(2*max_subset_size,&deluxe_ctx->workspace);CHKERRQ(ierr);
  }
  cum = cum2 = 0;
  ierr = ISGetIndices(sub_schurs->is_Ej_all,&idxs);CHKERRQ(ierr);
  ierr = MatSeqAIJGetArray(sub_schurs->S_Ej_all,&matdata);CHKERRQ(ierr);
  matdata2 = NULL;
  if (sub_schurs->sum_S_Ej_all) {
    ierr = MatSeqAIJGetArray(sub_schurs->sum_S_Ej_all,&matdata2);CHKERRQ(ierr);
  }
  for (i=0;i<deluxe_ctx->seq_n;i++) {
    PetscInt     subset_size;

    ierr = ISGetLocalSize(sub_schurs->is_subs[i],&subset_size);CHKERRQ(ierr);
    if (newsetup) {
      IS  sub;
      /* work vectors */
      ierr = VecCreateSeqWithArray(PETSC_COMM_SELF,1,subset_size,deluxe_ctx->workspace,&deluxe_ctx->seq_work1[i]);CHKERRQ(ierr);
      ierr = VecCreateSeqWithArray(PETSC_COMM_SELF,1,subset_size,deluxe_ctx->workspace+subset_size,&deluxe_ctx->seq_work2[i]);CHKERRQ(ierr);

      /* scatters */
      ierr = ISCreateGeneral(PETSC_COMM_SELF,subset_size,idxs+cum,PETSC_COPY_VALUES,&sub);CHKERRQ(ierr);
      ierr = VecScatterCreate(pcbddc->work_scaling,sub,deluxe_ctx->seq_work1[i],NULL,&deluxe_ctx->seq_scctx[i]);CHKERRQ(ierr);
      ierr = ISDestroy(&sub);CHKERRQ(ierr);

      /* S_E_j */
      ierr = MatCreateSeqDense(PETSC_COMM_SELF,subset_size,subset_size,matdata+cum2,&deluxe_ctx->seq_mat[i]);CHKERRQ(ierr);
    }
    /* \sum_k S^k_E_j */
    if (matdata2) {
      ierr = MatDestroy(&deluxe_ctx->seq_mat_inv_sum[i]);CHKERRQ(ierr);
      ierr = MatCreateSeqDense(PETSC_COMM_SELF,subset_size,subset_size,matdata2+cum2,&deluxe_ctx->seq_mat_inv_sum[i]);CHKERRQ(ierr);
      if (sub_schurs->is_hermitian && sub_schurs->is_posdef) {
        ierr = MatCholeskyFactor(deluxe_ctx->seq_mat_inv_sum[i],NULL,NULL);CHKERRQ(ierr);
      } else {
        ierr = MatLUFactor(deluxe_ctx->seq_mat_inv_sum[i],NULL,NULL,NULL);CHKERRQ(ierr);
      }
    }
    cum += subset_size;
    cum2 += subset_size*subset_size;
  }
  ierr = ISRestoreIndices(sub_schurs->is_Ej_all,&idxs);CHKERRQ(ierr);
  ierr = MatSeqAIJRestoreArray(sub_schurs->S_Ej_all,&matdata);CHKERRQ(ierr);
  if (sub_schurs->sum_S_Ej_all) {
    ierr = MatSeqAIJRestoreArray(sub_schurs->sum_S_Ej_all,&matdata2);CHKERRQ(ierr);
  }

  /* the change of basis is just a reference to sub_schurs->change (if any) */
  deluxe_ctx->change = sub_schurs->change;
  if (deluxe_ctx->change && !deluxe_ctx->change_with_qr) {
    for (i=0;i<deluxe_ctx->seq_n;i++) {
      if (newsetup) {
        PC pc;

        ierr = KSPGetPC(deluxe_ctx->change[i],&pc);CHKERRQ(ierr);
        ierr = PCSetType(pc,PCLU);CHKERRQ(ierr);
        ierr = KSPSetFromOptions(deluxe_ctx->change[i]);CHKERRQ(ierr);
      }
      ierr = KSPSetUp(deluxe_ctx->change[i]);CHKERRQ(ierr);
    }
  }
  PetscFunctionReturn(0);
}
