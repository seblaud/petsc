#ifndef lint
static char vcid[] = "$Id: bdiag.c,v 1.6 1995/04/29 18:17:07 curfman Exp curfman $";
#endif

/* Block diagonal matrix format */

#include "bdiag.h"
#include "vec/vecimpl.h"
#include "inline/spops.h"

static int MatSetValues_BDiag(Mat matin,int m,int *idxm,int n,
                            int *idxn,Scalar *v,InsertMode  addv)
{
  Mat_BDiag *dmat = (Mat_BDiag *) matin->data;
  int       kk, j, k, loc, ldiag, shift, row, nz = n, dfound;
  int       nb = dmat->nb, nd = dmat->nd, *diag = dmat->diag;
  Scalar    *valpt;
/* 
   Note:  This routine assumes that space has already been allocated for
   the gathered elements ... It does NOT currently allocate additional
   space! 
 */
  if (m!=1) SETERR(1,"Currently can set only 1 row at a time.");
  if (nb == 1) {
    for ( kk=0; kk<m; kk++ ) { /* loop over added rows */
      row  = idxm[kk];   
      if (row < 0) SETERR(1,"Negative row index");
      if (row >= dmat->m) SETERR(1,"Row index too large");
      for (j=0; j<nz; j++) {
        ldiag = row - idxn[j]; /* diagonal number */
        dfound = 0;
        for (k=0; k<nd; k++) {
	  if (diag[k] == ldiag) {
            dfound = 1;
	    if (ldiag > 0) /* lower triangle */
	      loc = row - dmat->diag[k];
	    else
	      loc = row;
	    if ((valpt = &((dmat->diagv[k])[loc]))) {
	      if (addv == AddValues) *valpt += v[j];
	      else                   *valpt = v[j];
            } else SETERR(1,
               "Does not support allocation of additional memory." );
            break;
          }
        }
        if (!dfound) SETERR(1,
         "Diagonal not allocated.  Set all diagonals in matrix creation.");
      }
    }
  } else {
    for ( kk=0; kk<m; kk++ ) { /* loop over added rows */
      row    = idxm[kk];   
      if (row < 0) SETERR(1,"Negative row index");
      if (row >= dmat->m) SETERR(1,"Row index too large");
      shift = (row/nb)*nb*nb + row%nb;
      for (j=0; j<nz; j++) {
        ldiag = row/nb - idxn[j]/nb; /* block diagonal */
        dfound = 0;
        for (k=0; k<nd; k++) {
          if (diag[k] == ldiag) {
            dfound = 1;
	    if (ldiag > 0) /* lower triangle */
	      loc = shift - ldiag*nb*nb;
             else
	      loc = shift;
	    if ((valpt = &((dmat->diagv[k])[loc + (idxn[j]%nb)*nb ]))) {
              printf("row=%d, col=%d, bdiag=%d, loc=%d, idx=%d, val=%g\n",
                 row,idxn[j],ldiag, loc, loc + (idxn[j]%nb)*nb, v[j] );             
	      if (addv == AddValues) *valpt += v[j];
	      else                   *valpt = v[j];
            } else SETERR(1,
                "Does not support allocation of additional memory." );
            break;
          }
        }
        if (!dfound) SETERR(1,
         "Diagonal not allocated.  Set all diagonals in matrix creation.");
      }
    }
  }
  return 0;
}

/*
  MatMult_BDiag_base - This routine is intended for use with 
  MatMult_BDiag() and MatMultAdd_BDiag().  It computes yy += mat * xx.
 */
static int MatMult_BDiag_base(Mat matin,Vec xx,Vec yy)
{ 
  Mat_BDiag *mat= (Mat_BDiag *) matin->data;
  int             nd = mat->nd, nb = mat->nb, diag;
  int             kshift, nbrows;
  Scalar          *vin, *vout;
  register Scalar *pvin, *pvout, *dv;
  register int    d, i, j, k, len;

  VecGetArray(xx,&vin); VecGetArray(yy,&vout);
  if (nb == 1) {
    for (d=0; d<nd; d++) {
      dv   = mat->diagv[d];
      diag = mat->diag[d];
      len  = mat->bdlen[d];
      if (diag > 0) {	/* lower triangle */
        pvin = vin;
	pvout = vout + diag;
      } else {		/* upper triangle, including main diagonal */
        pvin  = vin - diag;
        pvout = vout;
      }
      for (j=0; j<len; j++) pvout[j] += dv[j] * pvin[j];
    }
  } else { /* Block diagonal approach, assuming storage within dense blocks 
              in column-major order */
    for (d=0; d<nd; d++) {
      dv   = mat->diagv[d];
      diag = mat->diag[d];
      len  = mat->bdlen[d];
      if (diag > 0) {	/* lower triangle */
        pvin = vin;
	pvout = vout + nb*diag;
      } else {		/* upper triangle, including main diagonal */
        pvin  = vin - nb*diag;
        pvout = vout;
      }
      for (k=0; k<len; k++) {
        kshift = k*nb*nb;
        for (i=0; i<nb; i++) {	/* i = local row */
          for (j=0; j<nb; j++) {	/* j = local column */
            pvout[k*nb + i] += dv[kshift + j*nb + i] * pvin[k*nb + j];
          }
        }
      }
    }
  }
  return 0;
}

/*
  MatMultTrans_BDiag_base - This routine is intended for use with 
  MatMultTrans_BDiag() and MatMultTransAdd_BDiag().  It computes 
            yy += mat^T * xx.
 */
static int MatMultTrans_BDiag_base(Mat matin,Vec xx,Vec yy)
{
  Mat_BDiag       *mat = (Mat_BDiag *) matin->data;
  int             nd = mat->nd, nb = mat->nb, diag;
  int             kshift;
  register Scalar *pvin, *pvout, *dv;
  register int    d, i, j, k, len;
  Scalar          *vin, *vout;
  
  VecGetArray(xx,&vin); VecGetArray(yy,&vout);
  if (nb == 1) {
    for (d=0; d<nd; d++) {
      dv   = mat->diagv[d];
      diag = mat->diag[d];
      len  = mat->bdlen[d];
      /* diag of original matrix is (row/nb - col/nb) */
      /* diag of transpose matrix is (col/nb - row/nb) */
      if (diag < 0) {	/* transpose is lower triangle */
        pvin  = vin;
	pvout = vout - diag;
      } else {	/* transpose is upper triangle, including main diagonal */
        pvin  = vin + diag;
        pvout = vout;
      }
      for (j=0; j<len; j++) pvout[j] += dv[j] * pvin[j];
    }
  } else { /* Block diagonal approach, assuming storage within dense blocks
              in column-major order */
    for (d=0; d<nd; d++) {
      dv   = mat->diagv[d];
      diag = mat->diag[d];
      len  = mat->bdlen[d];
      /* diag of original matrix is (row/nb - col/nb) */
      /* diag of transpose matrix is (col/nb - row/nb) */
      if (diag < 0) {	/* transpose is lower triangle */
        pvin  = vin;
        pvout = vout - nb*diag;
      } else {	/* transpose is upper triangle, including main diagonal */
        pvin  = vin + nb*diag;
        pvout = vout;
      }
      for (k=0; k<len; k++) {
        kshift = k*nb*nb;
        for (i=0; i<nb; i++) {	 /* i = local column of transpose */
          for (j=0; j<nb; j++) { /* j = local row of transpose */
            pvout[k*nb + j] += dv[kshift + j*nb + i] * pvin[k*nb + i];
          }
        }
      }
    }
  }
  return 0;
}
static int MatMult_BDiag(Mat matin,Vec xx,Vec yy)
{
  Scalar zero = 0.0;
  int    ierr;
  ierr = VecSet(&zero,yy); CHKERR(ierr);
  return MatMult_BDiag_base(matin,xx,yy);
}
static int MatMultTrans_BDiag(Mat matin,Vec xx,Vec yy)
{
  Scalar zero = 0.0;
  int    ierr;
  ierr = VecSet(&zero,yy); CHKERR(ierr);
  return MatMultTrans_BDiag_base(matin,xx,yy);
}
static int MatMultAdd_BDiag(Mat matin,Vec xx,Vec zz,Vec yy)
{
  int ierr;
  ierr = VecCopy(zz,yy); CHKERR(ierr);
  return MatMult_BDiag_base(matin,xx,yy);
}
static int MatMultTransAdd_BDiag(Mat matin,Vec xx,Vec zz,Vec yy)
{
  int ierr;
  ierr = VecCopy(zz,yy); CHKERR(ierr);
  return MatMultTrans_BDiag_base(matin,xx,yy);
}

static int MatGetInfo_BDiag(Mat matin,int flag,int *nz,int *nzalloc,int *mem)
{
  Mat_BDiag *mat = (Mat_BDiag *) matin->data;
  *nz      = mat->nz;
  *nzalloc = mat->maxnz;
  *mem     = mat->mem;
  return 0;
}

/* -----------------------------------------------------------------*/
static int MatGetRow_BDiag(Mat matin,int row,int *nz,int **col,Scalar **v)
{
  Mat_BDiag *dmat = (Mat_BDiag *) matin->data;
  int       nd = dmat->nd, nb = dmat->nb, loc;
  int       nc = dmat->n, *diag = dmat->diag, pcol, shift, i, j, k;

/* For efficiency, if ((nz) && (col) && (v)) then do all at once */
  if ((nz) && (col) && (v)) {
    *col = dmat->colloc;
    *v   = dmat->dvalue;
    k    = 0;
    if (nb == 1) { 
      for (j=0; j<nd; j++) {
        pcol = row - diag[j];
        if (pcol > -1 && pcol < nc) {
	  if (diag[j] > 0)
	    loc = row - diag[j];
	  else
	    loc = row;
	  (*v)[k]   = (dmat->diagv[j])[loc];
          (*col)[k] = pcol;  k++;
	}
      }
      *nz = k;
    } else {
      shift = (row/nb)*nb*nb + row%nb;
      for (j=0; j<nd; j++) {
        pcol = nb * (row/nb - diag[j]);
        if (pcol > -1 && pcol < nc) {
          if (diag[j] > 0)
	    loc = shift - diag[j]*nb*nb;
	  else 
	    loc = shift;
          for (i=0; i<nb; i++) {
	    (*v)[k+i]   = (dmat->diagv[j])[loc + i*nb];
	    (*col)[k+i] = pcol + i;
	  }
          k += nb;
        } 
      }
      *nz = k;
    }
  }
  else {
    if (nb == 1) { 
      if (nz) {
        k = 0;
        for (j=0; j<nd; j++) {
          pcol = row - diag[j];
          if (pcol > -1 && pcol < nc) k++; 
        }
        *nz = k;
      }
      if (col) {
        *col = dmat->colloc;
        k = 0;
        for (j=0; j<nd; j++) {
          pcol = row - diag[j];
          if (pcol > -1 && pcol < nc) {
            (*col)[k] = pcol;  k++;
          }
        }
      }
      if (v) {
        *v = dmat->dvalue;
        k = 0;
        for (j=0; j<nd; j++) {
          pcol = row - diag[j];
          if (pcol > -1 && pcol < nc) {
	    if (diag[j] > 0)
	      loc = row - diag[j];
	    else
	      loc = row;
	    (*v)[k] = (dmat->diagv[j])[loc]; k++;
          }
        }
      }
    } 
    else {
      if (nz) {
        k = 0;
        for (j=0; j<nd; j++) {
          pcol = nb * (row/nb- diag[j]);
          if (pcol > -1 && pcol < nc) k += nb; 
        }
        *nz = k;
      }
      if (col) {
        *col = dmat->colloc;
        k = 0;
        for (j=0; j<nd; j++) {
          pcol = nb * (row/nb - diag[j]);
          if (pcol > -1 && pcol < nc) {
            for (i=0; i<nb; i++) {
	      (*col)[k+i] = pcol + i;
            }
	    k += nb;
          }
        }
      }
      if (v) {
        shift = (row/nb)*nb*nb + row%nb;
        *v = dmat->dvalue;
        k = 0;
        for (j=0; j<nd; j++) {
	  pcol = nb * (row/nb - diag[j]);
	  if (pcol > -1 && pcol < nc) {
	    if (diag[j] > 0)
	      loc = shift - diag[j]*nb*nb;
	    else 
	      loc = shift;
	    for (i=0; i<nb; i++) {
	     (*v)[k+i] = (dmat->diagv[j])[loc + i*nb];
            }
	    k += nb;
	  }
        }
      }
    }
  }
  return 0;
}
static int MatRestoreRow_BDiag(Mat matin,int row,int *ncols,int **cols,
                            Scalar **vals)
{
  /* Work space is allocated once during matrix creation and then freed
     when matrix is destroyed */
  return 0;
}
/* ----------------------------------------------------------------*/
#include "draw.h"

int MatView_BDiag(PetscObject obj,Viewer ptr)
{
  Mat       matin = (Mat) obj;
  Mat_BDiag *mat = (Mat_BDiag *) matin->data;
  int       ierr, *col, i, j, len, diag, nr = mat->m, nb = mat->nb;
  int       nz, nzalloc, mem;
  Scalar    *val, *dv, zero = 0.0;
  PetscObject vobj = (PetscObject) ptr;

  if (!mat->assembled) SETERR(1,"Cannot view unassembled matrix");
  if (!ptr) { /* so that viewers may be used from debuggers */
    ptr = STDOUT_VIEWER; vobj = (PetscObject) ptr;
  }
  if (vobj->cookie == DRAW_COOKIE && vobj->type == NULLWINDOW) return 0;
  if (vobj && vobj->cookie == VIEWER_COOKIE && vobj->type == MATLAB_VIEWER) {
    SETERR(1,"Matlab viewer not yet supported for block diagonal format.");
  }
  if (vobj && vobj->cookie == DRAW_COOKIE) {
    DrawCtx draw = (DrawCtx) ptr;
    double  xl,yl,xr,yr,w,h;
    xr = mat->n; yr = mat->m; h = yr/10.0; w = xr/10.0;
    xr += w; yr += h; xl = -w; yl = -h;
    ierr = DrawSetCoordinates(draw,xl,yl,xr,yr); CHKERR(ierr);
    /* loop over matrix elements drawing boxes; we really should do this
       by diagonals. */
    /* What do we really want to draw here?  nonzeros, allocated space? */
    for ( i=0; i<nr; i++ ) {
      yl = nr - i - 1.0; yr = yl + 1.0;
      ierr = MatGetRow(matin,i,&nz,&col,0); CHKERR(ierr);
      for ( j=0; j<nz; j++ ) {
        xl = col[j]; xr = xl + 1.0;
        DrawRectangle(draw,xl,yl,xr,yr,DRAW_BLACK,DRAW_BLACK,DRAW_BLACK,
                      DRAW_BLACK);
      }
    ierr = MatRestoreRow(matin,i,&nz,&col,0); CHKERR(ierr);
    }
    return 0;
  } else {
    FILE *fd = ViewerFileGetPointer_Private(ptr);
    char *outputname = (char *)ViewerFileGetOutputname_Private(ptr);
    int format = ViewerFileGetFormat_Private(ptr);
    if (format == FILE_FORMAT_MATLAB) {
      MatGetInfo(matin,MAT_LOCAL,&nz,&nzalloc,&mem);
      fprintf(fd,"%% Size = %d %d \n",nr, mat->n);
      fprintf(fd,"%% Nonzeros = %d \n",nz);
      fprintf(fd,"zzz = zeros(%d,3);\n",nz);
      fprintf(fd,"zzz = [\n");
      for ( i=0; i<mat->m; i++ ) {
        ierr = MatGetRow( matin, i, &nz, &col, &val ); CHKERR(ierr);
        for (j=0; j<nz; j++) {
          if (val[j] != zero)
#if defined(PETSC_COMPLEX)
            fprintf(fd,"%d %d  %18.16e  %18.16e \n",
               i+1, col[j]+1, real(val[j]), imag(val[j]) );
#else
            fprintf(fd,"%d %d  %18.16e\n", i+1, col[j]+1, val[j]);
#endif
        }
      }
      fprintf(fd,"];\n %s = spconvert(zzz);\n",outputname);
    } else if (format == FILE_FORMAT_IMPL) {
      if (nb == 1) { /* diagonal format */
        for (i=0; i< mat->nd; i++) {
          dv   = mat->diagv[i];
          diag = mat->diag[i];
          fprintf(fd,"\n<diagonal %d>\n",diag);
          /* diag[i] is (row-col)/nb */
          if (diag > 0) {  /* lower triangle */
            len = nr - diag;
            for (j=0; j<len; j++)
              if (dv[j]) fprintf(fd,"A[ %d , %d ] = %e\n", j+diag, j, dv[j]);
          } else {         /* upper triangle, including main diagonal */
            len = nr + diag;
            for (j=0; j<len; j++)
              if (dv[j]) fprintf(fd,"A[ %d , %d ] = %e\n", j, j-diag, dv[j]);
          }
        }
      } else {  /* Block diagonals */
        int d, k, kshift;
        for (d=0; d< mat->nd; d++) {
          dv   = mat->diagv[d];
          diag = mat->diag[d];
          len  = mat->bdlen[d];
          fprintf(fd,"\n<diagonal %d>\n", diag);
          if (diag > 0) {  /* lower triangle */
            for (k=0; k<len; k++) {
              kshift = k*nb*nb;
              for (i=0; i<nb; i++) {
                for (j=0; j<nb; j++) {
                  if (dv[kshift + j*nb + i])
                    fprintf(fd,"A[%d,%d]=%5.2e   ", (k+diag)*nb + i, 
	                k*nb + j, dv[kshift + j*nb + i] );
                }
                fprintf(fd,"\n");
              }
            }
         } else {  /* upper triangle, including main diagonal */
            for (k=0; k<len; k++) {
              kshift = k*nb*nb;
              for (i=0; i<nb; i++) {
                for (j=0; j<nb; j++) {
                  if (dv[kshift + j*nb + i])
                    fprintf(fd,"A[%d,%d]=%5.2e   ", k*nb + i, 
                          (k-diag)*nb + j, dv[kshift + j*nb + i] );
                }
                fprintf(fd,"\n");
              }
            }
          }
        }
      }
    } else {
      for (i=0; i<mat->m; i++) { /* the usual row format */
        fprintf(fd,"row %d:",i);
        ierr = MatGetRow( matin, i, &nz, &col, &val ); CHKERR(ierr);
        for (j=0; j<nz; j++) {
          if (val[j] != zero)
#if defined(PETSC_COMPLEX)
            fprintf(fd," %d %g ", col[j], real(val[j]), imag(val[j]) );
#else
            fprintf(fd," %d %g ", col[j], val[j] );
#endif
        }
        fprintf(fd,"\n");
        ierr = MatRestoreRow( matin, i, &nz, &col, &val ); CHKERR(ierr);
      }
    }
    fflush(fd);
  }
  return 0;
}

static int MatDestroy_BDiag(PetscObject obj)
{
  Mat       bmat = (Mat) obj;
  Mat_BDiag *mat = (Mat_BDiag *) bmat->data;

#if defined(PETSC_LOG)
  PLogObjectState(obj,"Rows %d Cols %d NZ %d",mat->m,mat->n,mat->nz);
#endif
  if (!mat->user_alloc) { /* Free the actual diagonals */
    FREE( mat->diagv[0] );
  }
  FREE(mat->diagv);
  FREE(mat->diag);
  FREE(mat->dvalue);
  FREE(mat);
  PLogObjectDestroy(bmat);
  PETSCHEADERDESTROY(bmat);
  return 0;
}

static int MatEndAssembly_BDiag(Mat matin,int mode)
{
  Mat_BDiag *mat = (Mat_BDiag *) matin->data;
  if (mode == FLUSH_ASSEMBLY) return 0;
  mat->assembled = 1;
  return 0;
}

static int MatGetDiagonal_BDiag(Mat matin,Vec v)
{
  Mat_BDiag *mat = (Mat_BDiag *) matin->data;
  int    i, j, n, ibase, nb = mat->nb, iloc;
  Scalar *x, *dvmain;
  VecGetArray(v,&x); VecGetLocalSize(v,&n);
  if (n != mat->m) SETERR(1,"Nonconforming matrix and vector");
  dvmain = mat->diagv[mat->mainbd];
  if (mat->nb == 1) {
    for (i=0; i<mat->m; i++) x[i] = dvmain[i];
  } else {
    for (i=0; i<mat->mblock; i++) {
      ibase = i*nb*nb;  iloc = i*nb;
      for (j=0; j<nb; j++) x[j + iloc] = dvmain[ibase + j*(nb+1)];
    }
  }
  return 0;
}

static int MatZero_BDiag(Mat A)
{
  return 0;
}

static int MatZeroRows_BDiag(Mat A,IS is,Scalar *diag)
{
  Mat_BDiag *l = (Mat_BDiag *) A->data;

  return 0;
}

static int MatGetSize_BDiag(Mat matin,int *m,int *n)
{
  Mat_BDiag *mat = (Mat_BDiag *) matin->data;
  *m = mat->m; *n = mat->n;
  return 0;
}

static int MatCopy_BDiag(Mat,Mat *);

/* -------------------------------------------------------------------*/
static struct _MatOps MatOps = {MatSetValues_BDiag,
       MatGetRow_BDiag, MatRestoreRow_BDiag,
       MatMult_BDiag, MatMultAdd_BDiag, 
       MatMultTrans_BDiag, MatMultTransAdd_BDiag, 
       0, 0, 0, 0,
       0, 0, 0, 0,
       MatGetInfo_BDiag, 0, MatCopy_BDiag,
       MatGetDiagonal_BDiag, 0, 0,
       0,MatEndAssembly_BDiag,
       0, 0, MatZero_BDiag,MatZeroRows_BDiag,0,
       0, 0, 0, 0,
       MatGetSize_BDiag,MatGetSize_BDiag,0,
       0, 0, 0
};
/*@
   MatCreateSequentialBDiag - Creates a sequential block diagonal matrix.

   Input Parameters:
.  comm - MPI communicator, set to MPI_COMM_SELF
.  m      - number of rows
.  n      - number of columns
.  nd     - number of block diagonals
.  nb     - each element of a diagonal is an nb x nb dense matrix
.  diag   - array of block diagonal numbers, values of (row-col)/nb
            NOTE:  currently, diagonals MUST be listed in descending order!
            You must store the main diagonal, even if it has zero values.
.  diagv  - pointer to actual diagonals (in same order as diag array), 
            if allocated by user.
            Otherwise, set diagv=0 on input for PETSc to control memory 
            allocation.

   Output Parameters:
.  newmat - the matrix

   Notes:
   Once the diagonals have been created, no new diagonals can be
   added.  Thus, only elements that fall on the specified diagonals
   can be set or altered; trying to modify other elements results in
   an error.

   The case nb=1 (conventional diagonal storage) is implemented as
   a special case. 

.keywords: Mat, matrix, block, diagonal

.seealso: MatCreateSequentialAIJ()
@*/
int MatCreateSequentialBDiag(MPI_Comm comm,int m,int n,int nd,int nb,
                             int *diag,Scalar **diagv,Mat *newmat)
{
  Mat       bmat;
  Mat_BDiag *mat;
  int       i, j, temp, sizetot;

  *newmat       = 0;
  if ((n%nb) || (m%nb)) SETERR(1,"Invalid block size.");
  PETSCHEADERCREATE(bmat,_Mat,MAT_COOKIE,MATBDIAG,comm);
  PLogObjectCreate(bmat);
  bmat->data    = (void *) (mat = NEW(Mat_BDiag)); CHKPTR(mat);
  bmat->ops     = &MatOps;
  bmat->destroy = MatDestroy_BDiag;
  bmat->view    = MatView_BDiag;
  bmat->factor  = 0;

  mat->m      = m;
  mat->n      = n;
  mat->mblock = m/nb;
  mat->nblock = n/nb;
  mat->nd     = nd;
  mat->nb     = nb;
  mat->ndim   = 0;
  mat->mainbd = -1;

  mat->diag   = (int *)MALLOC( (2+nb)*nd * sizeof(int) ); CHKPTR(mat->diag);
  mat->bdlen  = mat->diag + nd;
  mat->colloc = mat->bdlen + nd;
  sizetot = 0;

  /* Sort diagonals in decreasing order. */
  for (i=0; i<nd; i++) {
    for (j=i+1; j<nd; j++) {
      if (diag[i] < diag[j]) {
        temp = diag[i];
        diag[i] = diag[j];
        diag[j] = temp;
      }
    }
  }

  for (i=0; i<nd; i++) {
    mat->diag[i] = diag[i];
    if (diag[i] > 0) /* lower triangular */
      mat->bdlen[i] = 2 * mat->mblock - diag[i] - mat->nblock;
    else if (diag[i] < 0) /* upper triangular */
      mat->bdlen[i] = diag[i] + mat->nblock;
    else mat->bdlen[i] = mat->mblock;
    sizetot += mat->bdlen[i];
    if (diag[i] == 0) mat->mainbd = i;
    printf("i=%d, diag=%d, dlen=%d\n",i,mat->diag[i],mat->bdlen[i]);
  }
  if ((mat->mainbd == -1))
    SETERR(1,"No main diagonal.  Must set main diagonal, even if empty.")
  sizetot *= nb*nb;
  mat->maxnz  = sizetot;
  mat->dvalue = (Scalar *)MALLOC(nb*nd * sizeof(Scalar)); CHKPTR(mat->dvalue);
  mat->diagv  = (Scalar **)MALLOC(nd * sizeof(Scalar*)); CHKPTR(mat->diagv);
  mat->mem    = (nd*(nb+2)) * sizeof(int) + nb*nd * sizeof(Scalar)
                 + nd * sizeof(Scalar*) + sizeof(Mat_BDiag)
                 + sizetot * sizeof(Scalar);

  if (diagv) mat->user_alloc = 1; /* user allocated space */
  else       mat->user_alloc = 0;
  if (!mat->user_alloc) {
    Scalar *d;
    d = (Scalar *)MALLOC(sizetot * sizeof(Scalar)); CHKPTR(d);
    MEMSET(d,0,sizetot*sizeof(Scalar));
    for (i=0; i<nd; i++) {
      mat->diagv[i] = d;
      d += mat->bdlen[i];
    }
  } else {
    for (i=0; i<nd; i++) mat->diagv[i] = diagv[i];
  }
  mat->nz        = mat->maxnz; /* Currently not keeping track of exact count */
  mat->assembled = 0;
  mat->nonew     = 1; /* Currently all memory must be preallocated! */
  *newmat        = bmat;
  return 0;
}

static int MatCopy_BDiag(Mat matin,Mat *newmat)
{ 
  Mat_BDiag *old = (Mat_BDiag *) matin->data;
  int       ierr;

  *newmat = 0;
  SETERR(1,"MatCopy_BDiag:  Code not yet finished.");
  if (!old->assembled) SETERR(1,"Cannot copy unassembled matrix");
  ierr = MatCreateSequentialBDiag(matin->comm,old->m,old->n,old->nd,
         old->nb,old->diag,0,newmat); CHKERR(ierr);
  /* Copy contents of diagonals */
  return 0;
}
