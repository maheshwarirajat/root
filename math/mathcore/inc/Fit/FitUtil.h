// @(#)root/mathcore:$Id$
// Author: L. Moneta Tue Nov 28 10:52:47 2006

/**********************************************************************
 *                                                                    *
 * Copyright (c) 2006  LCG ROOT Math Team, CERN/PH-SFT                *
 *                                                                    *
 *                                                                    *
 **********************************************************************/

// Header file for class FitUtil

#ifndef ROOT_Fit_FitUtil
#define ROOT_Fit_FitUtil

#include "Math/IParamFunctionfwd.h"
#include "Math/IParamFunction.h"

#ifdef R__USE_IMT
#include "ROOT/TThreadExecutor.hxx"
#endif

// #include "ROOT/TProcessExecutor.hxx"

#include "Fit/BinData.h"
#include "Fit/UnBinData.h"
#include "Fit/FitExecutionPolicy.h"

#include "Math/Integrator.h"
#include "Math/IntegratorMultiDim.h"

#include "TError.h"
#include "TSystem.h"

// using parameter cache is not thread safe but needed for normalizing the functions
#define USE_PARAMCACHE

#ifdef R__HAS_VECCORE
namespace vecCore {
template <class T>
vecCore::Mask<T> Int2Mask(unsigned i)
{
   T x;
   for (unsigned j = 0; j < vecCore::VectorSize<T>(); j++)
      vecCore::Set<T>(x, j, j);
   return vecCore::Mask<T>(x < T(i));
}
}
#endif

namespace ROOT {

   namespace Fit {

/**
   namespace defining utility free functions using in Fit for evaluating the various fit method
   functions (chi2, likelihood, etc..)  given the data and the model function

   @ingroup FitMain
*/
namespace FitUtil {

  typedef  ROOT::Math::IParamMultiFunction IModelFunction;
  typedef  ROOT::Math::IParamMultiGradFunction IGradModelFunction;

  template <class T>
  using IGradModelFunctionTempl = ROOT::Math::IParamMultiGradFunctionTempl<T>;

  template <class T>
  using IModelFunctionTempl = ROOT::Math::IParamMultiFunctionTempl<T>;

  // internal class defining
  template <class T>
  class LikelihoodAux {
  public:
     LikelihoodAux(T logv = {}, T w = {}, T w2 = {}) : logvalue(logv), weight(w), weight2(w2) {}

     LikelihoodAux operator+(const LikelihoodAux &l) const
     {
        return LikelihoodAux<T>(logvalue + l.logvalue, weight + l.weight, weight2 + l.weight2);
     }

     LikelihoodAux &operator+=(const LikelihoodAux &l)
     {
        logvalue += l.logvalue;
        weight += l.weight;
        weight2 += l.weight2;
        return *this;
     }

     T logvalue;
     T weight;
     T weight2;
  };

  template <>
  class LikelihoodAux<double> {
  public:
     LikelihoodAux(double logv = 0.0, double w = 0.0, double w2 = 0.0) : logvalue(logv), weight(w), weight2(w2){};

     LikelihoodAux operator+(const LikelihoodAux &l) const
     {
        return LikelihoodAux<double>(logvalue + l.logvalue, weight + l.weight, weight2 + l.weight2);
     }

     LikelihoodAux &operator+=(const LikelihoodAux &l)
     {
        logvalue += l.logvalue;
        weight += l.weight;
        weight2 += l.weight2;
        return *this;
     }

     double logvalue;
     double weight;
     double weight2;
  };

  // internal class to evaluate the function or the integral
  // and cached internal integration details
  // if useIntegral is false no allocation is done
  // and this is a dummy class
  // class is templated on any parametric functor implementing operator()(x,p) and NDim()
  // contains a constant pointer to the function

  template <class ParamFunc = ROOT::Math::IParamMultiFunctionTempl<double>>
  class IntegralEvaluator {

  public:
     IntegralEvaluator(const ParamFunc &func, const double *p, bool useIntegral = true)
        : fDim(0), fParams(0), fFunc(0), fIg1Dim(0), fIgNDim(0), fFunc1Dim(0), fFuncNDim(0)
     {
        if (useIntegral) {
           SetFunction(func, p);
        }
     }

     void SetFunction(const ParamFunc &func, const double *p = 0)
     {
        // set the integrand function and create required wrapper
        // to perform integral in (x) of a generic  f(x,p)
        fParams = p;
        fDim = func.NDim();
        // copy the function object to be able to modify the parameters
        // fFunc = dynamic_cast<ROOT::Math::IParamMultiFunction *>( func.Clone() );
        fFunc = &func;
        assert(fFunc != 0);
        // set parameters in function
        // fFunc->SetParameters(p);
        if (fDim == 1) {
           fFunc1Dim =
              new ROOT::Math::WrappedMemFunction<IntegralEvaluator, double (IntegralEvaluator::*)(double) const>(
                 *this, &IntegralEvaluator::F1);
           fIg1Dim = new ROOT::Math::IntegratorOneDim();
           // fIg1Dim->SetFunction( static_cast<const ROOT::Math::IMultiGenFunction & >(*fFunc),false);
           fIg1Dim->SetFunction(static_cast<const ROOT::Math::IGenFunction &>(*fFunc1Dim));
        } else if (fDim > 1) {
           fFuncNDim =
              new ROOT::Math::WrappedMemMultiFunction<IntegralEvaluator, double (IntegralEvaluator::*)(const double *)
                                                                            const>(*this, &IntegralEvaluator::FN, fDim);
           fIgNDim = new ROOT::Math::IntegratorMultiDim();
           fIgNDim->SetFunction(*fFuncNDim);
        } else
           assert(fDim > 0);
     }

     void SetParameters(const double *p)
     {
        // copy just the pointer
        fParams = p;
     }

     ~IntegralEvaluator()
     {
        if (fIg1Dim)
           delete fIg1Dim;
        if (fIgNDim)
           delete fIgNDim;
        if (fFunc1Dim)
           delete fFunc1Dim;
        if (fFuncNDim)
           delete fFuncNDim;
        // if (fFunc) delete fFunc;
     }

     // evaluation of integrand function (one-dim)
     double F1(double x) const
     {
        double xx = x;
        return ExecFunc(fFunc, &xx, fParams);
     }
     // evaluation of integrand function (multi-dim)
     double FN(const double *x) const { return ExecFunc(fFunc, x, fParams); }

     double Integral(const double *x1, const double *x2)
     {
        // return unormalized integral
        return (fIg1Dim) ? fIg1Dim->Integral(*x1, *x2) : fIgNDim->Integral(x1, x2);
     }

     double operator()(const double *x1, const double *x2)
     {
        // return normalized integral, divided by bin volume (dx1*dx...*dxn)
        if (fIg1Dim) {
           double dV = *x2 - *x1;
           return fIg1Dim->Integral(*x1, *x2) / dV;
        } else if (fIgNDim) {
           double dV = 1;
           for (unsigned int i = 0; i < fDim; ++i)
              dV *= (x2[i] - x1[i]);
           return fIgNDim->Integral(x1, x2) / dV;
           //                   std::cout << " do integral btw x " << x1[0] << "  " << x2[0] << " y " << x1[1] << "  "
           //                   << x2[1] << " dV = " << dV << " result = " << result << std::endl; return result;
        } else
           assert(1.); // should never be here
        return 0;
     }

  private:
     template <class T>
     inline double ExecFunc(T *f, const double *x, const double *p) const
     {
        return (*f)(x, p);
     }

#ifdef R__HAS_VECCORE
     inline double ExecFunc(const IModelFunctionTempl<ROOT::Double_v> *f, const double *x, const double *p) const
     {
        if (fDim == 1) {
           ROOT::Double_v xx;
           vecCore::Load<ROOT::Double_v>(xx, x);
           const double *p0 = p;
           auto res = (*f)(&xx, (const double *)p0);
           return vecCore::Get<ROOT::Double_v>(res, 0);
        } else {
           std::vector<ROOT::Double_v> xx(fDim);
           for (unsigned int i = 0; i < fDim; ++i) {
              vecCore::Load<ROOT::Double_v>(xx[i], x + i);
           }
           auto res = (*f)(xx.data(), p);
           return vecCore::Get<ROOT::Double_v>(res, 0);
        }
     }
#endif

     // objects of this class are not meant to be copied / assigned
     IntegralEvaluator(const IntegralEvaluator &rhs);
     IntegralEvaluator &operator=(const IntegralEvaluator &rhs);

     unsigned int fDim;
     const double *fParams;
     // ROOT::Math::IParamMultiFunction * fFunc;  // copy of function in order to be able to change parameters
     // const ParamFunc * fFunc;       //  reference to a generic parametric function
     const ParamFunc *fFunc;
     ROOT::Math::IntegratorOneDim *fIg1Dim;
     ROOT::Math::IntegratorMultiDim *fIgNDim;
     ROOT::Math::IGenFunction *fFunc1Dim;
     ROOT::Math::IMultiGenFunction *fFuncNDim;
  };

  /** Chi2 Functions */

  /**
      evaluate the Chi2 given a model function and the data at the point x.
      return also nPoints as the effective number of used points in the Chi2 evaluation
  */
  double EvaluateChi2(const IModelFunction &func, const BinData &data, const double *x, unsigned int &nPoints,
                      ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0);

  /**
      evaluate the effective Chi2 given a model function and the data at the point x.
      The effective chi2 uses the errors on the coordinates : W = 1/(sigma_y**2 + ( sigma_x_i * df/dx_i )**2 )
      return also nPoints as the effective number of used points in the Chi2 evaluation
  */
  double EvaluateChi2Effective(const IModelFunction &func, const BinData &data, const double *x, unsigned int &nPoints);

  /**
      evaluate the Chi2 gradient given a model function and the data at the point x.
      return also nPoints as the effective number of used points in the Chi2 evaluation
  */
  void EvaluateChi2Gradient(const IModelFunction &func, const BinData &data, const double *x, double *grad,
                            unsigned int &nPoints,
                            ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                            unsigned nChunks = 0);

  /**
      evaluate the LogL given a model function and the data at the point x.
      return also nPoints as the effective number of used points in the LogL evaluation
  */
  double EvaluateLogL(const IModelFunction &func, const UnBinData &data, const double *p, int iWeight, bool extended,
                      unsigned int &nPoints, ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0);

  /**
      evaluate the LogL gradient given a model function and the data at the point x.
      return also nPoints as the effective number of used points in the LogL evaluation
  */
  void EvaluateLogLGradient(const IModelFunction &func, const UnBinData &data, const double *x, double *grad,
                            unsigned int &nPoints,
                            ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                            unsigned nChunks = 0);

  // #ifdef R__HAS_VECCORE
  //    template <class NotCompileIfScalarBackend = std::enable_if<!(std::is_same<double, ROOT::Double_v>::value)>>
  //    void EvaluateLogLGradient(const IModelFunctionTempl<ROOT::Double_v> &, const UnBinData &, const double *, double
  //    *, unsigned int & ) {}
  // #endif

  /**
      evaluate the Poisson LogL given a model function and the data at the point x.
      return also nPoints as the effective number of used points in the LogL evaluation
      By default is extended, pass extedend to false if want to be not extended (MultiNomial)
  */
  double EvaluatePoissonLogL(const IModelFunction &func, const BinData &data, const double *x, int iWeight,
                             bool extended, unsigned int &nPoints, ROOT::Fit::ExecutionPolicy executionPolicy,
                             unsigned nChunks = 0);

  /**
      evaluate the Poisson LogL given a model function and the data at the point x.
      return also nPoints as the effective number of used points in the LogL evaluation
  */
  void EvaluatePoissonLogLGradient(const IModelFunction &func, const BinData &data, const double *x, double *grad,
                                   unsigned int &nPoints,
                                   ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                                   unsigned nChunks = 0);

  // methods required by dedicate minimizer like Fumili

  /**
      evaluate the residual contribution to the Chi2 given a model function and the BinPoint data
      and if the pointer g is not null evaluate also the gradient of the residual.
      If the function provides parameter derivatives they are used otherwise a simple derivative calculation
      is used
  */
  double EvaluateChi2Residual(const IModelFunction &func, const BinData &data, const double *x, unsigned int ipoint,
                              double *g = 0);

  /**
      evaluate the pdf contribution to the LogL given a model function and the BinPoint data.
      If the pointer g is not null evaluate also the gradient of the pdf.
      If the function provides parameter derivatives they are used otherwise a simple derivative calculation
      is used
  */
  double
  EvaluatePdf(const IModelFunction &func, const UnBinData &data, const double *x, unsigned int ipoint, double *g = 0);

#ifdef R__HAS_VECCORE
   template <class NotCompileIfScalarBackend = std::enable_if<!(std::is_same<double, ROOT::Double_v>::value)>>
   double EvaluatePdf(const IModelFunctionTempl<ROOT::Double_v> &func, const UnBinData &data, const double *p, unsigned int i, double *) {
      // evaluate the pdf contribution to the generic logl function in case of bin data
      // return actually the log of the pdf and its derivatives
      // func.SetParameters(p);
      const auto x = vecCore::FromPtr<ROOT::Double_v>(data.GetCoordComponent(i, 0));
      auto fval = func(&x, p);
      auto logPdf = ROOT::Math::Util::EvalLog(fval);
      return vecCore::Get<ROOT::Double_v>(logPdf, 0);
   }
#endif

   /**
       evaluate the pdf contribution to the Poisson LogL given a model function and the BinPoint data.
       If the pointer g is not null evaluate also the gradient of the Poisson pdf.
       If the function provides parameter derivatives they are used otherwise a simple derivative calculation
       is used
   */
   double EvaluatePoissonBinPdf(const IModelFunction & func, const BinData & data, const double * x, unsigned int ipoint, double * g = 0);

   unsigned setAutomaticChunking(unsigned nEvents);

   template<class T>
   struct Evaluate {
#ifdef R__HAS_VECCORE
      static double EvalChi2(const IModelFunctionTempl<T> &func, const BinData &data, const double *p,
                             unsigned int &nPoints, ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0)
      {
         // evaluate the chi2 given a  vectorized function reference  , the data and returns the value and also in nPoints
         // the actual number of used points
         // normal chi2 using only error on values (from fitting histogram)
         // optionally the integral of function in the bin is used

         //Info("EvalChi2","Using vecorized implementation %d",(int) data.Opt().fIntegral);

         unsigned int n = data.Size();

         nPoints = 0; // count the effective non-zero points
         // set parameters of the function to cache integral value
#ifdef USE_PARAMCACHE
         (const_cast<IModelFunctionTempl<T> &>(func)).SetParameters(p);
#endif
         // do not cache parameter values (it is not thread safe)
         //func.SetParameters(p);


         // get fit option and check case if using integral of bins
         const DataOptions &fitOpt = data.Opt();
         if (fitOpt.fBinVolume || fitOpt.fIntegral || fitOpt.fExpErrors)
            Error("FitUtil::EvaluateChi2", "The vectorized implementation doesn't support Integrals, BinVolume or ExpErrors\n. Aborting operation.");

         (const_cast<IModelFunctionTempl<T> &>(func)).SetParameters(p);

         double maxResValue = std::numeric_limits<double>::max() / n;
         std::vector<double> ones{1, 1, 1, 1};
         auto vecSize = vecCore::VectorSize<T>();

         auto mapFunction = [&](unsigned int i) {
            // in case of no error in y invError=1 is returned
            T x1, y, invErrorVec;
            vecCore::Load<T>(x1, data.GetCoordComponent(i * vecSize, 0));
            vecCore::Load<T>(y, data.ValuePtr(i * vecSize));
            const auto invError = data.ErrorPtr(i * vecSize);
            auto invErrorptr = (invError != nullptr) ? invError : &ones.front();
            vecCore::Load<T>(invErrorVec, invErrorptr);

            const T *x;
            if(data.NDim() > 1) {
               std::vector<T> xc;
               xc.resize(data.NDim());
               xc[0] = x1;
               for (unsigned int j = 1; j < data.NDim(); ++j)
                  vecCore::Load<T>(xc[j], data.GetCoordComponent(i * vecSize, j));
               x = xc.data();
            } else {
               x = &x1;
            }

            T fval{};

#ifdef USE_PARAMCACHE
            fval = func(x);
#else
            fval = func(x, p);
#endif
            nPoints++;

            T tmp = (y - fval) * invErrorVec;
            T chi2 = tmp * tmp;


            // avoid inifinity or nan in chi2 values due to wrong function values
            auto m = vecCore::Mask_v<T>(chi2 > maxResValue);

            vecCore::MaskedAssign<T>(chi2, m, maxResValue);

            return chi2;
         };

#ifdef R__USE_IMT
         auto redFunction = [](const std::vector<T> &objs) {
            return std::accumulate(objs.begin(), objs.end(), T{});
         };
#else
         (void)nChunks;

         // If IMT is disabled, force the execution policy to the serial case
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            Warning("FitUtil::EvaluateChi2", "Multithread execution policy requires IMT, which is disabled. Changing "
                                             "to ROOT::Fit::ExecutionPolicy::kSerial.");
            executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial;
         }
#endif

         T res{};
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial) {
            for (unsigned int i = 0; i < (data.Size() / vecSize); i++) {
               res += mapFunction(i);
            }

#ifdef R__USE_IMT
         } else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            auto chunks = nChunks != 0 ? nChunks : setAutomaticChunking(data.Size() / vecSize);
            ROOT::TThreadExecutor pool;
            res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, data.Size() / vecSize), redFunction, chunks);
#endif
            // } else if(executionPolicy == ROOT::Fit::ExecutionPolicy::kMultiprocess){
            //   ROOT::TProcessExecutor pool;
            //   res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, data.Size()/vecSize), redFunction);
         } else {
            Error("FitUtil::EvaluateChi2", "Execution policy unknown. Avalaible choices:\n ROOT::Fit::ExecutionPolicy::kSerial (default)\n ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
         }
         nPoints = n;

         // Last SIMD vector of elements (if padding needed)
         vecCore::MaskedAssign(res, vecCore::Int2Mask<T>(data.Size() % vecSize),
                               res + mapFunction(data.Size() / vecSize));

         return vecCore::ReduceAdd(res);
      }

      static double EvalLogL(const IModelFunctionTempl<T> &func, const UnBinData &data, const double *const p,
                             int iWeight, bool extended, unsigned int &nPoints,
                             ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0)
      {
         // evaluate the LogLikelihood
         unsigned int n = data.Size();

         //unsigned int nRejected = 0;
         bool normalizeFunc = false;

         // set parameters of the function to cache integral value
#ifdef USE_PARAMCACHE
         (const_cast<IModelFunctionTempl<T> &>(func)).SetParameters(p);
#endif

#ifdef R__USE_IMT
         // in case parameter needs to be propagated to user function use trick to set parameters by calling one time the function
         // this will be done in sequential mode and parameters can be set in a thread safe manner
         if (!normalizeFunc) {
            if (data.NDim() == 1) {
               T x;
               vecCore::Load<T>(x, data.GetCoordComponent(0, 0));
               func( &x, p);
            }
            else {
               std::vector<T> x(data.NDim());
               for (unsigned int j = 0; j < data.NDim(); ++j)
                  vecCore::Load<T>(x[j], data.GetCoordComponent(0, j));
               func( x.data(), p);
            }
         }
#endif

         // this is needed if function must be normalized
         double norm = 1.0;
         if (normalizeFunc) {
            // compute integral of the function
            std::vector<double> xmin(data.NDim());
            std::vector<double> xmax(data.NDim());
            IntegralEvaluator<IModelFunctionTempl<T>> igEval(func, p, true);
            // compute integral in the ranges where is defined
            if (data.Range().Size() > 0) {
               norm = 0;
               for (unsigned int ir = 0; ir < data.Range().Size(); ++ir) {
                  data.Range().GetRange(&xmin[0], &xmax[0], ir);
                  norm += igEval.Integral(xmin.data(), xmax.data());
               }
            } else {
               // use (-inf +inf)
               data.Range().GetRange(&xmin[0], &xmax[0]);
               // check if funcition is zero at +- inf
               T xmin_v, xmax_v;
               vecCore::Load<T>(xmin_v, xmin.data());
               vecCore::Load<T>(xmax_v, xmax.data());
               if (vecCore::ReduceAdd(func(&xmin_v, p)) != 0 || vecCore::ReduceAdd(func(&xmax_v, p)) != 0) {
                  MATH_ERROR_MSG("FitUtil::EvaluateLogLikelihood", "A range has not been set and the function is not zero at +/- inf");
                  return 0;
               }
               norm = igEval.Integral(&xmin[0], &xmax[0]);
            }
         }

         // needed to compute effective global weight in case of extended likelihood

         auto vecSize = vecCore::VectorSize<T>();

         auto mapFunction = [ &, p](const unsigned i) {
            T W{};
            T W2{};
            T fval{};

            if(data.NDim() > 1) {
               std::vector<T> x(data.NDim());
               for (unsigned int j = 0; j < data.NDim(); ++j)
                  vecCore::Load<T>(x[j], data.GetCoordComponent(i * vecSize, j));
#ifdef USE_PARAMCACHE
               fval = func(x.data());
#else
               fval = func(x.data(), p);
#endif
               // one -dim case
            } else {
               T x;
               vecCore::Load<T>(x, data.GetCoordComponent(i * vecSize, 0));
#ifdef USE_PARAMCACHE
               fval = func(&x);
#else
               fval = func(&x, p);
#endif
            }

            if (normalizeFunc) fval = fval * (1 / norm);

            // function EvalLog protects against negative or too small values of fval
            auto logval =  ROOT::Math::Util::EvalLog(fval);
            if (iWeight > 0) {
               T weight{};
               if (data.WeightsPtr(i) == nullptr)
                  weight = 1;
               else
                  vecCore::Load<T>(weight, data.WeightsPtr(i*vecSize));
               logval *= weight;
               if (iWeight == 2) {
                  logval *= weight; // use square of weights in likelihood
                  if (!extended) {
                     // needed sum of weights and sum of weight square if likelkihood is extended
                     W  = weight;
                     W2 = weight * weight;
                  }
               }
            }
            nPoints++;
            return LikelihoodAux<T>(logval, W, W2);
         };

#ifdef R__USE_IMT
         auto redFunction = [](const std::vector<LikelihoodAux<T>> &objs) {
            return std::accumulate(objs.begin(), objs.end(), LikelihoodAux<T>(),
            [](const LikelihoodAux<T> &l1, const LikelihoodAux<T> &l2) {
               return l1 + l2;
            });
         };
#else
         (void)nChunks;

         // If IMT is disabled, force the execution policy to the serial case
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            Warning("FitUtil::EvaluateLogL", "Multithread execution policy requires IMT, which is disabled. Changing "
                                             "to ROOT::Fit::ExecutionPolicy::kSerial.");
            executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial;
         }
#endif

         T logl_v{};
         T sumW_v{};
         T sumW2_v{};
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial) {
            for (unsigned int i = 0; i < n / vecSize; ++i) {
               auto resArray = mapFunction(i);
               logl_v += resArray.logvalue;
               sumW_v += resArray.weight;
               sumW2_v += resArray.weight2;
            }
#ifdef R__USE_IMT
         } else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            auto chunks = nChunks != 0 ? nChunks : setAutomaticChunking(data.Size() / vecSize);
            ROOT::TThreadExecutor pool;
            auto resArray = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, data.Size() / vecSize), redFunction, chunks);
            logl_v = resArray.logvalue;
            sumW_v = resArray.weight;
            sumW2_v = resArray.weight2;
        //  } else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultiprocess) {
            // ROOT::TProcessExecutor pool;
            // res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction);
#endif
         } else {
            Error("FitUtil::EvaluateLogL", "Execution policy unknown. Avalaible choices:\n ROOT::Fit::ExecutionPolicy::kSerial (default)\n ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
         }

         //reduce vector type to double.
         double logl  = 0.;
         double sumW  = 0.;
         double sumW2 = 0;;

         for (unsigned vIt = 0; vIt < vecSize; vIt++) {
            logl += logl_v[vIt];
            sumW += sumW_v[vIt];
            sumW2 += sumW2_v[vIt];
         }

         if (extended) {
            // add Poisson extended term
            double extendedTerm = 0; // extended term in likelihood
            double nuTot = 0;
            // nuTot is integral of function in the range
            // if function has been normalized integral has been already computed
            if (!normalizeFunc) {
               IntegralEvaluator<IModelFunctionTempl<T>> igEval(func, p, true);
               std::vector<double> xmin(data.NDim());
               std::vector<double> xmax(data.NDim());

               // compute integral in the ranges where is defined
               if (data.Range().Size() > 0) {
                  nuTot = 0;
                  for (unsigned int ir = 0; ir < data.Range().Size(); ++ir) {
                     data.Range().GetRange(&xmin[0], &xmax[0], ir);
                     nuTot += igEval.Integral(xmin.data(), xmax.data());
                  }
               } else {
                  // use (-inf +inf)
                  data.Range().GetRange(&xmin[0], &xmax[0]);
                  // check if funcition is zero at +- inf
                  T xmin_v, xmax_v;
                  vecCore::Load<T>(xmin_v, xmin.data());
                  vecCore::Load<T>(xmax_v, xmax.data());
                  if (vecCore::ReduceAdd(func(&xmin_v, p)) != 0 || vecCore::ReduceAdd(func(&xmax_v, p)) != 0) {
                     MATH_ERROR_MSG("FitUtil::EvaluateLogLikelihood", "A range has not been set and the function is not zero at +/- inf");
                     return 0;
                  }
                  nuTot = igEval.Integral(&xmin[0], &xmax[0]);
               }

               // force to be last parameter value
               //nutot = p[func.NDim()-1];
               if (iWeight != 2)
                  extendedTerm = - nuTot;  // no need to add in this case n log(nu) since is already computed before
               else {
                  // case use weight square in likelihood : compute total effective weight = sw2/sw
                  // ignore for the moment case when sumW is zero
                  extendedTerm = - (sumW2 / sumW) * nuTot;
               }

            } else {
               nuTot = norm;
               extendedTerm = - nuTot + double(n) *  ROOT::Math::Util::EvalLog(nuTot);
               // in case of weights need to use here sum of weights (to be done)
            }

            logl += extendedTerm;
         }

         // reset the number of fitting data points
         //  nPoints = n;
         // std::cout<<", n: "<<nPoints<<std::endl;
         nPoints = 0;
         return -logl;

      }

      static double EvalPoissonLogL(const IModelFunctionTempl<T> &func, const BinData &data, const double *p,
                                    int iWeight, bool extended, unsigned int,
                                    ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0)
      {
         // evaluate the Poisson Log Likelihood
         // for binned likelihood fits
         // this is Sum ( f(x_i)  -  y_i * log( f (x_i) ) )
         // add as well constant term for saturated model to make it like a Chi2/2
         // by default is etended. If extended is false the fit is not extended and
         // the global poisson term is removed (i.e is a binomial fit)
         // (remember that in this case one needs to have a function with a fixed normalization
         // like in a non extended binned fit)
         //
         // if use Weight use a weighted dataset
         // iWeight = 1 ==> logL = Sum( w f(x_i) )
         // case of iWeight==1 is actually identical to weight==0
         // iWeight = 2 ==> logL = Sum( w*w * f(x_i) )
         //

#ifdef USE_PARAMCACHE
         (const_cast<IModelFunctionTempl<T> &>(func)).SetParameters(p);
#endif
         auto vecSize = vecCore::VectorSize<T>();
         // get fit option and check case of using integral of bins
         const DataOptions &fitOpt = data.Opt();
         if (fitOpt.fExpErrors || fitOpt.fIntegral)
            Error("FitUtil::EvaluateChi2",
                  "The vectorized implementation doesn't support Integrals or BinVolume\n. Aborting operation.");
         bool useW2 = (iWeight == 2);

         auto mapFunction = [&](unsigned int i) {
            T y;
            vecCore::Load<T>(y, data.ValuePtr(i * vecSize));
            T fval{};

            if (data.NDim() > 1) {
               std::vector<T> x(data.NDim());
               for (unsigned int j = 0; j < data.NDim(); ++j)
                  vecCore::Load<T>(x[j], data.GetCoordComponent(i * vecSize, j));
#ifdef USE_PARAMCACHE
               fval = func(x.data());
#else
               fval = func(x.data(), p);
#endif
               // one -dim case
            } else {
               T x;
               vecCore::Load<T>(x, data.GetCoordComponent(i * vecSize, 0));
#ifdef USE_PARAMCACHE
               fval = func(&x);
#else
               fval = func(&x, p);
#endif
            }

            // EvalLog protects against 0 values of fval but don't want to add in the -log sum
            // negative values of fval
            vecCore::MaskedAssign<T>(fval, fval < 0.0, 0.0);

            T nloglike{}; // negative loglikelihood

            if (useW2) {
               // apply weight correction . Effective weight is error^2/ y
               // and expected events in bins is fval/weight
               // can apply correction only when y is not zero otherwise weight is undefined
               // (in case of weighted likelihood I don't care about the constant term due to
               // the saturated model)
               auto m = vecCore::Mask_v<T>(y != 0.0);
               if (!vecCore::MaskFull(m)) {
                  T error = 1;
                  if (data.GetErrorType() != ROOT::Fit::BinData::ErrorType::kNoError)
                     vecCore::Load<T>(error, data.ErrorPtr(i * vecSize));
                  T weight;
                  vecCore::MaskedAssign<T>(weight, y != 0, (error * error) / y);
                  if (extended) {
                     nloglike = fval * weight;
                     // wTot  += weight;
                     // w2Tot += weight*weight;
                  }
                  vecCore::MaskedAssign<T>(nloglike, y != 0, nloglike - weight * y * ROOT::Math::Util::EvalLog(fval));
               }

            } else {
               // standard case no weights or iWeight=1
               // this is needed for Poisson likelihood (which are extened and not for multinomial)
               // the formula below  include constant term due to likelihood of saturated model (f(x) = y)
               // (same formula as in Baker-Cousins paper, page 439 except a factor of 2
               if (extended) nloglike = fval - y;

               vecCore::MaskedAssign<T>(
                  nloglike, y > 0, nloglike + y * (ROOT::Math::Util::EvalLog(y) - ROOT::Math::Util::EvalLog(fval)));
            }

            return nloglike;
         };

#ifdef R__USE_IMT
         auto redFunction = [](const std::vector<T> &objs) { return std::accumulate(objs.begin(), objs.end(), T{}); };
#else
         (void)nChunks;

         // If IMT is disabled, force the execution policy to the serial case
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            Warning("FitUtil::Evaluate<T>::EvalPoissonLogL",
                    "Multithread execution policy requires IMT, which is disabled. Changing "
                    "to ROOT::Fit::ExecutionPolicy::kSerial.");
            executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial;
         }
#endif

         T res{};
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial) {
            for (unsigned int i = 0; i < (data.Size() / vecSize); i++) {
               res += mapFunction(i);
            }
#ifdef R__USE_IMT
         } else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            auto chunks = nChunks != 0 ? nChunks : setAutomaticChunking(data.Size() / vecSize);
            ROOT::TThreadExecutor pool;
            res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, data.Size() / vecSize), redFunction, chunks);
#endif
            // } else if(executionPolicy == ROOT::Fit::ExecutionPolicy::kMultiprocess){
            //   ROOT::TProcessExecutor pool;
            //   res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, data.Size()/vecSize), redFunction);
         } else {
            Error(
               "FitUtil::Evaluate<T>::EvalPoissonLogL",
               "Execution policy unknown. Avalaible choices:\n ROOT::Fit::ExecutionPolicy::kSerial (default)\n ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
         }

         // Last padded SIMD vector of elements
         vecCore::MaskedAssign(res, vecCore::Int2Mask<T>(data.Size() % vecSize),
                               res + mapFunction(data.Size() / vecSize));

         return vecCore::ReduceAdd(res);
      }

      static double EvalChi2Effective(const IModelFunctionTempl<T> &, const BinData &, const double *, unsigned int &)
      {
         Error("FitUtil::Evaluate<T>::EvalChi2Effective", "The vectorized evaluation of the Chi2 with coordinate errors is still not supported");
         return -1.;
      }

      // Compute a mask to filter out infinite numbers and NaN values.
      // The argument rval is updated so infinite numbers and NaN values are replaced by
      // maximum finite values (preserving the original sign).
      static vecCore::Mask<T> CheckInfNaNValues(T &rval)
      {
         auto mask = rval > -vecCore::NumericLimits<T>::Max() && rval < vecCore::NumericLimits<T>::Max();

         // Case +inf or nan
         vecCore::MaskedAssign(rval, !mask, +vecCore::NumericLimits<T>::Max());

         // Case -inf
         vecCore::MaskedAssign(rval, !mask && rval < 0, -vecCore::NumericLimits<T>::Max());

         return mask;
      }

      static void EvalChi2Gradient(const IModelFunctionTempl<T> &f, const BinData &data, const double *p, double *grad,
                                   unsigned int &nPoints,
                                   ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                                   unsigned nChunks = 0)
      {
         // evaluate the gradient of the chi2 function
         // this function is used when the model function knows how to calculate the derivative and we can
         // avoid that the minimizer re-computes them
         //
         // case of chi2 effective (errors on coordinate) is not supported

         if (data.HaveCoordErrors()) {
            MATH_ERROR_MSG("FitUtil::EvaluateChi2Gradient",
                           "Error on the coordinates are not used in calculating Chi2 gradient");
            return; // it will assert otherwise later in GetPoint
         }

         const IGradModelFunctionTempl<T> *fg = dynamic_cast<const IGradModelFunctionTempl<T> *>(&f);
         assert(fg != nullptr); // must be called by a gradient function

         const IGradModelFunctionTempl<T> &func = *fg;

#ifdef DEBUG
         std::cout << "\n\nFit data size = " << n << std::endl;
         std::cout << "evaluate chi2 using function gradient " << &func << "  " << p << std::endl;
#endif

         const DataOptions &fitOpt = data.Opt();
         if (fitOpt.fBinVolume || fitOpt.fIntegral || fitOpt.fExpErrors)
            Error("FitUtil::EvaluateChi2Gradient", "The vectorized implementation doesn't support Integrals,"
                                                   "BinVolume or ExpErrors\n. Aborting operation.");

         unsigned int npar = func.NPar();
         auto vecSize = vecCore::VectorSize<T>();
         unsigned initialNPoints = data.Size();
         unsigned numVectors = initialNPoints / vecSize;

         // numVectors + 1 because of the padded data (call to mapFunction with i = numVectors after the main loop)
         std::vector<vecCore::Mask<T>> validPointsMasks(numVectors + 1);

         auto mapFunction = [&](const unsigned int i) {
            // set all vector values to zero
            std::vector<T> gradFunc(npar);
            std::vector<T> pointContributionVec(npar);

            T x1, y, invError;

            vecCore::Load<T>(x1, data.GetCoordComponent(i * vecSize, 0));
            vecCore::Load<T>(y, data.ValuePtr(i * vecSize));
            const auto invErrorPtr = data.ErrorPtr(i * vecSize);

            if (invErrorPtr == nullptr)
               invError = 1;
            else
               vecCore::Load<T>(invError, invErrorPtr);

            // TODO: Check error options and invert if needed

            T fval = 0;

            const T *x = nullptr;
            std::vector<T> xc;

            unsigned int ndim = data.NDim();
            if (ndim > 1) {
               xc.resize(ndim);
               xc[0] = x1;
               for (unsigned int j = 1; j < ndim; ++j)
                  vecCore::Load<T>(xc[j], data.GetCoordComponent(i * vecSize, j));
               x = xc.data();
            } else {
               x = &x1;
            }

            fval = func(x, p);
            func.ParameterGradient(x, p, &gradFunc[0]);

#ifdef DEBUG
            std::cout << x[0] << "  " << y << "  " << 1. / invError << " params : ";
            for (unsigned int ipar = 0; ipar < npar; ++ipar)
               std::cout << p[ipar] << "\t";
            std::cout << "\tfval = " << fval << std::endl;
#endif

            validPointsMasks[i] = CheckInfNaNValues(fval);
            if (vecCore::MaskEmpty(validPointsMasks[i])) {
               // Return a zero contribution to all partial derivatives on behalf of the current points
               return pointContributionVec;
            }

            // loop on the parameters
            for (unsigned int ipar = 0; ipar < npar; ++ipar) {
               // avoid singularity in the function (infinity and nan ) in the chi2 sum
               // eventually add possibility of excluding some points (like singularity)
               validPointsMasks[i] = CheckInfNaNValues(gradFunc[ipar]);

               if (vecCore::MaskEmpty(validPointsMasks[i])) {
                  break; // exit loop on parameters
               }

               // calculate derivative point contribution (only for valid points)
               vecCore::MaskedAssign(pointContributionVec[ipar], validPointsMasks[i],
                                     -2.0 * (y - fval) * invError * invError * gradFunc[ipar]);
            }

            return pointContributionVec;
         };

         // Reduce the set of vectors by summing its equally-indexed components
         auto redFunction = [&](const std::vector<std::vector<T>> &partialResults) {
            std::vector<T> result(npar);

            for (auto const &pointContributionVec : partialResults) {
               for (unsigned int parameterIndex = 0; parameterIndex < npar; parameterIndex++)
                  result[parameterIndex] += pointContributionVec[parameterIndex];
            }

            return result;
         };

         std::vector<T> gVec(npar);
         std::vector<double> g(npar);

#ifndef R__USE_IMT
         // to fix compiler warning
         (void)nChunks;

         // If IMT is disabled, force the execution policy to the serial case
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            Warning("FitUtil::EvaluateChi2Gradient",
                    "Multithread execution policy requires IMT, which is disabled. Changing "
                    "to ROOT::Fit::ExecutionPolicy::kSerial.");
            executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial;
         }
#endif

         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial) {
            std::vector<std::vector<T>> allGradients(numVectors);
            for (unsigned int i = 0; i < numVectors; ++i) {
               allGradients[i] = mapFunction(i);
            }

            gVec = redFunction(allGradients);
         }
#ifdef R__USE_IMT
         else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            auto chunks = nChunks != 0 ? nChunks : setAutomaticChunking(numVectors);
            ROOT::TThreadExecutor pool;
            gVec = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, numVectors), redFunction, chunks);
         }
#endif
         // else if(executionPolicy == ROOT::Fit::kMultiprocess){
         //    ROOT::TProcessExecutor pool;
         //    g = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction);
         // }
         else {
            Error(
               "FitUtil::EvaluateChi2Gradient",
               "Execution policy unknown. Avalaible choices:\n 0: Serial (default)\n 1: MultiThread (requires IMT)\n");
         }

         // Compute the contribution from the remaining points
         auto remainingPointsContribution = mapFunction(numVectors);

         // Add the contribution from the valid remaining points and store the result in the output variable
         auto remainingMask = vecCore::Int2Mask<T>(initialNPoints % vecSize);
         for (unsigned int param = 0; param < npar; param++) {
            vecCore::MaskedAssign(gVec[param], remainingMask, gVec[param] + remainingPointsContribution[param]);
            grad[param] = vecCore::ReduceAdd(gVec[param]);
         }

         // correct the number of points
         nPoints = initialNPoints;

         if (std::any_of(validPointsMasks.begin(), validPointsMasks.end(),
                         [](vecCore::Mask<T> validPoints) { return !vecCore::MaskFull(validPoints); })) {
            unsigned nRejected = 0;

            for (const auto &mask : validPointsMasks) {
               for (unsigned int i = 0; i < vecSize; i++) {
                  nRejected += !vecCore::Get(mask, i);
               }
            }

            assert(nRejected <= initialNPoints);
            nPoints = initialNPoints - nRejected;

            if (nPoints < npar) {
               MATH_ERROR_MSG("FitUtil::EvaluateChi2Gradient",
                              "Too many points rejected for overflow in gradient calculation");
            }
         }
      }

      static double EvalChi2Residual(const IModelFunctionTempl<T> &, const BinData &, const double *, unsigned int, double *)
      {
         Error("FitUtil::Evaluate<T>::EvalChi2Residual", "The vectorized evaluation of the Chi2 with the ith residual is still not supported");
         return -1.;
      }

      /// evaluate the pdf (Poisson) contribution to the logl (return actually log of pdf)
      /// and its gradient
static double EvalPoissonBinPdf(const IModelFunctionTempl<T> &, const BinData &, const double *, unsigned int , double * ) {
         Error("FitUtil::Evaluate<T>::EvaluatePoissonBinPdf", "The vectorized evaluation of the BinnedLikelihood fit evaluated point by point is still not supported");
         return -1.;
      }

      static void
      EvalPoissonLogLGradient(const IModelFunctionTempl<T> &f, const BinData &data, const double *p, double *grad,
                              unsigned int &,
                              ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                              unsigned nChunks = 0)
      {
         // evaluate the gradient of the Poisson log likelihood function

         const IGradModelFunctionTempl<T> *fg = dynamic_cast<const IGradModelFunctionTempl<T> *>(&f);
         assert(fg != nullptr); // must be called by a grad function

         const IGradModelFunctionTempl<T> &func = *fg;

         const DataOptions &fitOpt = data.Opt();
         if (fitOpt.fBinVolume || fitOpt.fIntegral || fitOpt.fExpErrors)
            Error("FitUtil::EvaluatePoissonLogLGradient", "The vectorized implementation doesn't support Integrals,"
                                                          "BinVolume or ExpErrors\n. Aborting operation.");

         unsigned int npar = func.NPar();
         auto vecSize = vecCore::VectorSize<T>();
         unsigned initialNPoints = data.Size();
         unsigned numVectors = initialNPoints / vecSize;

         auto mapFunction = [&](const unsigned int i) {
            // set all vector values to zero
            std::vector<T> gradFunc(npar);
            std::vector<T> pointContributionVec(npar);

            T x1, y;

            vecCore::Load<T>(x1, data.GetCoordComponent(i * vecSize, 0));
            vecCore::Load<T>(y, data.ValuePtr(i * vecSize));

            T fval = 0;

            const T *x = nullptr;
            std::vector<T> xc;

            unsigned ndim = data.NDim();
            if (ndim > 1) {
               xc.resize(ndim);
               xc[0] = x1;
               for (unsigned int j = 1; j < ndim; ++j)
                  vecCore::Load<T>(xc[j], data.GetCoordComponent(i * vecSize, j));
               x = xc.data();
            } else {
               x = &x1;
            }

            fval = func(x, p);
            func.ParameterGradient(x, p, &gradFunc[0]);

            // correct the gradient
            for (unsigned int ipar = 0; ipar < npar; ++ipar) {
               vecCore::Mask<T> positiveValuesMask = fval > 0;

               // df/dp * (1.  - y/f )
               vecCore::MaskedAssign(pointContributionVec[ipar], positiveValuesMask, gradFunc[ipar] * (1. - y / fval));

               vecCore::Mask<T> validNegativeValuesMask = !positiveValuesMask && gradFunc[ipar] != 0;

               if (!vecCore::MaskEmpty(validNegativeValuesMask)) {
                  const T kdmax1 = vecCore::math::Sqrt(vecCore::NumericLimits<T>::Max());
                  const T kdmax2 = vecCore::NumericLimits<T>::Max() / (4 * initialNPoints);
                  T gg = kdmax1 * gradFunc[ipar];
                  pointContributionVec[ipar] =
                     -vecCore::Blend(gg > 0, vecCore::math::Min(gg, kdmax2), vecCore::math::Max(gg, -kdmax2));
               }
            }

            return pointContributionVec;
         };

         // Vertically reduce the set of vectors by summing its equally-indexed components
         auto redFunction = [&](const std::vector<std::vector<T>> &partialResults) {
            std::vector<T> result(npar);

            for (auto const &pointContributionVec : partialResults) {
               for (unsigned int parameterIndex = 0; parameterIndex < npar; parameterIndex++)
                  result[parameterIndex] += pointContributionVec[parameterIndex];
            }

            return result;
         };

         std::vector<T> gVec(npar);

#ifndef R__USE_IMT
         // to fix compiler warning
         (void)nChunks;

         // If IMT is disabled, force the execution policy to the serial case
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            Warning("FitUtil::EvaluatePoissonLogLGradient",
                    "Multithread execution policy requires IMT, which is disabled. Changing "
                    "to ROOT::Fit::ExecutionPolicy::kSerial.");
            executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial;
         }
#endif

         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial) {
            std::vector<std::vector<T>> allGradients(numVectors);
            for (unsigned int i = 0; i < numVectors; ++i) {
               allGradients[i] = mapFunction(i);
            }

            gVec = redFunction(allGradients);
         }
#ifdef R__USE_IMT
         else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            auto chunks = nChunks != 0 ? nChunks : setAutomaticChunking(numVectors);
            ROOT::TThreadExecutor pool;
            gVec = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, numVectors), redFunction, chunks);
         }
#endif
         // else if(executionPolicy == ROOT::Fit::ExecutionPolicy::kMultiprocess){
         //    ROOT::TProcessExecutor pool;
         //    g = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction);
         // }
         else {
            Error("FitUtil::EvaluatePoissonLogLGradient", "Execution policy unknown. Avalaible choices:\n "
                                                          "ROOT::Fit::ExecutionPolicy::kSerial (default)\n "
                                                          "ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
         }

         // Compute the contribution from the remaining points
         auto remainingPointsContribution = mapFunction(numVectors);

         // Add the contribution from the valid remaining points and store the result in the output variable
         auto remainingMask = vecCore::Int2Mask<T>(initialNPoints % vecSize);
         for (unsigned int param = 0; param < npar; param++) {
            vecCore::MaskedAssign(gVec[param], remainingMask, gVec[param] + remainingPointsContribution[param]);
            grad[param] = vecCore::ReduceAdd(gVec[param]);
         }
      }

      static void EvalLogLGradient(const IModelFunctionTempl<T> &f, const UnBinData &data, const double *p,
                                   double *grad, unsigned int &,
                                   ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                                   unsigned nChunks = 0)
      {
         // evaluate the gradient of the log likelihood function

         const IGradModelFunctionTempl<T> *fg = dynamic_cast<const IGradModelFunctionTempl<T> *>(&f);
         assert(fg != nullptr); // must be called by a grad function

         const IGradModelFunctionTempl<T> &func = *fg;

         unsigned int npar = func.NPar();
         auto vecSize = vecCore::VectorSize<T>();
         unsigned initialNPoints = data.Size();
         unsigned numVectors = initialNPoints / vecSize;

         const T kdmax1 = vecCore::math::Sqrt(vecCore::NumericLimits<T>::Max());
         const T kdmax2 = vecCore::NumericLimits<T>::Max() / (4 * initialNPoints);

         auto mapFunction = [&](const unsigned int i) {
            std::vector<T> gradFunc(npar);
            std::vector<T> pointContributionVec(npar);

            T x1;
            vecCore::Load<T>(x1, data.GetCoordComponent(i * vecSize, 0));

            const T *x = nullptr;
            std::vector<T> xc;

            unsigned int ndim = data.NDim();
            if (ndim > 1) {
               xc.resize(ndim);
               xc[0] = x1;
               for (unsigned int j = 1; j < ndim; ++j)
                  vecCore::Load<T>(xc[j], data.GetCoordComponent(i * vecSize, j));
               x = xc.data();
            } else {
               x = &x1;
            }

            T fval = func(x, p);
            func.ParameterGradient(x, p, &gradFunc[0]);

            vecCore::Mask<T> positiveValues = fval > 0;

            for (unsigned int kpar = 0; kpar < npar; ++kpar) {
               if (!vecCore::MaskEmpty(positiveValues))
                  vecCore::MaskedAssign<T>(pointContributionVec[kpar], positiveValues, -1. / fval * gradFunc[kpar]);

               vecCore::Mask<T> nonZeroGradientValues = !positiveValues && gradFunc[kpar] != 0;
               if (!vecCore::MaskEmpty(nonZeroGradientValues)) {
                  T gg = kdmax1 * gradFunc[kpar];
                  pointContributionVec[kpar] =
                     vecCore::Blend(nonZeroGradientValues && gg > 0, -vecCore::math::Min(gg, kdmax2),
                                    -vecCore::math::Max(gg, -kdmax2));
               }
               // if func derivative is zero term is also zero so do not add in g[kpar]
            }

            return pointContributionVec;
         };

         // Vertically reduce the set of vectors by summing its equally-indexed components
         auto redFunction = [&](const std::vector<std::vector<T>> &pointContributions) {
            std::vector<T> result(npar);

            for (auto const &pointContributionVec : pointContributions) {
               for (unsigned int parameterIndex = 0; parameterIndex < npar; parameterIndex++)
                  result[parameterIndex] += pointContributionVec[parameterIndex];
            }

            return result;
         };

         std::vector<T> gVec(npar);
         std::vector<double> g(npar);

#ifndef R__USE_IMT
         // to fix compiler warning
         (void)nChunks;

         // If IMT is disabled, force the execution policy to the serial case
         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            Warning("FitUtil::EvaluateLogLGradient",
                    "Multithread execution policy requires IMT, which is disabled. Changing "
                    "to ROOT::Fit::ExecutionPolicy::kSerial.");
            executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial;
         }
#endif

         if (executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial) {
            std::vector<std::vector<T>> allGradients(numVectors);
            for (unsigned int i = 0; i < numVectors; ++i) {
               allGradients[i] = mapFunction(i);
            }
            gVec = redFunction(allGradients);
         }
#ifdef R__USE_IMT
         else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
            auto chunks = nChunks != 0 ? nChunks : setAutomaticChunking(numVectors);
            ROOT::TThreadExecutor pool;
            gVec = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, numVectors), redFunction, chunks);
         }
#endif
         // else if(executionPolicy == ROOT::Fit::ExecutionPolicy::kMultiprocess){
         //    ROOT::TProcessExecutor pool;
         //    g = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction);
         // }
         else {
            Error("FitUtil::EvaluateLogLGradient", "Execution policy unknown. Avalaible choices:\n "
                                                   "ROOT::Fit::ExecutionPolicy::kSerial (default)\n "
                                                   "ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
         }

         // Compute the contribution from the remaining points
         auto remainingPointsContribution = mapFunction(numVectors);

         // Add the contribution from the valid remaining points and store the result in the output variable
         auto remainingMask = vecCore::Int2Mask<T>(initialNPoints % vecSize);
         for (unsigned int param = 0; param < npar; param++) {
            vecCore::MaskedAssign(gVec[param], remainingMask, gVec[param] + remainingPointsContribution[param]);
            grad[param] = vecCore::ReduceAdd(gVec[param]);
         }
      }
   };

   template<>
   struct Evaluate<double>{
#endif

      static double EvalChi2(const IModelFunction &func, const BinData &data, const double *p, unsigned int &nPoints,
                             ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0)
      {
         // evaluate the chi2 given a  function reference, the data and returns the value and also in nPoints
         // the actual number of used points
         // normal chi2 using only error on values (from fitting histogram)
         // optionally the integral of function in the bin is used


         //Info("EvalChi2","Using non-vecorized implementation %d",(int) data.Opt().fIntegral);

         return FitUtil::EvaluateChi2(func, data, p, nPoints, executionPolicy, nChunks);
      }

      static double EvalLogL(const IModelFunctionTempl<double> &func, const UnBinData &data, const double *p,
                             int iWeight, bool extended, unsigned int &nPoints,
                             ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0)
      {
         return FitUtil::EvaluateLogL(func, data, p, iWeight, extended, nPoints, executionPolicy, nChunks);
      }

      static double EvalPoissonLogL(const IModelFunctionTempl<double> &func, const BinData &data, const double *p,
                                    int iWeight, bool extended, unsigned int &nPoints,
                                    ROOT::Fit::ExecutionPolicy executionPolicy, unsigned nChunks = 0)
      {
         return FitUtil::EvaluatePoissonLogL(func, data, p, iWeight, extended, nPoints, executionPolicy, nChunks);
      }

      static double EvalChi2Effective(const IModelFunctionTempl<double> &func, const BinData & data, const double * p, unsigned int &nPoints)
      {
         return FitUtil::EvaluateChi2Effective(func, data, p, nPoints);
      }
      static void EvalChi2Gradient(const IModelFunctionTempl<double> &func, const BinData &data, const double *p,
                                   double *g, unsigned int &nPoints,
                                   ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                                   unsigned nChunks = 0)
      {
         FitUtil::EvaluateChi2Gradient(func, data, p, g, nPoints, executionPolicy, nChunks);
      }
      static double EvalChi2Residual(const IModelFunctionTempl<double> &func, const BinData & data, const double * p, unsigned int i, double *g = 0)
      {
         return FitUtil::EvaluateChi2Residual(func, data, p, i, g);
      }

      /// evaluate the pdf (Poisson) contribution to the logl (return actually log of pdf)
      /// and its gradient
      static double EvalPoissonBinPdf(const IModelFunctionTempl<double> &func, const BinData & data, const double *p, unsigned int i, double *g ) {
         return FitUtil::EvaluatePoissonBinPdf(func, data, p, i, g);
      }

      static void
      EvalPoissonLogLGradient(const IModelFunctionTempl<double> &func, const BinData &data, const double *p, double *g,
                              unsigned int &nPoints,
                              ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                              unsigned nChunks = 0)
      {
         FitUtil::EvaluatePoissonLogLGradient(func, data, p, g, nPoints, executionPolicy, nChunks);
      }

      static void EvalLogLGradient(const IModelFunctionTempl<double> &func, const UnBinData &data, const double *p,
                                   double *g, unsigned int &nPoints,
                                   ROOT::Fit::ExecutionPolicy executionPolicy = ROOT::Fit::ExecutionPolicy::kSerial,
                                   unsigned nChunks = 0)
      {
         FitUtil::EvaluateLogLGradient(func, data, p, g, nPoints, executionPolicy, nChunks);
      }
   };

} // end namespace FitUtil

   } // end namespace Fit

} // end namespace ROOT

#if defined (R__HAS_VECCORE) && defined(R__HAS_VC)
//Fixes alignment for structures of SIMD structures
Vc_DECLARE_ALLOCATOR(ROOT::Fit::FitUtil::LikelihoodAux<ROOT::Double_v>);
#endif

#endif /* ROOT_Fit_FitUtil */
