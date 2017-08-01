/**
 * @file AKAZEFeatures.cpp
 * @brief Main class for detecting and describing binary features in an
 * accelerated nonlinear scale space
 * @date Sep 15, 2013
 * @author Pablo F. Alcantarilla, Jesus Nuevo
 */

#include "../precomp.hpp"
#include "AKAZEFeatures.h"
#include "fed.h"
#include "nldiffusion_functions.h"
#include "utils.h"
#include "opencl_kernels_features2d.hpp"

#include <iostream>

// Namespaces
namespace cv
{
using namespace std;

/* ************************************************************************* */
/**
 * @brief AKAZEFeatures constructor with input options
 * @param options AKAZEFeatures configuration options
 * @note This constructor allocates memory for the nonlinear scale space
 */
AKAZEFeatures::AKAZEFeatures(const AKAZEOptions& options) : options_(options) {

  ncycles_ = 0;
  reordering_ = true;

  if (options_.descriptor_size > 0 && options_.descriptor >= AKAZE::DESCRIPTOR_MLDB_UPRIGHT) {
    generateDescriptorSubsample(descriptorSamples_, descriptorBits_, options_.descriptor_size,
                                options_.descriptor_pattern_size, options_.descriptor_channels);
  }

  Allocate_Memory_Evolution();
}

/* ************************************************************************* */
/**
 * @brief This method allocates the memory for the nonlinear diffusion evolution
 */
void AKAZEFeatures::Allocate_Memory_Evolution(void) {
  CV_INSTRUMENT_REGION()

  float rfactor = 0.0f;
  int level_height = 0, level_width = 0;

  // Allocate the dimension of the matrices for the evolution
  for (int i = 0, power = 1; i <= options_.omax - 1; i++, power *= 2) {
    rfactor = 1.0f / power;
    level_height = (int)(options_.img_height*rfactor);
    level_width = (int)(options_.img_width*rfactor);

    // Smallest possible octave and allow one scale if the image is small
    if ((level_width < 80 || level_height < 40) && i != 0) {
      options_.omax = i;
      break;
    }

    for (int j = 0; j < options_.nsublevels; j++) {
      Evolution step;
      step.size = Size(level_width, level_height);
      step.esigma = options_.soffset*pow(2.f, (float)(j) / (float)(options_.nsublevels) + i);
      step.sigma_size = fRound(step.esigma * options_.derivative_factor / power);  // In fact sigma_size only depends on j
      step.etime = 0.5f * (step.esigma * step.esigma);
      step.octave = i;
      step.sublevel = j;
      step.octave_ratio = (float)power;
      evolution_.push_back(step);
    }
  }

  // Allocate memory for the number of cycles and time steps
  for (size_t i = 1; i < evolution_.size(); i++) {
    int naux = 0;
    vector<float> tau;
    float ttime = 0.0f;
    ttime = evolution_[i].etime - evolution_[i - 1].etime;
    naux = fed_tau_by_process_time(ttime, 1, 0.25f, reordering_, tau);
    nsteps_.push_back(naux);
    tsteps_.push_back(tau);
    ncycles_++;
  }
}

/* ************************************************************************* */
/**
 * @brief Computes kernel size for Gaussian smoothing if the image
 * @param sigma Kernel standard deviation
 * @returns kernel size
 */
static inline int getGaussianKernelSize(float sigma) {
  // Compute an appropriate kernel size according to the specified sigma
  int ksize = (int)ceil(2.0f*(1.0f + (sigma - 0.8f) / (0.3f)));
  ksize |= 1; // kernel should be odd
  return ksize;
}

/* ************************************************************************* */
/**
* @brief This function computes a scalar non-linear diffusion step
* @param Lt Base image in the evolution
* @param Lf Conductivity image
* @param Lstep Output image that gives the difference between the current
* Ld and the next Ld being evolved
* @param row_begin row where to start
* @param row_end last row to fill exclusive. the range is [row_begin, row_end).
* @note Forward Euler Scheme 3x3 stencil
* The function c is a scalar value that depends on the gradient norm
* dL_by_ds = d(c dL_by_dx)_by_dx + d(c dL_by_dy)_by_dy
*/
static inline void
nld_step_scalar_one_lane(const Mat& Lt, const Mat& Lf, Mat& Lstep, float step_size, int row_begin, int row_end)
{
  CV_INSTRUMENT_REGION()
  /* The labeling scheme for this five star stencil:
   [    a    ]
   [ -1 c +1 ]
   [    b    ]
   */

  Lstep.create(Lt.size(), Lt.type());
  const int cols = Lt.cols - 2;
  int row = row_begin;

  const float *lt_a, *lt_c, *lt_b;
  const float *lf_a, *lf_c, *lf_b;
  float *dst;
  float step_r = 0.f;

  // Process the top row
  if (row == 0) {
    lt_c = Lt.ptr<float>(0) + 1;  /* Skip the left-most column by +1 */
    lf_c = Lf.ptr<float>(0) + 1;
    lt_b = Lt.ptr<float>(1) + 1;
    lf_b = Lf.ptr<float>(1) + 1;

    // fill the corner to prevent uninitialized values
    dst = Lstep.ptr<float>(0);
    dst[0] = 0.0f;
    ++dst;

    for (int j = 0; j < cols; j++) {
      step_r = (lf_c[j] + lf_c[j + 1])*(lt_c[j + 1] - lt_c[j]) +
               (lf_c[j] + lf_c[j - 1])*(lt_c[j - 1] - lt_c[j]) +
               (lf_c[j] + lf_b[j    ])*(lt_b[j    ] - lt_c[j]);
      dst[j] = step_r * step_size;
    }

    // fill the corner to prevent uninitialized values
    dst[cols] = 0.0f;
    ++row;
  }

  // Process the middle rows
  int middle_end = std::min(Lt.rows - 1, row_end);
  for (; row < middle_end; ++row)
  {
    lt_a = Lt.ptr<float>(row - 1);
    lf_a = Lf.ptr<float>(row - 1);
    lt_c = Lt.ptr<float>(row    );
    lf_c = Lf.ptr<float>(row    );
    lt_b = Lt.ptr<float>(row + 1);
    lf_b = Lf.ptr<float>(row + 1);
    dst = Lstep.ptr<float>(row);

    // The left-most column
    step_r = (lf_c[0] + lf_c[1])*(lt_c[1] - lt_c[0]) +
             (lf_c[0] + lf_b[0])*(lt_b[0] - lt_c[0]) +
             (lf_c[0] + lf_a[0])*(lt_a[0] - lt_c[0]);
    dst[0] = step_r * step_size;

    lt_a++; lt_c++; lt_b++;
    lf_a++; lf_c++; lf_b++;
    dst++;

    // The middle columns
    for (int j = 0; j < cols; j++)
    {
      step_r = (lf_c[j] + lf_c[j + 1])*(lt_c[j + 1] - lt_c[j]) +
               (lf_c[j] + lf_c[j - 1])*(lt_c[j - 1] - lt_c[j]) +
               (lf_c[j] + lf_b[j    ])*(lt_b[j    ] - lt_c[j]) +
               (lf_c[j] + lf_a[j    ])*(lt_a[j    ] - lt_c[j]);
      dst[j] = step_r * step_size;
    }

    // The right-most column
    step_r = (lf_c[cols] + lf_c[cols - 1])*(lt_c[cols - 1] - lt_c[cols]) +
             (lf_c[cols] + lf_b[cols    ])*(lt_b[cols    ] - lt_c[cols]) +
             (lf_c[cols] + lf_a[cols    ])*(lt_a[cols    ] - lt_c[cols]);
    dst[cols] = step_r * step_size;
  }

  // Process the bottom row (row == Lt.rows - 1)
  if (row_end == Lt.rows) {
    lt_a = Lt.ptr<float>(row - 1) + 1;  /* Skip the left-most column by +1 */
    lf_a = Lf.ptr<float>(row - 1) + 1;
    lt_c = Lt.ptr<float>(row    ) + 1;
    lf_c = Lf.ptr<float>(row    ) + 1;

    // fill the corner to prevent uninitialized values
    dst = Lstep.ptr<float>(row);
    dst[0] = 0.0f;
    ++dst;

    for (int j = 0; j < cols; j++) {
      step_r = (lf_c[j] + lf_c[j + 1])*(lt_c[j + 1] - lt_c[j]) +
               (lf_c[j] + lf_c[j - 1])*(lt_c[j - 1] - lt_c[j]) +
               (lf_c[j] + lf_a[j    ])*(lt_a[j    ] - lt_c[j]);
      dst[j] = step_r * step_size;
    }

    // fill the corner to prevent uninitialized values
    dst[cols] = 0.0f;
  }
}

class NonLinearScalarDiffusionStep : public ParallelLoopBody
{
public:
  NonLinearScalarDiffusionStep(const Mat& Lt, const Mat& Lf, Mat& Lstep, float step_size)
    : Lt_(&Lt), Lf_(&Lf), Lstep_(&Lstep), step_size_(step_size)
  {}

  void operator()(const Range& range) const
  {
    nld_step_scalar_one_lane(*Lt_, *Lf_, *Lstep_, step_size_, range.start, range.end);
  }

private:
  const Mat* Lt_;
  const Mat* Lf_;
  Mat* Lstep_;
  float step_size_;
};

#ifdef HAVE_OPENCL
static inline bool
ocl_non_linear_diffusion_step(const UMat& Lt, const UMat& Lf, UMat& Lstep, float step_size)
{
  if(!Lt.isContinuous())
    return false;

  size_t globalSize[] = {(size_t)Lt.cols, (size_t)Lt.rows};

  ocl::Kernel ker("AKAZE_nld_step_scalar", ocl::features2d::akaze_oclsrc);
  if( ker.empty() )
    return false;

  return ker.args(
    ocl::KernelArg::ReadOnly(Lt),
    ocl::KernelArg::PtrReadOnly(Lf),
    ocl::KernelArg::PtrWriteOnly(Lstep),
    step_size).run(2, globalSize, 0, true);
}
#endif // HAVE_OPENCL

static inline void
non_linear_diffusion_step(const UMat& Lt, const UMat& Lf, UMat& Lstep, float step_size)
{
  CV_INSTRUMENT_REGION()

  Lstep.create(Lt.size(), Lt.type());

  CV_OCL_RUN(true, ocl_non_linear_diffusion_step(Lt, Lf, Lstep, step_size));

  // when on CPU UMats should be already allocated on CPU so getMat here is basicallly no-op
  Mat Mstep = Lstep.getMat(ACCESS_WRITE);
  parallel_for_(Range(0, Lt.rows), NonLinearScalarDiffusionStep(Lt.getMat(ACCESS_READ),
    Lf.getMat(ACCESS_READ), Mstep, step_size));
}

/**
 * @brief This function computes a good empirical value for the k contrast factor
 * given two gradient images, the percentile (0-1), the temporal storage to hold
 * gradient norms and the histogram bins
 * @param Lx Horizontal gradient of the input image
 * @param Ly Vertical gradient of the input image
 * @param nbins Number of histogram bins
 * @return k contrast factor
 */
static inline float
compute_kcontrast(const cv::Mat& Lx, const cv::Mat& Ly, float perc, int nbins)
{
  CV_INSTRUMENT_REGION()

  CV_Assert(nbins > 2);
  CV_Assert(!Lx.empty());

  // temporary square roots of dot product
  Mat modgs (Lx.rows - 2, Lx.cols - 2, CV_32F);
  const int total = modgs.cols * modgs.rows;
  float *modg = modgs.ptr<float>();
  float hmax = 0.0f;

  for (int i = 1; i < Lx.rows - 1; i++) {
    const float *lx = Lx.ptr<float>(i) + 1;
    const float *ly = Ly.ptr<float>(i) + 1;
    const int cols = Lx.cols - 2;

    for (int j = 0; j < cols; j++) {
      float dist = sqrtf(lx[j] * lx[j] + ly[j] * ly[j]);
      *modg++ = dist;
      hmax = std::max(hmax, dist);
    }
  }
  modg = modgs.ptr<float>();

  if (hmax == 0.0f)
    return 0.03f;  // e.g. a blank image

  // Compute the bin numbers: the value range [0, hmax] -> [0, nbins-1]
  modgs *= (nbins - 1) / hmax;

  // Count up histogram
  std::vector<int> hist(nbins, 0);
  for (int i = 0; i < total; i++)
    hist[(int)modg[i]]++;

  // Now find the perc of the histogram percentile
  const int nthreshold = (int)((total - hist[0]) * perc);  // Exclude hist[0] as background
  int nelements = 0;
  for (int k = 1; k < nbins; k++) {
    if (nelements >= nthreshold)
        return (float)hmax * k / nbins;

    nelements += hist[k];
  }

  return 0.03f;
}

#ifdef HAVE_OPENCL
static inline bool
ocl_pm_g2(const UMat& Lx, const UMat& Ly, UMat& Lflow, float kcontrast)
{
  int total = Lx.rows * Lx.cols;
  size_t globalSize[] = {(size_t)total};

  ocl::Kernel ker("AKAZE_pm_g2", ocl::features2d::akaze_oclsrc);
  if( ker.empty() )
    return false;

  return ker.args(
    ocl::KernelArg::PtrReadOnly(Lx),
    ocl::KernelArg::PtrReadOnly(Ly),
    ocl::KernelArg::PtrWriteOnly(Lflow),
    kcontrast, total).run(1, globalSize, 0, true);
}
#endif // HAVE_OPENCL

static inline void
compute_diffusivity(const UMat& Lx, const UMat& Ly, UMat& Lflow, float kcontrast, int diffusivity)
{
  CV_INSTRUMENT_REGION()

  Lflow.create(Lx.size(), Lx.type());

  switch (diffusivity) {
    case KAZE::DIFF_PM_G1:
      pm_g1(Lx, Ly, Lflow, kcontrast);
    break;
    case KAZE::DIFF_PM_G2:
      CV_OCL_RUN(true, ocl_pm_g2(Lx, Ly, Lflow, kcontrast));
      pm_g2(Lx, Ly, Lflow, kcontrast);
    break;
    case KAZE::DIFF_WEICKERT:
      weickert_diffusivity(Lx, Ly, Lflow, kcontrast);
    break;
    case KAZE::DIFF_CHARBONNIER:
      charbonnier_diffusivity(Lx, Ly, Lflow, kcontrast);
    break;
    default:
      CV_Error(diffusivity, "Diffusivity is not supported");
    break;
  }
}

/**
 * @brief Fetches pyramid from the gpu.
 * @details Setups mapping for matrices that might be probably on the GPU, if the
 * code executes with OpenCL. This will setup MLx, MLy, Mdet members in the pyramid with
 * mapping to respective UMats. This must be called before CPU-only parts of AKAZE, that work
 * only on these Mats.
 *
 * This prevents mapping/unmapping overhead (and possible uploads/downloads) that would occur, if
 * we just create Mats from UMats each time we need it later. This has devastating effects on OCL
 * performace.
 *
 * @param evolution Pyramid to download
 */
static inline void downloadPyramid(std::vector<Evolution>& evolution)
{
  CV_INSTRUMENT_REGION()

  for (size_t i = 0; i < evolution.size(); ++i) {
    Evolution& e = evolution[i];
    e.Mx = e.Lx.getMat(ACCESS_READ);
    e.My = e.Ly.getMat(ACCESS_READ);
    e.Mt = e.Lt.getMat(ACCESS_READ);
    e.Mdet = e.Ldet.getMat(ACCESS_READ);
  }
}

/**
 * @brief This method creates the nonlinear scale space for a given image
 * @param img Input image for which the nonlinear scale space needs to be created
 * @return 0 if the nonlinear scale space was created successfully, -1 otherwise
 */
void AKAZEFeatures::Create_Nonlinear_Scale_Space(InputArray img)
{
  CV_INSTRUMENT_REGION()
  CV_Assert(evolution_.size() > 0);

  // create first level of the evolution
  int ksize = getGaussianKernelSize(options_.soffset);
  GaussianBlur(img, evolution_[0].Lsmooth, Size(ksize, ksize), options_.soffset, options_.soffset, BORDER_REPLICATE);
  evolution_[0].Lsmooth.copyTo(evolution_[0].Lt);

  if (evolution_.size() == 1) {
    // we don't need to compute kcontrast factor
    Compute_Determinant_Hessian_Response();
    downloadPyramid(evolution_);
    return;
  }

  // derivatives, flow and diffusion step
  UMat Lx, Ly, Lsmooth, Lflow, Lstep;

  // compute derivatives for computing k contrast
  GaussianBlur(img, Lsmooth, Size(5, 5), 1.0f, 1.0f, BORDER_REPLICATE);
  Scharr(Lsmooth, Lx, CV_32F, 1, 0, 1, 0, BORDER_DEFAULT);
  Scharr(Lsmooth, Ly, CV_32F, 0, 1, 1, 0, BORDER_DEFAULT);
  Lsmooth.release();
  // compute the kcontrast factor
  float kcontrast = compute_kcontrast(Lx.getMat(ACCESS_READ), Ly.getMat(ACCESS_READ),
    options_.kcontrast_percentile, options_.kcontrast_nbins);

  // Now generate the rest of evolution levels
  for (size_t i = 1; i < evolution_.size(); i++) {
    Evolution &e = evolution_[i];

    if (e.octave > evolution_[i - 1].octave) {
      // new octave will be half the size
      resize(evolution_[i - 1].Lt, e.Lt, e.size, 0, 0, INTER_AREA);
      kcontrast *= 0.75f;
    }
    else {
      evolution_[i - 1].Lt.copyTo(e.Lt);
    }

    GaussianBlur(e.Lt, e.Lsmooth, Size(5, 5), 1.0f, 1.0f, BORDER_REPLICATE);

    // Compute the Gaussian derivatives Lx and Ly
    Scharr(e.Lsmooth, Lx, CV_32F, 1, 0, 1.0, 0, BORDER_DEFAULT);
    Scharr(e.Lsmooth, Ly, CV_32F, 0, 1, 1.0, 0, BORDER_DEFAULT);

    // Compute the conductivity equation
    compute_diffusivity(Lx, Ly, Lflow, kcontrast, options_.diffusivity);

    // Perform Fast Explicit Diffusion on Lt
    std::vector<float> &tsteps = tsteps_[i - 1];
    for (size_t j = 0; j < tsteps.size(); j++) {
      const float step_size = tsteps[j] * 0.5f;
      non_linear_diffusion_step(e.Lt, Lflow, Lstep, step_size);
      add(e.Lt, Lstep, e.Lt);
    }
  }

  Compute_Determinant_Hessian_Response();
  downloadPyramid(evolution_);

  return;
}

/* ************************************************************************* */
/**
 * @brief This method selects interesting keypoints through the nonlinear scale space
 * @param kpts Vector of detected keypoints
 */
void AKAZEFeatures::Feature_Detection(std::vector<KeyPoint>& kpts)
{
  CV_INSTRUMENT_REGION()

  kpts.clear();
  Find_Scale_Space_Extrema(kpts);
  Do_Subpixel_Refinement(kpts);
}

/* ************************************************************************* */

#ifdef HAVE_OPENCL
static inline bool
ocl_compute_determinant(const UMat& Lxx, const UMat& Lxy, const UMat& Lyy,
  UMat& Ldet, float sigma)
{
  const int total = Lxx.rows * Lxx.cols;
  size_t globalSize[] = {(size_t)total};

  ocl::Kernel ker("AKAZE_compute_determinant", ocl::features2d::akaze_oclsrc);
  if( ker.empty() )
    return false;

  return ker.args(
    ocl::KernelArg::PtrReadOnly(Lxx),
    ocl::KernelArg::PtrReadOnly(Lxy),
    ocl::KernelArg::PtrReadOnly(Lyy),
    ocl::KernelArg::PtrWriteOnly(Ldet),
    sigma, total).run(1, globalSize, 0, true);
}
#endif // HAVE_OPENCL

/**
 * @brief Compute determinant from hessians
 * @details Compute Ldet by (Lxx.mul(Lyy) - Lxy.mul(Lxy)) * sigma
 *
 * @param Lxx spatial derivates
 * @param Lxy spatial derivates
 * @param Lyy spatial derivates
 * @param Ldet output determinant
 * @param sigma determinant will be scaled by this sigma
 */
static inline void compute_determinant(const UMat& Lxx, const UMat& Lxy, const UMat& Lyy,
  UMat& Ldet, float sigma)
{
  CV_INSTRUMENT_REGION()

  Ldet.create(Lxx.size(), Lxx.type());

  CV_OCL_RUN(true, ocl_compute_determinant(Lxx, Lxy, Lyy, Ldet, sigma));

  // output determinant
  Mat Mxx = Lxx.getMat(ACCESS_READ), Mxy = Lxy.getMat(ACCESS_READ), Myy = Lyy.getMat(ACCESS_READ);
  Mat Mdet = Ldet.getMat(ACCESS_WRITE);
  float *lxx = Mxx.ptr<float>();
  float *lxy = Mxy.ptr<float>();
  float *lyy = Myy.ptr<float>();
  float *ldet = Mdet.ptr<float>();
  const int total = Lxx.cols * Lxx.rows;
  for (int j = 0; j < total; j++) {
    ldet[j] = (lxx[j] * lyy[j] - lxy[j] * lxy[j]) * sigma;
  }

}

class DeterminantHessianResponse : public ParallelLoopBody
{
public:
    explicit DeterminantHessianResponse(std::vector<Evolution>& ev)
    : evolution_(&ev)
  {
  }

  void operator()(const Range& range) const
  {
    UMat Lxx, Lxy, Lyy;

    for (int i = range.start; i < range.end; i++)
    {
      Evolution &e = (*evolution_)[i];

      // we cannot use cv:Scharr here, because we need to handle also
      // kernel sizes other than 3, by default we are using 9x9, 5x5 and 7x7

      // compute kernels
      Mat DxKx, DxKy, DyKx, DyKy;
      compute_derivative_kernels(DxKx, DxKy, 1, 0, e.sigma_size);
      compute_derivative_kernels(DyKx, DyKy, 0, 1, e.sigma_size);

      // compute the multiscale derivatives
      sepFilter2D(e.Lsmooth, e.Lx, CV_32F, DxKx, DxKy);
      sepFilter2D(e.Lx, Lxx, CV_32F, DxKx, DxKy);
      sepFilter2D(e.Lx, Lxy, CV_32F, DyKx, DyKy);
      sepFilter2D(e.Lsmooth, e.Ly, CV_32F, DyKx, DyKy);
      sepFilter2D(e.Ly, Lyy, CV_32F, DyKx, DyKy);

      // free Lsmooth to same some space in the pyramid, it is not needed anymore
      e.Lsmooth.release();

      // compute determinant scaled by sigma
      float sigma_size_quat = (float)(e.sigma_size * e.sigma_size * e.sigma_size * e.sigma_size);
      compute_determinant(Lxx, Lxy, Lyy, e.Ldet, sigma_size_quat);
    }
  }

private:
  std::vector<Evolution>*  evolution_;
};


/**
 * @brief This method computes the feature detector response for the nonlinear scale space
 * @note We use the Hessian determinant as the feature detector response
 */
void AKAZEFeatures::Compute_Determinant_Hessian_Response(void) {
  CV_INSTRUMENT_REGION()

  if (ocl::useOpenCL()) {
    DeterminantHessianResponse body (evolution_);
    body(Range(0, (int)evolution_.size()));
  } else {
    parallel_for_(Range(0, (int)evolution_.size()), DeterminantHessianResponse(evolution_));
  }
}

/* ************************************************************************* */
/**
 * @brief This method finds extrema in the nonlinear scale space
 * @param kpts Vector of detected keypoints
 */
void AKAZEFeatures::Find_Scale_Space_Extrema(std::vector<KeyPoint>& kpts)
{
  CV_INSTRUMENT_REGION()

  float value = 0.0;
  float dist = 0.0, ratio = 0.0, smax = 0.0;
  int npoints = 0, id_repeated = 0;
  int sigma_size_ = 0, left_x = 0, right_x = 0, up_y = 0, down_y = 0;
  bool is_extremum = false, is_repeated = false, is_out = false;
  KeyPoint point;
  vector<KeyPoint> kpts_aux;

  // Set maximum size
  if (options_.descriptor == AKAZE::DESCRIPTOR_MLDB_UPRIGHT || options_.descriptor == AKAZE::DESCRIPTOR_MLDB) {
    smax = 10.0f*sqrtf(2.0f);
  }
  else if (options_.descriptor == AKAZE::DESCRIPTOR_KAZE_UPRIGHT || options_.descriptor == AKAZE::DESCRIPTOR_KAZE) {
    smax = 12.0f*sqrtf(2.0f);
  }

  for (size_t i = 0; i < evolution_.size(); i++) {
    Mat Ldet = evolution_[i].Mdet;
    const float* prev = Ldet.ptr<float>(0);
    const float* curr = Ldet.ptr<float>(1);
    for (int ix = 1; ix < Ldet.rows - 1; ix++) {
      const float* next = Ldet.ptr<float>(ix + 1);

      for (int jx = 1; jx < Ldet.cols - 1; jx++) {
        is_extremum = false;
        is_repeated = false;
        is_out = false;
        value = *(Ldet.ptr<float>(ix)+jx);

        // Filter the points with the detector threshold
        if (value > options_.dthreshold && value >= options_.min_dthreshold &&
            value > curr[jx-1] &&
            value > curr[jx+1] &&
            value > prev[jx-1] &&
            value > prev[jx] &&
            value > prev[jx+1] &&
            value > next[jx-1] &&
            value > next[jx] &&
            value > next[jx+1]) {

          is_extremum = true;
          point.response = fabs(value);
          point.size = evolution_[i].esigma*options_.derivative_factor;
          point.octave = (int)evolution_[i].octave;
          point.class_id = (int)i;
          ratio = (float)fastpow(2, point.octave);
          sigma_size_ = fRound(point.size / ratio);
          point.pt.x = static_cast<float>(jx);
          point.pt.y = static_cast<float>(ix);

          // Compare response with the same and lower scale
          for (size_t ik = 0; ik < kpts_aux.size(); ik++) {

            if ((point.class_id - 1) == kpts_aux[ik].class_id ||
                point.class_id == kpts_aux[ik].class_id) {
              float distx = point.pt.x*ratio - kpts_aux[ik].pt.x;
              float disty = point.pt.y*ratio - kpts_aux[ik].pt.y;
              dist = distx * distx + disty * disty;
              if (dist <= point.size * point.size) {
                if (point.response > kpts_aux[ik].response) {
                  id_repeated = (int)ik;
                  is_repeated = true;
                }
                else {
                  is_extremum = false;
                }
                break;
              }
            }
          }

          // Check out of bounds
          if (is_extremum == true) {

            // Check that the point is under the image limits for the descriptor computation
            left_x = fRound(point.pt.x - smax*sigma_size_) - 1;
            right_x = fRound(point.pt.x + smax*sigma_size_) + 1;
            up_y = fRound(point.pt.y - smax*sigma_size_) - 1;
            down_y = fRound(point.pt.y + smax*sigma_size_) + 1;

            if (left_x < 0 || right_x >= Ldet.cols ||
                up_y < 0 || down_y >= Ldet.rows) {
              is_out = true;
            }

            if (is_out == false) {
              if (is_repeated == false) {
                point.pt.x = (float)(point.pt.x*ratio + .5*(ratio-1.0));
                point.pt.y = (float)(point.pt.y*ratio + .5*(ratio-1.0));
                kpts_aux.push_back(point);
                npoints++;
              }
              else {
                point.pt.x = (float)(point.pt.x*ratio + .5*(ratio-1.0));
                point.pt.y = (float)(point.pt.y*ratio + .5*(ratio-1.0));
                kpts_aux[id_repeated] = point;
              }
            } // if is_out
          } //if is_extremum
        }
      } // for jx
      prev = curr;
      curr = next;
    } // for ix
  } // for i

  // Now filter points with the upper scale level
  for (size_t i = 0; i < kpts_aux.size(); i++) {

    is_repeated = false;
    const KeyPoint& pt = kpts_aux[i];
    for (size_t j = i + 1; j < kpts_aux.size(); j++) {

      // Compare response with the upper scale
      if ((pt.class_id + 1) == kpts_aux[j].class_id) {
        float distx = pt.pt.x - kpts_aux[j].pt.x;
        float disty = pt.pt.y - kpts_aux[j].pt.y;
        dist = distx * distx + disty * disty;
        if (dist <= pt.size * pt.size) {
          if (pt.response < kpts_aux[j].response) {
            is_repeated = true;
            break;
          }
        }
      }
    }

    if (is_repeated == false)
      kpts.push_back(pt);
  }
}

/* ************************************************************************* */
/**
 * @brief This method performs subpixel refinement of the detected keypoints
 * @param kpts Vector of detected keypoints
 */
void AKAZEFeatures::Do_Subpixel_Refinement(std::vector<KeyPoint>& kpts)
{
  CV_INSTRUMENT_REGION()

  float Dx = 0.0, Dy = 0.0, ratio = 0.0;
  float Dxx = 0.0, Dyy = 0.0, Dxy = 0.0;
  int x = 0, y = 0;
  Matx22f A(0, 0, 0, 0);
  Vec2f b(0, 0);
  Vec2f dst(0, 0);

  for (size_t i = 0; i < kpts.size(); i++) {
    ratio = (float)fastpow(2, kpts[i].octave);
    x = fRound(kpts[i].pt.x / ratio);
    y = fRound(kpts[i].pt.y / ratio);
    Mat Ldet = evolution_[kpts[i].class_id].Mdet;

    // Compute the gradient
    Dx = (0.5f)*(*(Ldet.ptr<float>(y)+x + 1)
        - *(Ldet.ptr<float>(y)+x - 1));
    Dy = (0.5f)*(*(Ldet.ptr<float>(y + 1) + x)
        - *(Ldet.ptr<float>(y - 1) + x));

    // Compute the Hessian
    Dxx = (*(Ldet.ptr<float>(y)+x + 1)
        + *(Ldet.ptr<float>(y)+x - 1)
        - 2.0f*(*(Ldet.ptr<float>(y)+x)));

    Dyy = (*(Ldet.ptr<float>(y + 1) + x)
        + *(Ldet.ptr<float>(y - 1) + x)
        - 2.0f*(*(Ldet.ptr<float>(y)+x)));

    Dxy = (0.25f)*(*(Ldet.ptr<float>(y + 1) + x + 1)
        + (*(Ldet.ptr<float>(y - 1) + x - 1)))
        - (0.25f)*(*(Ldet.ptr<float>(y - 1) + x + 1)
        + (*(Ldet.ptr<float>(y + 1) + x - 1)));

    // Solve the linear system
    A(0, 0) = Dxx;
    A(1, 1) = Dyy;
    A(0, 1) = A(1, 0) = Dxy;
    b(0) = -Dx;
    b(1) = -Dy;

    solve(A, b, dst, DECOMP_LU);

    if (fabs(dst(0)) <= 1.0f && fabs(dst(1)) <= 1.0f) {
        kpts[i].pt.x = x + dst(0);
      kpts[i].pt.y = y + dst(1);
      int power = fastpow(2, evolution_[kpts[i].class_id].octave);
      kpts[i].pt.x = (float)(kpts[i].pt.x*power + .5*(power-1));
      kpts[i].pt.y = (float)(kpts[i].pt.y*power + .5*(power-1));
      kpts[i].angle = 0.0;

      // In OpenCV the size of a keypoint its the diameter
      kpts[i].size *= 2.0f;
    }
    // Delete the point since its not stable
    else {
      kpts.erase(kpts.begin() + i);
      i--;
    }
  }
}

/* ************************************************************************* */

class SURF_Descriptor_Upright_64_Invoker : public ParallelLoopBody
{
public:
  SURF_Descriptor_Upright_64_Invoker(std::vector<KeyPoint>& kpts, Mat& desc, std::vector<Evolution>& evolution)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_SURF_Descriptor_Upright_64((*keypoints_)[i], descriptors_->ptr<float>(i));
    }
  }

  void Get_SURF_Descriptor_Upright_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
};

class SURF_Descriptor_64_Invoker : public ParallelLoopBody
{
public:
  SURF_Descriptor_64_Invoker(std::vector<KeyPoint>& kpts, Mat& desc, std::vector<Evolution>& evolution)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
  {
  }

  void operator()(const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_SURF_Descriptor_64((*keypoints_)[i], descriptors_->ptr<float>(i));
    }
  }

  void Get_SURF_Descriptor_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
};

class MSURF_Upright_Descriptor_64_Invoker : public ParallelLoopBody
{
public:
  MSURF_Upright_Descriptor_64_Invoker(std::vector<KeyPoint>& kpts, Mat& desc, std::vector<Evolution>& evolution)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
  {
  }

  void operator()(const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_MSURF_Upright_Descriptor_64((*keypoints_)[i], descriptors_->ptr<float>(i));
    }
  }

  void Get_MSURF_Upright_Descriptor_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
};

class MSURF_Descriptor_64_Invoker : public ParallelLoopBody
{
public:
  MSURF_Descriptor_64_Invoker(std::vector<KeyPoint>& kpts, Mat& desc, std::vector<Evolution>& evolution)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_MSURF_Descriptor_64((*keypoints_)[i], descriptors_->ptr<float>(i));
    }
  }

  void Get_MSURF_Descriptor_64(const KeyPoint& kpt, float* desc) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
};

class Upright_MLDB_Full_Descriptor_Invoker : public ParallelLoopBody
{
public:
  Upright_MLDB_Full_Descriptor_Invoker(std::vector<KeyPoint>& kpts, Mat& desc, std::vector<Evolution>& evolution, AKAZEOptions& options)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
    , options_(&options)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_Upright_MLDB_Full_Descriptor((*keypoints_)[i], descriptors_->ptr<unsigned char>(i));
    }
  }

  void Get_Upright_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char* desc) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
  AKAZEOptions*              options_;
};

class Upright_MLDB_Descriptor_Subset_Invoker : public ParallelLoopBody
{
public:
  Upright_MLDB_Descriptor_Subset_Invoker(std::vector<KeyPoint>& kpts,
                                         Mat& desc,
                                         std::vector<Evolution>& evolution,
                                         AKAZEOptions& options,
                                         Mat descriptorSamples,
                                         Mat descriptorBits)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
    , options_(&options)
    , descriptorSamples_(descriptorSamples)
    , descriptorBits_(descriptorBits)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_Upright_MLDB_Descriptor_Subset((*keypoints_)[i], descriptors_->ptr<unsigned char>(i));
    }
  }

  void Get_Upright_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char* desc) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
  AKAZEOptions*              options_;

  Mat descriptorSamples_;  // List of positions in the grids to sample LDB bits from.
  Mat descriptorBits_;
};

class MLDB_Full_Descriptor_Invoker : public ParallelLoopBody
{
public:
  MLDB_Full_Descriptor_Invoker(std::vector<KeyPoint>& kpts, Mat& desc, std::vector<Evolution>& evolution, AKAZEOptions& options)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
    , options_(&options)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_MLDB_Full_Descriptor((*keypoints_)[i], descriptors_->ptr<unsigned char>(i));
    }
  }

  void Get_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char* desc) const;
  void MLDB_Fill_Values(float* values, int sample_step, int level,
                        float xf, float yf, float co, float si, float scale) const;
  void MLDB_Binary_Comparisons(float* values, unsigned char* desc,
                               int count, int& dpos) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
  AKAZEOptions*              options_;
};

class MLDB_Descriptor_Subset_Invoker : public ParallelLoopBody
{
public:
  MLDB_Descriptor_Subset_Invoker(std::vector<KeyPoint>& kpts,
                                 Mat& desc,
                                 std::vector<Evolution>& evolution,
                                 AKAZEOptions& options,
                                 Mat descriptorSamples,
                                 Mat descriptorBits)
    : keypoints_(&kpts)
    , descriptors_(&desc)
    , evolution_(&evolution)
    , options_(&options)
    , descriptorSamples_(descriptorSamples)
    , descriptorBits_(descriptorBits)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Get_MLDB_Descriptor_Subset((*keypoints_)[i], descriptors_->ptr<unsigned char>(i));
    }
  }

  void Get_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char* desc) const;

private:
  std::vector<KeyPoint>* keypoints_;
  Mat*                   descriptors_;
  std::vector<Evolution>*   evolution_;
  AKAZEOptions*              options_;

  Mat descriptorSamples_;  // List of positions in the grids to sample LDB bits from.
  Mat descriptorBits_;
};

/**
 * @brief This method  computes the set of descriptors through the nonlinear scale space
 * @param kpts Vector of detected keypoints
 * @param desc Matrix to store the descriptors
 */
void AKAZEFeatures::Compute_Descriptors(std::vector<KeyPoint>& kpts, OutputArray descriptors)
{
  CV_INSTRUMENT_REGION()

  for(size_t i = 0; i < kpts.size(); i++)
  {
      CV_Assert(0 <= kpts[i].class_id && kpts[i].class_id < static_cast<int>(evolution_.size()));
  }

  // Allocate memory for the matrix with the descriptors
  if (options_.descriptor < AKAZE::DESCRIPTOR_MLDB_UPRIGHT) {
    descriptors.create((int)kpts.size(), 64, CV_32FC1);
  }
  else {
    // We use the full length binary descriptor -> 486 bits
    if (options_.descriptor_size == 0) {
      int t = (6 + 36 + 120)*options_.descriptor_channels;
      descriptors.create((int)kpts.size(), (int)ceil(t / 8.), CV_8UC1);
    }
    else {
      // We use the random bit selection length binary descriptor
      descriptors.create((int)kpts.size(), (int)ceil(options_.descriptor_size / 8.), CV_8UC1);
    }
  }

  Mat desc = descriptors.getMat();

  switch (options_.descriptor)
  {
    case AKAZE::DESCRIPTOR_KAZE_UPRIGHT: // Upright descriptors, not invariant to rotation
    {
      parallel_for_(Range(0, (int)kpts.size()), MSURF_Upright_Descriptor_64_Invoker(kpts, desc, evolution_));
    }
    break;
    case AKAZE::DESCRIPTOR_KAZE:
    {
      parallel_for_(Range(0, (int)kpts.size()), MSURF_Descriptor_64_Invoker(kpts, desc, evolution_));
    }
    break;
    case AKAZE::DESCRIPTOR_MLDB_UPRIGHT: // Upright descriptors, not invariant to rotation
    {
      if (options_.descriptor_size == 0)
        parallel_for_(Range(0, (int)kpts.size()), Upright_MLDB_Full_Descriptor_Invoker(kpts, desc, evolution_, options_));
      else
        parallel_for_(Range(0, (int)kpts.size()), Upright_MLDB_Descriptor_Subset_Invoker(kpts, desc, evolution_, options_, descriptorSamples_, descriptorBits_));
    }
    break;
    case AKAZE::DESCRIPTOR_MLDB:
    {
      if (options_.descriptor_size == 0)
        parallel_for_(Range(0, (int)kpts.size()), MLDB_Full_Descriptor_Invoker(kpts, desc, evolution_, options_));
      else
        parallel_for_(Range(0, (int)kpts.size()), MLDB_Descriptor_Subset_Invoker(kpts, desc, evolution_, options_, descriptorSamples_, descriptorBits_));
    }
    break;
  }
}

/* ************************************************************************* */
/**
 * @brief This function samples the derivative responses Lx and Ly for the points
 * within the radius of 6*scale from (x0, y0), then multiply 2D Gaussian weight
 * @param Lx Horizontal derivative
 * @param Ly Vertical derivative
 * @param x0 X-coordinate of the center point
 * @param y0 Y-coordinate of the center point
 * @param scale The sampling step
 * @param resX Output array of the weighted horizontal derivative responses
 * @param resY Output array of the weighted vertical derivative responses
 */
static inline
void Sample_Derivative_Response_Radius6(const Mat &Lx, const Mat &Ly,
                                  const int x0, const int y0, const int scale,
                                  float *resX, float *resY)
{
    /* ************************************************************************* */
    /// Lookup table for 2d gaussian (sigma = 2.5) where (0,0) is top left and (6,6) is bottom right
    static const float gauss25[7][7] =
    {
        { 0.02546481f, 0.02350698f, 0.01849125f, 0.01239505f, 0.00708017f, 0.00344629f, 0.00142946f },
        { 0.02350698f, 0.02169968f, 0.01706957f, 0.01144208f, 0.00653582f, 0.00318132f, 0.00131956f },
        { 0.01849125f, 0.01706957f, 0.01342740f, 0.00900066f, 0.00514126f, 0.00250252f, 0.00103800f },
        { 0.01239505f, 0.01144208f, 0.00900066f, 0.00603332f, 0.00344629f, 0.00167749f, 0.00069579f },
        { 0.00708017f, 0.00653582f, 0.00514126f, 0.00344629f, 0.00196855f, 0.00095820f, 0.00039744f },
        { 0.00344629f, 0.00318132f, 0.00250252f, 0.00167749f, 0.00095820f, 0.00046640f, 0.00019346f },
        { 0.00142946f, 0.00131956f, 0.00103800f, 0.00069579f, 0.00039744f, 0.00019346f, 0.00008024f }
    };
    static const int id[] = { 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6 };
    static const struct gtable
    {
      float weight[109];
      int8_t xidx[109];
      int8_t yidx[109];

      explicit gtable(void)
      {
        // Generate the weight and indices by one-time initialization
        int k = 0;
        for (int i = -6; i <= 6; ++i) {
          for (int j = -6; j <= 6; ++j) {
            if (i*i + j*j < 36) {
              weight[k] = gauss25[id[i + 6]][id[j + 6]];
              yidx[k] = static_cast<int8_t>(i);
              xidx[k] = static_cast<int8_t>(j);
              ++k;
            }
          }
        }
        CV_DbgAssert(k == 109);
      }
    } g;

  const float * lx = Lx.ptr<float>(0);
  const float * ly = Ly.ptr<float>(0);
  int cols = Lx.cols;

  for (int i = 0; i < 109; i++) {
    int j = (y0 + g.yidx[i] * scale) * cols + (x0 + g.xidx[i] * scale);

    resX[i] = g.weight[i] * lx[j];
    resY[i] = g.weight[i] * ly[j];

    CV_DbgAssert(isfinite(resX[i]));
    CV_DbgAssert(isfinite(resY[i]));
  }
}

/**
 * @brief This function sorts a[] by quantized float values
 * @param a[] Input floating point array to sort
 * @param n The length of a[]
 * @param quantum The interval to convert a[i]'s float values to integers
 * @param max The upper bound of a[], meaning a[i] must be in [0, max]
 * @param idx[] Output array of the indices: a[idx[i]] forms a sorted array
 * @param cum[] Output array of the starting indices of quantized floats
 * @note The values of a[] in [k*quantum, (k + 1)*quantum) is labeled by
 * the integer k, which is calculated by floor(a[i]/quantum).  After sorting,
 * the values from a[idx[cum[k]]] to a[idx[cum[k+1]-1]] are all labeled by k.
 * This sorting is unstable to reduce the memory access.
 */
static inline
void quantized_counting_sort(const float a[], const int n,
                             const float quantum, const float max,
                             uint8_t idx[], uint8_t cum[])
{
  const int nkeys = (int)(max / quantum);

  // The size of cum[] must be nkeys + 1
  memset(cum, 0, nkeys + 1);

  // Count up the quantized values
  for (int i = 0; i < n; i++)
    cum[(int)(a[i] / quantum)]++;

  // Compute the inclusive prefix sum i.e. the end indices; cum[nkeys] is the total
  for (int i = 1; i <= nkeys; i++)
    cum[i] += cum[i - 1];

  // Generate the sorted indices; cum[] becomes the exclusive prefix sum i.e. the start indices of keys
  for (int i = 0; i < n; i++)
    idx[--cum[(int)(a[i] / quantum)]] = static_cast<uint8_t>(i);
}

/**
 * @brief This function computes the main orientation for a given keypoint
 * @param kpt Input keypoint
 * @note The orientation is computed using a similar approach as described in the
 * original SURF method. See Bay et al., Speeded Up Robust Features, ECCV 2006
 */
static inline
void Compute_Main_Orientation(KeyPoint& kpt, const std::vector<Evolution>& evolution)
{
  // get the right evolution level for this keypoint
  const Evolution& e = evolution[kpt.class_id];
  // Get the information from the keypoint
  int scale = fRound(0.5f * kpt.size / e.octave_ratio);
  int x0 = fRound(kpt.pt.x / e.octave_ratio);
  int y0 = fRound(kpt.pt.y / e.octave_ratio);

  // Sample derivatives responses for the points within radius of 6*scale
  const int ang_size = 109;
  float resX[ang_size], resY[ang_size];
  Sample_Derivative_Response_Radius6(e.Mx, e.My, x0, y0, scale, resX, resY);

  // Compute the angle of each gradient vector
  float Ang[ang_size];
  hal::fastAtan2(resY, resX, Ang, ang_size, false);

  // Sort by the angles; angles are labeled by slices of 0.15 radian
  const int slices = 42;
  const float ang_step = (float)(2.0 * CV_PI / slices);
  uint8_t slice[slices + 1];
  uint8_t sorted_idx[ang_size];
  quantized_counting_sort(Ang, ang_size, ang_step, (float)(2.0 * CV_PI), sorted_idx, slice);

  // Find the main angle by sliding a window of 7-slice size(=PI/3) around the keypoint
  const int win = 7;

  float maxX = 0.0f, maxY = 0.0f;
  for (int i = slice[0]; i < slice[win]; i++) {
    maxX += resX[sorted_idx[i]];
    maxY += resY[sorted_idx[i]];
  }
  float maxNorm = maxX * maxX + maxY * maxY;

  for (int sn = 1; sn <= slices - win; sn++) {

    if (slice[sn] == slice[sn - 1] && slice[sn + win] == slice[sn + win - 1])
      continue;  // The contents of the window didn't change; don't repeat the computation

    float sumX = 0.0f, sumY = 0.0f;
    for (int i = slice[sn]; i < slice[sn + win]; i++) {
      sumX += resX[sorted_idx[i]];
      sumY += resY[sorted_idx[i]];
    }

    float norm = sumX * sumX + sumY * sumY;
    if (norm > maxNorm)
        maxNorm = norm, maxX = sumX, maxY = sumY;  // Found bigger one; update
  }

  for (int sn = slices - win + 1; sn < slices; sn++) {
    int remain = sn + win - slices;

    if (slice[sn] == slice[sn - 1] && slice[remain] == slice[remain - 1])
      continue;

    float sumX = 0.0f, sumY = 0.0f;
    for (int i = slice[sn]; i < slice[slices]; i++) {
      sumX += resX[sorted_idx[i]];
      sumY += resY[sorted_idx[i]];
    }
    for (int i = slice[0]; i < slice[remain]; i++) {
      sumX += resX[sorted_idx[i]];
      sumY += resY[sorted_idx[i]];
    }

    float norm = sumX * sumX + sumY * sumY;
    if (norm > maxNorm)
        maxNorm = norm, maxX = sumX, maxY = sumY;
  }

  // Store the final result
  kpt.angle = fastAtan2(maxY, maxX);
}

class ComputeKeypointOrientation : public ParallelLoopBody
{
public:
  ComputeKeypointOrientation(std::vector<KeyPoint>& kpts,
                             const std::vector<Evolution>& evolution)
    : keypoints_(&kpts)
    , evolution_(&evolution)
  {
  }

  void operator() (const Range& range) const
  {
    for (int i = range.start; i < range.end; i++)
    {
      Compute_Main_Orientation((*keypoints_)[i], *evolution_);
    }
  }
private:
  std::vector<KeyPoint>* keypoints_;
  const std::vector<Evolution>* evolution_;
};

/**
 * @brief This method computes the main orientation for a given keypoints
 * @param kpts Input keypoints
 */
void AKAZEFeatures::Compute_Keypoints_Orientation(std::vector<KeyPoint>& kpts) const
{
  CV_INSTRUMENT_REGION()

  parallel_for_(Range(0, (int)kpts.size()), ComputeKeypointOrientation(kpts, evolution_));
}

/* ************************************************************************* */
/**
 * @brief This method computes the upright descriptor (not rotation invariant) of
 * the provided keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 * @note Rectangular grid of 24 s x 24 s. Descriptor Length 64. The descriptor is inspired
 * from Agrawal et al., CenSurE: Center Surround Extremas for Realtime Feature Detection and Matching,
 * ECCV 2008
 */
void MSURF_Upright_Descriptor_64_Invoker::Get_MSURF_Upright_Descriptor_64(const KeyPoint& kpt, float *desc) const {

  float dx = 0.0, dy = 0.0, mdx = 0.0, mdy = 0.0, gauss_s1 = 0.0, gauss_s2 = 0.0;
  float rx = 0.0, ry = 0.0, len = 0.0, xf = 0.0, yf = 0.0, ys = 0.0, xs = 0.0;
  float sample_x = 0.0, sample_y = 0.0;
  int x1 = 0, y1 = 0, sample_step = 0, pattern_size = 0;
  int x2 = 0, y2 = 0, kx = 0, ky = 0, i = 0, j = 0, dcount = 0;
  float fx = 0.0, fy = 0.0, ratio = 0.0, res1 = 0.0, res2 = 0.0, res3 = 0.0, res4 = 0.0;
  int scale = 0, dsize = 0;

  // Subregion centers for the 4x4 gaussian weighting
  float cx = -0.5f, cy = 0.5f;

  const std::vector<Evolution>& evolution = *evolution_;

  // Set the descriptor size and the sample and pattern sizes
  dsize = 64;
  sample_step = 5;
  pattern_size = 12;

  // Get the information from the keypoint
  ratio = (float)(1 << kpt.octave);
  scale = fRound(0.5f*kpt.size / ratio);
  const int level = kpt.class_id;
  Mat Lx = evolution[level].Mx;
  Mat Ly = evolution[level].My;
  yf = kpt.pt.y / ratio;
  xf = kpt.pt.x / ratio;

  i = -8;

  // Calculate descriptor for this interest point
  // Area of size 24 s x 24 s
  while (i < pattern_size) {
    j = -8;
    i = i - 4;

    cx += 1.0f;
    cy = -0.5f;

    while (j < pattern_size) {
      dx = dy = mdx = mdy = 0.0;
      cy += 1.0f;
      j = j - 4;

      ky = i + sample_step;
      kx = j + sample_step;

      ys = yf + (ky*scale);
      xs = xf + (kx*scale);

      for (int k = i; k < i + 9; k++) {
        for (int l = j; l < j + 9; l++) {
          sample_y = k*scale + yf;
          sample_x = l*scale + xf;

          //Get the gaussian weighted x and y responses
          gauss_s1 = gaussian(xs - sample_x, ys - sample_y, 2.50f*scale);

          y1 = (int)(sample_y - .5);
          x1 = (int)(sample_x - .5);

          y2 = (int)(sample_y + .5);
          x2 = (int)(sample_x + .5);

          fx = sample_x - x1;
          fy = sample_y - y1;

          res1 = *(Lx.ptr<float>(y1)+x1);
          res2 = *(Lx.ptr<float>(y1)+x2);
          res3 = *(Lx.ptr<float>(y2)+x1);
          res4 = *(Lx.ptr<float>(y2)+x2);
          rx = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          res1 = *(Ly.ptr<float>(y1)+x1);
          res2 = *(Ly.ptr<float>(y1)+x2);
          res3 = *(Ly.ptr<float>(y2)+x1);
          res4 = *(Ly.ptr<float>(y2)+x2);
          ry = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          rx = gauss_s1*rx;
          ry = gauss_s1*ry;

          // Sum the derivatives to the cumulative descriptor
          dx += rx;
          dy += ry;
          mdx += fabs(rx);
          mdy += fabs(ry);
        }
      }

      // Add the values to the descriptor vector
      gauss_s2 = gaussian(cx - 2.0f, cy - 2.0f, 1.5f);

      desc[dcount++] = dx*gauss_s2;
      desc[dcount++] = dy*gauss_s2;
      desc[dcount++] = mdx*gauss_s2;
      desc[dcount++] = mdy*gauss_s2;

      len += (dx*dx + dy*dy + mdx*mdx + mdy*mdy)*gauss_s2*gauss_s2;

      j += 9;
    }

    i += 9;
  }

  // convert to unit vector
  len = sqrt(len);

  for (i = 0; i < dsize; i++) {
    desc[i] /= len;
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the descriptor of the provided keypoint given the
 * main orientation of the keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 * @note Rectangular grid of 24 s x 24 s. Descriptor Length 64. The descriptor is inspired
 * from Agrawal et al., CenSurE: Center Surround Extremas for Realtime Feature Detection and Matching,
 * ECCV 2008
 */
void MSURF_Descriptor_64_Invoker::Get_MSURF_Descriptor_64(const KeyPoint& kpt, float *desc) const {

  float dx = 0.0, dy = 0.0, mdx = 0.0, mdy = 0.0, gauss_s1 = 0.0, gauss_s2 = 0.0;
  float rx = 0.0, ry = 0.0, rrx = 0.0, rry = 0.0, len = 0.0, xf = 0.0, yf = 0.0, ys = 0.0, xs = 0.0;
  float sample_x = 0.0, sample_y = 0.0, co = 0.0, si = 0.0, angle = 0.0;
  float fx = 0.0, fy = 0.0, ratio = 0.0, res1 = 0.0, res2 = 0.0, res3 = 0.0, res4 = 0.0;
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0, sample_step = 0, pattern_size = 0;
  int kx = 0, ky = 0, i = 0, j = 0, dcount = 0;
  int scale = 0, dsize = 0;

  // Subregion centers for the 4x4 gaussian weighting
  float cx = -0.5f, cy = 0.5f;

  const std::vector<Evolution>& evolution = *evolution_;

  // Set the descriptor size and the sample and pattern sizes
  dsize = 64;
  sample_step = 5;
  pattern_size = 12;

  // Get the information from the keypoint
  ratio = (float)(1 << kpt.octave);
  scale = fRound(0.5f*kpt.size / ratio);
  angle = (kpt.angle * static_cast<float>(CV_PI)) / 180.f;
  const int level = kpt.class_id;
  Mat Lx = evolution[level].Mx;
  Mat Ly = evolution[level].My;
  yf = kpt.pt.y / ratio;
  xf = kpt.pt.x / ratio;
  co = cos(angle);
  si = sin(angle);

  i = -8;

  // Calculate descriptor for this interest point
  // Area of size 24 s x 24 s
  while (i < pattern_size) {
    j = -8;
    i = i - 4;

    cx += 1.0f;
    cy = -0.5f;

    while (j < pattern_size) {
      dx = dy = mdx = mdy = 0.0;
      cy += 1.0f;
      j = j - 4;

      ky = i + sample_step;
      kx = j + sample_step;

      xs = xf + (-kx*scale*si + ky*scale*co);
      ys = yf + (kx*scale*co + ky*scale*si);

      for (int k = i; k < i + 9; ++k) {
        for (int l = j; l < j + 9; ++l) {
          // Get coords of sample point on the rotated axis
          sample_y = yf + (l*scale*co + k*scale*si);
          sample_x = xf + (-l*scale*si + k*scale*co);

          // Get the gaussian weighted x and y responses
          gauss_s1 = gaussian(xs - sample_x, ys - sample_y, 2.5f*scale);

          y1 = fRound(sample_y - 0.5f);
          x1 = fRound(sample_x - 0.5f);

          y2 = fRound(sample_y + 0.5f);
          x2 = fRound(sample_x + 0.5f);

          // fix crash: indexing with out-of-bounds index, this might happen near the edges of image
          // clip values so they fit into the image
          const MatSize& size = Lx.size;
          y1 = min(max(0, y1), size[0] - 1);
          x1 = min(max(0, x1), size[1] - 1);
          y2 = min(max(0, y2), size[0] - 1);
          x2 = min(max(0, x2), size[1] - 1);
          CV_DbgAssert(Lx.size == Ly.size);

          fx = sample_x - x1;
          fy = sample_y - y1;

          res1 = *(Lx.ptr<float>(y1, x1));
          res2 = *(Lx.ptr<float>(y1, x2));
          res3 = *(Lx.ptr<float>(y2, x1));
          res4 = *(Lx.ptr<float>(y2, x2));
          rx = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          res1 = *(Ly.ptr<float>(y1, x1));
          res2 = *(Ly.ptr<float>(y1, x2));
          res3 = *(Ly.ptr<float>(y2, x1));
          res4 = *(Ly.ptr<float>(y2, x2));
          ry = (1.0f - fx)*(1.0f - fy)*res1 + fx*(1.0f - fy)*res2 + (1.0f - fx)*fy*res3 + fx*fy*res4;

          // Get the x and y derivatives on the rotated axis
          rry = gauss_s1*(rx*co + ry*si);
          rrx = gauss_s1*(-rx*si + ry*co);

          // Sum the derivatives to the cumulative descriptor
          dx += rrx;
          dy += rry;
          mdx += fabs(rrx);
          mdy += fabs(rry);
        }
      }

      // Add the values to the descriptor vector
      gauss_s2 = gaussian(cx - 2.0f, cy - 2.0f, 1.5f);
      desc[dcount++] = dx*gauss_s2;
      desc[dcount++] = dy*gauss_s2;
      desc[dcount++] = mdx*gauss_s2;
      desc[dcount++] = mdy*gauss_s2;

      len += (dx*dx + dy*dy + mdx*mdx + mdy*mdy)*gauss_s2*gauss_s2;

      j += 9;
    }

    i += 9;
  }

  // convert to unit vector
  len = sqrt(len);

  for (i = 0; i < dsize; i++) {
    desc[i] /= len;
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the rupright descriptor (not rotation invariant) of
 * the provided keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void Upright_MLDB_Full_Descriptor_Invoker::Get_Upright_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char *desc) const {

  float di = 0.0, dx = 0.0, dy = 0.0;
  float ri = 0.0, rx = 0.0, ry = 0.0, xf = 0.0, yf = 0.0;
  float sample_x = 0.0, sample_y = 0.0, ratio = 0.0;
  int x1 = 0, y1 = 0;
  int nsamples = 0, scale = 0;
  int dcount1 = 0, dcount2 = 0;

  const AKAZEOptions & options = *options_;
  const std::vector<Evolution>& evolution = *evolution_;

  // Matrices for the M-LDB descriptor
  Mat values[3] = {
    Mat(4, options.descriptor_channels, CV_32FC1),
    Mat(9, options.descriptor_channels, CV_32FC1),
    Mat(16, options.descriptor_channels, CV_32FC1)
  };

  // Get the information from the keypoint
  ratio = (float)(1 << kpt.octave);
  scale = fRound(0.5f*kpt.size / ratio);
  const int level = kpt.class_id;
  Mat Lx = evolution[level].Mx;
  Mat Ly = evolution[level].My;
  Mat Lt = evolution[level].Mt;
  yf = kpt.pt.y / ratio;
  xf = kpt.pt.x / ratio;

  // For 2x2 grid, 3x3 grid and 4x4 grid
  const int pattern_size = options_->descriptor_pattern_size;
  int sample_step[3] = {
    pattern_size,
    static_cast<int>(ceil(pattern_size*2./3.)),
    pattern_size / 2
  };

  // For the three grids
  for (int z = 0; z < 3; z++) {
    dcount2 = 0;
    const int step = sample_step[z];
    for (int i = -pattern_size; i < pattern_size; i += step) {
      for (int j = -pattern_size; j < pattern_size; j += step) {
        di = dx = dy = 0.0;
        nsamples = 0;

        for (int k = i; k < i + step; k++) {
          for (int l = j; l < j + step; l++) {

            // Get the coordinates of the sample point
            sample_y = yf + l*scale;
            sample_x = xf + k*scale;

            y1 = fRound(sample_y);
            x1 = fRound(sample_x);

            ri = *(Lt.ptr<float>(y1)+x1);
            rx = *(Lx.ptr<float>(y1)+x1);
            ry = *(Ly.ptr<float>(y1)+x1);

            di += ri;
            dx += rx;
            dy += ry;
            nsamples++;
          }
        }

        di /= nsamples;
        dx /= nsamples;
        dy /= nsamples;

        float *val = values[z].ptr<float>(dcount2);
        *(val) = di;
        *(val+1) = dx;
        *(val+2) = dy;
        dcount2++;
      }
    }

    // Do binary comparison
    const int num = (z + 2) * (z + 2);
    for (int i = 0; i < num; i++) {
      for (int j = i + 1; j < num; j++) {
        const float * valI = values[z].ptr<float>(i);
        const float * valJ = values[z].ptr<float>(j);
        for (int k = 0; k < 3; ++k) {
          if (*(valI + k) > *(valJ + k)) {
            desc[dcount1 / 8] |= (1 << (dcount1 % 8));
          } else {
            desc[dcount1 / 8] &= ~(1 << (dcount1 % 8));
          }
          dcount1++;
        }
      }
    }

  } // for (int z = 0; z < 3; z++)
}

void MLDB_Full_Descriptor_Invoker::MLDB_Fill_Values(float* values, int sample_step, const int level,
                                                    float xf, float yf, float co, float si, float scale) const
{
    const std::vector<Evolution>& evolution = *evolution_;
    int pattern_size = options_->descriptor_pattern_size;
    int chan = options_->descriptor_channels;
    int valpos = 0;
    Mat Lx = evolution[level].Mx;
    Mat Ly = evolution[level].My;
    Mat Lt = evolution[level].Mt;

    for (int i = -pattern_size; i < pattern_size; i += sample_step) {
        for (int j = -pattern_size; j < pattern_size; j += sample_step) {
            float di, dx, dy;
            di = dx = dy = 0.0;
            int nsamples = 0;

            for (int k = i; k < i + sample_step; k++) {
              for (int l = j; l < j + sample_step; l++) {
                float sample_y = yf + (l*co * scale + k*si*scale);
                float sample_x = xf + (-l*si * scale + k*co*scale);

                int y1 = fRound(sample_y);
                int x1 = fRound(sample_x);

                // fix crash: indexing with out-of-bounds index, this might happen near the edges of image
                // clip values so they fit into the image
                const MatSize& size = Lt.size;
                CV_DbgAssert(size == Lx.size &&
                             size == Ly.size);
                y1 = min(max(0, y1), size[0] - 1);
                x1 = min(max(0, x1), size[1] - 1);

                float ri = *(Lt.ptr<float>(y1, x1));
                di += ri;

                if(chan > 1) {
                    float rx = *(Lx.ptr<float>(y1, x1));
                    float ry = *(Ly.ptr<float>(y1, x1));
                    if (chan == 2) {
                      dx += sqrtf(rx*rx + ry*ry);
                    }
                    else {
                      float rry = rx*co + ry*si;
                      float rrx = -rx*si + ry*co;
                      dx += rrx;
                      dy += rry;
                    }
                }
                nsamples++;
              }
            }
            di /= nsamples;
            dx /= nsamples;
            dy /= nsamples;

            values[valpos] = di;
            if (chan > 1) {
                values[valpos + 1] = dx;
            }
            if (chan > 2) {
              values[valpos + 2] = dy;
            }
            valpos += chan;
          }
        }
}

void MLDB_Full_Descriptor_Invoker::MLDB_Binary_Comparisons(float* values, unsigned char* desc,
                                                           int count, int& dpos) const {
    int chan = options_->descriptor_channels;
    int* ivalues = (int*) values;
    for(int i = 0; i < count * chan; i++) {
        ivalues[i] = CV_TOGGLE_FLT(ivalues[i]);
    }

    for(int pos = 0; pos < chan; pos++) {
        for (int i = 0; i < count; i++) {
            int ival = ivalues[chan * i + pos];
            for (int j = i + 1; j < count; j++) {
                if (ival > ivalues[chan * j + pos]) {
                  desc[dpos >> 3] |= (1 << (dpos & 7));
                }
                else {
                  desc[dpos >> 3] &= ~(1 << (dpos & 7));
                }

                dpos++;
            }
        }
    }
}

/* ************************************************************************* */
/**
 * @brief This method computes the descriptor of the provided keypoint given the
 * main orientation of the keypoint
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void MLDB_Full_Descriptor_Invoker::Get_MLDB_Full_Descriptor(const KeyPoint& kpt, unsigned char *desc) const {

  const int max_channels = 3;
  CV_Assert(options_->descriptor_channels <= max_channels);
  float values[16*max_channels];
  const double size_mult[3] = {1, 2.0/3.0, 1.0/2.0};

  float ratio = (float)(1 << kpt.octave);
  float scale = (float)fRound(0.5f*kpt.size / ratio);
  float xf = kpt.pt.x / ratio;
  float yf = kpt.pt.y / ratio;
  float angle = (kpt.angle * static_cast<float>(CV_PI)) / 180.f;
  float co = cos(angle);
  float si = sin(angle);
  int pattern_size = options_->descriptor_pattern_size;

  int dpos = 0;
  for(int lvl = 0; lvl < 3; lvl++) {

      int val_count = (lvl + 2) * (lvl + 2);
      int sample_step = static_cast<int>(ceil(pattern_size * size_mult[lvl]));
      MLDB_Fill_Values(values, sample_step, kpt.class_id, xf, yf, co, si, scale);
      MLDB_Binary_Comparisons(values, desc, val_count, dpos);
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the M-LDB descriptor of the provided keypoint given the
 * main orientation of the keypoint. The descriptor is computed based on a subset of
 * the bits of the whole descriptor
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void MLDB_Descriptor_Subset_Invoker::Get_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char *desc) const {

  float di = 0.f, dx = 0.f, dy = 0.f;
  float rx = 0.f, ry = 0.f;
  float sample_x = 0.f, sample_y = 0.f;
  int x1 = 0, y1 = 0;

  const AKAZEOptions & options = *options_;
  const std::vector<Evolution>& evolution = *evolution_;

  // Get the information from the keypoint
  float ratio = (float)(1 << kpt.octave);
  int scale = fRound(0.5f*kpt.size / ratio);
  float angle = (kpt.angle * static_cast<float>(CV_PI)) / 180.f;
  const int level = kpt.class_id;
  Mat Lx = evolution[level].Mx;
  Mat Ly = evolution[level].My;
  Mat Lt = evolution[level].Mt;
  float yf = kpt.pt.y / ratio;
  float xf = kpt.pt.x / ratio;
  float co = cos(angle);
  float si = sin(angle);

  // Allocate memory for the matrix of values
  Mat values((4 + 9 + 16)*options.descriptor_channels, 1, CV_32FC1);

  // Sample everything, but only do the comparisons
  vector<int> steps(3);
  steps.at(0) = options.descriptor_pattern_size;
  steps.at(1) = (int)ceil(2.f*options.descriptor_pattern_size / 3.f);
  steps.at(2) = options.descriptor_pattern_size / 2;

  for (int i = 0; i < descriptorSamples_.rows; i++) {
    const int *coords = descriptorSamples_.ptr<int>(i);
    int sample_step = steps.at(coords[0]);
    di = 0.0f;
    dx = 0.0f;
    dy = 0.0f;

    for (int k = coords[1]; k < coords[1] + sample_step; k++) {
      for (int l = coords[2]; l < coords[2] + sample_step; l++) {

        // Get the coordinates of the sample point
        sample_y = yf + (l*scale*co + k*scale*si);
        sample_x = xf + (-l*scale*si + k*scale*co);

        y1 = fRound(sample_y);
        x1 = fRound(sample_x);

        di += *(Lt.ptr<float>(y1)+x1);

        if (options.descriptor_channels > 1) {
          rx = *(Lx.ptr<float>(y1)+x1);
          ry = *(Ly.ptr<float>(y1)+x1);

          if (options.descriptor_channels == 2) {
            dx += sqrtf(rx*rx + ry*ry);
          }
          else if (options.descriptor_channels == 3) {
            // Get the x and y derivatives on the rotated axis
            dx += rx*co + ry*si;
            dy += -rx*si + ry*co;
          }
        }
      }
    }

    *(values.ptr<float>(options.descriptor_channels*i)) = di;

    if (options.descriptor_channels == 2) {
      *(values.ptr<float>(options.descriptor_channels*i + 1)) = dx;
    }
    else if (options.descriptor_channels == 3) {
      *(values.ptr<float>(options.descriptor_channels*i + 1)) = dx;
      *(values.ptr<float>(options.descriptor_channels*i + 2)) = dy;
    }
  }

  // Do the comparisons
  const float *vals = values.ptr<float>(0);
  const int *comps = descriptorBits_.ptr<int>(0);

  for (int i = 0; i<descriptorBits_.rows; i++) {
    if (vals[comps[2 * i]] > vals[comps[2 * i + 1]]) {
      desc[i / 8] |= (1 << (i % 8));
    } else {
      desc[i / 8] &= ~(1 << (i % 8));
    }
  }
}

/* ************************************************************************* */
/**
 * @brief This method computes the upright (not rotation invariant) M-LDB descriptor
 * of the provided keypoint given the main orientation of the keypoint.
 * The descriptor is computed based on a subset of the bits of the whole descriptor
 * @param kpt Input keypoint
 * @param desc Descriptor vector
 */
void Upright_MLDB_Descriptor_Subset_Invoker::Get_Upright_MLDB_Descriptor_Subset(const KeyPoint& kpt, unsigned char *desc) const {

  float di = 0.0f, dx = 0.0f, dy = 0.0f;
  float rx = 0.0f, ry = 0.0f;
  float sample_x = 0.0f, sample_y = 0.0f;
  int x1 = 0, y1 = 0;

  const AKAZEOptions & options = *options_;
  const std::vector<Evolution>& evolution = *evolution_;

  // Get the information from the keypoint
  float ratio = (float)(1 << kpt.octave);
  int scale = fRound(0.5f*kpt.size / ratio);
  const int level = kpt.class_id;
  Mat Lx = evolution[level].Mx;
  Mat Ly = evolution[level].My;
  Mat Lt = evolution[level].Mt;
  float yf = kpt.pt.y / ratio;
  float xf = kpt.pt.x / ratio;

  // Allocate memory for the matrix of values
  Mat values ((4 + 9 + 16)*options.descriptor_channels, 1, CV_32FC1);

  vector<int> steps(3);
  steps.at(0) = options.descriptor_pattern_size;
  steps.at(1) = static_cast<int>(ceil(2.f*options.descriptor_pattern_size / 3.f));
  steps.at(2) = options.descriptor_pattern_size / 2;

  for (int i = 0; i < descriptorSamples_.rows; i++) {
    const int *coords = descriptorSamples_.ptr<int>(i);
    int sample_step = steps.at(coords[0]);
    di = 0.0f, dx = 0.0f, dy = 0.0f;

    for (int k = coords[1]; k < coords[1] + sample_step; k++) {
      for (int l = coords[2]; l < coords[2] + sample_step; l++) {

        // Get the coordinates of the sample point
        sample_y = yf + l*scale;
        sample_x = xf + k*scale;

        y1 = fRound(sample_y);
        x1 = fRound(sample_x);
        di += *(Lt.ptr<float>(y1)+x1);

        if (options.descriptor_channels > 1) {
          rx = *(Lx.ptr<float>(y1)+x1);
          ry = *(Ly.ptr<float>(y1)+x1);

          if (options.descriptor_channels == 2) {
            dx += sqrtf(rx*rx + ry*ry);
          }
          else if (options.descriptor_channels == 3) {
            dx += rx;
            dy += ry;
          }
        }
      }
    }

    *(values.ptr<float>(options.descriptor_channels*i)) = di;

    if (options.descriptor_channels == 2) {
      *(values.ptr<float>(options.descriptor_channels*i + 1)) = dx;
    }
    else if (options.descriptor_channels == 3) {
      *(values.ptr<float>(options.descriptor_channels*i + 1)) = dx;
      *(values.ptr<float>(options.descriptor_channels*i + 2)) = dy;
    }
  }

  // Do the comparisons
  const float *vals = values.ptr<float>(0);
  const int *comps = descriptorBits_.ptr<int>(0);

  for (int i = 0; i<descriptorBits_.rows; i++) {
    if (vals[comps[2 * i]] > vals[comps[2 * i + 1]]) {
      desc[i / 8] |= (1 << (i % 8));
    } else {
      desc[i / 8] &= ~(1 << (i % 8));
    }
  }
}

/* ************************************************************************* */
/**
 * @brief This function computes a (quasi-random) list of bits to be taken
 * from the full descriptor. To speed the extraction, the function creates
 * a list of the samples that are involved in generating at least a bit (sampleList)
 * and a list of the comparisons between those samples (comparisons)
 * @param sampleList
 * @param comparisons The matrix with the binary comparisons
 * @param nbits The number of bits of the descriptor
 * @param pattern_size The pattern size for the binary descriptor
 * @param nchannels Number of channels to consider in the descriptor (1-3)
 * @note The function keeps the 18 bits (3-channels by 6 comparisons) of the
 * coarser grid, since it provides the most robust estimations
 */
void generateDescriptorSubsample(Mat& sampleList, Mat& comparisons, int nbits,
                                 int pattern_size, int nchannels) {

  int ssz = 0;
  for (int i = 0; i < 3; i++) {
    int gz = (i + 2)*(i + 2);
    ssz += gz*(gz - 1) / 2;
  }
  ssz *= nchannels;

  CV_Assert(nbits <= ssz); // Descriptor size can't be bigger than full descriptor

  // Since the full descriptor is usually under 10k elements, we pick
  // the selection from the full matrix.  We take as many samples per
  // pick as the number of channels. For every pick, we
  // take the two samples involved and put them in the sampling list

  Mat_<int> fullM(ssz / nchannels, 5);
  for (int i = 0, c = 0; i < 3; i++) {
    int gdiv = i + 2; //grid divisions, per row
    int gsz = gdiv*gdiv;
    int psz = (int)ceil(2.f*pattern_size / (float)gdiv);

    for (int j = 0; j < gsz; j++) {
      for (int k = j + 1; k < gsz; k++, c++) {
        fullM(c, 0) = i;
        fullM(c, 1) = psz*(j % gdiv) - pattern_size;
        fullM(c, 2) = psz*(j / gdiv) - pattern_size;
        fullM(c, 3) = psz*(k % gdiv) - pattern_size;
        fullM(c, 4) = psz*(k / gdiv) - pattern_size;
      }
    }
  }

  srand(1024);
  Mat_<int> comps = Mat_<int>(nchannels * (int)ceil(nbits / (float)nchannels), 2);
  comps = 1000;

  // Select some samples. A sample includes all channels
  int count = 0;
  int npicks = (int)ceil(nbits / (float)nchannels);
  Mat_<int> samples(29, 3);
  Mat_<int> fullcopy = fullM.clone();
  samples = -1;

  for (int i = 0; i < npicks; i++) {
    int k = rand() % (fullM.rows - i);
    if (i < 6) {
      // Force use of the coarser grid values and comparisons
      k = i;
    }

    bool n = true;

    for (int j = 0; j < count; j++) {
      if (samples(j, 0) == fullcopy(k, 0) && samples(j, 1) == fullcopy(k, 1) && samples(j, 2) == fullcopy(k, 2)) {
        n = false;
        comps(i*nchannels, 0) = nchannels*j;
        comps(i*nchannels + 1, 0) = nchannels*j + 1;
        comps(i*nchannels + 2, 0) = nchannels*j + 2;
        break;
      }
    }

    if (n) {
      samples(count, 0) = fullcopy(k, 0);
      samples(count, 1) = fullcopy(k, 1);
      samples(count, 2) = fullcopy(k, 2);
      comps(i*nchannels, 0) = nchannels*count;
      comps(i*nchannels + 1, 0) = nchannels*count + 1;
      comps(i*nchannels + 2, 0) = nchannels*count + 2;
      count++;
    }

    n = true;
    for (int j = 0; j < count; j++) {
      if (samples(j, 0) == fullcopy(k, 0) && samples(j, 1) == fullcopy(k, 3) && samples(j, 2) == fullcopy(k, 4)) {
        n = false;
        comps(i*nchannels, 1) = nchannels*j;
        comps(i*nchannels + 1, 1) = nchannels*j + 1;
        comps(i*nchannels + 2, 1) = nchannels*j + 2;
        break;
      }
    }

    if (n) {
      samples(count, 0) = fullcopy(k, 0);
      samples(count, 1) = fullcopy(k, 3);
      samples(count, 2) = fullcopy(k, 4);
      comps(i*nchannels, 1) = nchannels*count;
      comps(i*nchannels + 1, 1) = nchannels*count + 1;
      comps(i*nchannels + 2, 1) = nchannels*count + 2;
      count++;
    }

    Mat tmp = fullcopy.row(k);
    fullcopy.row(fullcopy.rows - i - 1).copyTo(tmp);
  }

  sampleList = samples.rowRange(0, count).clone();
  comparisons = comps.rowRange(0, nbits).clone();
}

}
