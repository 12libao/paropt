#include <math.h>
#include <string.h>
#include "ParOptVec.h"
#include "ParOptBlasLapack.h"

/*
  Copyright (c) 2014-2015 Graeme Kennedy. All rights reserved
*/

/*
  Create a parallel vector for optimization

  input:
  comm: the communicator for this vector
  n:    the number of vector components on this processor
*/
ParOptVec::ParOptVec( MPI_Comm _comm, int n ){
  comm = _comm;
  size = n;
  x = new ParOptScalar[ size ];
  memset(x, 0, size*sizeof(ParOptScalar));
}

/*
  Free the internally stored data
*/
ParOptVec::~ParOptVec(){
  delete [] x;
}

/*
  Set the vector value
*/
void ParOptVec::set( ParOptScalar alpha ){
  for ( int i = 0; i < size; i++ ){
    x[i] = alpha;
  }
}

/*
  Zero the entries of the vector
*/
void ParOptVec::zeroEntries(){
  memset(x, 0, size*sizeof(ParOptScalar));
}

/*
  Copy the values from the given vector
*/
void ParOptVec::copyValues( ParOptVec * vec ){
  memcpy(x, vec->x, size*sizeof(ParOptScalar));
}

/*
  Compute the l2 norm of the vector
*/
double ParOptVec::norm(){
  int one = 1;
  double res = BLASdnrm2(&size, x, &one);
  res *= res;

  double sum = 0.0;
  MPI_Allreduce(&res, &sum, 1, MPI_DOUBLE, MPI_SUM, comm);

  return sqrt(sum);
}

/*
  Compute the l-infinity norm of the vector
*/
double ParOptVec::maxabs(){
  double res = 0.0;
  for ( int i = 0; i < size; i++ ){
    if (fabs(RealPart(x[i])) > res){
      res = fabs(RealPart(x[i]));
    }
  }

  double infty_norm = 0.0;
  MPI_Allreduce(&res, &infty_norm, 1, MPI_DOUBLE, MPI_MAX, comm);

  return infty_norm;
}

/*
  Compute the dot-product of two vectors and return the result.
*/
ParOptScalar ParOptVec::dot( ParOptVec * vec ){
  int one = 1;
  ParOptScalar res = BLASddot(&size, x, &one, vec->x, &one);

  ParOptScalar sum = 0.0;
  MPI_Allreduce(&res, &sum, 1, PAROPT_MPI_TYPE, MPI_SUM, comm);

  return sum;
}

/*
  Compute multiple dot-products simultaneously. This reduces the
  parallel communication overhead.
*/
void ParOptVec::mdot( ParOptVec ** vecs, int nvecs, ParOptScalar * output ){
  int one = 1;
  for ( int i = 0; i < nvecs; i++ ){
    output[i] = BLASddot(&size, x, &one, vecs[i]->x, &one);
  }

  MPI_Allreduce(MPI_IN_PLACE, output, nvecs, PAROPT_MPI_TYPE, MPI_SUM, comm);
}

/*
  Compute the dot product of the
*/
void ParOptVec::scale( ParOptScalar alpha ){
  int one = 1;
  BLASdscal(&size, &alpha, x, &one);
}

/*
  Compute: self <- self + alpha*x
*/
void ParOptVec::axpy( ParOptScalar alpha, ParOptVec * vec ){
  int one = 1;
  BLASdaxpy(&size, &alpha, vec->x, &one, x, &one);
}

/*
  Retrieve the locally stored values from the array
*/
int ParOptVec::getArray( ParOptScalar ** array ){
  *array = x;
  return size;
}

/*
  Write the vector to a binary file. This is useful for checkpointing
  or recording the optimization history.

  intput:
  filename: the name of the file to write
*/
int ParOptVec::writeToFile( const char * filename ){
  int rank, mpisize;
  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &mpisize);

  // Copy the file name to a non-constant array
  char * fname = new char[ strlen(filename)+1 ];
  strcpy(fname, filename);

  // Determine the range
  int * range = new int[ mpisize+1 ];
  range[0] = 0;
  MPI_Allgather(&size, 1, MPI_INT, &range[1], 1, MPI_INT, comm);
  for ( int i = 0; i < mpisize; i++ ){
    range[i+1] += range[i];
  }

  // Try and open the file for writing
  int fail = 0;
  MPI_File fp = NULL;
  MPI_File_open(comm, fname, MPI_MODE_WRONLY | MPI_MODE_CREATE, 
                MPI_INFO_NULL, &fp);

  // Only write the file if it actually exists...
  if (fp){
    char datarep[] = "native";
    MPI_File_set_view(fp, 0, PAROPT_MPI_TYPE, PAROPT_MPI_TYPE,
                      datarep, MPI_INFO_NULL);
    MPI_File_write_at_all(fp, range[rank], x, size, 
			  PAROPT_MPI_TYPE,
                          MPI_STATUS_IGNORE);
    MPI_File_close(&fp);
  }
  else {
    fail = 1;
  }

  delete [] range;
  delete [] fname;

  return fail;
}

/*
  The following class implements the limited-memory BFGS update.  

  The limited-memory BFGS formula takes the following form:

  b0*I - Z*diag{d)*M^{-1}*diag{d}*Z^{T}

  input:
  comm:     the communicator
  nvars:    the number of local variables
  msub_max: the maximum subspace size
*/
LBFGS::LBFGS( MPI_Comm _comm, int _nvars,
	      int _msub_max ){
  comm = _comm;
  nvars = _nvars;
  msub_max = _msub_max;
  msub = 0;

  b0 = 1.0;

  // Allocate space for the vectors
  S = new ParOptVec*[ msub_max ];
  Y = new ParOptVec*[ msub_max ];
  Z = new ParOptVec*[ 2*msub_max ];

  for ( int i = 0; i < msub_max; i++ ){
    S[i] = new ParOptVec(comm, nvars);
    Y[i] = new ParOptVec(comm, nvars);
  }

  // A temporary vector for the damped update
  r = new ParOptVec(comm, nvars);

  // The full M-matrix
  M = new ParOptScalar[ 4*msub_max*msub_max ];

  // The diagonal scaling matrix
  d0 = new ParOptScalar[ 2*msub_max ];

  // The factored M-matrix
  M_factor = new ParOptScalar[ 4*msub_max*msub_max ];
  mfpiv = new int[ 2*msub_max ];
  
  // Temporary vector required for multiplications
  rz = new ParOptScalar[ 2*msub_max ];

  // The components of the M matrix that must be
  // updated each iteration
  D = new ParOptScalar[ msub_max ];
  L = new ParOptScalar[ msub_max*msub_max ];
  B = new ParOptScalar[ msub_max*msub_max ];  

  // Zero the initial values of everything
  memset(d0, 0, 2*msub_max*sizeof(ParOptScalar));
  memset(rz, 0, 2*msub_max*sizeof(ParOptScalar));

  memset(M, 0, 4*msub_max*msub_max*sizeof(ParOptScalar));
  memset(M_factor, 0, 4*msub_max*msub_max*sizeof(ParOptScalar));

  memset(D, 0, msub_max*sizeof(ParOptScalar));
  memset(L, 0, msub_max*msub_max*sizeof(ParOptScalar));
  memset(B, 0, msub_max*msub_max*sizeof(ParOptScalar));
}

/*
  Free the memory allocated by the BFGS update
*/
LBFGS::~LBFGS(){
  // Delete the vectors
  for ( int i = 0; i < msub_max; i++ ){
    delete Y[i];
    delete S[i];
  }

  delete [] S;
  delete [] Y;
  delete [] Z;
  delete r;

  // Delete the matrices/data
  delete [] M;
  delete [] M_factor;
  delete [] mfpiv;
  delete [] rz;

  delete [] D;
  delete [] L;
  delete [] B;
  delete [] d0;
}

/*
  Get the maximum size of the limited-memory BFGS update
*/
int LBFGS::getMaxLimitedMemorySize(){
  return msub_max;
}

/*
  Reset the Hessian approximation
*/
void LBFGS::reset(){
  msub = 0;
  b0 = 1.0;

  // Zero the initial values of everything
  memset(d0, 0, 2*msub_max*sizeof(ParOptScalar));
  memset(rz, 0, 2*msub_max*sizeof(ParOptScalar));

  memset(M, 0, 4*msub_max*msub_max*sizeof(ParOptScalar));
  memset(M_factor, 0, 4*msub_max*msub_max*sizeof(ParOptScalar));

  memset(D, 0, msub_max*sizeof(ParOptScalar));
  memset(L, 0, msub_max*msub_max*sizeof(ParOptScalar));
  memset(B, 0, msub_max*msub_max*sizeof(ParOptScalar));
}

/*
  Compute the update to the limited-memory BFGS approximate Hessian.
  The BFGS formula takes the form:

  B*x = (b0*I - Z*diag{d}*M^{-1}*diag{d}*Z^{T})*x
  
  This code computes a damped update to ensure that the curvature
  condition is satisfied.

  input:
  s:  the step in the design variable values
  y:  the difference in the gradient

  returns:
  update type: 0 = normal, 1 = damped update
*/
int LBFGS::update( ParOptVec * s, ParOptVec * y ){
  int update_type = 0;

  // Set the diagonal entries of the matrix
  ParOptScalar gamma = y->dot(y);
  ParOptScalar alpha = y->dot(s);

  // Set the diagonal components on the first time through
  if (msub == 0){
    b0 = gamma/alpha;
    if (b0 <= 0.0){
      b0 = 1.0;
    }
  }
 
  // Compute the step times the old Hessian approximation
  mult(s, r);
  ParOptScalar beta = r->dot(s);

  ParOptVec *new_vec = y;

  // Compute the damped update if the curvature condition is violated
  if (alpha <= 0.2*beta){
    // Damped update return
    update_type = 1;

    // Compute r = theta*y + (1.0 - theta)*B*s
    ParOptScalar theta = 0.8*beta/(beta - alpha);
    r->scale(1.0 - theta);
    r->axpy(theta, y);
    new_vec = r;

    gamma = r->dot(r);
    alpha = r->dot(s);
  }

  // Update the diagonal component of the BFGS matrix
  b0 = gamma/alpha;

  // Set up the new values
  if (msub < msub_max){
    S[msub]->copyValues(s);
    Y[msub]->copyValues(new_vec);
    msub++;
  }
  else { // msub == msub_max
    // Shift the pointers to the vectors so that everything
    // will work out
    S[0]->copyValues(s);
    Y[0]->copyValues(new_vec);

    // Shift the pointers
    ParOptVec *stemp = S[0];
    ParOptVec *ytemp = Y[0];
    for ( int i = 0; i < msub-1; i++ ){
      S[i] = S[i+1];
      Y[i] = Y[i+1];
    }
    S[msub-1] = stemp;
    Y[msub-1] = ytemp;

    // Now, shift the values in the matrices
    for ( int i = 0; i < msub-1; i++ ){
      D[i] = D[i+1];
    }

    for ( int i = 0; i < msub-1; i++ ){
      for ( int j = 0; j < msub-1; j++ ){
	B[i + j*msub_max] = B[i+1 + (j+1)*msub_max];
      }
    }

    for ( int i = 0; i < msub-1; i++ ){
      for ( int j = 0; j < i; j++ ){
	L[i + j*msub_max] = L[i+1 + (j+1)*msub_max];
      }
    }
  }

  // Update the matrices required for the limited-memory update.
  // Update the S^{T}S matrix:
  for ( int i = 0; i < msub; i++ ){
    B[msub-1 + i*msub_max] = S[msub-1]->dot(S[i]);
    B[i + (msub-1)*msub_max] = B[msub-1 + i*msub_max];
  }

  // Update the diagonal D-matrix
  D[msub-1] = S[msub-1]->dot(Y[msub-1]);

  // By definition, we have the L matrix:
  // For j < i: L[i + j*msub_max] = S[i]->dot(Y[j]);
  for ( int i = 0; i < msub-1; i++ ){
    L[msub-1 + i*msub_max] = S[msub-1]->dot(Y[i]);
  }

  // Set the values into the M-matrix
  memset(M, 0, 4*msub*msub*sizeof(ParOptScalar));

  // Populate the result in the M-matrix
  for ( int i = 0; i < msub; i++ ){
    for ( int j = 0; j < msub; j++ ){
      M[i + 2*msub*j] = b0*B[i + msub_max*j];
    } 
  }

  // Add the L-terms in the matrix
  for ( int i = 0; i < msub; i++ ){
    for ( int j = 0; j < i; j++ ){
      M[i + 2*msub*(j+msub)] = L[i + msub_max*j];
      M[j+msub + 2*msub*i] = L[i + msub_max*j];
    }
  }

  // Add the trailing diagonal term
  for ( int i = 0; i < msub; i++ ){
    M[msub+i + 2*msub*(msub+i)] = -D[i];
  }

  // Copy over the new ordering for the Z-vectors
  for ( int i = 0; i < msub; i++ ){
    // Set the vector ordering
    Z[i] = S[i];
    Z[i+msub] = Y[i];

    // Set the values of the diagonal vector b0
    d0[i] = b0;
    d0[i+msub] = 1.0;
  }

  // Copy out the M matrix for factorization
  memcpy(M_factor, M, 4*msub*msub*sizeof(ParOptScalar));
  
  // Factor the M matrix for later useage
  int n = 2*msub, info = 0;
  LAPACKdgetrf(&n, &n, M_factor, &n, mfpiv, &info);

  return update_type;
}

/*
  Given the input vector, multiply the BFGS approximation by the input
  vector

  This code computes the product of the LBFGS matrix with the vector x:

  y <- b0*x - Z*diag{d}*M^{-1}*diag{d}*Z^{T}*x
*/
void LBFGS::mult( ParOptVec * x, ParOptVec * y ){
  // Set y = b0*x
  y->copyValues(x);
  y->scale(b0);

  if (msub > 0){
    // Compute rz = Z^{T}*x
    x->mdot(Z, 2*msub, rz);
    
    // Set rz *= d0
    for ( int i = 0; i < 2*msub; i++ ){
      rz[i] *= d0[i];
    }
    
    // Solve rz = M^{-1}*rz
    int n = 2*msub, one = 1, info = 0;
    LAPACKdgetrs("N", &n, &one, 
		 M_factor, &n, mfpiv, 
		 rz, &n, &info);
    
    // Compute rz *= d0
    for ( int i = 0; i < 2*msub; i++ ){
      rz[i] *= d0[i];
    }

    // Now compute: y <- Z*rz
    for ( int i = 0; i < 2*msub; i++ ){
      y->axpy(-rz[i], Z[i]);
    }
  }
}

/*
  Given the input vector, multiply the BFGS approximation by the input
  vector and add the result to the output vector

  This code computes the product of the LBFGS matrix with the vector x:

  y <- y + alpha*(b0*x - Z*diag{d}*M^{-1}*diag{d}*Z^{T}*x)
*/
void LBFGS::multAdd( ParOptScalar alpha, ParOptVec * x, ParOptVec * y ){
  // Set y = b0*x
  y->axpy(b0*alpha, x);

  if (msub > 0){
    // Compute rz = Z^{T}*x
    x->mdot(Z, 2*msub, rz);
    
    // Set rz *= d0
    for ( int i = 0; i < 2*msub; i++ ){
      rz[i] *= d0[i];
    }
    
    // Solve rz = M^{-1}*rz
    int n = 2*msub, one = 1, info = 0;
    LAPACKdgetrs("N", &n, &one, 
		 M_factor, &n, mfpiv, 
		 rz, &n, &info);
    
    // Compute rz *= d0
    for ( int i = 0; i < 2*msub; i++ ){
      rz[i] *= d0[i];
    }

    // Now compute: y <- Z*rz
    for ( int i = 0; i < 2*msub; i++ ){
      y->axpy(-alpha*rz[i], Z[i]);
    }
  }
}

/*
  Retrieve the internal data for the limited-memory BFGS
  representation
*/
int LBFGS::getCompactMat( ParOptScalar * _b0,
			  const ParOptScalar ** _d,
			  const ParOptScalar ** _M,
			  ParOptVec *** _Z ){
  if (_b0){ *_b0 = b0; }
  if (_d){ *_d = d0; }
  if (_M){ *_M = M; }
  if (_Z){ *_Z = Z; }

  return 2*msub;
}

/*
  The following class implements the limited-memory SR1 update.  

  The limited-memory SR1 formula takes the following form:

  b0*I - Z*diag{d)*M^{-1}*diag{d}*Z^{T}

  input:
  comm:     the communicator
  nvars:    the number of local variables
  msub_max: the maximum subspace size
*/
LSR1::LSR1( MPI_Comm _comm, int _nvars, int _msub_max ){
  comm = _comm;
  nvars = _nvars;
  msub_max = _msub_max;
  msub = 0;

  b0 = 1.0;

  // Allocate space for the vectors
  S = new ParOptVec*[ msub_max ];
  Y = new ParOptVec*[ msub_max ];
  Z = new ParOptVec*[ msub_max ];

  for ( int i = 0; i < msub_max; i++ ){
    S[i] = new ParOptVec(comm, nvars);
    Y[i] = new ParOptVec(comm, nvars);
    Z[i] = new ParOptVec(comm, nvars);
  }

  // A temporary vector for the damped update
  r = new ParOptVec(comm, nvars);

  // The full M-matrix
  M = new ParOptScalar[ msub_max*msub_max ];

  // The diagonal scaling matrix
  d0 = new ParOptScalar[ msub_max ];

  // The factored M-matrix
  M_factor = new ParOptScalar[ msub_max*msub_max ];
  mfpiv = new int[ msub_max ];
  
  // Temporary vector required for multiplications
  rz = new ParOptScalar[ msub_max ];

  // The components of the M matrix that must be
  // updated each iteration
  D = new ParOptScalar[ msub_max ];
  L = new ParOptScalar[ msub_max*msub_max ];
  B = new ParOptScalar[ msub_max*msub_max ];  

  // Zero the initial values of everything
  memset(d0, 0, msub_max*sizeof(ParOptScalar));
  memset(rz, 0, msub_max*sizeof(ParOptScalar));

  memset(M, 0, msub_max*msub_max*sizeof(ParOptScalar));
  memset(M_factor, 0, msub_max*msub_max*sizeof(ParOptScalar));

  memset(D, 0, msub_max*sizeof(ParOptScalar));
  memset(L, 0, msub_max*msub_max*sizeof(ParOptScalar));
  memset(B, 0, msub_max*msub_max*sizeof(ParOptScalar));
}

/*
  Free the memory allocated by the BFGS update
*/
LSR1::~LSR1(){
  // Delete the vectors
  for ( int i = 0; i < msub_max; i++ ){
    delete Y[i];
    delete S[i];
    delete Z[i];
  }

  delete [] S;
  delete [] Y;
  delete [] Z;
  delete r;

  // Delete the matrices/data
  delete [] M;
  delete [] M_factor;
  delete [] mfpiv;
  delete [] rz;

  delete [] D;
  delete [] L;
  delete [] B;
  delete [] d0;
}

/*
  Get the maximum size of the limited-memory BFGS update
*/
int LSR1::getMaxLimitedMemorySize(){
  return msub_max;
}

/*
  Reset the Hessian approximation
*/
void LSR1::reset(){
  msub = 0;
  b0 = 1.0;

  // Zero the initial values of everything
  memset(d0, 0, msub_max*sizeof(ParOptScalar));
  memset(rz, 0, msub_max*sizeof(ParOptScalar));

  memset(M, 0, msub_max*msub_max*sizeof(ParOptScalar));
  memset(M_factor, 0, msub_max*msub_max*sizeof(ParOptScalar));

  memset(D, 0, msub_max*sizeof(ParOptScalar));
  memset(L, 0, msub_max*msub_max*sizeof(ParOptScalar));
  memset(B, 0, msub_max*msub_max*sizeof(ParOptScalar));
}

/*
  Compute the update to the limited-memory SR1 approximate
  Hessian. The SR1 formula takes the form:

  B*x = (b0*I - Z*diag{d}*M^{-1}*diag{d}*Z^{T})*x
  
  Note that the

  input:
  s:  the step in the design variable values
  y:  the difference in the gradient

  returns:
  update type: 0 = normal, 1 = damped update
*/
int LSR1::update( ParOptVec * s, ParOptVec * y ){
  int update_type = 0;

  // Set the diagonal entries of the matrix
  ParOptScalar gamma = y->dot(y);
  ParOptScalar alpha = y->dot(s);

  // Set the diagonal components on the first time through
  if (msub == 0){
    b0 = gamma/alpha;
  }
 
  // Set up the new values
  if (msub < msub_max){
    S[msub]->copyValues(s);
    Y[msub]->copyValues(y);
    msub++;
  }
  else { // msub == msub_max
    // Shift the pointers to the vectors so that everything
    // will work out
    S[0]->copyValues(s);
    Y[0]->copyValues(y);

    // Shift the pointers
    ParOptVec *stemp = S[0];
    ParOptVec *ytemp = Y[0];
    for ( int i = 0; i < msub-1; i++ ){
      S[i] = S[i+1];
      Y[i] = Y[i+1];
    }
    S[msub-1] = stemp;
    Y[msub-1] = ytemp;

    // Now, shift the values in the matrices
    for ( int i = 0; i < msub-1; i++ ){
      D[i] = D[i+1];
    }

    for ( int i = 0; i < msub-1; i++ ){
      for ( int j = 0; j < msub-1; j++ ){
	B[i + j*msub_max] = B[i+1 + (j+1)*msub_max];
      }
    }

    for ( int i = 0; i < msub-1; i++ ){
      for ( int j = 0; j < i; j++ ){
	L[i + j*msub_max] = L[i+1 + (j+1)*msub_max];
      }
    }
  }

  // Update the matrices required for the limited-memory update.
  // Update the S^{T}S matrix:
  for ( int i = 0; i < msub; i++ ){
    B[msub-1 + i*msub_max] = S[msub-1]->dot(S[i]);
    B[i + (msub-1)*msub_max] = B[msub-1 + i*msub_max];
  }

  // Update the diagonal D-matrix
  D[msub-1] = S[msub-1]->dot(Y[msub-1]);

  // By definition, we have the L matrix:
  // For j < i: L[i + j*msub_max] = S[i]->dot(Y[j]);
  for ( int i = 0; i < msub-1; i++ ){
    L[msub-1 + i*msub_max] = S[msub-1]->dot(Y[i]);
  }

  // Set the values into the M-matrix
  memset(M, 0, msub*msub*sizeof(ParOptScalar));

  // Populate the result in the M-matrix
  for ( int i = 0; i < msub; i++ ){
    for ( int j = 0; j < msub; j++ ){
      M[i + msub*j] += b0*B[i + msub_max*j];
    }
  }

  for ( int i = 0; i < msub; i++ ){
    for ( int j = 0; j < i; j++ ){
      M[i + msub*j] -= L[i + msub_max*j];
      M[j + msub*i] -= L[i + msub_max*j];
    }
  }

  for ( int i = 0; i < msub; i++ ){
    M[i*(msub+1)] -= D[i];
  }

  // Set the new values of the Z-vectors
  for ( int i = 0; i < msub; i++ ){
    Z[i]->copyValues(Y[i]);
    Z[i]->axpy(-b0, S[i]);

    d0[i] = 1.0;
  }

  // Copy out the M matrix for factorization
  memcpy(M_factor, M, msub*msub*sizeof(ParOptScalar));
  
  // Factor the M matrix for later useage
  int n = msub, info = 0;
  LAPACKdgetrf(&n, &n, M_factor, &n, mfpiv, &info);

  return update_type;
}

/*
  Given the input vector, multiply the SR1 approximation by the input
  vector

  This code computes the product of the LSR1 matrix with the vector x:

  y <- b0*x - Z*M^{-1}*Z^{T}*x
*/
void LSR1::mult( ParOptVec * x, ParOptVec * y ){
  // Set y = b0*x
  y->copyValues(x);
  y->scale(b0);

  if (msub > 0){
    // Compute rz = Z^{T}*x
    x->mdot(Z, msub, rz);
        
    // Solve rz = M^{-1}*rz
    int n = msub, one = 1, info = 0;
    LAPACKdgetrs("N", &n, &one, 
		 M_factor, &n, mfpiv, 
		 rz, &n, &info);
    
    // Now compute: y <- Z*rz
    for ( int i = 0; i < msub; i++ ){
      y->axpy(-rz[i], Z[i]);
    }
  }
}

/*
  Given the input vector, multiply the SR1 approximation by the input
  vector and add the result to the output vector

  This code computes the product of the LSR1 matrix with the vector x:

  y <- y + alpha*(b0*x - Z*M^{-1}*Z^{T}*x)
*/
void LSR1::multAdd( ParOptScalar alpha, ParOptVec * x, ParOptVec * y ){
  // Set y = b0*x
  y->axpy(b0*alpha, x);

  if (msub > 0){
    // Compute rz = Z^{T}*x
    x->mdot(Z, msub, rz);
        
    // Solve rz = M^{-1}*rz
    int n = msub, one = 1, info = 0;
    LAPACKdgetrs("N", &n, &one, 
		 M_factor, &n, mfpiv, 
		 rz, &n, &info);
    
    // Now compute: y <- Z*rz
    for ( int i = 0; i < msub; i++ ){
      y->axpy(-alpha*rz[i], Z[i]);
    }
  }
}

/*
  Retrieve the internal data for the limited-memory BFGS
  representation
*/
int LSR1::getCompactMat( ParOptScalar * _b0,
			 const ParOptScalar ** _d,
			 const ParOptScalar ** _M,
			 ParOptVec *** _Z ){
  if (_b0){ *_b0 = b0; }
  if (_d){ *_d = d0; }
  if (_M){ *_M = M; }
  if (_Z){ *_Z = Z; }

  return msub;
}
