
#ifndef lint
static char vcid[] = "$Id: memc.c,v 1.16 1996/03/13 01:16:26 bsmith Exp curfman $";
#endif
/*
    We define the memory operations here. The reason we just don't use 
  the standard memory routines in the PETSc code is that on some machines 
  they are broken.

*/
#include "petsc.h"        /*I  "petsc.h"   I*/
#include <memory.h>
#include "pinclude/petscfix.h"



#if defined(PARCH_rs6000_test)
void PetscMemcpy(void *a,void *b,int n)
{
  double *aa, *bb;
  int  i;
#pragma disjoint(aa,bb)
  aa = (double *) a; bb = (double *) b;
  n = n/sizeof(double);
  for ( i=0; i<n; i++ ) {
    aa[i] = bb[i];
  }
}
#else
/*@C
   PetscMemcpy - Copies n bytes, beginning at location b, to the space
   beginning at location a.

   Input Parameters:
.  a - pointer to copy space
.  b - pointer to initial memory space
.  n - length (in bytes) of space to copy

   Note:
   This routine is analogous to memcpy().

.keywords: Petsc, copy, memory

.seealso: PetscMemcpy()
@*/
void PetscMemcpy(void *a,void *b,int n)
{
  memcpy((char*)(a),(char*)(b),n);
}
#endif

/*@C
   PetscMemzero - Zeros the specified memory.

   Input Parameters:
.  a - pointer to beginning memory location
.  n - length (in bytes) of memory to initialize

   Note:
   This routine is analogous to memset().

.keywords: Petsc, zero, initialize, memory

.seealso: PetscMemcpy()
@*/
void PetscMemzero(void *a,int n)
{
  memset((char*)(a),0,n);
}

/*@C
   PetscMemcmp - Compares two byte streams in memory.

   Input Parameters:
.  str1 - Pointer to the first byte stream
.  str2 - Pointer to the second byte stream
.  len  - The length of the byte stream
         (both str1 and str2 are assumed to be of length 'len')

   Output Parameters:
.  returns integer less than, equal to, or 
   greater than 0, according to whether str11 is 
   less than, equal to, or greater than str2.

   Note: 
   This routine is anologous to memcmp()
@*/
int PetscMemcmp(void * str1, void *str2, int len)
{
  return memcmp((char *)str1, (char *)str2, len);
}




