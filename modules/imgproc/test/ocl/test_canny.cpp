/*M///////////////////////////////////////////////////////////////////////////////////////
//
//  IMPORTANT: READ BEFORE DOWNLOADING, COPYING, INSTALLING OR USING.
//
//  By downloading, copying, installing or using the software you agree to this license.
//  If you do not agree to this license, do not download, install,
//  copy or use the software.
//
//
//                           License Agreement
//                For Open Source Computer Vision Library
//
// Copyright (C) 2010-2012, Multicoreware, Inc., all rights reserved.
// Copyright (C) 2010-2012, Advanced Micro Devices, Inc., all rights reserved.
// Third party copyrights are property of their respective owners.
//
// @Authors
//    Peng Xiao, pengxiao@multicorewareinc.com
//
// Redistribution and use in source and binary forms, with or without modification,
// are permitted provided that the following conditions are met:
//
//   * Redistribution's of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistribution's in binary form must reproduce the above copyright notice,
//     this list of conditions and the following disclaimer in the documentation
//     and/or other materials provided with the distribution.
//
//   * The name of the copyright holders may not be used to endorse or promote products
//     derived from this software without specific prior written permission.
//
// This software is provided by the copyright holders and contributors as is and
// any express or implied warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose are disclaimed.
// In no event shall the Intel Corporation or contributors be liable for any direct,
// indirect, incidental, special, exemplary, or consequential damages
// (including, but not limited to, procurement of substitute goods or services;
// loss of use, data, or profits; or business interruption) however caused
// and on any theory of liability, whether in contract, strict liability,
// or tort (including negligence or otherwise) arising in any way out of
// the use of this software, even if advised of the possibility of such damage.
//
//M*/

#include "test_precomp.hpp"
#include "opencv2/ts/ocl_test.hpp"

#ifdef HAVE_OPENCL

namespace cvtest {
namespace ocl {

////////////////////////////////////////////////////////
// Canny

IMPLEMENT_PARAM_CLASS(AppertureSize, int)
IMPLEMENT_PARAM_CLASS(L2gradient, bool)
IMPLEMENT_PARAM_CLASS(UseRoi, bool)

PARAM_TEST_CASE(Canny, AppertureSize, L2gradient, UseRoi)
{
    int apperture_size;
    bool useL2gradient, use_roi;

    TEST_DECLARE_INPUT_PARAMETER(src)
    TEST_DECLARE_OUTPUT_PARAMETER(dst)

    virtual void SetUp()
    {
        apperture_size = GET_PARAM(0);
        useL2gradient = GET_PARAM(1);
        use_roi = GET_PARAM(2);
    }

    void generateTestData()
    {
        Mat img = readImage("shared/fruits.png", IMREAD_GRAYSCALE);
        ASSERT_FALSE(img.empty()) << "cann't load shared/fruits.png";

        Size roiSize = img.size();
        int type = img.type();
        ASSERT_EQ(CV_8UC1, type);

        Border srcBorder = randomBorder(0, use_roi ? MAX_VALUE : 0);
        randomSubMat(src, src_roi, roiSize, srcBorder, type, 2, 100);
        img.copyTo(src_roi);

        Border dstBorder = randomBorder(0, use_roi ? MAX_VALUE : 0);
        randomSubMat(dst, dst_roi, roiSize, dstBorder, type, 5, 16);

        UMAT_UPLOAD_INPUT_PARAMETER(src)
        UMAT_UPLOAD_OUTPUT_PARAMETER(dst)
    }
};

OCL_TEST_P(Canny, Accuracy)
{
    generateTestData();

    const double low_thresh = 50.0, high_thresh = 100.0;

    OCL_OFF(cv::Canny(src_roi, dst_roi, low_thresh, high_thresh, apperture_size, useL2gradient));
    OCL_ON(cv::Canny(usrc_roi, udst_roi, low_thresh, high_thresh, apperture_size, useL2gradient));

    EXPECT_MAT_SIMILAR(dst_roi, udst_roi, 1e-2);
    EXPECT_MAT_SIMILAR(dst, udst, 1e-2);
}

OCL_INSTANTIATE_TEST_CASE_P(ImgProc, Canny, testing::Combine(
                                testing::Values(AppertureSize(3), AppertureSize(5)),
                                testing::Values(L2gradient(false), L2gradient(true)),
                                testing::Values(UseRoi(false), UseRoi(true))));

} } // namespace cvtest::ocl

#endif // HAVE_OPENCL
