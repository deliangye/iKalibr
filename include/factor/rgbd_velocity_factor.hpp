// iKalibr: Unified Targetless Spatiotemporal Calibration Framework
// Copyright 2024, the School of Geodesy and Geomatics (SGG), Wuhan University, China
// https://github.com/Unsigned-Long/iKalibr.git
//
// Author: Shuolong Chen (shlchen@whu.edu.cn)
// GitHub: https://github.com/Unsigned-Long
//  ORCID: 0000-0002-5283-9057
//
// Purpose: See .h/.hpp file.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice,
//   this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
// * The names of its contributors can not be
//   used to endorse or promote products derived from this software without
//   specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef IKALIBR_RGBD_VELOCITY_FACTOR_HPP
#define IKALIBR_RGBD_VELOCITY_FACTOR_HPP

#include "ctraj/utils/eigen_utils.hpp"
#include "ctraj/utils/sophus_utils.hpp"
#include "ctraj/spline/spline_segment.h"
#include "ctraj/spline/ceres_spline_helper.h"
#include "ctraj/spline/ceres_spline_helper_jet.h"
#include "ceres/dynamic_autodiff_cost_function.h"
#include "util/utils.h"
#include "util/utils_tpl.hpp"
#include "sensor/camera.h"
#include "config/configor.h"

namespace {
bool IKALIBR_UNIQUE_NAME(_2_) = ns_ikalibr::_1_(__FILE__);
}

namespace ns_ikalibr {

struct OpticalFlowCorr {
public:
    using Ptr = std::shared_ptr<OpticalFlowCorr>;
    static constexpr int MID = 1;

public:
    std::array<double, 3> timeAry;
    std::array<double, 3> xTraceAry;
    std::array<double, 3> yTraceAry;
    // row / image height - rsExpFactor
    std::array<double, 3> rdFactorAry;
    double depth;
    double invDepth;
    CameraFrame::Ptr frame;
    // if this dynamic is with depth observability
    bool withDepthObservability;
    double weight = 1.0;

public:
    OpticalFlowCorr(const std::array<double, 3> &timeAry,
                    const std::array<double, 3> &xTraceAry,
                    const std::array<double, 3> &yTraceAry,
                    double depth,
                    const CameraFrame::Ptr &frame,
                    double rsExpFactor)
        : timeAry(timeAry),
          xTraceAry(xTraceAry),
          yTraceAry(yTraceAry),
          rdFactorAry(),
          depth(depth),
          invDepth(depth > 1E-3 ? 1.0 / depth : -1.0),
          frame(frame),
          withDepthObservability(false) {
        int imgHeight = frame->GetImage().rows;
        for (int i = 0; i < 3; ++i) {
            rdFactorAry[i] = yTraceAry[i] / (double)imgHeight - rsExpFactor;
        }
    }

    static Ptr Create(const std::array<double, 3> &timeAry,
                      const std::array<double, 3> &xDynamicAry,
                      const std::array<double, 3> &yDynamicAry,
                      double depth,
                      const CameraFrame::Ptr &frame,
                      double rsExpFactor) {
        return std::make_shared<OpticalFlowCorr>(timeAry, xDynamicAry, yDynamicAry, depth, frame,
                                                 rsExpFactor);
    }

    [[nodiscard]] Eigen::Vector2d MidPoint() const {
        return {xTraceAry.at(MID), yTraceAry.at(MID)};
    }

    template <class Type>
    [[nodiscard]] Type MidPointTime(Type readout) const {
        return timeAry[MID] + rdFactorAry[MID] * readout;
    }

    [[nodiscard]] double MidReadoutFactor() const { return rdFactorAry[MID]; }

    template <class Type>
    [[nodiscard]] Eigen::Vector2<Type> MidPointVel(Type readout) const {
        if constexpr (std::is_same<Type, double>::value) {
            std::array<Type, 3> newTimeAry{};
            for (int i = 0; i < 3; ++i) {
                newTimeAry[i] = timeAry[i] + rdFactorAry[i] * readout;
            }
            return {ns_ikalibr::LagrangePolynomialTripleMidFOD<Type>(newTimeAry, xTraceAry),
                    ns_ikalibr::LagrangePolynomialTripleMidFOD<Type>(newTimeAry, yTraceAry)};
        } else {
            std::array<Type, 3> newTimeAry{};
            std::array<Type, 3> newXAry{};
            std::array<Type, 3> newYAry{};
            for (int i = 0; i < 3; ++i) {
                newTimeAry[i] = timeAry[i] + rdFactorAry[i] * readout;
                newXAry[i] = (Type)xTraceAry[i];
                newYAry[i] = (Type)yTraceAry[i];
            }
            return {ns_ikalibr::LagrangePolynomialTripleMidFOD<Type>(newTimeAry, newXAry),
                    ns_ikalibr::LagrangePolynomialTripleMidFOD<Type>(newTimeAry, newYAry)};
        }
    }
};

template <int Order, int TimeDeriv, bool IsInvDepth = true>
struct RGBDVelocityFactor {
private:
    ns_ctraj::SplineMeta<Order> _so3Meta, _scaleMeta;

    OpticalFlowCorr::Ptr _corr;

    double _so3DtInv, _scaleDtInv;
    double _weight;

public:
    explicit RGBDVelocityFactor(const ns_ctraj::SplineMeta<Order> &so3Meta,
                                const ns_ctraj::SplineMeta<Order> &scaleMeta,
                                OpticalFlowCorr::Ptr corr,
                                double weight)
        : _so3Meta(so3Meta),
          _scaleMeta(scaleMeta),
          _corr(std::move(corr)),
          _so3DtInv(1.0 / _so3Meta.segments.front().dt),
          _scaleDtInv(1.0 / _scaleMeta.segments.front().dt),
          _weight(weight) {}

    static auto Create(const ns_ctraj::SplineMeta<Order> &so3Meta,
                       const ns_ctraj::SplineMeta<Order> &scaleMeta,
                       const OpticalFlowCorr::Ptr &corr,
                       double weight) {
        return new ceres::DynamicAutoDiffCostFunction<RGBDVelocityFactor>(
            new RGBDVelocityFactor(so3Meta, scaleMeta, corr, weight));
    }

    static std::size_t TypeHashCode() { return typeid(RGBDVelocityFactor).hash_code(); }

public:
    template <class T>
    static void SubAMat(
        const T *fx, const T *fy, const T &up, const T &vp, Eigen::Matrix<T, 2, 3> *aMat) {
        *aMat = Eigen::Matrix<T, 2, 3>::Zero();
        (*aMat)(0, 0) = -*fx;
        (*aMat)(1, 1) = -*fy;
        (*aMat)(0, 2) = up;
        (*aMat)(1, 2) = vp;
    }

    template <class T>
    static void SubBMat(
        const T *fx, const T *fy, const T &up, const T &vp, Eigen::Matrix<T, 2, 3> *bMat) {
        *bMat = Eigen::Matrix<T, 2, 3>::Zero();
        (*bMat)(0, 0) = up * vp / *fy;
        (*bMat)(0, 1) = -*fx - up * up / *fx;
        (*bMat)(0, 2) = *fx * vp / *fy;
        (*bMat)(1, 0) = *fy + vp * vp / *fy;
        (*bMat)(1, 1) = -up * vp / *fx;
        (*bMat)(1, 2) = -*fy * up / *fx;
    }

    template <class T>
    static void SubMats(const T *fx,
                        const T *fy,
                        const T *cx,
                        const T *cy,
                        const Eigen::Vector2<T> feat,
                        Eigen::Matrix<T, 2, 3> *aMat,
                        Eigen::Matrix<T, 2, 3> *bMat) {
        const T up = feat(0) - *cx, vp = feat(1) - *cy;
        SubAMat(fx, fy, up, vp, aMat);
        SubBMat(fx, fy, up, vp, bMat);
    }

    /**
     * param blocks:
     * [ SO3 | ... | SO3 | LIN_SCALE | ... | LIN_SCALE | SO3_DnToBr | POS_DnInBr | TO_DnToBr |
     *   READOUT_TIME | FX | FY | CX | CY | ALPHA | BETA | DEPTH_INFO ]
     */
    template <class T>
    bool operator()(T const *const *sKnots, T *sResiduals) const {
        std::size_t SO3_OFFSET, LIN_SCALE_OFFSET;
        std::size_t SO3_DnToBr_OFFSET = _so3Meta.NumParameters() + _scaleMeta.NumParameters();
        std::size_t POS_DnInBr_OFFSET = SO3_DnToBr_OFFSET + 1;
        std::size_t TO_DnToBr_OFFSET = POS_DnInBr_OFFSET + 1;
        std::size_t READOUT_TIME_OFFSET = TO_DnToBr_OFFSET + 1;
        std::size_t FX_OFFSET = READOUT_TIME_OFFSET + 1;
        std::size_t FY_OFFSET = FX_OFFSET + 1;
        std::size_t CX_OFFSET = FY_OFFSET + 1;
        std::size_t CY_OFFSET = CX_OFFSET + 1;
        std::size_t ALPHA_OFFSET = CY_OFFSET + 1;
        std::size_t BETA_OFFSET = ALPHA_OFFSET + 1;
        std::size_t DEPTH_INFO_OFFSET = BETA_OFFSET + 1;

        // get value
        Eigen::Map<const Sophus::SO3<T>> SO3_DnToBr(sKnots[SO3_DnToBr_OFFSET]);
        Eigen::Map<const Eigen::Vector3<T>> POS_DnInBr(sKnots[POS_DnInBr_OFFSET]);
        Sophus::SO3<T> SO3_BrToDn = SO3_DnToBr.inverse();

        T TO_DnToBr = sKnots[TO_DnToBr_OFFSET][0];
        T READOUT_TIME = sKnots[READOUT_TIME_OFFSET][0];

        T FX = sKnots[FX_OFFSET][0];
        T FY = sKnots[FY_OFFSET][0];
        T CX = sKnots[CX_OFFSET][0];
        T CY = sKnots[CY_OFFSET][0];

        T ALPHA = sKnots[ALPHA_OFFSET][0];
        T BETA = sKnots[BETA_OFFSET][0];
        T DEPTH_INFO = sKnots[DEPTH_INFO_OFFSET][0];

        auto timeByBr = _corr->MidPointTime(READOUT_TIME) + TO_DnToBr;

        // calculate the so3 and pos offset
        std::pair<std::size_t, T> iuSo3, iuScale;
        _so3Meta.ComputeSplineIndex(timeByBr, iuSo3.first, iuSo3.second);
        _scaleMeta.ComputeSplineIndex(timeByBr, iuScale.first, iuScale.second);

        SO3_OFFSET = iuSo3.first;
        LIN_SCALE_OFFSET = iuScale.first + _so3Meta.NumParameters();

        // query
        Sophus::SO3<T> SO3_BrToBr0;
        Eigen::Vector3<T> ANG_VEL_BrToBr0InBr;
        ns_ctraj::CeresSplineHelperJet<T, Order>::EvaluateLie(
            sKnots + SO3_OFFSET, iuSo3.second, _so3DtInv, &SO3_BrToBr0, &ANG_VEL_BrToBr0InBr);

        Eigen::Vector3<T> ANG_VEL_BrToBr0InBr0 = SO3_BrToBr0 * ANG_VEL_BrToBr0InBr;
        Eigen::Vector3<T> ANG_VEL_DnToBr0InDn = SO3_BrToDn * ANG_VEL_BrToBr0InBr;

        Eigen::Vector3<T> LIN_VEL_BrToBr0InBr0;
        ns_ctraj::CeresSplineHelperJet<T, Order>::template Evaluate<3, TimeDeriv>(
            sKnots + LIN_SCALE_OFFSET, iuScale.second, _scaleDtInv, &LIN_VEL_BrToBr0InBr0);

        Eigen::Vector3<T> LIN_VEL_DnToBr0InBr0 =
            -Sophus::SO3<T>::hat(SO3_BrToBr0 * POS_DnInBr) * ANG_VEL_BrToBr0InBr0 +
            LIN_VEL_BrToBr0InBr0;

        Eigen::Vector3<T> LIN_VEL_DnToBr0InDn =
            SO3_BrToDn * SO3_BrToBr0.inverse() * LIN_VEL_DnToBr0InBr0;

        Eigen::Matrix<T, 2, 3> subAMat, subBMat;
        SubMats<T>(&FX, &FY, &CX, &CY, _corr->MidPoint().cast<T>(), &subAMat, &subBMat);

        Eigen::Vector2<T> pred;
        if constexpr (IsInvDepth) {
            // inverse depth
            pred = DEPTH_INFO / (ALPHA + BETA * DEPTH_INFO) * subAMat * LIN_VEL_DnToBr0InDn +
                   subBMat * ANG_VEL_DnToBr0InDn;
        } else {
            // depth
            pred = 1.0 / (ALPHA * DEPTH_INFO + BETA) * subAMat * LIN_VEL_DnToBr0InDn +
                   subBMat * ANG_VEL_DnToBr0InDn;
        }

        Eigen::Map<Eigen::Vector2<T>> residuals(sResiduals);
        residuals = T(_weight) * (pred - _corr->MidPointVel(READOUT_TIME));

        return true;
    }
};

extern template struct RGBDVelocityFactor<Configor::Prior::SplineOrder, 2, true>;
extern template struct RGBDVelocityFactor<Configor::Prior::SplineOrder, 2, false>;
extern template struct RGBDVelocityFactor<Configor::Prior::SplineOrder, 1, true>;
extern template struct RGBDVelocityFactor<Configor::Prior::SplineOrder, 1, false>;
extern template struct RGBDVelocityFactor<Configor::Prior::SplineOrder, 0, true>;
extern template struct RGBDVelocityFactor<Configor::Prior::SplineOrder, 0, false>;
}  // namespace ns_ikalibr

#endif  // IKALIBR_RGBD_VELOCITY_FACTOR_HPP
