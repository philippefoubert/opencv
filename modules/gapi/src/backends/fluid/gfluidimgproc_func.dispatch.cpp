// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2018 Intel Corporation

#if !defined(GAPI_STANDALONE)

#include "gfluidimgproc_func.hpp"
#include "gfluidimgproc_func.simd.hpp"
#include "backends/fluid/gfluidimgproc_func.simd_declarations.hpp"

#include "gfluidutils.hpp"

#include "opencv2/core/cvdef.h"
#include "opencv2/core/hal/intrin.hpp"

#include <cmath>
#include <cstdlib>

#ifdef __GNUC__
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wstrict-overflow"
#endif

namespace cv {
namespace gapi {
namespace fluid {

//----------------------------------
//
// Fluid kernels: RGB2Gray, BGR2Gray
//
//----------------------------------

void run_rgb2gray_impl(uchar out[], const uchar in[], int width,
                       float coef_r, float coef_g, float coef_b)
{
    CV_CPU_DISPATCH(run_rgb2gray_impl,
        (out, in, width, coef_r, coef_g, coef_b),
        CV_CPU_DISPATCH_MODES_ALL);
}

//--------------------------------------
//
// Fluid kernels: RGB-to-YUV, YUV-to-RGB
//
//--------------------------------------

void run_rgb2yuv_impl(uchar out[], const uchar in[], int width, const float coef[5])
{
    CV_CPU_DISPATCH(run_rgb2yuv_impl, (out, in, width, coef), CV_CPU_DISPATCH_MODES_ALL);
}

void run_yuv2rgb_impl(uchar out[], const uchar in[], int width, const float coef[4])
{
    CV_CPU_DISPATCH(run_yuv2rgb_impl, (out, in, width, coef), CV_CPU_DISPATCH_MODES_ALL);
}

//-------------------------
//
// Fluid kernels: sepFilter
//
//-------------------------

#define RUN_SEPFILTER3X3_IMPL(DST, SRC)                                     \
void run_sepfilter3x3_impl(DST out[], const SRC *in[], int width, int chan, \
                           const float kx[], const float ky[], int border,  \
                           float scale, float delta,                        \
                           float *buf[], int y, int y0)                     \
{                                                                           \
    CV_CPU_DISPATCH(run_sepfilter3x3_impl,                                  \
        (out, in, width, chan, kx, ky, border, scale, delta, buf,y, y0),    \
        CV_CPU_DISPATCH_MODES_ALL);                                         \
}

RUN_SEPFILTER3X3_IMPL(uchar , uchar )
RUN_SEPFILTER3X3_IMPL( short, uchar )
RUN_SEPFILTER3X3_IMPL( float, uchar )
RUN_SEPFILTER3X3_IMPL(ushort, ushort)
RUN_SEPFILTER3X3_IMPL( short, ushort)
RUN_SEPFILTER3X3_IMPL( float, ushort)
RUN_SEPFILTER3X3_IMPL( short,  short)
RUN_SEPFILTER3X3_IMPL( float,  short)
RUN_SEPFILTER3X3_IMPL( float,  float)

#undef RUN_SEPFILTER3X3_IMPL

} // namespace fliud
} // namespace gapi
} // namespace cv

#endif // !defined(GAPI_STANDALONE)
