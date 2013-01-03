
static char help[] = "Power grid stability analysis of WECC 9 bus system.\n\
This example is based on the 9-bus (node) example given in the book Power\n\
Systems Dynamics and Stability (Chapter 7) by P. Sauer and M. A. Pai.\n\
The power grid in this example consists of 9 buses (nodes), 3 generators,\n\
3 loads, and 9 transmission lines. The network equations are written\n\
in current balance form using rectangular coordiantes.\n\n";

/*
   The equations for the stability analysis are described by the DAE

   \dot{x} = f(x,y,t)
     0     = g(x,y,t)

   where the generators are described by differential equations, while the algebraic
   constraints define the network equations.

   The generators are modeled with a 4th order differential equation describing the electrical
   and mechanical dynamics. Each generator also has an exciter system modeled by 3rd order
   diff. eqns. describing the exciter, voltage regulator, and the feedback stabilizer
   mechanism.

   The network equations are described by nodal current balance equations.
    I(x,y) - Y*V = 0

   where:
    I(x,y) is the current injected from generators and loads.
      Y    is the admittance matrix, and
      V    is the voltage vector
*/

/*
   Include "petscts.h" so that we can use TS solvers.  Note that this
   file automatically includes:
     petscsys.h       - base PETSc routines   petscvec.h - vectors
     petscmat.h - matrices
     petscis.h     - index sets            petscksp.h - Krylov subspace methods
     petscviewer.h - viewers               petscpc.h  - preconditioners
     petscksp.h   - linear solvers
*/
#include <petscts.h>
#include <petscdmda.h>
#include <petscdmcomposite.h>

#define freq 60
#define w_s (2*PETSC_PI*freq)

/* Sizes and indices */
const PetscInt nbus = 9; /* Number of network buses */
const PetscInt ngen = 3; /* Number of generators */
const PetscInt nload = 3; /* Number of loads */
const PetscInt gbus[3] = {0,1,2}; /* Buses at which generators are incident */
const PetscInt lbus[3] = {4,5,7}; /* Buses at which loads are incident */

/* Generator real and reactive powers (found via loadflow) */
const PetscScalar PG[3] = {0.716786142395021,1.630000000000000,0.850000000000000};
const PetscScalar QG[3] = {0.270702180178785,0.066120127797275,-0.108402221791588};
/* Generator constants */
const PetscScalar H[3]   = {23.64,6.4,3.01}; /* Inertia constant */
const PetscScalar Rs[3] = {0.0,0.0,0.0}; /* Stator Resistance */
const PetscScalar Xd[3]  = {0.146,0.8958,1.3125}; /* d-axis reactance */
const PetscScalar Xdp[3] = {0.0608,0.1198,0.1813}; /* d-axis transient reactance */
const PetscScalar Xq[3]  = {0.4360,0.8645,1.2578}; /* q-axis reactance Xq(1) set to 0.4360, value given in text 0.0969 */
const PetscScalar Xqp[3] = {0.0969,0.1969,0.25}; /* q-axis transient reactance */
const PetscScalar Td0p[3] = {8.96,6.0,5.89}; /* d-axis open circuit time constant */
const PetscScalar Tq0p[3] = {0.31,0.535,0.6}; /* q-axis open circuit time constant */
PetscScalar M[3]; /* M = 2*H/w_s */
PetscScalar D[3]; /* D = 0.1*M */

PetscScalar TM[3]; /* Mechanical Torque */
/* Exciter system constants */
const PetscScalar KA[3] = {20.0,20.0,20.0};  /* Voltage regulartor gain constant */
const PetscScalar TA[3] = {0.2,0.2,0.2};     /* Voltage regulator time constant */
const PetscScalar KE[3] = {1.0,1.0,1.0};     /* Exciter gain constant */
const PetscScalar TE[3] = {0.314,0.314,0.314}; /* Exciter time constant */
const PetscScalar KF[3] = {0.063,0.063,0.063};  /* Feedback stabilizer gain constant */
const PetscScalar TF[3] = {0.35,0.35,0.35};    /* Feedback stabilizer time constant */
const PetscScalar k1[3] = {0.0039,0.0039,0.0039};
const PetscScalar k2[3] = {1.555,1.555,1.555};  /* k1 and k2 for calculating the saturation function SE = k1*exp(k2*Efd) */

PetscScalar Vref[3];
/* Load constants
  We use a composite load model that describes the load and reactive powers at each time instant as follows
  P(t) = \sum\limits_{i=0}^ld_nsegsp \ld_alphap_i*P_D0(\frac{V_m(t)}{V_m0})^\ld_betap_i
  Q(t) = \sum\limits_{i=0}^ld_nsegsq \ld_alphaq_i*Q_D0(\frac{V_m(t)}{V_m0})^\ld_betaq_i
  where
    ld_nsegsp,ld_nsegsq - Number of individual load models for real and reactive power loads
    ld_alphap,ld_alphap - Percentage contribution (weights) or loads
    P_D0                - Real power load
    Q_D0                - Reactive power load
    V_m(t)              - Voltage magnitude at time t
    V_m0                - Voltage magnitude at t = 0
    ld_betap, ld_betaq  - exponents describing the load model for real and reactive part

    Note: All loads have the same characteristic currently.
*/
const PetscScalar PD0[3] = {1.25,0.9,1.0};
const PetscScalar QD0[3] = {0.5,0.3,0.35};
const PetscInt    ld_nsegsp[3] = {3,3,3};
const PetscScalar ld_alphap[3] = {1.0,0.0,0.0};
const PetscScalar ld_betap[3]  = {2.0,1.0,0.0};
const PetscInt    ld_nsegsq[3] = {3,3,3};
const PetscScalar ld_alphaq[3] = {1.0,0.0,0.0};
const PetscScalar ld_betaq[3]  = {2.0,1.0,0.0};

typedef struct{
  DM dmgen, dmnet; /* DMs to manage generator and network subsystem */
  DM dmpgrid;        /* Composite DM to manage the entire power grid */
  Mat Ybus;        /* Network admittance matrix */
  Vec V0;          /* Initial voltage vector (Power flow solution) */
  PetscReal tfaulton,tfaultoff; /* Fault on and off times */
  PetscInt  faultbus;  /* Fault bus */
  PetscScalar Rfault;
  PetscReal t0,tmax;
  PetscInt  neqs_gen,neqs_net,neqs_pgrid;
  Mat       Sol; /* Matrix to save solution at each time step */
  PetscInt  stepnum;
}Userctx;


/* Converts from machine frame (dq) to network (phase a real,imag) reference frame */
#undef __FUNCT__
#define __FUNCT__ "dq2ri"
PetscErrorCode dq2ri(PetscScalar Fd,PetscScalar Fq,PetscScalar delta,PetscScalar *Fr, PetscScalar *Fi)
{
  PetscFunctionBegin;
  *Fr =  Fd*sin(delta) + Fq*cos(delta);
  *Fi = -Fd*cos(delta) + Fq*sin(delta);
  PetscFunctionReturn(0);
}

/* Converts from network frame ([phase a real,imag) to machine (dq) reference frame */
#undef __FUNCT__
#define __FUNCT__ "ri2dq"
PetscErrorCode ri2dq(PetscScalar Fr,PetscScalar Fi,PetscScalar delta,PetscScalar *Fd, PetscScalar *Fq)
{
  PetscFunctionBegin;
  *Fd =  Fr*sin(delta) - Fi*cos(delta);
  *Fq =  Fr*cos(delta) + Fi*sin(delta);
  PetscFunctionReturn(0);
}

/* Saves the solution at each time to a matrix */
#undef __FUNCT__
#define __FUNCT__ "SaveSolution"
PetscErrorCode SaveSolution(TS ts)
{
  PetscErrorCode ierr;
  Userctx  *user;
  Vec       X;
  PetscScalar *x,*mat;
  PetscInt   idx;
  PetscReal  t;

  PetscFunctionBegin;
  ierr = TSGetApplicationContext(ts,&user);CHKERRQ(ierr);
  ierr = TSGetTime(ts,&t);CHKERRQ(ierr);
  ierr = TSGetSolution(ts,&X);CHKERRQ(ierr);
  idx = user->stepnum*(user->neqs_pgrid+1);
  ierr = MatDenseGetArray(user->Sol,&mat);CHKERRQ(ierr);
  ierr = VecGetArray(X,&x);CHKERRQ(ierr);
  mat[idx] = t;
  ierr = PetscMemcpy(mat+idx+1,x,user->neqs_pgrid*sizeof(PetscScalar));CHKERRQ(ierr);
  ierr = MatDenseRestoreArray(user->Sol,&mat);CHKERRQ(ierr);
  ierr = VecRestoreArray(X,&x);CHKERRQ(ierr);
  user->stepnum++;

  PetscFunctionReturn(0);
}
#undef __FUNCT__
#define __FUNCT__ "SetInitialGuess"
PetscErrorCode SetInitialGuess(Vec X,Userctx* user)
{
  PetscErrorCode ierr;
  Vec            Xgen,Xnet;
  PetscScalar    *xgen,*xnet;
  PetscInt       i,idx=0;
  PetscScalar    Vr,Vi,IGr,IGi,Vm,Vm2;
  PetscScalar    Eqp,Edp,delta;
  PetscScalar    Efd,RF,VR; /* Exciter variables */
  PetscScalar    Id,Iq;  /* Generator dq axis currents */
  PetscScalar    theta,Vd,Vq,SE;

  PetscFunctionBegin;

  M[0] = 2*H[0]/w_s; M[1] = 2*H[1]/w_s; M[2] = 2*H[2]/w_s;
  D[0] = 0.1*M[0]; D[1] = 0.1*M[1]; D[2] = 0.1*M[2];

  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);

  /* Network subsystem initialization */
  ierr = VecCopy(user->V0,Xnet);CHKERRQ(ierr);

  /* Generator subsystem initialization */
  ierr = VecGetArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecGetArray(Xnet,&xnet);CHKERRQ(ierr);

  for(i=0; i < ngen; i++) {
    Vr = xnet[2*gbus[i]]; /* Real part of generator terminal voltage */
    Vi = xnet[2*gbus[i]+1]; /* Imaginary part of the generator terminal voltage */
    Vm = PetscSqrtScalar(Vr*Vr + Vi*Vi); Vm2 = Vm*Vm;
    IGr = (Vr*PG[i] + Vi*QG[i])/Vm2;
    IGi = (Vi*PG[i] - Vr*QG[i])/Vm2;

    delta = atan2(Vi+Xq[i]*IGr,Vr-Xq[i]*IGi); /* Machine angle */

    theta = PETSC_PI/2.0 - delta;

    Id = IGr*cos(theta) - IGi*sin(theta); /* d-axis stator current */
    Iq = IGr*sin(theta) + IGi*cos(theta); /* q-axis stator current */

    Vd = Vr*cos(theta) - Vi*sin(theta);
    Vq = Vr*sin(theta) + Vi*cos(theta);

    Edp = Vd + Rs[i]*Id - Xqp[i]*Iq; /* d-axis transient EMF */
    Eqp = Vq + Rs[i]*Iq + Xdp[i]*Id; /* q-axis transient EMF */

    TM[i] = PG[i];

    /* The generator variables are ordered as [Eqp,Edp,delta,w,Id,Iq] */
    xgen[idx]   = Eqp;
    xgen[idx+1] = Edp;
    xgen[idx+2] = delta;
    xgen[idx+3] = w_s;

    idx = idx + 4;

    xgen[idx] = Id;
    xgen[idx+1] = Iq;

    idx = idx + 2;

    /* Exciter */
    Efd = Eqp + (Xd[i] - Xdp[i])*Id;
    SE  = k1[i]*PetscExpScalar(k2[i]*Efd);
    VR  =  KE[i]*Efd + SE;
    RF  =  KF[i]*Efd/TF[i];

    xgen[idx]   = Efd;
    xgen[idx+1] = RF;
    xgen[idx+2] = VR;

    Vref[i] = Vm + (VR/KA[i]);

    idx = idx + 3;
  }

  ierr = VecRestoreArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Xnet,&xnet);CHKERRQ(ierr);

  /* ierr = VecView(Xgen,0);CHKERRQ(ierr); */
  ierr = DMCompositeGather(user->dmpgrid,X,INSERT_VALUES,Xgen,Xnet);CHKERRQ(ierr);
  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "IFunction"
PetscErrorCode IFunction(TS ts,PetscReal t,Vec X, Vec Xdot, Vec F, Userctx* user)
{
  PetscErrorCode ierr;
  Vec            Xgen,Xnet,Xdotgen,Xdotnet,Fgen,Fnet;
  PetscScalar    *xgen,*xnet,*xdotgen,*fgen,*fnet;
  PetscInt       i,idx=0;
  PetscScalar    Vr,Vi,Vm,Vm2;
  PetscScalar    Eqp,Edp,delta,w; /* Generator variables */
  PetscScalar    Efd,RF,VR; /* Exciter variables */
  PetscScalar    Id,Iq;  /* Generator dq axis currents */
  PetscScalar    Vd,Vq,SE;
  PetscScalar    IGr,IGi,IDr,IDi;

  PetscFunctionBegin;

  ierr = VecZeroEntries(F);CHKERRQ(ierr);
  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);
  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Xdotgen,&Xdotnet);CHKERRQ(ierr);
  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Fgen,&Fnet);CHKERRQ(ierr);
  ierr = DMCompositeScatter(user->dmpgrid,X,Xgen,Xnet);CHKERRQ(ierr);
  ierr = DMCompositeScatter(user->dmpgrid,Xdot,Xdotgen,Xdotnet);CHKERRQ(ierr);
  ierr = DMCompositeScatter(user->dmpgrid,F,Fgen,Fnet);CHKERRQ(ierr);

  /* Network current balance residual IG + Y*V + IL = 0. Only YV is added here.
     The generator current injection, IG, and load current injection, ID are added later
  */
  /* Note that the values in Ybus are stored assuming the imaginary current balance
     equation is ordered first followed by real current balance equation for each bus.
     Thus imaginary current contribution goes in location 2*i, and
     real current contribution in 2*i+1
  */
  ierr = MatMult(user->Ybus,Xnet,Fnet);

  ierr = VecGetArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecGetArray(Xnet,&xnet);CHKERRQ(ierr);
  ierr = VecGetArray(Xdotgen,&xdotgen);CHKERRQ(ierr);
  ierr = VecGetArray(Fgen,&fgen);CHKERRQ(ierr);
  ierr = VecGetArray(Fnet,&fnet);CHKERRQ(ierr);

  /* Generator subsystem */
  for(i=0; i < ngen; i++) {
    Eqp   = xgen[idx];
    Edp   = xgen[idx+1];
    delta = xgen[idx+2];
    w     = xgen[idx+3];
    Id    = xgen[idx+4];
    Iq    = xgen[idx+5];
    Efd   = xgen[idx+6];
    RF    = xgen[idx+7];
    VR    = xgen[idx+8];

    /* Generator differential equations */
    fgen[idx]   = Td0p[i]*xdotgen[idx] + Eqp + (Xd[i] - Xdp[i])*Id - Efd;
    fgen[idx+1] = Tq0p[i]*xdotgen[idx+1] + Edp - (Xq[i] - Xqp[i])*Iq;
    fgen[idx+2] = xdotgen[idx+2] - w + w_s;
    fgen[idx+3] = M[i]*xdotgen[idx+3] - TM[i] + Edp*Id + Eqp*Iq + (Xqp[i] - Xdp[i])*Id*Iq + D[i]*(w - w_s);

    Vr = xnet[2*gbus[i]]; /* Real part of generator terminal voltage */
    Vi = xnet[2*gbus[i]+1]; /* Imaginary part of the generator terminal voltage */

    ierr = ri2dq(Vr,Vi,delta,&Vd,&Vq);CHKERRQ(ierr);
    /* Algebraic equations for stator currents */
    PetscScalar Zdq_inv[4],det;
    det = Rs[i]*Rs[i] + Xdp[i]*Xqp[i];
    Zdq_inv[0] = Rs[i]/det;
    Zdq_inv[1] = Xqp[i]/det;
    Zdq_inv[2] = -Xdp[i]/det;
    Zdq_inv[3] = Rs[i]/det;

    fgen[idx+4] = Zdq_inv[0]*(-Edp + Vd) + Zdq_inv[1]*(-Eqp + Vq) + Id;
    fgen[idx+5] = Zdq_inv[2]*(-Edp + Vd) + Zdq_inv[3]*(-Eqp + Vq) + Iq;

    /* Add generator current injection to network */
    ierr = dq2ri(Id,Iq,delta,&IGr,&IGi);CHKERRQ(ierr);
    fnet[2*gbus[i]]   -= IGi;
    fnet[2*gbus[i]+1] -= IGr;

    Vm = PetscSqrtScalar(Vd*Vd + Vq*Vq); Vm2 = Vm*Vm;

    SE  = k1[i]*PetscExpScalar(k2[i]*Efd);

    /* Exciter differential equations */
    fgen[idx+6] = TE[i]*xdotgen[idx+6] + KE[i]*Efd + SE - VR;
    fgen[idx+7] = TF[i]*xdotgen[idx+7] + RF - KF[i]*Efd/TF[i];
    fgen[idx+8] = TA[i]*xdotgen[idx+8] + VR - KA[i]*RF + KA[i]*KF[i]*Efd/TF[i] - KA[i]*(Vref[i] - Vm);

    idx = idx + 9;
  }

  PetscScalar PD,QD,Vm0,*v0;
  PetscInt    k;
  ierr = VecGetArray(user->V0,&v0);CHKERRQ(ierr);
  for(i=0; i < nload; i++) {
    Vr = xnet[2*lbus[i]]; /* Real part of load bus voltage */
    Vi = xnet[2*lbus[i]+1]; /* Imaginary part of the load bus voltage */
    Vm = PetscSqrtScalar(Vr*Vr + Vi*Vi); Vm2 = Vm*Vm;
    Vm0 = PetscSqrtScalar(v0[2*lbus[i]]*v0[2*lbus[i]] + v0[2*lbus[i]+1]*v0[2*lbus[i]+1]);
    PD = QD = 0.0;
    for(k=0; k < ld_nsegsp[i];k++) PD += ld_alphap[k]*PD0[i]*PetscPowScalar((Vm/Vm0),ld_betap[k]);
    for(k=0; k < ld_nsegsq[i];k++) QD += ld_alphaq[k]*QD0[i]*PetscPowScalar((Vm/Vm0),ld_betaq[k]);

    /* Load currents */
    IDr = (PD*Vr + QD*Vi)/Vm2;
    IDi = (-QD*Vr + PD*Vi)/Vm2;

    fnet[2*lbus[i]]   += IDi;
    fnet[2*lbus[i]+1] += IDr;
  }
  ierr = VecRestoreArray(user->V0,&v0);CHKERRQ(ierr);

  ierr = VecRestoreArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Xnet,&xnet);CHKERRQ(ierr);
  ierr = VecRestoreArray(Xdotgen,&xdotgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Fgen,&fgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Fnet,&fnet);CHKERRQ(ierr);

  ierr = DMCompositeGather(user->dmpgrid,F,INSERT_VALUES,Fgen,Fnet);CHKERRQ(ierr);
  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);
  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Xdotgen,&Xdotnet);CHKERRQ(ierr);
  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Fgen,&Fnet);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "PreallocateJacobian"
PetscErrorCode PreallocateJacobian(Mat J, Userctx *user)
{
  PetscErrorCode ierr;
  PetscInt       *d_nnz;
  PetscInt       i,idx=0,start=0;

  PetscFunctionBegin;

  ierr = PetscMalloc(user->neqs_pgrid*sizeof(PetscInt),&d_nnz);CHKERRQ(ierr);
  for(i=0;i<user->neqs_pgrid;i++) d_nnz[i] = 0;
  /* Generator subsystem */
  for(i=0; i < ngen; i++) {

    d_nnz[idx] += 3;
    d_nnz[idx+1] += 2;
    d_nnz[idx+2] += 2;
    d_nnz[idx+3] += 5;
    d_nnz[idx+4] += 6;
    d_nnz[idx+5] += 6;

    d_nnz[user->neqs_gen+2*gbus[i]] += 3;
    d_nnz[user->neqs_gen+2*gbus[i]+1] += 3;

    d_nnz[idx+6] += 2;
    d_nnz[idx+7] += 2;
    d_nnz[idx+8] += 5;

    idx = idx + 9;
  }

  start = user->neqs_gen;

  PetscInt       ncols;

  for(i=0;i < nbus;i++) {
    ierr = MatGetRow(user->Ybus,2*i,&ncols,PETSC_NULL,PETSC_NULL);CHKERRQ(ierr);
    d_nnz[start+2*i]   += ncols;
    d_nnz[start+2*i+1] += ncols;
    ierr = MatRestoreRow(user->Ybus,2*i,&ncols,PETSC_NULL,PETSC_NULL);CHKERRQ(ierr);
  }

  ierr = MatSeqAIJSetPreallocation(J,PETSC_NULL,d_nnz);CHKERRQ(ierr);

  ierr = PetscFree(d_nnz);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "IJacobian"
PetscErrorCode IJacobian(TS ts,PetscReal t,Vec X,Vec Xdot,PetscReal a,Mat *A,Mat *B,MatStructure *flag,Userctx* user)
{
  PetscErrorCode ierr;
  Vec            Xgen,Xnet;
  PetscScalar    *xgen,*xnet;
  PetscInt       i,idx=0;
  PetscScalar    Vr,Vi,Vm,Vm2;
  PetscScalar    Eqp,Edp,delta,w; /* Generator variables */
  PetscScalar    Efd,RF,VR; /* Exciter variables */
  PetscScalar    Id,Iq;  /* Generator dq axis currents */
  PetscScalar    Vd,Vq;
  Mat            J=*A;
  PetscScalar    val[10];
  PetscInt       row[2],col[10];
  PetscInt       net_start=user->neqs_gen;

  PetscFunctionBegin;

  *flag = SAME_NONZERO_PATTERN;
  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);
  ierr = DMCompositeScatter(user->dmpgrid,X,Xgen,Xnet);CHKERRQ(ierr);

  ierr = VecGetArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecGetArray(Xnet,&xnet);CHKERRQ(ierr);

  /* Generator subsystem */
  for(i=0; i < ngen; i++) {
    Eqp   = xgen[idx];
    Edp   = xgen[idx+1];
    delta = xgen[idx+2];
    w     = xgen[idx+3];
    Id    = xgen[idx+4];
    Iq    = xgen[idx+5];
    Efd   = xgen[idx+6];
    RF    = xgen[idx+7];
    VR    = xgen[idx+8];

    //    fgen[idx]   = Td0p[i]*xdotgen[idx] + Eqp + (Xd[i] - Xdp[i])*Id - Efd;
    row[0] = idx;
    col[0] = idx;           col[1] = idx+4;          col[2] = idx+6;
    val[0] = Td0p[i]*a + 1; val[1] = Xd[i] - Xdp[i]; val[2] = -1;

    ierr = MatSetValues(J,1,row,3,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+1] = Tq0p[i]*xdotgen[idx+1] + Edp - (Xq[i] - Xqp[i])*Iq;
    row[0] = idx + 1;
    col[0] = idx + 1;       col[1] = idx+5;
    val[0] = Tq0p[i]*a + 1; val[1] = -(Xq[i] - Xqp[i]);
    ierr = MatSetValues(J,1,row,2,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+2] = xdotgen[idx+2] - w + w_s;
    row[0]  = idx + 2;
    col[0] = idx + 2; col[1] = idx + 3;
    val[0] = a;       val[1] = -1;
    ierr = MatSetValues(J,1,row,2,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+3] = M[i]*xdotgen[idx+3] - TM[i] + Edp*Id + Eqp*Iq + (Xqp[i] - Xdp[i])*Id*Iq + D[i]*(w - w_s);
    row[0] = idx + 3;
    col[0] = idx; col[1] = idx + 1; col[2] = idx + 3;       col[3] = idx + 4;                  col[4] = idx + 5;
    val[0] = Iq;  val[1] = Id;      val[2] = M[i]*a + D[i]; val[3] = Edp + (Xqp[i]-Xdp[i])*Iq; val[4] = Eqp + (Xqp[i] - Xdp[i])*Id;
    ierr = MatSetValues(J,1,row,5,col,val,INSERT_VALUES);CHKERRQ(ierr);

    Vr = xnet[2*gbus[i]]; /* Real part of generator terminal voltage */
    Vi = xnet[2*gbus[i]+1]; /* Imaginary part of the generator terminal voltage */
    ierr = ri2dq(Vr,Vi,delta,&Vd,&Vq);CHKERRQ(ierr);

    PetscScalar Zdq_inv[4],det;
    det = Rs[i]*Rs[i] + Xdp[i]*Xqp[i];
    Zdq_inv[0] = Rs[i]/det;
    Zdq_inv[1] = Xqp[i]/det;
    Zdq_inv[2] = -Xdp[i]/det;
    Zdq_inv[3] = Rs[i]/det;

    PetscScalar dVd_dVr,dVd_dVi,dVq_dVr,dVq_dVi,dVd_ddelta,dVq_ddelta;
    dVd_dVr = sin(delta); dVd_dVi = -cos(delta);
    dVq_dVr = cos(delta); dVq_dVi = sin(delta);
    dVd_ddelta = Vr*cos(delta) + Vi*sin(delta);
    dVq_ddelta = -Vr*sin(delta) + Vi*cos(delta);

    //    fgen[idx+4] = Zdq_inv[0]*(-Edp + Vd) + Zdq_inv[1]*(-Eqp + Vq) + Id;
    row[0] = idx+4;
    col[0] = idx;         col[1] = idx+1;        col[2] = idx + 2;
    val[0] = -Zdq_inv[1]; val[1] = -Zdq_inv[0];  val[2] = Zdq_inv[0]*dVd_ddelta + Zdq_inv[1]*dVq_ddelta;
    col[3] = idx + 4; col[4] = net_start+2*gbus[i];                     col[5] = net_start + 2*gbus[i]+1;
    val[3] = 1;       val[4] = Zdq_inv[0]*dVd_dVr + Zdq_inv[1]*dVq_dVr; val[5] = Zdq_inv[0]*dVd_dVi + Zdq_inv[1]*dVq_dVi;
    ierr = MatSetValues(J,1,row,6,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //  fgen[idx+5] = Zdq_inv[2]*(-Edp + Vd) + Zdq_inv[3]*(-Eqp + Vq) + Iq;
    row[0] = idx+5;
    col[0] = idx;         col[1] = idx+1;        col[2] = idx + 2;
    val[0] = -Zdq_inv[3]; val[1] = -Zdq_inv[2];  val[2] = Zdq_inv[2]*dVd_ddelta + Zdq_inv[3]*dVq_ddelta;
    col[3] = idx + 5; col[4] = net_start+2*gbus[i];                     col[5] = net_start + 2*gbus[i]+1;
    val[3] = 1;       val[4] = Zdq_inv[2]*dVd_dVr + Zdq_inv[3]*dVq_dVr; val[5] = Zdq_inv[2]*dVd_dVi + Zdq_inv[3]*dVq_dVi;
    ierr = MatSetValues(J,1,row,6,col,val,INSERT_VALUES);CHKERRQ(ierr);

    PetscScalar dIGr_ddelta,dIGi_ddelta,dIGr_dId,dIGr_dIq,dIGi_dId,dIGi_dIq;
    dIGr_ddelta = Id*cos(delta) - Iq*sin(delta);
    dIGi_ddelta = Id*sin(delta) + Iq*cos(delta);
    dIGr_dId = sin(delta);  dIGr_dIq = cos(delta);
    dIGi_dId = -cos(delta); dIGi_dIq = sin(delta);

    // fnet[2*gbus[i]]   -= IGi;
    row[0] = net_start + 2*gbus[i];
    col[0] = idx+2;        col[1] = idx + 4;   col[2] = idx + 5;
    val[0] = -dIGi_ddelta; val[1] = -dIGi_dId; val[2] = -dIGi_dIq;
    ierr = MatSetValues(J,1,row,3,col,val,INSERT_VALUES);CHKERRQ(ierr);

    // fnet[2*gbus[i]+1]   -= IGr;
    row[0] = net_start + 2*gbus[i]+1;
    col[0] = idx+2;        col[1] = idx + 4;   col[2] = idx + 5;
    val[0] = -dIGr_ddelta; val[1] = -dIGr_dId; val[2] = -dIGr_dIq;
    ierr = MatSetValues(J,1,row,3,col,val,INSERT_VALUES);CHKERRQ(ierr);

    Vm = PetscSqrtScalar(Vd*Vd + Vq*Vq); Vm2 = Vm*Vm;

    //    fgen[idx+6] = TE[i]*xdotgen[idx+6] + KE[i]*Efd + SE - VR;
    /*    SE  = k1[i]*PetscExpScalar(k2[i]*Efd); */
    PetscScalar dSE_dEfd;
    dSE_dEfd = k1[i]*k2[i]*PetscExpScalar(k2[i]*Efd);

    row[0] = idx + 6;
    col[0] = idx + 6;                     col[1] = idx + 8;
    val[0] = TE[i]*a + KE[i] + dSE_dEfd;  val[1] = -1;
    ierr = MatSetValues(J,1,row,2,col,val,INSERT_VALUES);CHKERRQ(ierr);

    /* Exciter differential equations */

    //    fgen[idx+7] = TF[i]*xdotgen[idx+7] + RF - KF[i]*Efd/TF[i];
    row[0] = idx + 7;
    col[0] = idx + 6;       col[1] = idx + 7;
    val[0] = -KF[i]/TF[i];  val[1] = TF[i]*a + 1;
    ierr = MatSetValues(J,1,row,2,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+8] = TA[i]*xdotgen[idx+8] + VR - KA[i]*RF + KA[i]*KF[i]*Efd/TF[i] - KA[i]*(Vref[i] - Vm);
    // Vm = (Vd^2 + Vq^2)^0.5;
    PetscScalar dVm_dVd,dVm_dVq,dVm_dVr,dVm_dVi,dVm_ddelta;
    dVm_dVd = Vd/Vm; dVm_dVq = Vq/Vm;
    dVm_ddelta = dVm_dVd*dVd_ddelta + dVm_dVq*dVq_ddelta;
    dVm_dVr = dVm_dVd*dVd_dVr + dVm_dVq*dVq_dVr;
    dVm_dVi = dVm_dVd*dVd_dVi + dVm_dVq*dVq_dVi;
    row[0] = idx + 8;
    col[0] = idx + 6;           col[1] = idx + 7; col[2] = idx + 8;
    val[0] = KA[i]*KF[i]/TF[i]; val[1] = -KA[i];  val[2] = TA[i]*a + 1;
    col[3] = net_start + 2*gbus[i]; col[4] = net_start + 2*gbus[i]+1;
    val[3] = KA[i]*dVm_dVr;         val[4] = KA[i]*dVm_dVi;
    ierr = MatSetValues(J,1,row,5,col,val,INSERT_VALUES);CHKERRQ(ierr);
    idx = idx + 9;
  }

  PetscInt ncols;
  const PetscInt *cols;
  const PetscScalar *yvals;
  PetscInt k;

  for(i=0;i<nbus;i++) {
    ierr = MatGetRow(user->Ybus,2*i,&ncols,&cols,&yvals);CHKERRQ(ierr);
    row[0] = net_start + 2*i;
    for(k=0;k<ncols;k++) {
      col[k] = net_start + cols[k];
      val[k] = yvals[k];
    }
    ierr = MatSetValues(J,1,row,ncols,col,val,INSERT_VALUES);CHKERRQ(ierr);
    ierr = MatRestoreRow(user->Ybus,2*i,&ncols,&cols,&yvals);CHKERRQ(ierr);

    ierr = MatGetRow(user->Ybus,2*i+1,&ncols,&cols,&yvals);CHKERRQ(ierr);
    row[0] = net_start + 2*i+1;
    for(k=0;k<ncols;k++) {
      col[k] = net_start + cols[k];
      val[k] = yvals[k];
    }
    ierr = MatSetValues(J,1,row,ncols,col,val,INSERT_VALUES);CHKERRQ(ierr);
    ierr = MatRestoreRow(user->Ybus,2*i+1,&ncols,&cols,&yvals);CHKERRQ(ierr);
  }

  ierr = MatAssemblyBegin(J,MAT_FLUSH_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(J,MAT_FLUSH_ASSEMBLY);CHKERRQ(ierr);

  PetscScalar PD,QD,Vm0,*v0,Vm4;
  PetscScalar dPD_dVr,dPD_dVi,dQD_dVr,dQD_dVi;
  PetscScalar dIDr_dVr,dIDr_dVi,dIDi_dVr,dIDi_dVi;

  ierr = VecGetArray(user->V0,&v0);CHKERRQ(ierr);
  for(i=0; i < nload; i++) {
    Vr = xnet[2*lbus[i]]; /* Real part of load bus voltage */
    Vi = xnet[2*lbus[i]+1]; /* Imaginary part of the load bus voltage */
    Vm = PetscSqrtScalar(Vr*Vr + Vi*Vi); Vm2 = Vm*Vm; Vm4 = Vm2*Vm2;
    Vm0 = PetscSqrtScalar(v0[2*lbus[i]]*v0[2*lbus[i]] + v0[2*lbus[i]+1]*v0[2*lbus[i]+1]);
    PD = QD = 0.0;
    dPD_dVr = dPD_dVi = dQD_dVr = dQD_dVi = 0.0;
    for(k=0; k < ld_nsegsp[i];k++) {
      PD += ld_alphap[k]*PD0[i]*PetscPowScalar((Vm/Vm0),ld_betap[k]);
      dPD_dVr += ld_alphap[k]*ld_betap[k]*PD0[i]*PetscPowScalar((1/Vm0),ld_betap[k])*Vr*PetscPowScalar(Vm,(ld_betap[k]-2));
      dPD_dVi += ld_alphap[k]*ld_betap[k]*PD0[i]*PetscPowScalar((1/Vm0),ld_betap[k])*Vi*PetscPowScalar(Vm,(ld_betap[k]-2));
    }
    for(k=0; k < ld_nsegsq[i];k++) {
      QD += ld_alphaq[k]*QD0[i]*PetscPowScalar((Vm/Vm0),ld_betaq[k]);
      dQD_dVr += ld_alphaq[k]*ld_betaq[k]*QD0[i]*PetscPowScalar((1/Vm0),ld_betaq[k])*Vr*PetscPowScalar(Vm,(ld_betaq[k]-2));
      dQD_dVi += ld_alphaq[k]*ld_betaq[k]*QD0[i]*PetscPowScalar((1/Vm0),ld_betaq[k])*Vi*PetscPowScalar(Vm,(ld_betaq[k]-2));
    }

    //    IDr = (PD*Vr + QD*Vi)/Vm2;
    //    IDi = (-QD*Vr + PD*Vi)/Vm2;

    dIDr_dVr = (dPD_dVr*Vr + dQD_dVr*Vi + PD)/Vm2 - ((PD*Vr + QD*Vi)*2*Vr)/Vm4;
    dIDr_dVi = (dPD_dVi*Vr + dQD_dVi*Vi + QD)/Vm2 - ((PD*Vr + QD*Vi)*2*Vi)/Vm4;

    dIDi_dVr = (-dQD_dVr*Vr + dPD_dVr*Vi - QD)/Vm2 - ((-QD*Vr + PD*Vi)*2*Vr)/Vm4;
    dIDi_dVi = (-dQD_dVi*Vr + dPD_dVi*Vi + PD)/Vm2 - ((-QD*Vr + PD*Vi)*2*Vi)/Vm4;


    //    fnet[2*lbus[i]]   += IDi;
    row[0] = net_start + 2*lbus[i];
    col[0] = net_start + 2*lbus[i];  col[1] = net_start + 2*lbus[i]+1;
    val[0] = dIDi_dVr;               val[1] = dIDi_dVi;
    ierr = MatSetValues(J,1,row,2,col,val,ADD_VALUES);CHKERRQ(ierr);
    //    fnet[2*lbus[i]+1] += IDr;
    row[0] = net_start + 2*lbus[i]+1;
    col[0] = net_start + 2*lbus[i];  col[1] = net_start + 2*lbus[i]+1;
    val[0] = dIDr_dVr;               val[1] = dIDr_dVi;
    ierr = MatSetValues(J,1,row,2,col,val,ADD_VALUES);CHKERRQ(ierr);
  }
  ierr = VecRestoreArray(user->V0,&v0);CHKERRQ(ierr);

  ierr = VecRestoreArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Xnet,&xnet);CHKERRQ(ierr);

  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);

  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}

/* This is the same function as IFunction except that only the functions relating
   to Idq and V are active
*/
#undef __FUNCT__
#define __FUNCT__ "AlgFunction"
PetscErrorCode AlgFunction(SNES snes,Vec X, Vec F, void* ctx)
{
  PetscErrorCode ierr;
  Userctx        *user=(Userctx*)ctx;
  Vec            Xgen,Xnet,Fgen,Fnet;
  PetscScalar    *xgen,*xnet,*fgen,*fnet;
  PetscInt       i,idx=0;
  PetscScalar    Vr,Vi,Vm,Vm2;
  PetscScalar    Eqp,Edp,delta,w; /* Generator variables */
  PetscScalar    Efd,RF,VR; /* Exciter variables */
  PetscScalar    Id,Iq;  /* Generator dq axis currents */
  PetscScalar    Vd,Vq;
  PetscScalar    IGr,IGi,IDr,IDi;

  PetscFunctionBegin;

  ierr = VecZeroEntries(F);CHKERRQ(ierr);
  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);
  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Fgen,&Fnet);CHKERRQ(ierr);
  ierr = DMCompositeScatter(user->dmpgrid,X,Xgen,Xnet);CHKERRQ(ierr);
  ierr = DMCompositeScatter(user->dmpgrid,F,Fgen,Fnet);CHKERRQ(ierr);

  /* Network current balance residual IG + Y*V + IL = 0. Only YV is added here.
     The generator current injection, IG, and load current injection, ID are added later
  */
  /* Note that the values in Ybus are stored assuming the imaginary current balance
     equation is ordered first followed by real current balance equation for each bus.
     Thus imaginary current contribution goes in location 2*i, and
     real current contribution in 2*i+1
  */
  ierr = MatMult(user->Ybus,Xnet,Fnet);

  ierr = VecGetArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecGetArray(Xnet,&xnet);CHKERRQ(ierr);
  ierr = VecGetArray(Fgen,&fgen);CHKERRQ(ierr);
  ierr = VecGetArray(Fnet,&fnet);CHKERRQ(ierr);

  /* Generator subsystem */
  for(i=0; i < ngen; i++) {
    Eqp   = xgen[idx];
    Edp   = xgen[idx+1];
    delta = xgen[idx+2];
    w     = xgen[idx+3];
    Id    = xgen[idx+4];
    Iq    = xgen[idx+5];
    Efd   = xgen[idx+6];
    RF    = xgen[idx+7];
    VR    = xgen[idx+8];

    Vr = xnet[2*gbus[i]]; /* Real part of generator terminal voltage */
    Vi = xnet[2*gbus[i]+1]; /* Imaginary part of the generator terminal voltage */

    ierr = ri2dq(Vr,Vi,delta,&Vd,&Vq);CHKERRQ(ierr);
    /* Algebraic equations for stator currents */
    PetscScalar Zdq_inv[4],det;
    det = Rs[i]*Rs[i] + Xdp[i]*Xqp[i];
    Zdq_inv[0] = Rs[i]/det;
    Zdq_inv[1] = Xqp[i]/det;
    Zdq_inv[2] = -Xdp[i]/det;
    Zdq_inv[3] = Rs[i]/det;

    fgen[idx+4] = Zdq_inv[0]*(-Edp + Vd) + Zdq_inv[1]*(-Eqp + Vq) + Id;
    fgen[idx+5] = Zdq_inv[2]*(-Edp + Vd) + Zdq_inv[3]*(-Eqp + Vq) + Iq;

    /* Add generator current injection to network */
    ierr = dq2ri(Id,Iq,delta,&IGr,&IGi);CHKERRQ(ierr);
    fnet[2*gbus[i]]   -= IGi;
    fnet[2*gbus[i]+1] -= IGr;

    idx = idx + 9;
  }

  PetscScalar PD,QD,Vm0,*v0;
  PetscInt    k;
  ierr = VecGetArray(user->V0,&v0);CHKERRQ(ierr);
  for(i=0; i < nload; i++) {
    Vr = xnet[2*lbus[i]]; /* Real part of load bus voltage */
    Vi = xnet[2*lbus[i]+1]; /* Imaginary part of the load bus voltage */
    Vm = PetscSqrtScalar(Vr*Vr + Vi*Vi); Vm2 = Vm*Vm;
    Vm0 = PetscSqrtScalar(v0[2*lbus[i]]*v0[2*lbus[i]] + v0[2*lbus[i]+1]*v0[2*lbus[i]+1]);
    PD = QD = 0.0;
    for(k=0; k < ld_nsegsp[i];k++) PD += ld_alphap[k]*PD0[i]*PetscPowScalar((Vm/Vm0),ld_betap[k]);
    for(k=0; k < ld_nsegsq[i];k++) QD += ld_alphaq[k]*QD0[i]*PetscPowScalar((Vm/Vm0),ld_betaq[k]);

    /* Load currents */
    IDr = (PD*Vr + QD*Vi)/Vm2;
    IDi = (-QD*Vr + PD*Vi)/Vm2;

    fnet[2*lbus[i]]   += IDi;
    fnet[2*lbus[i]+1] += IDr;
  }
  ierr = VecRestoreArray(user->V0,&v0);CHKERRQ(ierr);

  ierr = VecRestoreArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Xnet,&xnet);CHKERRQ(ierr);
  ierr = VecRestoreArray(Fgen,&fgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Fnet,&fnet);CHKERRQ(ierr);

  ierr = DMCompositeGather(user->dmpgrid,F,INSERT_VALUES,Fgen,Fnet);CHKERRQ(ierr);
  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);
  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Fgen,&Fnet);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}

/* This has the same Jacobian elements as IJacobian except for rows corresponding to
   xgen. Since all xgen variables are held constant, we put a 1.0 on the diagonal
*/
#undef __FUNCT__
#define __FUNCT__ "AlgJacobian"
PetscErrorCode AlgJacobian(SNES snes,Vec X,Mat *A,Mat *B,MatStructure *flag,void* ctx)
{
  PetscErrorCode ierr;
  Userctx        *user=(Userctx*)ctx;
  Vec            Xgen,Xnet;
  PetscScalar    *xgen,*xnet;
  PetscInt       i,idx=0;
  PetscScalar    Vr,Vi,Vm,Vm2;
  PetscScalar    Eqp,Edp,delta,w; /* Generator variables */
  PetscScalar    Efd,RF,VR; /* Exciter variables */
  PetscScalar    Id,Iq;  /* Generator dq axis currents */
  PetscScalar    Vd,Vq;
  Mat            J=*A;
  PetscScalar    val[10];
  PetscInt       row[2],col[10];
  PetscInt       net_start=user->neqs_gen;

  PetscFunctionBegin;

  *flag = SAME_NONZERO_PATTERN;
  ierr = DMCompositeGetLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);
  ierr = DMCompositeScatter(user->dmpgrid,X,Xgen,Xnet);CHKERRQ(ierr);

  ierr = VecGetArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecGetArray(Xnet,&xnet);CHKERRQ(ierr);

  /* Generator subsystem */
  for(i=0; i < ngen; i++) {
    Eqp   = xgen[idx];
    Edp   = xgen[idx+1];
    delta = xgen[idx+2];
    w     = xgen[idx+3];
    Id    = xgen[idx+4];
    Iq    = xgen[idx+5];
    Efd   = xgen[idx+6];
    RF    = xgen[idx+7];
    VR    = xgen[idx+8];

    //    fgen[idx]   = Td0p[i]*xdotgen[idx] + Eqp + (Xd[i] - Xdp[i])*Id - Efd;
    row[0] = idx;
    col[0] = idx;
    val[0] = 1.0;

    ierr = MatSetValues(J,1,row,1,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+1] = Tq0p[i]*xdotgen[idx+1] + Edp - (Xq[i] - Xqp[i])*Iq;
    row[0] = idx + 1;
    col[0] = idx + 1;
    val[0] = 1;
    ierr = MatSetValues(J,1,row,1,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+2] = xdotgen[idx+2] - w + w_s;
    row[0]  = idx + 2;
    col[0] = idx + 2;
    val[0] = 1;
    ierr = MatSetValues(J,1,row,1,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+3] = M[i]*xdotgen[idx+3] - TM[i] + Edp*Id + Eqp*Iq + (Xqp[i] - Xdp[i])*Id*Iq + D[i]*(w - w_s);
    row[0] = idx + 3;
    col[0] = idx+3;
    val[0] = 1.0;
    ierr = MatSetValues(J,1,row,1,col,val,INSERT_VALUES);CHKERRQ(ierr);

    Vr = xnet[2*gbus[i]]; /* Real part of generator terminal voltage */
    Vi = xnet[2*gbus[i]+1]; /* Imaginary part of the generator terminal voltage */
    ierr = ri2dq(Vr,Vi,delta,&Vd,&Vq);CHKERRQ(ierr);

    PetscScalar Zdq_inv[4],det;
    det = Rs[i]*Rs[i] + Xdp[i]*Xqp[i];
    Zdq_inv[0] = Rs[i]/det;
    Zdq_inv[1] = Xqp[i]/det;
    Zdq_inv[2] = -Xdp[i]/det;
    Zdq_inv[3] = Rs[i]/det;

    PetscScalar dVd_dVr,dVd_dVi,dVq_dVr,dVq_dVi,dVd_ddelta,dVq_ddelta;
    dVd_dVr = sin(delta); dVd_dVi = -cos(delta);
    dVq_dVr = cos(delta); dVq_dVi = sin(delta);
    dVd_ddelta = Vr*cos(delta) + Vi*sin(delta);
    dVq_ddelta = -Vr*sin(delta) + Vi*cos(delta);

    //    fgen[idx+4] = Zdq_inv[0]*(-Edp + Vd) + Zdq_inv[1]*(-Eqp + Vq) + Id;
    row[0] = idx+4;
    col[0] = idx + 4; col[1] = net_start+2*gbus[i];                     col[2] = net_start + 2*gbus[i]+1;
    val[0] = 1;       val[1] = Zdq_inv[0]*dVd_dVr + Zdq_inv[1]*dVq_dVr; val[2] = Zdq_inv[0]*dVd_dVi + Zdq_inv[1]*dVq_dVi;
    ierr = MatSetValues(J,1,row,3,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //  fgen[idx+5] = Zdq_inv[2]*(-Edp + Vd) + Zdq_inv[3]*(-Eqp + Vq) + Iq;
    row[0] = idx+5;
    col[0] = idx + 5; col[1] = net_start+2*gbus[i];                     col[2] = net_start + 2*gbus[i]+1;
    val[0] = 1;       val[1] = Zdq_inv[2]*dVd_dVr + Zdq_inv[3]*dVq_dVr; val[2] = Zdq_inv[2]*dVd_dVi + Zdq_inv[3]*dVq_dVi;
    ierr = MatSetValues(J,1,row,3,col,val,INSERT_VALUES);CHKERRQ(ierr);

    PetscScalar dIGr_ddelta,dIGi_ddelta,dIGr_dId,dIGr_dIq,dIGi_dId,dIGi_dIq;
    dIGr_ddelta = Id*cos(delta) - Iq*sin(delta);
    dIGi_ddelta = Id*sin(delta) + Iq*cos(delta);
    dIGr_dId = sin(delta);  dIGr_dIq = cos(delta);
    dIGi_dId = -cos(delta); dIGi_dIq = sin(delta);

    // fnet[2*gbus[i]]   -= IGi;
    row[0] = net_start + 2*gbus[i];
    col[0] = idx + 4;   col[1] = idx + 5;
    val[0] = -dIGi_dId; val[1] = -dIGi_dIq;
    ierr = MatSetValues(J,1,row,2,col,val,INSERT_VALUES);CHKERRQ(ierr);

    // fnet[2*gbus[i]+1]   -= IGr;
    row[0] = net_start + 2*gbus[i]+1;
    col[0] = idx + 4;   col[1] = idx + 5;
    val[0] = -dIGr_dId; val[1] = -dIGr_dIq;
    ierr = MatSetValues(J,1,row,2,col,val,INSERT_VALUES);CHKERRQ(ierr);

    Vm = PetscSqrtScalar(Vd*Vd + Vq*Vq); Vm2 = Vm*Vm;

    //    fgen[idx+6] = TE[i]*xdotgen[idx+6] + KE[i]*Efd + SE - VR;
    row[0] = idx + 6;
    col[0] = idx + 6;
    val[0] = 1.0;
    ierr = MatSetValues(J,1,row,1,col,val,INSERT_VALUES);CHKERRQ(ierr);

    /* Exciter differential equations */

    //    fgen[idx+7] = TF[i]*xdotgen[idx+7] + RF - KF[i]*Efd/TF[i];
    row[0] = idx + 7;
    col[0] = idx + 7;
    val[0] = 1.0;
    ierr = MatSetValues(J,1,row,1,col,val,INSERT_VALUES);CHKERRQ(ierr);

    //    fgen[idx+8] = TA[i]*xdotgen[idx+8] + VR - KA[i]*RF + KA[i]*KF[i]*Efd/TF[i] - KA[i]*(Vref[i] - Vm);
    row[0] = idx + 8;
    col[0] = idx + 8;
    val[0] = 1.0;
    ierr = MatSetValues(J,1,row,1,col,val,INSERT_VALUES);CHKERRQ(ierr);
    idx = idx + 9;
  }

  PetscInt ncols;
  const PetscInt *cols;
  const PetscScalar *yvals;
  PetscInt k;

  for(i=0;i<nbus;i++) {
    ierr = MatGetRow(user->Ybus,2*i,&ncols,&cols,&yvals);CHKERRQ(ierr);
    row[0] = net_start + 2*i;
    for(k=0;k<ncols;k++) {
      col[k] = net_start + cols[k];
      val[k] = yvals[k];
    }
    ierr = MatSetValues(J,1,row,ncols,col,val,INSERT_VALUES);CHKERRQ(ierr);
    ierr = MatRestoreRow(user->Ybus,2*i,&ncols,&cols,&yvals);CHKERRQ(ierr);

    ierr = MatGetRow(user->Ybus,2*i+1,&ncols,&cols,&yvals);CHKERRQ(ierr);
    row[0] = net_start + 2*i+1;
    for(k=0;k<ncols;k++) {
      col[k] = net_start + cols[k];
      val[k] = yvals[k];
    }
    ierr = MatSetValues(J,1,row,ncols,col,val,INSERT_VALUES);CHKERRQ(ierr);
    ierr = MatRestoreRow(user->Ybus,2*i+1,&ncols,&cols,&yvals);CHKERRQ(ierr);
  }

  ierr = MatAssemblyBegin(J,MAT_FLUSH_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(J,MAT_FLUSH_ASSEMBLY);CHKERRQ(ierr);

  PetscScalar PD,QD,Vm0,*v0,Vm4;
  PetscScalar dPD_dVr,dPD_dVi,dQD_dVr,dQD_dVi;
  PetscScalar dIDr_dVr,dIDr_dVi,dIDi_dVr,dIDi_dVi;

  ierr = VecGetArray(user->V0,&v0);CHKERRQ(ierr);
  for(i=0; i < nload; i++) {
    Vr = xnet[2*lbus[i]]; /* Real part of load bus voltage */
    Vi = xnet[2*lbus[i]+1]; /* Imaginary part of the load bus voltage */
    Vm = PetscSqrtScalar(Vr*Vr + Vi*Vi); Vm2 = Vm*Vm; Vm4 = Vm2*Vm2;
    Vm0 = PetscSqrtScalar(v0[2*lbus[i]]*v0[2*lbus[i]] + v0[2*lbus[i]+1]*v0[2*lbus[i]+1]);
    PD = QD = 0.0;
    dPD_dVr = dPD_dVi = dQD_dVr = dQD_dVi = 0.0;
    for(k=0; k < ld_nsegsp[i];k++) {
      PD += ld_alphap[k]*PD0[i]*PetscPowScalar((Vm/Vm0),ld_betap[k]);
      dPD_dVr += ld_alphap[k]*ld_betap[k]*PD0[i]*PetscPowScalar((1/Vm0),ld_betap[k])*Vr*PetscPowScalar(Vm,(ld_betap[k]-2));
      dPD_dVi += ld_alphap[k]*ld_betap[k]*PD0[i]*PetscPowScalar((1/Vm0),ld_betap[k])*Vi*PetscPowScalar(Vm,(ld_betap[k]-2));
    }
    for(k=0; k < ld_nsegsq[i];k++) {
      QD += ld_alphaq[k]*QD0[i]*PetscPowScalar((Vm/Vm0),ld_betaq[k]);
      dQD_dVr += ld_alphaq[k]*ld_betaq[k]*QD0[i]*PetscPowScalar((1/Vm0),ld_betaq[k])*Vr*PetscPowScalar(Vm,(ld_betaq[k]-2));
      dQD_dVi += ld_alphaq[k]*ld_betaq[k]*QD0[i]*PetscPowScalar((1/Vm0),ld_betaq[k])*Vi*PetscPowScalar(Vm,(ld_betaq[k]-2));
    }

    //    IDr = (PD*Vr + QD*Vi)/Vm2;
    //    IDi = (-QD*Vr + PD*Vi)/Vm2;

    dIDr_dVr = (dPD_dVr*Vr + dQD_dVr*Vi + PD)/Vm2 - ((PD*Vr + QD*Vi)*2*Vr)/Vm4;
    dIDr_dVi = (dPD_dVi*Vr + dQD_dVi*Vi + QD)/Vm2 - ((PD*Vr + QD*Vi)*2*Vi)/Vm4;

    dIDi_dVr = (-dQD_dVr*Vr + dPD_dVr*Vi - QD)/Vm2 - ((-QD*Vr + PD*Vi)*2*Vr)/Vm4;
    dIDi_dVi = (-dQD_dVi*Vr + dPD_dVi*Vi + PD)/Vm2 - ((-QD*Vr + PD*Vi)*2*Vi)/Vm4;


    //    fnet[2*lbus[i]]   += IDi;
    row[0] = net_start + 2*lbus[i];
    col[0] = net_start + 2*lbus[i];  col[1] = net_start + 2*lbus[i]+1;
    val[0] = dIDi_dVr;               val[1] = dIDi_dVi;
    ierr = MatSetValues(J,1,row,2,col,val,ADD_VALUES);CHKERRQ(ierr);
    //    fnet[2*lbus[i]+1] += IDr;
    row[0] = net_start + 2*lbus[i]+1;
    col[0] = net_start + 2*lbus[i];  col[1] = net_start + 2*lbus[i]+1;
    val[0] = dIDr_dVr;               val[1] = dIDr_dVi;
    ierr = MatSetValues(J,1,row,2,col,val,ADD_VALUES);CHKERRQ(ierr);
  }
  ierr = VecRestoreArray(user->V0,&v0);CHKERRQ(ierr);

  ierr = VecRestoreArray(Xgen,&xgen);CHKERRQ(ierr);
  ierr = VecRestoreArray(Xnet,&xnet);CHKERRQ(ierr);

  ierr = DMCompositeRestoreLocalVectors(user->dmpgrid,&Xgen,&Xnet);CHKERRQ(ierr);

  ierr = MatAssemblyBegin(J,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(J,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);

  PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "main"
int main(int argc,char **argv)
{
  TS             ts;
  SNES           snes_alg;
  PetscErrorCode ierr;
  PetscMPIInt    size;
  Userctx        user;
  PetscViewer    Xview,Ybusview;
  Vec            X;
  Mat            J;

  ierr = PetscInitialize(&argc,&argv,"petscoptions",help);CHKERRQ(ierr);
  ierr = MPI_Comm_size(PETSC_COMM_WORLD,&size);CHKERRQ(ierr);
  if (size > 1) SETERRQ(PETSC_COMM_WORLD,PETSC_ERR_SUP,"Only for sequential runs");

  user.neqs_gen   = 9*ngen; /* # eqs. for generator subsystem */
  user.neqs_net   = 2*nbus; /* # eqs. for network subsystem   */
  user.neqs_pgrid = user.neqs_gen + user.neqs_net;

  /* Read initial voltage vector and Ybus */
  ierr = PetscViewerBinaryOpen(PETSC_COMM_WORLD,"X.bin",FILE_MODE_READ,&Xview);CHKERRQ(ierr);
  ierr = PetscViewerBinaryOpen(PETSC_COMM_WORLD,"Ybus.bin",FILE_MODE_READ,&Ybusview);CHKERRQ(ierr);

  ierr = VecCreate(PETSC_COMM_WORLD,&user.V0);CHKERRQ(ierr);
  ierr = VecSetSizes(user.V0,PETSC_DECIDE,user.neqs_net);CHKERRQ(ierr);
  ierr = VecLoad(user.V0,Xview);CHKERRQ(ierr);

  ierr = MatCreate(PETSC_COMM_WORLD,&user.Ybus);CHKERRQ(ierr);
  ierr = MatSetSizes(user.Ybus,PETSC_DECIDE,PETSC_DECIDE,user.neqs_net,user.neqs_net);CHKERRQ(ierr);
  ierr = MatSetType(user.Ybus,MATBAIJ);CHKERRQ(ierr);
  /*  ierr = MatSetBlockSize(user.Ybus,2);CHKERRQ(ierr); */
  ierr = MatLoad(user.Ybus,Ybusview);CHKERRQ(ierr);

  /* Set run time options */
  ierr = PetscOptionsBegin(PETSC_COMM_WORLD,PETSC_NULL,"Transient stability fault options","");CHKERRQ(ierr);
  {
    user.tfaulton = 1.0;
    user.tfaultoff = 1.2;
    user.Rfault   = 0.0001;
    user.faultbus = 8;
    ierr  = PetscOptionsReal("-tfaulton","","",user.tfaulton,&user.tfaulton,PETSC_NULL);CHKERRQ(ierr);
    ierr  = PetscOptionsReal("-tfaultoff","","",user.tfaultoff,&user.tfaultoff,PETSC_NULL);CHKERRQ(ierr);
    ierr  = PetscOptionsInt("-faultbus","","",user.faultbus,&user.faultbus,PETSC_NULL);CHKERRQ(ierr);
    user.t0 = 0.0;
    user.tmax = 5.0;
    ierr  = PetscOptionsReal("-t0","","",user.t0,&user.t0,PETSC_NULL);CHKERRQ(ierr);
    ierr  = PetscOptionsReal("-tmax","","",user.tmax,&user.tmax,PETSC_NULL);CHKERRQ(ierr);
  }
  ierr = PetscOptionsEnd();CHKERRQ(ierr);

  ierr = PetscViewerDestroy(&Xview);CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&Ybusview);CHKERRQ(ierr);

  /* Create DMs for generator and network subsystems */
  ierr = DMDACreate1d(PETSC_COMM_WORLD,DMDA_BOUNDARY_NONE,user.neqs_gen,1,1,PETSC_NULL,&user.dmgen);CHKERRQ(ierr);
  ierr = DMSetOptionsPrefix(user.dmgen,"dmgen_");CHKERRQ(ierr);
  ierr = DMDACreate1d(PETSC_COMM_WORLD,DMDA_BOUNDARY_NONE,user.neqs_net,1,1,PETSC_NULL,&user.dmnet);CHKERRQ(ierr);
  ierr = DMSetOptionsPrefix(user.dmnet,"dmnet_");CHKERRQ(ierr);
  /* Create a composite DM packer and add the two DMs */
  ierr = DMCompositeCreate(PETSC_COMM_WORLD,&user.dmpgrid);CHKERRQ(ierr);
  ierr = DMSetOptionsPrefix(user.dmpgrid,"pgrid_");CHKERRQ(ierr);
  ierr = DMCompositeAddDM(user.dmpgrid,user.dmgen);CHKERRQ(ierr);
  ierr = DMCompositeAddDM(user.dmpgrid,user.dmnet);CHKERRQ(ierr);

  ierr = DMCreateGlobalVector(user.dmpgrid,&X);CHKERRQ(ierr);

  ierr = MatCreate(PETSC_COMM_WORLD,&J);CHKERRQ(ierr);
  ierr = MatSetSizes(J,PETSC_DECIDE,PETSC_DECIDE,user.neqs_pgrid,user.neqs_pgrid);CHKERRQ(ierr);
  ierr = MatSetFromOptions(J);CHKERRQ(ierr);
  ierr = PreallocateJacobian(J,&user);CHKERRQ(ierr);

  /* Create matrix to save solutions at each time step */
  user.stepnum = 0;
  ierr = MatCreateSeqDense(PETSC_COMM_SELF,user.neqs_pgrid+1,1002,PETSC_NULL,&user.Sol);CHKERRQ(ierr);
  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Create timestepping solver context
     - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  ierr = TSCreate(PETSC_COMM_WORLD,&ts);CHKERRQ(ierr);
  ierr = TSSetProblemType(ts,TS_NONLINEAR);CHKERRQ(ierr);
  ierr = TSSetIFunction(ts,PETSC_NULL,(TSIFunction) IFunction,&user);CHKERRQ(ierr);
  ierr = TSSetIJacobian(ts,J,J,(TSIJacobian)IJacobian,&user);CHKERRQ(ierr);
  ierr = TSSetApplicationContext(ts,&user);CHKERRQ(ierr);

  /* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
     Set initial conditions
   - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */
  ierr = SetInitialGuess(X,&user);CHKERRQ(ierr);
  /* Just to set up the Jacobian structure */
  Vec Xdot;
  MatStructure flg;
  ierr = VecDuplicate(X,&Xdot);CHKERRQ(ierr);
  ierr = IJacobian(ts,0.0,X,Xdot,0.0,&J,&J,&flg,&user);CHKERRQ(ierr);
  ierr = VecDestroy(&Xdot);CHKERRQ(ierr);

  /* Save initial solution */
  PetscScalar *x,*mat;
  PetscInt idx=user.stepnum*(user.neqs_pgrid+1);
  ierr = MatDenseGetArray(user.Sol,&mat);CHKERRQ(ierr);
  ierr = VecGetArray(X,&x);CHKERRQ(ierr);
  mat[idx] = 0.0;
  ierr = PetscMemcpy(mat+idx+1,x,user.neqs_pgrid*sizeof(PetscScalar));CHKERRQ(ierr);
  ierr = MatDenseRestoreArray(user.Sol,&mat);CHKERRQ(ierr);
  ierr = VecRestoreArray(X,&x);CHKERRQ(ierr);
  user.stepnum++;

  ierr = TSSetDuration(ts,1000,user.tfaulton);CHKERRQ(ierr);
  ierr = TSSetInitialTimeStep(ts,0.0,0.01);CHKERRQ(ierr);
  ierr = TSSetFromOptions(ts);CHKERRQ(ierr);
  ierr = TSSetPostStep(ts,SaveSolution);CHKERRQ(ierr);

  /* Prefault period */
  ierr = TSSolve(ts,X);CHKERRQ(ierr);

  /* Create the nonlinear solver for solving the algebraic system */
  /* Note that although the algebraic system needs to be solved only for
     Idq and V, we reuse the entire system including xgen. The xgen
     variables are held constant by setting their residuals to 0 and
     putting a 1 on the Jacobian diagonal for xgen rows
  */
  Vec F_alg;
  ierr = VecDuplicate(X,&F_alg);CHKERRQ(ierr);
  ierr = SNESCreate(PETSC_COMM_WORLD,&snes_alg);CHKERRQ(ierr);
  ierr = SNESSetFunction(snes_alg,F_alg,AlgFunction,&user);CHKERRQ(ierr);
  ierr = MatZeroEntries(J);CHKERRQ(ierr);
  ierr = SNESSetJacobian(snes_alg,J,J,AlgJacobian,&user);CHKERRQ(ierr);
  ierr = SNESSetOptionsPrefix(snes_alg,"alg_");CHKERRQ(ierr);
  ierr = SNESSetFromOptions(snes_alg);CHKERRQ(ierr);

  /* Apply disturbance - resistive fault at user.faultbus */
  /* This is done by adding shunt conductance to the diagonal location
     in the Ybus matrix */
  PetscInt row_loc,col_loc;
  PetscScalar val;
  row_loc = 2*user.faultbus; col_loc = 2*user.faultbus+1; /* Location for G */
  val = 1/user.Rfault;
  ierr = MatSetValues(user.Ybus,1,&row_loc,1,&col_loc,&val,ADD_VALUES);CHKERRQ(ierr);
  row_loc = 2*user.faultbus+1; col_loc = 2*user.faultbus; /* Location for G */
  val = 1/user.Rfault;
  ierr = MatSetValues(user.Ybus,1,&row_loc,1,&col_loc,&val,ADD_VALUES);CHKERRQ(ierr);

  ierr = MatAssemblyBegin(user.Ybus,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(user.Ybus,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);

  /* Solve the algebraic equations */
  ierr = SNESSolve(snes_alg,PETSC_NULL,X);CHKERRQ(ierr);

  /* Save fault-on solution */
  idx = user.stepnum*(user.neqs_pgrid+1);
  ierr = MatDenseGetArray(user.Sol,&mat);CHKERRQ(ierr);
  ierr = VecGetArray(X,&x);CHKERRQ(ierr);
  mat[idx] = user.tfaulton;
  ierr = PetscMemcpy(mat+idx+1,x,user.neqs_pgrid*sizeof(PetscScalar));CHKERRQ(ierr);
  ierr = MatDenseRestoreArray(user.Sol,&mat);CHKERRQ(ierr);
  ierr = VecRestoreArray(X,&x);CHKERRQ(ierr);
  user.stepnum++;

  /* Disturbance period */
  ierr = TSSetDuration(ts,1000,user.tfaultoff);CHKERRQ(ierr);
  ierr = TSSetInitialTimeStep(ts,user.tfaulton,.01);CHKERRQ(ierr);
  ierr = TSSolve(ts,X);CHKERRQ(ierr);

  /* Remove the fault */
  row_loc = 2*user.faultbus; col_loc = 2*user.faultbus+1;
  val = -1/user.Rfault;
  ierr = MatSetValues(user.Ybus,1,&row_loc,1,&col_loc,&val,ADD_VALUES);CHKERRQ(ierr);
  row_loc = 2*user.faultbus+1; col_loc = 2*user.faultbus;
  val = -1/user.Rfault;
  ierr = MatSetValues(user.Ybus,1,&row_loc,1,&col_loc,&val,ADD_VALUES);CHKERRQ(ierr);

  ierr = MatAssemblyBegin(user.Ybus,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(user.Ybus,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);

  ierr = MatZeroEntries(J);CHKERRQ(ierr);
  /* Solve the algebraic equations */
  ierr = SNESSolve(snes_alg,PETSC_NULL,X);CHKERRQ(ierr);

  /* Save tfault off solution */
  idx = user.stepnum*(user.neqs_pgrid+1);
  ierr = MatDenseGetArray(user.Sol,&mat);CHKERRQ(ierr);
  ierr = VecGetArray(X,&x);CHKERRQ(ierr);
  mat[idx] = user.tfaultoff;
  ierr = PetscMemcpy(mat+idx+1,x,user.neqs_pgrid*sizeof(PetscScalar));CHKERRQ(ierr);
  ierr = MatDenseRestoreArray(user.Sol,&mat);CHKERRQ(ierr);
  ierr = VecRestoreArray(X,&x);CHKERRQ(ierr);
  user.stepnum++;

  /* Post-disturbance period */
  ierr = TSSetDuration(ts,1000,user.tmax);CHKERRQ(ierr);
  ierr = TSSetInitialTimeStep(ts,user.tfaultoff,.01);CHKERRQ(ierr);
  ierr = TSSolve(ts,X);CHKERRQ(ierr);

  ierr = MatAssemblyBegin(user.Sol,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);
  ierr = MatAssemblyEnd(user.Sol,MAT_FINAL_ASSEMBLY);CHKERRQ(ierr);

  Mat A;
  PetscScalar *amat;
  ierr = MatCreateSeqDense(PETSC_COMM_SELF,user.neqs_pgrid+1,user.stepnum,PETSC_NULL,&A);CHKERRQ(ierr);
  ierr = MatDenseGetArray(user.Sol,&mat);CHKERRQ(ierr);
  ierr = MatDenseGetArray(A,&amat);CHKERRQ(ierr);
  ierr = PetscMemcpy(amat,mat,(user.stepnum*(user.neqs_pgrid+1))*sizeof(PetscScalar));CHKERRQ(ierr);
  ierr = MatDenseRestoreArray(A,&amat);CHKERRQ(ierr);
  ierr = MatDenseRestoreArray(user.Sol,&mat);CHKERRQ(ierr);
  PetscViewer viewer;
  ierr = PetscViewerBinaryOpen(PETSC_COMM_SELF,"out.bin",FILE_MODE_WRITE,&viewer);CHKERRQ(ierr);
  ierr = MatView(A,viewer);CHKERRQ(ierr);
  ierr = PetscViewerDestroy(&viewer);CHKERRQ(ierr);
  ierr = MatDestroy(&A);CHKERRQ(ierr);
  ierr = SNESDestroy(&snes_alg);CHKERRQ(ierr);
  ierr = VecDestroy(&F_alg);CHKERRQ(ierr);
  ierr = MatDestroy(&J);CHKERRQ(ierr);
  ierr = MatDestroy(&user.Ybus);CHKERRQ(ierr);
  ierr = MatDestroy(&user.Sol);CHKERRQ(ierr);
  ierr = VecDestroy(&X);CHKERRQ(ierr);
  ierr = VecDestroy(&user.V0);CHKERRQ(ierr);
  ierr = DMDestroy(&user.dmgen);CHKERRQ(ierr);
  ierr = DMDestroy(&user.dmnet);CHKERRQ(ierr);
  ierr = DMDestroy(&user.dmpgrid);CHKERRQ(ierr);
  ierr = TSDestroy(&ts);CHKERRQ(ierr);
  ierr = PetscFinalize();
  return(0);
}