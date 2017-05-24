#distuils: language = c++
#distuils: sources = ParOpt.c

# For the use of MPI
from mpi4py.MPI cimport *
cimport mpi4py.MPI as MPI

# Import the declarations required from the pxd file
from ParOpt cimport *

# Import numpy 
import numpy as np
cimport numpy as np

# Ensure that numpy is initialized
np.import_array()

# Import C methods for python
from cpython cimport PyObject, Py_INCREF

# Include the definitions
include "ParOptDefs.pxi"

# Include the mpi4py header
cdef extern from "mpi-compat.h":
   pass

# Read in a ParOpt checkpoint file and produce python variables
def unpack_checkpoint(str filename):
   '''Convert the checkpoint file to usable python objects'''

   # Open the file in read-only binary mode
   fp = open(filename, 'rb')
   sfp = fp.read()
   
   # Get the sizes of c integers and doubles
   ib = np.dtype(np.intc).itemsize
   fb = np.dtype(np.double).itemsize

   # Convert the sizes stored in the checkpoint file
   sizes = np.fromstring(sfp[:3*ib], dtype=np.intc)
   nvars = sizes[0]
   nwcon = sizes[1]
   ncon = sizes[2]

   # Skip first three integers and the barrier parameter value
   offset = 3*ib
   barrier = np.fromstring(sfp[offset:offset+fb], dtype=np.double)[0]   
   offset += fb

   # Convert the slack variables and multipliers
   s = np.fromstring(sfp[offset:offset+fb*ncon], dtype=np.double)
   offset += fb*ncon
   z = np.fromstring(sfp[offset:offset+fb*ncon], dtype=np.double)
   offset += fb*ncon

   # Convert the variables and multipliers
   x = np.fromstring(sfp[offset:offset+fb*nvars], dtype=np.double)
   offset += fb*nvars
   zl = np.fromstring(sfp[offset:offset+fb*nvars], dtype=np.double)
   offset += fb*nvars
   zu = np.fromstring(sfp[offset:offset+fb*nvars], dtype=np.double)
   offset += fb*nvars

   return barrier, s, z, x, zl, zu

# This wraps a C++ array with a numpy array for later useage
cdef inplace_array_1d(int nptype, int dim1, void *data_ptr):
   '''Return a numpy version of the array'''
   # Set the shape of the array
   cdef int size = 1
   cdef np.npy_intp shape[1]
   cdef np.ndarray ndarray

   # Set the first entry of the shape array
   shape[0] = <np.npy_intp>dim1
      
   # Create the array itself - Note that this function will not
   # delete the data once the ndarray goes out of scope
   ndarray = np.PyArray_SimpleNewFromData(size, shape,
                                          nptype, data_ptr)
   
   return ndarray

# This wraps a C++ array with a numpy array for later useage
cdef inplace_array_2d(int nptype, int dim1, int dim2, void *data_ptr):
   '''Return a numpy version of the array'''
   # Set the shape of the array
   cdef int size = 2
   cdef np.npy_intp shape[2]
   cdef np.ndarray ndarray

   # Set the first entry of the shape array
   shape[0] = <np.npy_intp>dim1
   shape[1] = <np.npy_intp>dim2
      
   # Create the array itself - Note that this function will not
   # delete the data once the ndarray goes out of scope
   ndarray = np.PyArray_SimpleNewFromData(size, shape,
                                          nptype, data_ptr)
   
   return ndarray

cdef void _getvarsandbounds(void *_self, int nvars,
                            ParOptScalar *x, ParOptScalar *lb, ParOptScalar *ub):
   # The numpy arrays that will be used to wrap x/lb/ub
   cdef np.ndarray xnp, lbnp, ubnp

   # Create the array wrappers 
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   lbnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>lb)
   ubnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>ub)

   # Retrieve the initial variables and their bounds 
   (<object>_self).getVarsAndBounds(xnp, lbnp, ubnp)

   return

cdef int _evalobjcon(void *_self, int nvars, int ncon,
                     ParOptScalar *x, ParOptScalar *fobj, ParOptScalar *cons):
   # The numpy arrays that will be used for x
   cdef np.ndarray xnp
   cdef int i
   
   # Create the array wrapper
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   
   # Call the objective function
   fail, _fobj, _cons = (<object>_self).evalObjCon(xnp)

   # Copy over the objective value
   fobj[0] = _fobj

   # Copy the values from the numpy arrays
   for i in range(ncon):
      cons[i] = _cons[i]
         
   return fail

cdef int _evalobjcongradient(void *_self, int nvars, int ncon,
                             ParOptScalar *x, ParOptScalar *g, ParOptScalar *A):
   # The numpy arrays that will be used for x
   cdef np.ndarray xnp, gnp, Anp
   
   # Create the array wrapper
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   gnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>g)
   Anp = inplace_array_2d(PAROPT_NPY_SCALAR, ncon, nvars, <void*>A)
   
   # Call the objective function
   fail = (<object>_self).evalObjConGradient(xnp, gnp, Anp)

   return fail

cdef int _evalhvecproduct(void *_self, int nvars, int ncon, int nwcon,
                          ParOptScalar *x, ParOptScalar *z, ParOptScalar *zw,
                          ParOptScalar *px, ParOptScalar *hvec):
   # The numpy arrays that will be used for x
   cdef np.ndarray xnp, znp, pxnp, hnp
   cdef np.ndarray zwnp = None
   
   # Create the array wrapper
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   znp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>z)
   pxnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>px)
   hnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>hvec)
   
   if nwcon > 0:
      zwnp = inplace_array_1d(PAROPT_NPY_SCALAR, nwcon, <void*>zw)

   # Call the objective function
   fail = (<object>_self).evalHvecProduct(xnp, znp, zwnp,
                                          pxnp, hnp)
   return fail

cdef void _evalsparsecon(void *_self, int nvars, int nwcon,
                         ParOptScalar *x, ParOptScalar *con):
   # The numpy arrays
   cdef np.ndarray xnp, cnp
   
   # Create the array wrapper
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   cnp = inplace_array_1d(PAROPT_NPY_SCALAR, nwcon, <void*>con)

   (<object>_self).evalSparseCon(xnp, cnp)
   
   return

cdef void _addsparsejacobian(void *_self, int nvars, 
                             int nwcon, ParOptScalar alpha, 
                             ParOptScalar *x, ParOptScalar *px, ParOptScalar *con):
   # The numpy arrays
   cdef np.ndarray xnp, pxnp, cnp
   
   # Create the wrapper
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   pxnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>px)
   cnp = inplace_array_1d(PAROPT_NPY_SCALAR, nwcon, <void*>con)

   (<object>_self).addSparseJacobian(alpha, xnp, pxnp, cnp)

   return

cdef void _addsparsejacobiantranspose(void *_self, int nvars, 
                                      int nwcon, ParOptScalar alpha, 
                                      ParOptScalar *x, ParOptScalar *pzw, ParOptScalar *out):
   # The numpy arrays
   cdef np.ndarray xnp, pzwnp, outnp

   # Create the wrapper
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   pzwnp = inplace_array_1d(PAROPT_NPY_SCALAR, nwcon, <void*>pzw)
   outnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>out)

   (<object>_self).addSparseJacobianTranspose(alpha, xnp, pzwnp, outnp)

   return

cdef void _addsparseinnerproduct(void *_self, int nvars,
                                 int nwcon, int nwblock, ParOptScalar alpha,
                                 ParOptScalar *x, ParOptScalar *c, ParOptScalar *A):
   # The numpy arrays
   cdef np.ndarray xnp, cnp, Anp

   # Create the wrapper
   xnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>x)
   cnp = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>c)
   Anp = inplace_array_1d(PAROPT_NPY_SCALAR, nwcon*nwblock*nwblock,
                          <void*>A)

   (<object>_self).addSparseInnerProduct(alpha, xnp, cnp, Anp)

   return

# "Wrap" the abtract base class ParOptProblem 
cdef class pyParOptProblem:
   cdef CyParOptProblem *this_ptr
   
   def __init__(self, MPI.Comm comm, int nvars, int ncon,
                int nwcon=0, int nwblock=0):
      # Convert the communicator
      cdef MPI_Comm c_comm = comm.ob_mpi

      # Create the pointer to the underlying C++ object
      self.this_ptr = new CyParOptProblem(c_comm, nvars, ncon,
                                          nwcon, nwblock)
      self.this_ptr.setSelfPointer(<void*>self)
      self.this_ptr.setGetVarsAndBounds(_getvarsandbounds)
      self.this_ptr.setEvalObjCon(_evalobjcon)
      self.this_ptr.setEvalObjConGradient(_evalobjcongradient)
      self.this_ptr.setEvalHvecProduct(_evalhvecproduct)
      self.this_ptr.setEvalSparseCon(_evalsparsecon)
      self.this_ptr.setAddSparseJacobian(_addsparsejacobian)
      self.this_ptr.setAddSparseJacobianTranspose(_addsparsejacobiantranspose)
      self.this_ptr.setAddSparseInnerProduct(_addsparseinnerproduct)
      return

   def __dealloc__(self):
      del self.this_ptr
      return

   def setInequalityOptions(self, dense_ineq=True, sparse_ineq=True,
                            use_lower=True, use_upper=True):
      # Assume that everything is false
      cdef int dense = 0
      cdef int sparse = 0
      cdef int lower = 0
      cdef int upper = 0

      # Swap the integer values if the flags are set
      if dense_ineq: dense = 1
      if sparse_ineq: sparse = 1
      if use_lower: lower = 1
      if use_upper: upper = 1

      # Set the options
      self.this_ptr.setInequalityOptions(dense, sparse, lower, upper)
   
      return

# Constants that define what Quasi-Newton method to use
BFGS = PAROPT_BFGS
SR1 = PAROPT_SR1

# Python class for corresponding instance ParOpt
cdef class pyParOpt:
   cdef ParOpt *this_ptr
      
   def __cinit__(self, pyParOptProblem _prob, 
                 int max_qn_subspace, 
                 QuasiNewtonType qn_type):
      self.this_ptr = new ParOpt(_prob.this_ptr, 
                                 max_qn_subspace, qn_type)
      
   def __dealloc__(self):
      del self.this_ptr
      
   # Perform the optimization
   def optimize(self, char *checkpoint=''):
      if checkpoint is None: 
         return self.this_ptr.optimize(NULL)
      else:
         return self.this_ptr.optimize(&checkpoint[0])
   
   def getOptimizedPoint(self):
      '''Get the optimized solution from ParOpt'''
      cdef int n = 0
      cdef ParOptScalar *values = NULL
      cdef ParOptVec *vec = NULL
      
      # Retrieve the optimized vector
      self.this_ptr.getOptimizedPoint(&vec, NULL, NULL, NULL, NULL)
      
      # Get the variables from the vector
      n = vec.getArray(&values)

      # Allocate a new numpy array
      x = np.zeros(n, dtype)

      # Assign the new entries
      for i in xrange(n):
         x[i] = values[i]

      return x

   def getOptimizedMultipliers(self):
      '''Get the optimized multipliers'''
      cdef int n = 0, nc = 0, nw = 0
      cdef const ParOptScalar *zvals = NULL
      cdef ParOptScalar *zwvals = NULL
      cdef ParOptScalar *zlvals = NULL
      cdef ParOptScalar *zuvals = NULL
      cdef ParOptVec *zwvec = NULL
      cdef ParOptVec *zlvec = NULL
      cdef ParOptVec *zuvec = NULL
      
      # Set the initial values for the multipliers etc.
      z = None
      zw = None
      zl = None
      zu = None

      # Retrieve the optimized vector
      self.this_ptr.getOptimizedPoint(NULL, &zvals, &zwvec, &zlvec, &zuvec)

      # Get the number of constraints
      self.this_ptr.getProblemSizes(NULL, &nc, NULL, NULL)
      
      # Copy over the Lagrange multipliers
      z = np.zeros(nc, dtype)
      for i in xrange(nc):
         z[i] = zvals[i]

      # Convert the weighting multipliers
      if zwvec:
         nw = zwvec.getArray(&zwvals)
         zw = np.zeros(nw, dtype)
         for i in xrange(nw):
            zw[i] = zwvals[i]

      # Convert the lower bound multipliers
      if zlvec:
         n = zlvec.getArray(&zlvals)
         zl = np.zeros(n, dtype)
         for i in xrange(n):
            zl[i] = zlvals[i]

      # Convert the upper bound multipliers
      if zuvec:
         n = zuvec.getArray(&zuvals)
         zu = np.zeros(n, dtype)
         for i in xrange(n):
            zu[i] = zuvals[i]

      return z, zw, zl, zu

   def getDesignPoint(self):
      cdef int nvars
      cdef ParOptScalar *xvals = NULL
      cdef ParOptVec *xvec = NULL
      self.this_ptr.getOptimizedPoint(&xvec, NULL, NULL, NULL, NULL)
      xvec.getArray(&xvals)
      return inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>xvals)

   def getDualPoint(self):
      cdef int nvars = 0, nw = 0, nc = 0
      cdef ParOptScalar *zvals = NULL
      cdef ParOptScalar *zwvals = NULL
      cdef ParOptScalar *zlvals = NULL
      cdef ParOptScalar *zuvals = NULL
      cdef ParOptVec *zwvec = NULL
      cdef ParOptVec *zlvec = NULL
      cdef ParOptVec *zuvec = NULL
      cdef np.ndarray z = None
      cdef np.ndarray zw = None
      cdef np.ndarray zl = None
      cdef np.ndarray zu = None
      
      # Retrieve the optimized vector
      self.this_ptr.getInitMultipliers(&zvals, &zwvec, &zlvec, &zuvec)

      # Get the number of constraints
      self.this_ptr.getProblemSizes(NULL, &nc, NULL, NULL)

      # Convert things to in-place numpy arrays
      z = inplace_array_1d(PAROPT_NPY_SCALAR, nc, <void*>zvals)

      # Convert the weighting multipliers
      if zwvec:
         nw = zwvec.getArray(&zwvals)
         zw = inplace_array_1d(PAROPT_NPY_SCALAR, nw, <void*>zwvals)

      # Convert the lower bound multipliers
      if zlvec:
         nvars = zlvec.getArray(&zlvals)
         zl = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>zlvals)

      # Convert the upper bound multipliers
      if zuvec:
         nvars = zuvec.getArray(&zuvals)
         zu = inplace_array_1d(PAROPT_NPY_SCALAR, nvars, <void*>zuvals)

      return z, zw, zl, zu

   # Check objective and constraint gradients
   def checkGradients(self, double dh):    
      self.this_ptr.checkGradients(dh)
      
   # Set optimizer parameters
   def setInitStartingPoint(self, int init):
      self.this_ptr.setInitStartingPoint(init)
      
   def setMaxMajorIterations(self, int iters):
      self.this_ptr.setMaxMajorIterations(iters)
      
   def setAbsOptimalityTol(self, double tol):
      self.this_ptr.setAbsOptimalityTol(tol)

   def setRelFunctionTol(self, double tol):
      self.this_ptr.setRelFunctionTol(tol)
      
   def setBarrierFraction(self, double frac):
      self.this_ptr.setBarrierFraction(frac)
      
   def setBarrierPower(self, double power):
      self.this_ptr.setBarrierPower(power)
      
   def setHessianResetFreq(self, int freq):
      self.this_ptr.setHessianResetFreq(freq)
   
   def setQNDiagonalFactor(self, double sigma):
      self.this_ptr.setQNDiagonalFactor(sigma)

   def setBFGSUpdateType(self, str update):
      if update == 'damped':
         self.this_ptr.setBFGSUpdateType(DAMPED_UPDATE)
      elif update == 'skip':
         self.this_ptr.setBFGSUpdateType(SKIP_NEGATIVE_CURVATURE)
      return
      
   def setSequentialLinearMethod(self, int truth):
      self.this_ptr.setSequentialLinearMethod(truth)
      
   # Set/obtain the barrier parameter
   def setInitBarrierParameter(self, double mu):
      self.this_ptr.setInitBarrierParameter(mu)
      
   def getBarrierParameter(self):
      return self.this_ptr.getBarrierParameter()

   def getComplementarity(self):
      return self.this_ptr.getComplementarity()
  
   # Reset the quasi-Newton Hessian
   def resetQuasiNewtonHessian(self):
      self.this_ptr.resetQuasiNewtonHessian()

   # Reset the design variables and bounds
   def resetDesignAndBounds(self):
      self.this_ptr.resetDesignAndBounds()

   # Set parameters associated with the linesearch
   def setUseLineSearch(self, int truth):
      self.this_ptr.setUseLineSearch(truth)
      
   def setMaxLineSearchIters(self, int iters):
      self.this_ptr.setMaxLineSearchIters(iters)
      
   def setBacktrackingLineSearch(self, int truth):
      self.this_ptr.setBacktrackingLineSearch(truth)
      
   def setArmijioParam(self, double c1):
      self.this_ptr.setArmijioParam(c1)
      
   def setPenaltyDescentFraction(self, double frac):
      self.this_ptr.setPenaltyDescentFraction(frac)
      
   # Set parameters for the interal GMRES algorithm
   def setUseHvecProduct(self, int truth):
      self.this_ptr.setUseHvecProduct(truth)
      
   def setUseQNGMRESPreCon(self, int truth):
      self.this_ptr.setUseQNGMRESPreCon(truth)
      
   def setNKSwitchTolerance(self, double tol):
      self.this_ptr.setNKSwitchTolerance(tol)
      
   def setEisenstatWalkerParameters(self, double gamma, double alpha):
      self.this_ptr.setEisenstatWalkerParameters(gamma, alpha)
      
   def setGMRESTolerances(self, double rtol, double atol):
      self.this_ptr.setGMRESTolerances(rtol, atol)
      
   def setGMRESSubspaceSize(self, int _gmres_subspace_size):
      self.this_ptr.setGMRESSubspaceSize(_gmres_subspace_size)
      
   # Set other parameters
   def setOutputFrequency(self, int freq):
      self.this_ptr.setOutputFrequency(freq)
      
   def setMajorIterStepCheck(self, int step):
      self.this_ptr.setMajorIterStepCheck(step)
      
   def setOutputFile(self, char *filename):
      if filename is not None:
         self.this_ptr.setOutputFile(filename)
         
   def setGradientCheckFrequency(self, int freq, double step_size):
       self.this_ptr.setGradientCheckFrequency(freq, step_size)
         
   # Write out the design variables to binary format (fast MPI/IO)
   def writeSolutionFile(self, char *filename):
      if filename is not None:
         return self.this_ptr.writeSolutionFile(filename)
  
   def readSolutionFile(self, char *filename):
      if filename is not None:
         return self.this_ptr.readSolutionFile(filename)
      