#ifndef lint
static char vcid[] = "$Id: ex4.c,v 1.41 1996/09/18 12:09:09 curfman Exp $";
#endif

/* 
  Program usage:  mpirun -np <procs> usg [-help] [all PETSc options] 
*/

static char help[] = "An Unstructured Grid Example\n\
This example demonstrates how to solve a nonlinear system in parallel\n\
with SNES for an unstructured mesh. The mesh and partitioning information\n\
is read in an application defined ordering, which is later transformed\n\
into another convenient ordering (called the local ordering).The local\n\
ordering, apart from being efficient on cpu cycles and memory, allows\n\
the use of the SPMD model of parallel programming. After partitioning\n\
is done, scatters are created between local (sequential)and global\n\
(distributed) vectors. Finally, we set up the nonlinear solver context\n\
in the usual way as a structured grid  (see\n\
petsc/src/snes/examples/tutorials/ex5.c).\n\
The command line options include:\n\
  -vert <Nv>, where Nv is the global number of nodes\n\
  -elem <Ne>, where Ne is the global number of elements\n\
  -nl_par <lambda>, where lambda is the multiplier for the non linear term (u*u) term\n\
  -lin_par <alpha>, where alpha is the multiplier for the linear term (u) \n";

/*T
   Concepts: SNES^Solving a system of nonlinear equations (Pseudo Laplacian example);
   Concepts: AO^Applying application to PETSc ordering or vice versa;
   Concepts: VecScatter^Using vector scatter operations;
   Routines: SNESCreate(); SNESSetFunction(); SNESSetJacobian();
   Routines: SNESSolve(); SNESSetFromOptions();
   Routines: VecScatterCreate(); VecScatterBegin(); VecScatterEnd();
   Routines: AOCreateBasic(); AOApplicationToPetsc(); AOPetscToApplication();
   Routines: ISCreateStride(); ISCreateBlock(); ISLocalToGlobalMappingCreate();
   Routines: MatSetLocalToGlobalMapping();
   Processors: n
T*/

/* ------------------------------------------------------------------------

   PDE Solved : L(u) + lambda*u*u + alpha*u = 0 where L(u) is the Laplacian.

   The Laplacian is approximated in the following way: each edge is given a weight
   of one meaning that the diagonal term will have the weight equal to the degree
   of a node. The off diagonal terms will get a weight of -1. 

   -----------------------------------------------------------------------*/

/*
   Include ao.h so that we can use AO (Application Ordering) object's services.
   Include "snes.h" so that we can use SNES solvers.  Note that this
   file automatically includes:
     petsc.h  - base PETSc routines   vec.h - vectors
     sys.h    - system routines       mat.h - matrices
     is.h     - index sets            ksp.h - Krylov subspace methods
     viewer.h - viewers               pc.h  - preconditioners
     sles.h   - linear solvers
*/
#include "ao.h"
#include "snes.h"


#define MAX_ELEM      500  /* Maximum number of elements */
#define MAX_VERT      100  /* Maximum number of vertices */
#define MAX_VERT_ELEM   3  /* Vertices per element       */

/*
  Application-defined context for problem specific data 
*/
typedef struct {
      int         Nvglobal, Nvlocal;            /* global and local number of vertices */
      int         Neglobal, Nelocal;            /* global and local number of vertices */
      int 	  AdjM[MAX_VERT][50];           /* adjacency list of a vertex */
      int 	  itot[MAX_VERT];               /* total number of neighbors for a vertex */
      int 	  icv[MAX_ELEM][MAX_VERT_ELEM]; /* vertices belonging to an element */
      int	  v2p[MAX_VERT];                /* processor number for a vertex */
      int         *locInd, *gloInd;             /* local and global orderings for a node */
      Vec 	  localX, localF;               /* local solution (u) and f(u) vectors */ 
      double	  non_lin_param;                /* nonlinear parameter for the PDE */
      double	  lin_param;                    /* linear parameter for the PDE */
      VecScatter  scatter;                      /* scatter context for the local and 
                                                    distributed vectors */
} AppCtx;

/*
  User-defined routines
*/
int  FormJacobian(SNES,Vec,Mat*,Mat*,MatStructure*,void*),
     FormFunction(SNES,Vec,Vec,void*),
     FormInitialGuess(AppCtx*,Vec);

int main( int argc, char **argv )
{
  SNES     snes;                 /* SNES context */
  SNESType method = SNES_EQ_LS;  /* default nonlinear solution method */
  Vec      x, r;                 /* solution, residual vectors */
  Mat      Jac;                  /* Jacobian matrix */
  AppCtx   user;                 /* user-defined application context */
  Draw     draw;                 /* drawing context */
  AO       ao;                   /* Application Ordering object */
  IS       isglobal,islocal;     /* global and local index sets */
  int	   rank, size;           /* rank of a process, number of processors */
  int      rstart;               /* starting index of PETSc ordering for a processor */
  int      nfails;               /* number of unsuccessful Newton steps */
  int      bs = 1;               /* block size for multicomponent systems */
  int      nvertices;            /* number of local plus ghost nodes of a processor */
  int 	   *pordering;           /* PETSc ordering */
  int      *vertices;            /* list of all vertices (incl. ghost ones) 
                                    on a processor */ 
  int      *verticesmask, *svertices;
  int      *tmp, *tmp1, *tmp2;
  int      i, j, jstart, inode, nb, nbrs, Nvneighborstotal = 0;
  int      ierr, its, N, flg;
  double   tiny = 1.0e-10, big = 1.0e+10;
  double   zero = 0.0, one = 1.0;
  Scalar   *xx, *ff, *gg;
  char     str[256], form[256], part_name[256];
  FILE     *fptr, *fptr1;
  ISLocalToGlobalMapping isl2g;

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Initialize program
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  PetscInitialize( &argc, &argv,"options.inf",help );
  MPI_Comm_rank(MPI_COMM_WORLD,&rank);
  MPI_Comm_size(MPI_COMM_WORLD,&size);

  /*
     Initialize problem parameters
  */
  user.Nvglobal = 16;      /*Global # of vertices  */
  user.Neglobal = 18;      /*Global # of elements  */
  ierr = OptionsGetInt(PETSC_NULL,"-vert",&user.Nvglobal,&flg); CHKERRA(ierr);
  ierr = OptionsGetInt(PETSC_NULL,"-elem",&user.Neglobal,&flg); CHKERRA(ierr);
  user.non_lin_param = 0.06;
  ierr = OptionsGetDouble(PETSC_NULL,"-nl_par",&user.non_lin_param,&flg); 
  CHKERRA(ierr);
  user.lin_param = -1.0;
  ierr = OptionsGetDouble(PETSC_NULL,"-lin_par",&user.lin_param,&flg); 
  CHKERRA(ierr);
  user.Nvlocal = 0;
  user.Nelocal = 0;

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
      Read the mesh and partitioning information  
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  
  /*
     Read the mesh and partitioning information from 'adj.in'.
     The file format is as follows.
     For each line the first entry is the processor rank where the 
     current node belongs. The second entry is the number of 
     neighbors of a node. The rest of the line is the adjacency
     list of a node. Currently this file is set up to work on two 
     processors.

     This is not a very good example of reading input. In the future, 
     we will put an example that shows the style that should be 
     used in a real application, where partitioning will be done 
     dynamically by calling partitioning routines (at present, we have 
     a  ready interface to ParMeTiS). 
   */
  fptr = fopen("adj.in", "r");
  
  /*
     Each processor writes to the file output.<rank> where rank is the
     processor's rank.
  */
  sprintf(part_name,"output.%d",rank);
  fptr1 = fopen(part_name,"w");
  user.gloInd = (int *) PetscMalloc(user.Nvglobal*sizeof(int));
  fprintf(fptr1,"Rank is %d\n",rank);
  for (inode = 0; inode < user.Nvglobal; inode++)
   {
    fgets(str,256,fptr);
    sscanf(str,"%d",&user.v2p[inode]);
    if (user.v2p[inode] == rank) 
     {
       fprintf(fptr1,"Node %d belongs to processor %d\n", inode, user.v2p[inode]);
       user.gloInd[user.Nvlocal] = inode;
       sscanf(str,"%*d %d",&nbrs);
       fprintf(fptr1,"Number of neighbors for the vertex %d is %d\n",inode,nbrs);
       user.itot[user.Nvlocal] = nbrs;
       Nvneighborstotal += nbrs;
       for (i = 0; i < user.itot[user.Nvlocal]; i++)
        {
         form[0]='\0';
         for (j=0; j < i+2; j++)
          PetscStrcat(form, "%*d ");
         PetscStrcat(form, "%d");
         sscanf(str,form,&user.AdjM[user.Nvlocal][i]);
         fprintf(fptr1,"%d ",user.AdjM[user.Nvlocal][i]);
        }
        fprintf(fptr1,"\n");
	user.Nvlocal++;
     }
   }
  fprintf(fptr1,"Total # of Local Vertices is %d \n", user.Nvlocal);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Create different orderings
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  /*
    Create the local ordering list for vertices. First a list using the PETSc global 
    ordering is created. Then we use the AO object to get the PETSc-to-application and 
    application-to-PETSc mappings. Each vertex also gets a local index (stored in the 
    locInd array). 
  */
  MPI_Scan(&user.Nvlocal,&rstart,1,MPI_INT,MPI_SUM,MPI_COMM_WORLD);
  rstart -= user.Nvlocal;
  pordering = (int *) PetscMalloc(user.Nvlocal*sizeof(int));CHKPTRA(pordering);

  for ( i=0; i < user.Nvlocal; i++ ) {
    pordering[i] = rstart + i;
  }

  /* 
    Create the AO object 
  */
  AOCreateBasic(MPI_COMM_WORLD,user.Nvlocal,user.gloInd,pordering,&ao); CHKERRA(ierr);
  PetscFree(pordering);
 
  /* 
    Keep the global indices for later use 
  */
  user.locInd = (int *) PetscMalloc(user.Nvlocal*sizeof(int));
  tmp = (int *) PetscMalloc(Nvneighborstotal*sizeof(int));
  
  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    Demonstrate the use of AO functionality 
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  fprintf(fptr1,"Before AOApplicationToPetsc, local indices are : \n");
  for (i=0; i < user.Nvlocal; i++) {
   fprintf(fptr1, " %d ", user.gloInd[i]);
   user.locInd[i] = user.gloInd[i];
  }
  fprintf(fptr1,"\n");
  jstart = 0;
  for (i=0; i < user.Nvlocal; i++) {
   fprintf(fptr1, "Neghbors of local vertex %d are : ", user.gloInd[i]);
   for (j=0; j < user.itot[i]; j++) {
    fprintf(fptr1,"%d ",user.AdjM[i][j]);
    tmp[j + jstart] = user.AdjM[i][j];
   }
   jstart += user.itot[i];
   fprintf(fptr1,"\n");
  }

  /* 
    Now map the vlocal and neighbor lists to the PETSc ordering 
  */
  ierr = AOApplicationToPetsc(ao,user.Nvlocal,user.locInd); CHKERRA(ierr);
  ierr = AOApplicationToPetsc(ao,Nvneighborstotal,tmp); CHKERRA(ierr);
 
  fprintf(fptr1,"After AOApplicationToPetsc, local indices are : \n");
  for (i=0; i < user.Nvlocal; i++) {
   fprintf(fptr1, " %d ", user.locInd[i]);
  }
  fprintf(fptr1,"\n");

  jstart = 0;
  for (i=0; i < user.Nvlocal; i++) {
   fprintf(fptr1, "Neghbors of local vertex %d are : ", user.locInd[i]);
   for (j=0; j < user.itot[i]; j++) {
    user.AdjM[i][j] = tmp[j+jstart];
    fprintf(fptr1,"%d ",user.AdjM[i][j]);
   }
   jstart += user.itot[i];
   fprintf(fptr1,"\n");
  }

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Extract the ghost vertex information for each processor
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  /*
   Next, we need to generate a list of vertices required for this processor
   and a local numbering scheme for all vertices required on this processor.
      vertices - integer array of all vertices needed on this processor in PETSc 
                 global numbering; this list consists of first the "locally owned" 
                 vertices followed by the ghost vertices.
      verticesmask - integer array that for each global vertex lists its local 
                     vertex number (in vertices) + 1. If the global vertex is not
                     represented on this processor, then the corresponding
                     entry in verticesmask is zero
 
      Note: vertices and verticesmask are both Nvglobal in length; this may
    sound terribly non-scalable, but in fact is not so bad for a reasonable
    number of processors. Importantly, it allows us to use NO SEARCHING
    in setting up the data structures.
  */
  vertices = (int *) PetscMalloc(user.Nvglobal*sizeof(int));CHKPTRA(vertices);
  verticesmask = (int *) PetscMalloc(user.Nvglobal*sizeof(int));CHKPTRA(verticesmask);
  PetscMemzero(verticesmask,user.Nvglobal*sizeof(int));
  nvertices = 0;
 
  /* 
    First load "owned vertices" into list 
  */
  for ( i=0; i < user.Nvlocal; i++ ) {
    vertices[nvertices++]   = user.locInd[i];
    verticesmask[user.locInd[i]] = nvertices;
  }
 
  /* 
    Now load ghost vertices into list 
  */
  for ( i=0; i < user.Nvlocal; i++ ) {
    for ( j=0; j < user.itot[i]; j++ ) {
      nb = user.AdjM[i][j];
      if (!verticesmask[nb]) {
        vertices[nvertices++] = nb;
        verticesmask[nb]      = nvertices;
      }
    }
  }

  fprintf(fptr1,"\n");
  fprintf(fptr1,"The array vertices is :\n");
  for ( i=0; i < nvertices; i++ ) {
   fprintf(fptr1,"%d ", vertices[i]);
   }
  fprintf(fptr1,"\n");
 
  /*
     Map the vertices listed in the neighbors to the local numbering from
    the global ordering that they contained initially.
  */
  fprintf(fptr1,"\n");
  fprintf(fptr1,"After mapping neighbors in the local contiguous ordering\n");
  for ( i=0; i<user.Nvlocal; i++ ) {
    fprintf(fptr1, "Neghbors of local vertex %d are :\n",i);
    for ( j = 0; j < user.itot[i]; j++ ) {
      nb = user.AdjM[i][j];
      user.AdjM[i][j] = verticesmask[nb] - 1;
      fprintf(fptr1,"%d ",user.AdjM[i][j]);
    }
   fprintf(fptr1,"\n");
  }

  N = user.Nvglobal;
  
  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Create vector and matrix data structures
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  /* 
    Create vector data structures 
  */
  ierr = VecCreateMPI(MPI_COMM_WORLD,user.Nvlocal,N,&x); CHKERRA(ierr);
  ierr = VecDuplicate(x,&r); CHKERRA(ierr);
  ierr = VecCreateSeq(MPI_COMM_SELF,bs*nvertices,&user.localX); CHKERRA(ierr);
  ierr = VecDuplicate(user.localX,&user.localF); CHKERRA(ierr);

  /*
    Create the scatter between the global representation and the 
    local representation
  */
  ierr = ISCreateStride(MPI_COMM_SELF,bs*nvertices,0,1,&islocal); CHKERRA(ierr);
  svertices = (int *) PetscMalloc(nvertices*sizeof(int));CHKPTRA(svertices);
  for ( i=0; i<nvertices; i++ ) svertices[i] = bs*vertices[i];
  ierr = ISCreateBlock(MPI_COMM_SELF,bs,nvertices,svertices,&isglobal); CHKERRA(ierr);
  PetscFree(svertices);
  ierr = VecScatterCreate(x,isglobal,user.localX,islocal,&user.scatter);CHKERRA(ierr);

  /* 
     Create matrix data structure; Just to keep the example simple, we have not done any 
     preallocation of memory for the matrix. In real application code with big matrices,
     preallocation should always be done to expedite the matrix creation. 
  */
  ierr = MatCreate(MPI_COMM_WORLD,N,N,&Jac); CHKERRA(ierr);

  /* 
    The following routine allows us to set the matrix values in local ordering 
  */
  ierr = ISLocalToGlobalMappingCreate(MPI_COMM_SELF,bs*nvertices,vertices,&isl2g);
  CHKERRA(ierr);
  ierr = MatSetLocalToGlobalMapping(Jac,isl2g);CHKERRA(ierr);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Create nonlinear solver context
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  ierr = SNESCreate(MPI_COMM_WORLD,SNES_NONLINEAR_EQUATIONS,&snes);CHKERRA(ierr);
  ierr = SNESSetType(snes,method); CHKERRA(ierr);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    Set routines for function and Jacobian evaluation
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

   ierr = FormInitialGuess(&user,x); CHKERRA(ierr);
   ierr = SNESSetFunction(snes,r,FormFunction,(void *)&user); CHKERRA(ierr);
   ierr = SNESSetJacobian(snes,Jac,Jac,FormJacobian,(void *)&user);CHKERRA(ierr);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Customize nonlinear solver; set runtime options
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  ierr = SNESSetFromOptions(snes); CHKERRA(ierr);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Evaluate initial guess; then solve nonlinear system
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  /*
     Note: The user should initialize the vector, x, with the initial guess
     for the nonlinear solver prior to calling SNESSolve().  In particular,
     to employ an initial guess of zero, the user should explicitly set
     this vector to zero by calling VecSet().
  */
   ierr = FormInitialGuess(&user,x); CHKERRA(ierr);

   /* 
     Print the initial guess 
   */
   ierr = VecGetArray(x,&xx); CHKERRQ(ierr);
   for (inode = 0; inode < user.Nvlocal; inode++)
    fprintf(fptr1, "Initial Solution at node %d is %f \n",inode,xx[inode]);
   ierr = VecRestoreArray(x,&xx); CHKERRQ(ierr);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Now solve the nonlinear system
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  ierr = SNESSolve(snes,x,&its);  CHKERRA(ierr);
  ierr = SNESGetNumberUnsuccessfulSteps(snes,&nfails);  CHKERRA(ierr);
 
  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
    Print the output : solution vector and other information
     Each processor writes to the file output.<rank> where rank is the
     processor's rank.
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  ierr = VecGetArray(x,&xx); CHKERRQ(ierr);
  for (inode = 0; inode < user.Nvlocal; inode++)
   fprintf(fptr1, "Solution at node %d is %f \n",inode,xx[inode]);
  ierr = VecRestoreArray(x,&xx); CHKERRQ(ierr);
  fclose(fptr1);
  PetscPrintf(MPI_COMM_WORLD,"number of Newton iterations = %d, ",its);
  PetscPrintf(MPI_COMM_WORLD,"number of unsuccessful steps = %d\n\n",nfails);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Free work space.  All PETSc objects should be destroyed when they
     are no longer needed.
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

  ierr = VecDestroy(x); CHKERRA(ierr);  
  ierr = VecDestroy(r); CHKERRA(ierr);
  ierr = VecDestroy(user.localX); CHKERRA(ierr);  
  ierr = VecDestroy(user.localF); CHKERRA(ierr);
  ierr = MatDestroy(Jac); CHKERRA(ierr);  ierr = SNESDestroy(snes); CHKERRA(ierr);
  /*ierr = DrawDestroy(draw); CHKERRA(ierr);*/
  PetscFinalize();

  return 0;
}

/* --------------------  Form initial approximation ----------------- */

/* 
   FormInitialGuess - Forms initial approximation.

   Input Parameters:
   user - user-defined application context
   X - vector

   Output Parameter:
   X - vector
 */
int FormInitialGuess(AppCtx *user,Vec X)
{
  int     i, j, row, Nvglobal, Nvlocal, Neglobal, ierr;
  int     *gloInd;
  double  alpha, lambda, temp1, temp, hx, hy, hxdhy, hydhx,sc;
  Scalar  *x;

  Nvglobal = user->Nvglobal; 
  Nvlocal  = user->Nvlocal; 
  Neglobal = user->Neglobal;
  gloInd   = user->gloInd;
  lambda   = user->non_lin_param;
  alpha    =  user->lin_param;

  /*
     Get a pointer to vector data.
       - For default PETSc vectors, VecGetArray() returns a pointer to
         the data array.  Otherwise, the routine is implementation dependent.
       - You MUST call VecRestoreArray() when you no longer need access to
         the array.
  */
  ierr = VecGetArray(X,&x); CHKERRQ(ierr);

  /*
     Compute initial guess over the locally owned part of the grid
  */
  for (i=0; i < Nvlocal; i++) {
    x[i] = (double) gloInd[i];
  }

  /*
     Restore vector
  */
  ierr = VecRestoreArray(X,&x); CHKERRQ(ierr);
  return 0;
}
/* --------------------  Evaluate Function F(x) --------------------- */
/* 
   FormFunction - Evaluates nonlinear function, F(x).

   Input Parameters:
.  snes - the SNES context
.  X - input vector
.  ptr - optional user-defined context, as set by SNESSetFunction()

   Output Parameter:
.  F - function vector
 */
int FormFunction(SNES snes,Vec X,Vec F,void *ptr)
{
  AppCtx     *user = (AppCtx *) ptr;
  int        ierr, i, j, row, Nvglobal, Nvlocal, Neglobal;
  int        *gloInd;
  double     two = 2.0, one = 1.0, alpha, lambda,hx, hy, hxdhy, hydhx;
  Scalar     ut, ub, ul, ur, u, uxx, uyy, sc,*x,*f,*g;
  VecScatter scatter;
  Vec        localX = user->localX;

  Nvglobal = user->Nvglobal;
  Nvlocal  = user->Nvlocal;
  Neglobal = user->Neglobal;
  gloInd   = user->gloInd;
  lambda   = user->non_lin_param;
  alpha    =  user->lin_param;
  scatter  = user->scatter;

  /* 
     PDE : L(u) + lambda*u*u +alpha*u = 0 where L(u) is the approximate Laplacian as
     described in the beginning of this code 
                                                                                   
     First scatter the distributed vector X into local vector localX (that includes
     values for ghost nodes. If we wish, we can put some other work between 
     VecScatterBegin() and VecScatterEnd() to overlap the communication with
     computation.
 */
  ierr = VecScatterBegin(X,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRA(ierr);
  ierr = VecScatterEnd(X,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRA(ierr);

  /*
     Get pointers to vector data
  */
  ierr = VecGetArray(localX,&x); CHKERRQ(ierr);
  ierr = VecGetArray(F,&f); CHKERRQ(ierr);

  /* 
    Now compute the f(x). As mentioned earlier, the computed Laplacian is just an 
    approximate one chosen for illustrative purpose only. Another point to notice 
    is that this is a local (completly parallel) calculation. In practical application 
    codes, function calculation time is a dominat portion of the overall execution time.
  */
  for (i=0; i < Nvlocal; i++) {
    f[i] = (user->itot[i] - alpha)*x[i] - lambda*x[i]*x[i];
    for (j = 0; j < user->itot[i]; j++) {
      f[i] -= x[user->AdjM[i][j]];
    }
  }

  /*
     Restore vectors
  */
  ierr = VecRestoreArray(localX,&x); CHKERRQ(ierr);
  ierr = VecRestoreArray(F,&f); CHKERRQ(ierr);
  /*ierr = VecView(F,VIEWER_STDOUT_WORLD); CHKERRQ(ierr);*/

  return 0; 
}

/* --------------------  Evaluate Jacobian F'(x) -------------------- */
/*
   FormJacobian - Evaluates Jacobian matrix.

   Input Parameters:
.  snes - the SNES context
.  X - input vector
.  ptr - optional user-defined context, as set by SNESSetJacobian()

   Output Parameters:
.  A - Jacobian matrix
.  B - optionally different preconditioning matrix
.  flag - flag indicating matrix structure

*/
int FormJacobian(SNES snes,Vec X,Mat *J,Mat *B,MatStructure *flag,void *ptr)
{
  AppCtx *user = (AppCtx *) ptr;
  Mat     jac = *B;
  int     i, j, row, Nvglobal,Nvlocal, Neglobal, col[50], ierr;
  int     *gloInd;
  Scalar  two = 2.0, one = 1.0, alpha, lambda, value[50];
  Vec     localX = user->localX;
  VecScatter scatter;
  Scalar  *x;
  
  /*printf("Entering into FormJacobian \n");*/
  Nvglobal = user->Nvglobal; 
  Nvlocal  = user->Nvlocal; 
  Neglobal = user->Neglobal;
  gloInd   = user->gloInd;
  lambda   = user->non_lin_param;
  alpha    =  user->lin_param;
  scatter  = user->scatter;

  /* 
     PDE : L(u) + lambda*u*u +alpha*u = 0 where L(u) is the approximate Laplacian as
     described in the beginning of this code 
                                                                                   
     First scatter the distributed vector X into local vector localX (that includes
     values for ghost nodes. If we wish, we can put some other work between 
     VecScatterBegin() and VecScatterEnd() to overlap the communication with
     computation.
  */
  ierr = VecScatterBegin(X,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRA(ierr);
  ierr = VecScatterEnd(X,localX,INSERT_VALUES,SCATTER_FORWARD,scatter);CHKERRA(ierr);
  
  /*
     Get pointer to vector data
  */
  ierr = VecGetArray(localX,&x); CHKERRQ(ierr);

  for (i=0; i < Nvlocal; i++) {
    col[0] = i;
    value[0] = user->itot[i] - 2.0*lambda*x[i] - alpha;
    for (j = 0; j < user->itot[i]; j++) {
      col[j+1] = user->AdjM[i][j];
      value[j+1] = -1.0;
    }

  /* 
    Set the matrix values in the local ordering. Note that in order to use this
    feature we must call the routine MatSetLocalToGlobalMapping() after the 
    matrix has been created. 
  */
    ierr = MatSetValuesLocal(jac,1,&i,1+user->itot[i],col,value,INSERT_VALUES); 
    CHKERRQ(ierr);
  }

  /* 
     Assemble matrix, using the 2-step process:
       MatAssemblyBegin(), MatAssemblyEnd(). 
     Between these two calls, the pointer to vector data has been restored to
     demonstrate the use of overlapping communicationn with computation.
  */
  ierr = MatAssemblyBegin(jac,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);
  ierr = VecRestoreArray(localX,&x); CHKERRQ(ierr);
  ierr = MatAssemblyEnd(jac,MAT_FINAL_ASSEMBLY); CHKERRQ(ierr);

  /*
     Set flag to indicate that the Jacobian matrix retains an identical
     nonzero structure throughout all nonlinear iterations (although the
     values of the entries change). Thus, we can save some work in setting
     up the preconditioner (e.g., no need to redo symbolic factorization for
     ILU/ICC preconditioners).
      - If the nonzero structure of the matrix is different during
        successive linear solves, then the flag DIFFERENT_NONZERO_PATTERN
        must be used instead.  If you are unsure whether the matrix
        structure has changed or not, use the flag DIFFERENT_NONZERO_PATTERN.
      - Caution:  If you specify SAME_NONZERO_PATTERN, PETSc
        believes your assertion and does not check the structure
        of the matrix.  If you erroneously claim that the structure
        is the same when it actually is not, the new preconditioner
        will not function correctly.  Thus, use this optimization
        feature with caution!
  */
  *flag = SAME_NONZERO_PATTERN;

  /*
     Tell the matrix we will never add a new nonzero location to the
     matrix. If we do, it will generate an error.
  */
  ierr = MatSetOption(jac,MAT_NEW_NONZERO_LOCATION_ERR); CHKERRQ(ierr);
  /*MatView(jac,VIEWER_STDOUT_SELF);*/
  return 0;
}


