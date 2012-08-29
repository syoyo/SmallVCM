/*
 * Copyright (C) 2012, Tomas Davidovic (http://www.davidovic.cz)
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * (The above is MIT License: http://en.wikipedia.org/wiki/MIT_License)
 */

#ifndef __BSDF_HXX__
#define __BSDF_HXX__

#include <vector>
#include <cmath>
#include "math.hxx"
#include "frame.hxx"
#include "ray.hxx"
#include "scene.hxx"
#include "utils.hxx"

//////////////////////////////////////////////////////////////////////////
// Bsdf, most magic happens here
//
// One of important conventions is prefixing direction with World when
// are in world coordinates and with Local when they are in local frame,
// i.e., mFrame.
//
// Another important convention if suffix Fix and Gen.
// For PDF computation, we need to know which direction is given (Fix),
// and which is the generated (Gen) direction. This is important even
// when simply evaluating Bsdf.
// In BPT, we call EvaluateBrdfPdf when directly connecting to light/camera.
// This gives us both directions required for evaluating Bsdf.
// However, for MIS we also need to know probabilities of having sampled
// this path via Bsdf sampling, and we need that for both possible directions.
// The Fix/Gen convention (along with Direct and Reverse for PDF) clearly
// establishes which PDF is which.
//
// The Bsdf is also templated by direction of tracing, whether from camera
// (Bsdf<false>) or from light (Bsdf<true>). This is identical to Veach's
// Adjoint BRDF (except the name is more straightforward).
// For us this is only used when refracting.

#define EPS_PHONG 1e-3f

template<bool FixIsLight>
class BSDF
{
    struct ComponentProbabilities
    {
        float diffProb;
        float phongProb;
        float reflProb;
        float refrProb;
    };
public:
    enum Events
    {
        kNONE        = 0,
        kDiffuse     = 1,
        kPhong       = 2,
        kReflect     = 4,
        kRefract     = 8,
        kSpecular    = (kReflect  | kRefract),
        kNonSpecular = (kDiffuse  | kPhong),
        kAll         = (kSpecular | kNonSpecular)
    };

public:
    BSDF():mMaterialID(-1){};

    BSDF(const Ray& aRay, const Isect& aIsect, const Scene& aScene)
    {
        Setup(aRay, aIsect, aScene);
    }

    void Setup(const Ray& aRay, const Isect& aIsect, const Scene& aScene)
    {
        mMaterialID = -1;
        mFrame.SetFromZ(aIsect.normal);
        mLocalOmegaFix = mFrame.ToLocal(-aRay.dir);

        // reject rays that are too parallel with tangent plane
        if(std::abs(mLocalOmegaFix.z) < EPS_COSINE)
        {
            return;
        }

        const Material &mat = aScene.GetMaterial(aIsect.matID);
        GetComponentProbabilities(mat, mProbabilities);

        mIsDelta = (mProbabilities.diffProb == 0) && (mProbabilities.phongProb == 0);

        // now it becomes valid
        mMaterialID = aIsect.matID;
    }

    /* \brief Given a direction, evaluates Bsdf
     *
     * Returns value of Bsdf, as well as cosine for the
     * aWorldOmegaGen direction.
     * Can return probability (w.r.t. solid angle W),
     * of having sampled aWorldOmegaGen given mLocalOmegaFix (oDirectPdfW),
     * and of having sampled mLocalOmegaFix given aWorldOmegaGen (oReversePdfW).
     *
     * Optionally can be limited to just some events.
     */
    Vec3f EvaluateBrdfPdfW(const Scene &aScene, const Vec3f &aWorldOmegaGen,
        float &oCosThetaGen, float *oDirectPdfW = NULL, float *oReversePdfW = NULL) const
    {
        Vec3f result(0);
        if(oDirectPdfW)  *oDirectPdfW = 0;
        if(oReversePdfW) *oReversePdfW = 0;

        const Vec3f localOmegaGen = mFrame.ToLocal(aWorldOmegaGen);
        if(localOmegaGen.z * mLocalOmegaFix.z < 0)
            return result;

        oCosThetaGen = std::abs(localOmegaGen.z);

        const Material &mat = aScene.GetMaterial(mMaterialID);

        result += EvaluateDiffuse(mat, localOmegaGen,
            oDirectPdfW, oReversePdfW);
        result += EvaluatePhong(mat, localOmegaGen,
            oDirectPdfW, oReversePdfW);
        return result;
    }

    /* \brief Given a direction, evaluates Pdf
     *
     * By default returns PDF with which would be aWorldOmegaGen
     * generated from mLocalOmegaFix. When aEvalRevPdf == true,
     * it provides PDF for the reverse direction.
     */
    float EvaluatePdfW(
        const Scene &aScene, const Vec3f &aWorldOmegaGen,
        const bool aEvalRevPdf = false) const
    {
        const Vec3f localOmegaGen = mFrame.ToLocal(aWorldOmegaGen);
        if(localOmegaGen.z * mLocalOmegaFix.z < 0)
            return 0;

        const Material &mat = aScene.GetMaterial(mMaterialID);

        float directPdfW  = 0;
        float reversePdfW = 0;

        EvaluatePdfWDiffuse(mat, localOmegaGen,
            &directPdfW, &reversePdfW);
        EvaluatePdfWPhong(mat, localOmegaGen,
            &directPdfW, &reversePdfW);

        return aEvalRevPdf ? reversePdfW : directPdfW;
    }

    /* \brief Given 3 random numbers, samples new direction from Bsdf.
     *
     * Uses z component of random triplet to pick Bsdf component from
     * which it will sample direction. If non-specular component is chosen,
     * it will also evaluate the other (non-specular) Bsdf components.
     * Return Bsdf factor for given direction, as well as PDF choosing that direction.
     * Can return event which has been sampled.
     * If result is Vec3f(0,0,0), then the sample should be discarded.
     */
    Vec3f SampleBrdf(const Scene &aScene, const Vec3f &aRndTriplet, Vec3f &oWorldOmegaGen,
        float &oPdfW, float &oCosThetaGen, uint *oSampledEvent = NULL) const
    {
        uint sampledEvent;
        if(aRndTriplet.z < mProbabilities.diffProb)
            sampledEvent = kDiffuse;
        else if(aRndTriplet.z < mProbabilities.diffProb + mProbabilities.phongProb)
            sampledEvent = kPhong;
        else if(aRndTriplet.z < mProbabilities.diffProb + mProbabilities.phongProb +
            mProbabilities.reflProb)
            sampledEvent = kReflect;
        else
            sampledEvent = kRefract;

        if(oSampledEvent) *oSampledEvent = sampledEvent;

        const Material &mat = aScene.GetMaterial(mMaterialID);

        oPdfW = 0;
        Vec3f result(0);
        Vec3f localOmegaGen;

        if(sampledEvent == kDiffuse)
        {
            result += SampleDiffuse(mat, aRndTriplet.GetXY(), localOmegaGen, oPdfW);
            if(result.IsZero()) return Vec3f(0);
            result += EvaluatePhong(mat, localOmegaGen, &oPdfW);
        }
        else if(sampledEvent == kPhong)
        {
            result += SamplePhong(mat, aRndTriplet.GetXY(), localOmegaGen, oPdfW);
            if(result.IsZero()) return Vec3f(0);
            result += EvaluateDiffuse(mat, localOmegaGen, &oPdfW);
        }
        else if(sampledEvent == kReflect)
        {
            result += SampleReflect(mat, aRndTriplet.GetXY(), localOmegaGen, oPdfW);
            if(result.IsZero()) return Vec3f(0);
        }
        else
        {
            result += SampleRefract(mat, aRndTriplet.GetXY(), localOmegaGen, oPdfW);
            if(result.IsZero()) return Vec3f(0);
        }

        oCosThetaGen   = std::abs(localOmegaGen.z);
        if(oCosThetaGen < EPS_COSINE)
            return Vec3f(0.f);

        oWorldOmegaGen = mFrame.ToWorld(localOmegaGen);
        return result;
    }


    bool         IsValid()          const  { return mMaterialID >= 0;               }
    bool         IsDelta()          const  { return mIsDelta;                       }
    float        ContinuationProb() const  { return mContinuationProb;              }
    float        CosThetaFix()      const  { return mLocalOmegaFix.z;               }
    Vec3f        WorldOmegaFix()    const  { return mFrame.ToWorld(mLocalOmegaFix); }
private:
    //////////////////////////////////////////////////////////////////////////
    // Sampling methods
    // All sampling methods take material, 2 random numbers [0-1[,
    // and return BRDF factor, generated direction in local coordinates,
    // and PDF
    Vec3f SampleDiffuse(const Material &aMaterial, const Vec2f &aRndTuple,
        Vec3f &oLocalOmegaGen, float &oPdfW) const
    {
        if(mLocalOmegaFix.z < EPS_COSINE)
            return Vec3f(0);
        float unweightedPdfW;
        oLocalOmegaGen = SampleCosHemisphereW(aRndTuple, &unweightedPdfW);
        oPdfW += unweightedPdfW * mProbabilities.diffProb;

        return aMaterial.mDiffuseReflectance * INV_PI_F;
    }

    Vec3f SamplePhong(const Material &aMaterial, const Vec2f &aRndTuple,
        Vec3f &oLocalOmegaGen, float &oPdfW) const
    {
        oLocalOmegaGen = SamplePowerCosHemisphereW(aRndTuple, aMaterial.mGlossiness, NULL);
        // due to numeric issues in MIS, we actually need to compute all Pdfs exactly
        // the same way all the time!!!
        const Vec3f reflLocalOmegaFixed = reflect001(mLocalOmegaFix);
        {
            Frame frame;
            frame.SetFromZ(reflLocalOmegaFixed);
            oLocalOmegaGen = frame.ToWorld(oLocalOmegaGen);
        }

        const float dot_R_Wi = Dot(reflLocalOmegaFixed, oLocalOmegaGen);
        if(dot_R_Wi <= EPS_PHONG)
            return Vec3f(0.f);

        EvaluatePdfWPhong(aMaterial, oLocalOmegaGen, &oPdfW);

        const Vec3f rho = aMaterial.mPhongReflectance *
            (aMaterial.mGlossiness + 2.f) * 0.5f * INV_PI_F;
        return rho * std::pow(dot_R_Wi, aMaterial.mGlossiness);
    }

    Vec3f SampleReflect(const Material &aMaterial, const Vec2f &aRndTuple,
        Vec3f &oLocalOmegaGen, float &oPdfW) const
    {
        oLocalOmegaGen = reflect001(mLocalOmegaFix);

        oPdfW += mProbabilities.reflProb;
        // BRDF is multiplied (outside) by cosine (oLocalOmegaGen.z),
        // for mirror this shouldn't be done, so we pre-divide here instead
        return mReflectCoeff * aMaterial.mMirrorReflectance /
            std::abs(oLocalOmegaGen.z);
    }

    Vec3f SampleRefract(const Material &aMaterial, const Vec2f &aRndTuple,
        Vec3f &oLocalOmegaGen, float &oPdfW) const
    {
        if(aMaterial.mIOR < 0)
            return Vec3f(0);

        float cosI = mLocalOmegaFix.z;

        float cosT;
        float etaIncOverEtaTrans;

        if(cosI < 0.f) // hit from inside
        {
            etaIncOverEtaTrans = aMaterial.mIOR;
            cosI = -cosI;
            cosT = 1.f;
        }
        else
        {
            etaIncOverEtaTrans = 1.f / aMaterial.mIOR;
            cosT = -1.f;
        }

        const float sinI2 = 1.f - cosI * cosI;
        const float sinT2 = Sqr(etaIncOverEtaTrans) * sinI2;

        if(sinT2 < 1.f) // no total internal reflection
        {
            cosT *= std::sqrt(std::max(0.f, 1.f - sinT2));

            oLocalOmegaGen = Vec3f(
                -etaIncOverEtaTrans * mLocalOmegaFix.x,
                -etaIncOverEtaTrans * mLocalOmegaFix.y,
                cosT);

            oPdfW += mProbabilities.refrProb;

            const float refractCoeff = 1.f - mReflectCoeff;
            // only camera paths are multiplied by this factor, and etas
            // are swapped because radiance flows in the opposite direction
            if(!FixIsLight)
                return Vec3f(refractCoeff * Sqr(etaIncOverEtaTrans) / std::abs(cosT));
            else
                return Vec3f(refractCoeff / std::abs(cosT));
        }
        //else total internal reflection, do nothing
        oPdfW += 0.f;
        return Vec3f(0.f);
    }

    //////////////////////////////////////////////////////////////////////////
    // Evaluating methods
    Vec3f EvaluateDiffuse(const Material& aMaterial, const Vec3f& aLocalOmegaGen,
        float *oDirectPdfW = NULL, float *oReversePdfW = NULL) const
    {
        if(mProbabilities.diffProb == 0) return Vec3f(0);
        if(mLocalOmegaFix.z < EPS_COSINE || aLocalOmegaGen.z < EPS_COSINE)
            return Vec3f(0);

        if(oDirectPdfW)
            *oDirectPdfW  += mProbabilities.diffProb *
            std::max(0.f, aLocalOmegaGen.z * INV_PI_F);

        if(oReversePdfW)
            *oReversePdfW += mProbabilities.diffProb *
            std::max(0.f, mLocalOmegaFix.z * INV_PI_F);

        return aMaterial.mDiffuseReflectance * INV_PI_F;
    }

    Vec3f EvaluatePhong(const Material& aMaterial, const Vec3f& aLocalOmegaGen,
        float *oDirectPdfW = NULL, float *oReversePdfW = NULL) const
    {
        if(mProbabilities.phongProb == 0) return Vec3f(0);
        if(mLocalOmegaFix.z < EPS_COSINE || aLocalOmegaGen.z < EPS_COSINE)
            return Vec3f(0);

        // assumes this is never called when rejectShadingCos(oLocalOmegaGen.z) is true
        const Vec3f reflLocalOmegaIn = reflect001(mLocalOmegaFix);
        const float dot_R_Wi = Dot(reflLocalOmegaIn, aLocalOmegaGen);

        if(dot_R_Wi <= EPS_PHONG)
            return Vec3f(0.f);

        if(oDirectPdfW || oReversePdfW)
        {
            // the sampling is symmetric
            const float pdfW = EvalPowerCosHemispherePdfW(reflLocalOmegaIn, aLocalOmegaGen,
                aMaterial.mGlossiness) * mProbabilities.phongProb;
            if(oDirectPdfW)  *oDirectPdfW  += pdfW;
            if(oReversePdfW) *oReversePdfW += pdfW;
        }

        const Vec3f rho = aMaterial.mPhongReflectance *
            (aMaterial.mGlossiness + 2.f) * 0.5f * INV_PI_F;
        return rho * std::pow(dot_R_Wi, aMaterial.mGlossiness);
    }

    //////////////////////////////////////////////////////////////////////////
    // Pdf rvaluating methods
    void EvaluatePdfWDiffuse(const Material& aMaterial, const Vec3f& aLocalOmegaGen,
        float *oDirectPdfW = NULL, float *oReversePdfW = NULL) const
    {
        if(mProbabilities.diffProb == 0)
            return;

        if(oDirectPdfW)
            *oDirectPdfW  += mProbabilities.diffProb *
            std::max(0.f, aLocalOmegaGen.z * INV_PI_F);

        if(oReversePdfW)
            *oReversePdfW += mProbabilities.diffProb *
            std::max(0.f, mLocalOmegaFix.z * INV_PI_F);
    }

    void EvaluatePdfWPhong(const Material& aMaterial, const Vec3f& aLocalOmegaGen,
        float *oDirectPdfW = NULL, float *oReversePdfW = NULL) const
    {
        if(mProbabilities.phongProb == 0) return;

        // assumes this is never called when rejectShadingCos(oLocalOmegaGen.z) is true
        const Vec3f reflLocalOmegaIn = reflect001(mLocalOmegaFix);
        const float dot_R_Wi = Dot(reflLocalOmegaIn, aLocalOmegaGen);

        if(dot_R_Wi <= EPS_PHONG)
            return;

        if(oDirectPdfW || oReversePdfW)
        {
            // the sampling is symmetric
            const float pdfW = EvalPowerCosHemispherePdfW(reflLocalOmegaIn, aLocalOmegaGen,
                aMaterial.mGlossiness) * mProbabilities.phongProb;
            if(oDirectPdfW)  *oDirectPdfW  += pdfW;
            if(oReversePdfW) *oReversePdfW += pdfW;
        }
    }

    //////////////////////////////////////////////////////////////////////////
    // Albedo methods
    float AlbedoDiffuse(const Material& aMaterial) const
    {
        return Luminance(aMaterial.mDiffuseReflectance);
    }

    float AlbedoPhong(const Material& aMaterial) const
    {
        return Luminance(aMaterial.mPhongReflectance);
    }

    float AlbedoReflect(const Material& aMaterial) const
    {
        return Luminance(aMaterial.mMirrorReflectance);
    }

    float AlbedoRefract(const Material& aMaterial) const
    {
        return 1.f;
    }

    // returns false when the material is completely black
    void GetComponentProbabilities(const Material& aMaterial,
        ComponentProbabilities& oProbabilities)
    {
        mReflectCoeff = FresnelDielectric(mLocalOmegaFix.z, aMaterial.mIOR);

        const float albedoDiffuse = AlbedoDiffuse(aMaterial);
        const float albedoPhong   = AlbedoPhong(aMaterial);
        const float albedoReflect = mReflectCoeff         * AlbedoReflect(aMaterial);
        const float albedoRefract = (1.f - mReflectCoeff) * AlbedoRefract(aMaterial);

        const float totalAlbedo = albedoDiffuse + albedoPhong + albedoReflect + albedoRefract;

        if(totalAlbedo < 1e-9f)
        {
            oProbabilities.diffProb  = 0.f;
            oProbabilities.phongProb = 0.f;
            oProbabilities.reflProb  = 0.f;
            oProbabilities.refrProb  = 0.f;
            mContinuationProb = 0.f;
        }
        else
        {
            oProbabilities.diffProb  = albedoDiffuse / totalAlbedo;
            oProbabilities.phongProb = albedoPhong   / totalAlbedo;
            oProbabilities.reflProb  = albedoReflect / totalAlbedo;
            oProbabilities.refrProb  = albedoRefract / totalAlbedo;
            // The continuation probability is max component from reflectance.
            // That way the weight of sample will never rise.
            // Luminance is another very valid option.
            mContinuationProb =
                (aMaterial.mDiffuseReflectance +
                aMaterial.mPhongReflectance +
                mReflectCoeff * aMaterial.mMirrorReflectance).Max() +
                (1.f - mReflectCoeff);
            mContinuationProb = std::min(1.f, std::max(0.f, mContinuationProb));
        }
    }

private:
    int   mMaterialID;    //!< Id of scene material, < 0 ~ invalid
    Frame mFrame;         //!< Local frame of reference
    Vec3f mLocalOmegaFix; //!< Incoming (fixed) direction, in local
    bool  mIsDelta;       //!< True when material is purely specular
    ComponentProbabilities mProbabilities; //!< Sampling probabilities
    float mContinuationProb; //!< Russian roulette probability
    float mReflectCoeff;     //!< Fresnel reflection coefficient (for glass)
};

#endif //__BSDF_HXX__