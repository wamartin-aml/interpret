// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#include "PrecompiledHeader.h"

#include <assert.h>
#include <string.h> // memset
#include <stdlib.h> // malloc, realloc, free
#include <stddef.h> // size_t, ptrdiff_t
#include <limits> // numeric_limits

#include "ebmcore.h"

#include "Logging.h"
#include "EbmInternal.h"
// very independent includes
#include "Logging.h" // EBM_ASSERT & LOG
#include "InitializeResiduals.h"
#include "RandomStream.h"
#include "SegmentedRegion.h"
#include "EbmStatistics.h"
// this depends on TreeNode pointers, but doesn't require the full definition of TreeNode
#include "CachedThreadResources.h"
// attribute includes
#include "AttributeInternal.h"
// AttributeCombination.h depends on AttributeInternal.h
#include "AttributeCombinationInternal.h"
// dataset depends on attributes
#include "DataSetByAttributeCombination.h"
// samples is somewhat independent from datasets, but relies on an indirect coupling with them
#include "SamplingWithReplacement.h"
// TreeNode depends on almost everything
#include "SingleDimensionalTraining.h"
#include "MultiDimensionalTraining.h"

static void DeleteSegmentsCore(const size_t cAttributeCombinations, SegmentedRegionCore<ActiveDataType, FractionalDataType> ** const apSegmentedRegions) {
   LOG(TraceLevelInfo, "Entered DeleteSegmentsCore");

   if(UNLIKELY(nullptr != apSegmentedRegions)) {
      EBM_ASSERT(0 < cAttributeCombinations);
      SegmentedRegionCore<ActiveDataType, FractionalDataType> ** ppSegmentedRegions = apSegmentedRegions;
      const SegmentedRegionCore<ActiveDataType, FractionalDataType> * const * const ppSegmentedRegionsEnd = &apSegmentedRegions[cAttributeCombinations];
      do {
         SegmentedRegionCore<ActiveDataType, FractionalDataType>::Free(*ppSegmentedRegions);
         ++ppSegmentedRegions;
      } while(ppSegmentedRegionsEnd != ppSegmentedRegions);
      delete[] apSegmentedRegions;
   }
   LOG(TraceLevelInfo, "Exited DeleteSegmentsCore");
}

static SegmentedRegionCore<ActiveDataType, FractionalDataType> ** InitializeSegmentsCore(const size_t cAttributeCombinations, const AttributeCombinationCore * const * const apAttributeCombinations, const size_t cVectorLength) {
   LOG(TraceLevelInfo, "Entered InitializeSegmentsCore");

   EBM_ASSERT(0 < cAttributeCombinations);
   EBM_ASSERT(nullptr != apAttributeCombinations);
   EBM_ASSERT(1 <= cVectorLength);

   SegmentedRegionCore<ActiveDataType, FractionalDataType> ** const apSegmentedRegions = new (std::nothrow) SegmentedRegionCore<ActiveDataType, FractionalDataType> *[cAttributeCombinations];
   if(UNLIKELY(nullptr == apSegmentedRegions)) {
      LOG(TraceLevelWarning, "WARNING InitializeSegmentsCore nullptr == apSegmentedRegions");
      return nullptr;
   }
   memset(apSegmentedRegions, 0, sizeof(*apSegmentedRegions) * cAttributeCombinations); // this needs to be done immediately after allocation otherwise we might attempt to free random garbage on an error

   SegmentedRegionCore<ActiveDataType, FractionalDataType> ** ppSegmentedRegions = apSegmentedRegions;
   for(size_t iAttributeCombination = 0; iAttributeCombination < cAttributeCombinations; ++iAttributeCombination) {
      const AttributeCombinationCore * const pAttributeCombination = apAttributeCombinations[iAttributeCombination];
      SegmentedRegionCore<ActiveDataType, FractionalDataType> * const pSegmentedRegions = SegmentedRegionCore<ActiveDataType, FractionalDataType>::Allocate(pAttributeCombination->m_cAttributes, cVectorLength);
      if(UNLIKELY(nullptr == pSegmentedRegions)) {
         LOG(TraceLevelWarning, "WARNING InitializeSegmentsCore nullptr == pSegmentedRegions");
         DeleteSegmentsCore(cAttributeCombinations, apSegmentedRegions);
         return nullptr;
      }

      if(0 == pAttributeCombination->m_cAttributes) {
         // if there are zero dimensions, then we have a tensor with 1 item, and we're already expanded
         pSegmentedRegions->m_bExpanded = true;
      } else {
         // if our segmented region has no dimensions, then it's already a fully expanded with 1 bin

         // TODO optimize the next few lines
         // TODO there might be a nicer way to expand this at allocation time (fill with zeros is easier)
         // we want to return a pointer to our interior state in the GetCurrentModel and GetBestModel functions.  For simplicity we don't transmit the divions, so we need to expand our SegmentedRegion before returning
         // the easiest way to ensure that the SegmentedRegion is expanded is to start it off expanded, and then we don't have to check later since anything merged into an expanded SegmentedRegion will itself be expanded
         size_t acDivisionIntegersEnd[k_cDimensionsMax];
         size_t iDimension = 0;
         do {
            acDivisionIntegersEnd[iDimension] = pAttributeCombination->m_AttributeCombinationEntry[iDimension].m_pAttribute->m_cStates;
            ++iDimension;
         } while(iDimension < pAttributeCombination->m_cAttributes);

         if(pSegmentedRegions->Expand(acDivisionIntegersEnd)) {
            LOG(TraceLevelWarning, "WARNING InitializeSegmentsCore pSegmentedRegions->Expand(acDivisionIntegersEnd)");
            DeleteSegmentsCore(cAttributeCombinations, apSegmentedRegions);
            return nullptr;
         }
      }

      *ppSegmentedRegions = pSegmentedRegions;
      ++ppSegmentedRegions;
   }

   LOG(TraceLevelInfo, "Exited InitializeSegmentsCore");
   return apSegmentedRegions;
}

// a*PredictionScores = logOdds for binary classification
// a*PredictionScores = logWeights for multiclass classification
// a*PredictionScores = predictedValue for regression
template<unsigned int cInputBits, unsigned int cTargetBits, ptrdiff_t countCompilerClassificationTargetStates>
static void TrainingSetTargetAttributeLoop(const AttributeCombinationCore * const pAttributeCombination, DataSetAttributeCombination * const pTrainingSet, const FractionalDataType * const aModelUpdateTensor, const size_t cTargetStates) {
   LOG(TraceLevelVerbose, "Entered TrainingSetTargetAttributeLoop");

   const size_t cVectorLength = GET_VECTOR_LENGTH(countCompilerClassificationTargetStates, cTargetStates);
   const size_t cCases = pTrainingSet->GetCountCases();
   EBM_ASSERT(0 < cCases);

   if(0 == pAttributeCombination->m_cAttributes) {
      FractionalDataType * pResidualError = pTrainingSet->GetResidualPointer();
      const FractionalDataType * const pResidualErrorEnd = pResidualError + cVectorLength * cCases;
      if(IsRegression(countCompilerClassificationTargetStates)) {
         const FractionalDataType smallChangeToPrediction = aModelUpdateTensor[0];
         while(pResidualErrorEnd != pResidualError) {
            // this will apply a small fix to our existing TrainingPredictionScores, either positive or negative, whichever is needed
            const FractionalDataType residualError = EbmStatistics::ComputeRegressionResidualError(*pResidualError - smallChangeToPrediction);
            *pResidualError = residualError;
            ++pResidualError;
         }
      } else {
         EBM_ASSERT(IsClassification(countCompilerClassificationTargetStates));
         FractionalDataType * pTrainingPredictionScores = pTrainingSet->GetPredictionScores();
         const StorageDataTypeCore * pTargetData = pTrainingSet->GetTargetDataPointer();
         if(IsBinaryClassification(countCompilerClassificationTargetStates)) {
            const FractionalDataType smallChangeToPredictionScores = aModelUpdateTensor[0];
            while(pResidualErrorEnd != pResidualError) {
               StorageDataTypeCore targetData = *pTargetData;
               // TODO : because there is only one bin for a zero attribute attribute combination, we can move the fetch of smallChangeToPredictionScores outside of our loop so that the code doesn't have this dereference each loop
               // this will apply a small fix to our existing TrainingPredictionScores, either positive or negative, whichever is needed
               const FractionalDataType trainingPredictionScore = *pTrainingPredictionScores + smallChangeToPredictionScores;
               *pTrainingPredictionScores = trainingPredictionScore;
               const FractionalDataType residualError = EbmStatistics::ComputeClassificationResidualErrorBinaryclass(trainingPredictionScore, targetData);
               *pResidualError = residualError;
               ++pResidualError;
               ++pTrainingPredictionScores;
               ++pTargetData;
            }
         } else {
            const FractionalDataType * pValues = aModelUpdateTensor;
            while(pResidualErrorEnd != pResidualError) {
               StorageDataTypeCore targetData = *pTargetData;
               FractionalDataType sumExp = 0;
               size_t iVector1 = 0;
               do {
                  // TODO : because there is only one bin for a zero attribute attribute combination, we could move these values to the stack where the copmiler could reason about their visibility and optimize small arrays into registers
                  const FractionalDataType smallChangeToPredictionScores = pValues[iVector1];
                  // this will apply a small fix to our existing TrainingPredictionScores, either positive or negative, whichever is needed
                  const FractionalDataType trainingPredictionScores = pTrainingPredictionScores[iVector1] + smallChangeToPredictionScores;
                  pTrainingPredictionScores[iVector1] = trainingPredictionScores;
                  sumExp += std::exp(trainingPredictionScores);
                  ++iVector1;
               } while(iVector1 < cVectorLength);

               EBM_ASSERT((IsNumberConvertable<StorageDataTypeCore, size_t>(cVectorLength)));
               const StorageDataTypeCore cVectorLengthStorage = static_cast<StorageDataTypeCore>(cVectorLength);
               StorageDataTypeCore iVector2 = 0;
               do {
                  // TODO : we're calculating exp(predictionScore) above, and then again in ComputeClassificationResidualErrorMulticlass.  exp(..) is expensive so we should just do it once instead and store the result in a small memory array here
                  const FractionalDataType residualError = EbmStatistics::ComputeClassificationResidualErrorMulticlass(sumExp, pTrainingPredictionScores[iVector2], targetData, iVector2);
                  *pResidualError = residualError;
                  ++pResidualError;
                  ++iVector2;
               } while(iVector2 < cVectorLengthStorage);
               // TODO: this works as a way to remove one parameter, but it obviously insn't as efficient as omitting the parameter
               // 
               // this works out in the math as making the first model vector parameter equal to zero, which in turn removes one degree of freedom
               // from the model vector parameters.  Since the model vector weights need to be normalized to sum to a probabilty of 100%, we can set the first
               // one to the constant 1 (0 in log space) and force the other parameters to adjust to that scale which fixes them to a single valid set of values
               // insted of allowing them to be scaled.  
               // Probability = exp(T1 + I1) / [exp(T1 + I1) + exp(T2 + I2) + exp(T3 + I3)] => we can add a constant inside each exp(..) term, which will be multiplication outside the exp(..), which
               // means the numerator and denominator are multiplied by the same constant, which cancels eachother out.  We can thus set exp(T2 + I2) to exp(0) and adjust the other terms
               constexpr bool bZeroingResiduals = 0 <= k_iZeroResidual;
               if(bZeroingResiduals) {
                  pResidualError[k_iZeroResidual - static_cast<ptrdiff_t>(cVectorLength)] = 0;
               }
               pTrainingPredictionScores += cVectorLength;
               ++pTargetData;
            }
         }
      }
      LOG(TraceLevelVerbose, "Exited TrainingSetTargetAttributeLoop - Zero dimensions");
      return;
   }

   const size_t cItemsPerBitPackDataUnit = pAttributeCombination->m_cItemsPerBitPackDataUnit;
   const size_t cBitsPerItemMax = GetCountBits(cItemsPerBitPackDataUnit);
   const size_t maskBits = std::numeric_limits<size_t>::max() >> (k_cBitsForStorageType - cBitsPerItemMax);

   const StorageDataTypeCore * pInputData = pTrainingSet->GetDataPointer(pAttributeCombination);
   FractionalDataType * pResidualError = pTrainingSet->GetResidualPointer();
   const FractionalDataType * const pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete = pResidualError + cVectorLength * (static_cast<ptrdiff_t>(cCases) - cItemsPerBitPackDataUnit);

   if(IsRegression(countCompilerClassificationTargetStates)) {
      size_t cItemsRemaining;
      while(pResidualError < pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete) {
         cItemsRemaining = cItemsPerBitPackDataUnit;
         // TODO : jumping back into this loop and changing cItemsRemaining to a dynamic value that isn't compile time determinable
         // causes this function to NOT be optimized as much as it could if we had two separate loops.  We're just trying this out for now though
      one_last_loop_regression:;
         // we store the already multiplied dimensional value in *pInputData
         size_t iBinCombined = static_cast<size_t>(*pInputData);
         ++pInputData;
         do {
            const size_t iBin = maskBits & iBinCombined;
            const FractionalDataType smallChangeToPrediction = aModelUpdateTensor[iBin * cVectorLength];
            // this will apply a small fix to our existing TrainingPredictionScores, either positive or negative, whichever is needed
            const FractionalDataType residualError = EbmStatistics::ComputeRegressionResidualError(*pResidualError - smallChangeToPrediction);
            *pResidualError = residualError;
            ++pResidualError;

            iBinCombined >>= cBitsPerItemMax;
            // TODO : try replacing cItemsRemaining with a pResidualErrorInnerLoopEnd which eliminates one subtact operation, but might make it harder for the compiler to optimize the loop away
            --cItemsRemaining;
         } while(0 != cItemsRemaining);
      }
      const FractionalDataType * const pResidualErrorEnd = pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete + cVectorLength * cItemsPerBitPackDataUnit;
      if(pResidualError < pResidualErrorEnd) {
         // first time through?
         EBM_ASSERT(0 == (pResidualErrorEnd - pResidualError) % cVectorLength);
         cItemsRemaining = (pResidualErrorEnd - pResidualError) / cVectorLength;
         EBM_ASSERT(0 < cItemsRemaining);
         EBM_ASSERT(cItemsRemaining <= cItemsPerBitPackDataUnit);
         goto one_last_loop_regression;
      }
      EBM_ASSERT(pResidualError == pResidualErrorEnd); // after our second iteration we should have finished everything!
   } else {
      EBM_ASSERT(IsClassification(countCompilerClassificationTargetStates));
      FractionalDataType * pTrainingPredictionScores = pTrainingSet->GetPredictionScores();
      const StorageDataTypeCore * pTargetData = pTrainingSet->GetTargetDataPointer();

      size_t cItemsRemaining;

      while(pResidualError < pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete) {
         cItemsRemaining = cItemsPerBitPackDataUnit;
         // TODO : jumping back into this loop and changing cItemsRemaining to a dynamic value that isn't compile time determinable
         // causes this function to NOT be optimized as much as it could if we had two separate loops.  We're just trying this out for now though
      one_last_loop_classification:;
         // we store the already multiplied dimensional value in *pInputData
         size_t iBinCombined = static_cast<size_t>(*pInputData);
         ++pInputData;
         do {
            StorageDataTypeCore targetData = *pTargetData;

            const size_t iBin = maskBits & iBinCombined;
            const FractionalDataType * pValues = &aModelUpdateTensor[iBin * cVectorLength];

            if(IsBinaryClassification(countCompilerClassificationTargetStates)) {
               const FractionalDataType smallChangeToPredictionScores = pValues[0];
               // this will apply a small fix to our existing TrainingPredictionScores, either positive or negative, whichever is needed
               const FractionalDataType trainingPredictionScore = *pTrainingPredictionScores + smallChangeToPredictionScores;
               *pTrainingPredictionScores = trainingPredictionScore;
               const FractionalDataType residualError = EbmStatistics::ComputeClassificationResidualErrorBinaryclass(trainingPredictionScore, targetData);
               *pResidualError = residualError;
               ++pResidualError;
            } else {
               FractionalDataType sumExp = 0;
               size_t iVector1 = 0;
               do {
                  const FractionalDataType smallChangeToPredictionScores = pValues[iVector1];
                  // this will apply a small fix to our existing TrainingPredictionScores, either positive or negative, whichever is needed
                  const FractionalDataType trainingPredictionScores = pTrainingPredictionScores[iVector1] + smallChangeToPredictionScores;
                  pTrainingPredictionScores[iVector1] = trainingPredictionScores;
                  sumExp += std::exp(trainingPredictionScores);
                  ++iVector1;
               } while(iVector1 < cVectorLength);

               EBM_ASSERT((IsNumberConvertable<StorageDataTypeCore, size_t>(cVectorLength)));
               const StorageDataTypeCore cVectorLengthStorage = static_cast<StorageDataTypeCore>(cVectorLength);
               StorageDataTypeCore iVector2 = 0;
               do {
                  // TODO : we're calculating exp(predictionScore) above, and then again in ComputeClassificationResidualErrorMulticlass.  exp(..) is expensive so we should just do it once instead and store the result in a small memory array here
                  const FractionalDataType residualError = EbmStatistics::ComputeClassificationResidualErrorMulticlass(sumExp, pTrainingPredictionScores[iVector2], targetData, iVector2);
                  *pResidualError = residualError;
                  ++pResidualError;
                  ++iVector2;
               } while(iVector2 < cVectorLengthStorage);
               // TODO: this works as a way to remove one parameter, but it obviously insn't as efficient as omitting the parameter
               // 
               // this works out in the math as making the first model vector parameter equal to zero, which in turn removes one degree of freedom
               // from the model vector parameters.  Since the model vector weights need to be normalized to sum to a probabilty of 100%, we can set the first
               // one to the constant 1 (0 in log space) and force the other parameters to adjust to that scale which fixes them to a single valid set of values
               // insted of allowing them to be scaled.  
               // Probability = exp(T1 + I1) / [exp(T1 + I1) + exp(T2 + I2) + exp(T3 + I3)] => we can add a constant inside each exp(..) term, which will be multiplication outside the exp(..), which
               // means the numerator and denominator are multiplied by the same constant, which cancels eachother out.  We can thus set exp(T2 + I2) to exp(0) and adjust the other terms
               constexpr bool bZeroingResiduals = 0 <= k_iZeroResidual;
               if(bZeroingResiduals) {
                  pResidualError[k_iZeroResidual - static_cast<ptrdiff_t>(cVectorLength)] = 0;
               }
            }
            pTrainingPredictionScores += cVectorLength;
            ++pTargetData;

            iBinCombined >>= cBitsPerItemMax;
            // TODO : try replacing cItemsRemaining with a pResidualErrorInnerLoopEnd which eliminates one subtact operation, but might make it harder for the compiler to optimize the loop away
            --cItemsRemaining;
         } while(0 != cItemsRemaining);
      }
      const FractionalDataType * const pResidualErrorEnd = pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete + cVectorLength * cItemsPerBitPackDataUnit;
      if(pResidualError < pResidualErrorEnd) {
         // first time through?
         EBM_ASSERT(0 == (pResidualErrorEnd - pResidualError) % cVectorLength);
         cItemsRemaining = (pResidualErrorEnd - pResidualError) / cVectorLength;
         EBM_ASSERT(0 < cItemsRemaining);
         EBM_ASSERT(cItemsRemaining <= cItemsPerBitPackDataUnit);
         goto one_last_loop_classification;
      }
      EBM_ASSERT(pResidualError == pResidualErrorEnd); // after our second iteration we should have finished everything!
   }
   LOG(TraceLevelVerbose, "Exited TrainingSetTargetAttributeLoop");
}

// a*PredictionScores = logOdds for binary classification
// a*PredictionScores = logWeights for multiclass classification
// a*PredictionScores = predictedValue for regression
template<unsigned int cInputBits, ptrdiff_t countCompilerClassificationTargetStates>
static void TrainingSetInputAttributeLoop(const AttributeCombinationCore * const pAttributeCombination, DataSetAttributeCombination * const pTrainingSet, const FractionalDataType * const aModelUpdateTensor, const size_t cTargetStates) {
   if(cTargetStates <= 1 << 1) {
      TrainingSetTargetAttributeLoop<cInputBits, 1, countCompilerClassificationTargetStates>(pAttributeCombination, pTrainingSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 2) {
      TrainingSetTargetAttributeLoop<cInputBits, 2, countCompilerClassificationTargetStates>(pAttributeCombination, pTrainingSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 4) {
      TrainingSetTargetAttributeLoop<cInputBits, 4, countCompilerClassificationTargetStates>(pAttributeCombination, pTrainingSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 8) {
      TrainingSetTargetAttributeLoop<cInputBits, 8, countCompilerClassificationTargetStates>(pAttributeCombination, pTrainingSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 16) {
      TrainingSetTargetAttributeLoop<cInputBits, 16, countCompilerClassificationTargetStates>(pAttributeCombination, pTrainingSet, aModelUpdateTensor, cTargetStates);
   } else if(static_cast<uint64_t>(cTargetStates) <= uint64_t { 1 } << 32) {
      // if this is a 32 bit system, then m_cStates can't be 0x100000000 or above, because we would have checked that when converting the 64 bit numbers into size_t, and m_cStates will be promoted to a 64 bit number for the above comparison
      // if this is a 64 bit system, then this comparison is fine

      // TODO : perhaps we should change m_cStates into m_iStateMax so that we don't need to do the above promotion to 64 bits.. we can make it <= 0xFFFFFFFF.  Write a function to fill the lowest bits with ones for any number of bits

      TrainingSetTargetAttributeLoop<cInputBits, 32, countCompilerClassificationTargetStates>(pAttributeCombination, pTrainingSet, aModelUpdateTensor, cTargetStates);
   } else {
      // our interface doesn't allow more than 64 bits, so even if size_t was bigger then we don't need to examine higher
      static_assert(63 == CountBitsRequiredPositiveMax<IntegerDataType>(), "");
      TrainingSetTargetAttributeLoop<cInputBits, 64, countCompilerClassificationTargetStates>(pAttributeCombination, pTrainingSet, aModelUpdateTensor, cTargetStates);
   }
}

// a*PredictionScores = logOdds for binary classification
// a*PredictionScores = logWeights for multiclass classification
// a*PredictionScores = predictedValue for regression
template<unsigned int cInputBits, unsigned int cTargetBits, ptrdiff_t countCompilerClassificationTargetStates>
static FractionalDataType ValidationSetTargetAttributeLoop(const AttributeCombinationCore * const pAttributeCombination, DataSetAttributeCombination * const pValidationSet, const FractionalDataType * const aModelUpdateTensor, const size_t cTargetStates) {
   LOG(TraceLevelVerbose, "Entering ValidationSetTargetAttributeLoop");

   const size_t cVectorLength = GET_VECTOR_LENGTH(countCompilerClassificationTargetStates, cTargetStates);
   const size_t cCases = pValidationSet->GetCountCases();
   EBM_ASSERT(0 < cCases);

   if(0 == pAttributeCombination->m_cAttributes) {
      if(IsRegression(countCompilerClassificationTargetStates)) {
         FractionalDataType * pResidualError = pValidationSet->GetResidualPointer();
         const FractionalDataType * const pResidualErrorEnd = pResidualError + cCases;

         const FractionalDataType smallChangeToPrediction = aModelUpdateTensor[0];

         FractionalDataType rootMeanSquareError = 0;
         while(pResidualErrorEnd != pResidualError) {
            // this will apply a small fix to our existing ValidationPredictionScores, either positive or negative, whichever is needed
            const FractionalDataType residualError = EbmStatistics::ComputeRegressionResidualError(*pResidualError - smallChangeToPrediction);
            rootMeanSquareError += residualError * residualError;
            *pResidualError = residualError;
            ++pResidualError;
         }

         rootMeanSquareError /= pValidationSet->GetCountCases();
         LOG(TraceLevelVerbose, "Exited ValidationSetTargetAttributeLoop - Zero dimensions");
         return sqrt(rootMeanSquareError);
      } else {
         EBM_ASSERT(IsClassification(countCompilerClassificationTargetStates));
         FractionalDataType * pValidationPredictionScores = pValidationSet->GetPredictionScores();
         const StorageDataTypeCore * pTargetData = pValidationSet->GetTargetDataPointer();

         const FractionalDataType * const pValidationPredictionEnd = pValidationPredictionScores + cVectorLength * cCases;

         FractionalDataType sumLogLoss = 0;
         if(IsBinaryClassification(countCompilerClassificationTargetStates)) {
            const FractionalDataType smallChangeToPredictionScores = aModelUpdateTensor[0];
            while(pValidationPredictionEnd != pValidationPredictionScores) {
               StorageDataTypeCore targetData = *pTargetData;
               // this will apply a small fix to our existing ValidationPredictionScores, either positive or negative, whichever is needed
               const FractionalDataType validationPredictionScores = *pValidationPredictionScores + smallChangeToPredictionScores;
               *pValidationPredictionScores = validationPredictionScores;
               sumLogLoss += EbmStatistics::ComputeClassificationSingleCaseLogLossBinaryclass(validationPredictionScores, targetData);
               ++pValidationPredictionScores;
               ++pTargetData;
            }
         } else {
            const FractionalDataType * pValues = aModelUpdateTensor;
            while(pValidationPredictionEnd != pValidationPredictionScores) {
               StorageDataTypeCore targetData = *pTargetData;
               FractionalDataType sumExp = 0;
               size_t iVector = 0;
               do {
                  const FractionalDataType smallChangeToPredictionScores = pValues[iVector];
                  // this will apply a small fix to our existing validationPredictionScores, either positive or negative, whichever is needed

                  // TODO : this is no longer a prediction for multiclass.  It is a weight.  Change all instances of this naming. -> validationLogWeight
                  const FractionalDataType validationPredictionScores = *pValidationPredictionScores + smallChangeToPredictionScores;
                  *pValidationPredictionScores = validationPredictionScores;
                  sumExp += std::exp(validationPredictionScores);
                  ++pValidationPredictionScores;

                  // TODO : consider replacing iVector with pValidationPredictionScoresInnerEnd
                  ++iVector;
               } while(iVector < cVectorLength);
               // TODO: store the result of std::exp above for the index that we care about above since exp(..) is going to be expensive and probably even more expensive than an unconditional branch
               sumLogLoss += EbmStatistics::ComputeClassificationSingleCaseLogLossMulticlass(sumExp, pValidationPredictionScores - cVectorLength, targetData);
               ++pTargetData;
            }
         }
         LOG(TraceLevelVerbose, "Exited ValidationSetTargetAttributeLoop - Zero dimensions");
         return sumLogLoss;
      }
      EBM_ASSERT(false);
   }

   const size_t cItemsPerBitPackDataUnit = pAttributeCombination->m_cItemsPerBitPackDataUnit;
   const size_t cBitsPerItemMax = GetCountBits(cItemsPerBitPackDataUnit);
   const size_t maskBits = std::numeric_limits<size_t>::max() >> (k_cBitsForStorageType - cBitsPerItemMax);
   const StorageDataTypeCore * pInputData = pValidationSet->GetDataPointer(pAttributeCombination);

   if(IsRegression(countCompilerClassificationTargetStates)) {
      FractionalDataType * pResidualError = pValidationSet->GetResidualPointer();
      const FractionalDataType * const pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete = pResidualError + (static_cast<ptrdiff_t>(cCases) - cItemsPerBitPackDataUnit);

      FractionalDataType rootMeanSquareError = 0;
      size_t cItemsRemaining;
      while(pResidualError < pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete) {
         cItemsRemaining = cItemsPerBitPackDataUnit;
         // TODO : jumping back into this loop and changing cItemsRemaining to a dynamic value that isn't compile time determinable
         // causes this function to NOT be optimized as much as it could if we had two separate loops.  We're just trying this out for now though
      one_last_loop_regression:;
         // we store the already multiplied dimensional value in *pInputData
         size_t iBinCombined = static_cast<size_t>(*pInputData);
         ++pInputData;
         do {
            const size_t iBin = maskBits & iBinCombined;
            const FractionalDataType smallChangeToPrediction = aModelUpdateTensor[iBin * cVectorLength];
            // this will apply a small fix to our existing ValidationPredictionScores, either positive or negative, whichever is needed
            const FractionalDataType residualError = EbmStatistics::ComputeRegressionResidualError(*pResidualError - smallChangeToPrediction);
            rootMeanSquareError += residualError * residualError;
            *pResidualError = residualError;
            ++pResidualError;

            iBinCombined >>= cBitsPerItemMax;
            // TODO : try replacing cItemsRemaining with a pResidualErrorInnerLoopEnd which eliminates one subtact operation, but might make it harder for the compiler to optimize the loop away
            --cItemsRemaining;
         } while(0 != cItemsRemaining);
      }
      const FractionalDataType * const pResidualErrorEnd = pResidualErrorLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete + cVectorLength * cItemsPerBitPackDataUnit;
      if(pResidualError < pResidualErrorEnd) {
         // first time through?
         EBM_ASSERT(0 == (pResidualErrorEnd - pResidualError) % cVectorLength);
         cItemsRemaining = (pResidualErrorEnd - pResidualError) / cVectorLength;
         EBM_ASSERT(0 < cItemsRemaining);
         EBM_ASSERT(cItemsRemaining <= cItemsPerBitPackDataUnit);
         goto one_last_loop_regression;
      }
      EBM_ASSERT(pResidualError == pResidualErrorEnd); // after our second iteration we should have finished everything!

      rootMeanSquareError /= pValidationSet->GetCountCases();
      LOG(TraceLevelVerbose, "Exited ValidationSetTargetAttributeLoop");
      return sqrt(rootMeanSquareError);
   } else {
      EBM_ASSERT(IsClassification(countCompilerClassificationTargetStates));
      FractionalDataType * pValidationPredictionScores = pValidationSet->GetPredictionScores();
      const StorageDataTypeCore * pTargetData = pValidationSet->GetTargetDataPointer();

      size_t cItemsRemaining;

      const FractionalDataType * const pValidationPredictionScoresLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete = pValidationPredictionScores + cVectorLength * (static_cast<ptrdiff_t>(cCases) - cItemsPerBitPackDataUnit);

      FractionalDataType sumLogLoss = 0;
      while(pValidationPredictionScores < pValidationPredictionScoresLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete) {
         cItemsRemaining = cItemsPerBitPackDataUnit;
         // TODO : jumping back into this loop and changing cItemsRemaining to a dynamic value that isn't compile time determinable
         // causes this function to NOT be optimized as much as it could if we had two separate loops.  We're just trying this out for now though
      one_last_loop_classification:;
         // we store the already multiplied dimensional value in *pInputData
         size_t iBinCombined = static_cast<size_t>(*pInputData);
         ++pInputData;
         do {
            StorageDataTypeCore targetData = *pTargetData;

            const size_t iBin = maskBits & iBinCombined;
            const FractionalDataType * pValues = &aModelUpdateTensor[iBin * cVectorLength];

            if(IsBinaryClassification(countCompilerClassificationTargetStates)) {
               const FractionalDataType smallChangeToPredictionScores = pValues[0];
               // this will apply a small fix to our existing ValidationPredictionScores, either positive or negative, whichever is needed
               const FractionalDataType validationPredictionScores = *pValidationPredictionScores + smallChangeToPredictionScores;
               *pValidationPredictionScores = validationPredictionScores;
               sumLogLoss += EbmStatistics::ComputeClassificationSingleCaseLogLossBinaryclass(validationPredictionScores, targetData);
               ++pValidationPredictionScores;
            } else {
               FractionalDataType sumExp = 0;
               size_t iVector = 0;
               do {
                  const FractionalDataType smallChangeToPredictionScores = pValues[iVector];
                  // this will apply a small fix to our existing validationPredictionScores, either positive or negative, whichever is needed

                  // TODO : this is no longer a prediction for multiclass.  It is a weight.  Change all instances of this naming. -> validationLogWeight
                  const FractionalDataType validationPredictionScores = *pValidationPredictionScores + smallChangeToPredictionScores;
                  *pValidationPredictionScores = validationPredictionScores;
                  sumExp += std::exp(validationPredictionScores);
                  ++pValidationPredictionScores;

                  // TODO : consider replacing iVector with pValidationPredictionScoresInnerEnd
                  ++iVector;
               } while(iVector < cVectorLength);
               // TODO: store the result of std::exp above for the index that we care about above since exp(..) is going to be expensive and probably even more expensive than an unconditional branch
               sumLogLoss += EbmStatistics::ComputeClassificationSingleCaseLogLossMulticlass(sumExp, pValidationPredictionScores - cVectorLength, targetData);
            }
            ++pTargetData;

            iBinCombined >>= cBitsPerItemMax;
            // TODO : try replacing cItemsRemaining with a pResidualErrorInnerLoopEnd which eliminates one subtact operation, but might make it harder for the compiler to optimize the loop away
            --cItemsRemaining;
         } while(0 != cItemsRemaining);
      }

      const FractionalDataType * const pValidationPredictionScoresEnd = pValidationPredictionScoresLastItemWhereNextLoopCouldDoFullLoopOrLessAndComplete + cVectorLength * cItemsPerBitPackDataUnit;
      if(pValidationPredictionScores < pValidationPredictionScoresEnd) {
         // first time through?
         EBM_ASSERT(0 == (pValidationPredictionScoresEnd - pValidationPredictionScores) % cVectorLength);
         cItemsRemaining = (pValidationPredictionScoresEnd - pValidationPredictionScores) / cVectorLength;
         EBM_ASSERT(0 < cItemsRemaining);
         EBM_ASSERT(cItemsRemaining <= cItemsPerBitPackDataUnit);
         goto one_last_loop_classification;
      }
      EBM_ASSERT(pValidationPredictionScores == pValidationPredictionScoresEnd); // after our second iteration we should have finished everything!

      LOG(TraceLevelVerbose, "Exited ValidationSetTargetAttributeLoop");
      return sumLogLoss;
   }
}

// a*PredictionScores = logOdds for binary classification
// a*PredictionScores = logWeights for multiclass classification
// a*PredictionScores = predictedValue for regression
template<unsigned int cInputBits, ptrdiff_t countCompilerClassificationTargetStates>
static FractionalDataType ValidationSetInputAttributeLoop(const AttributeCombinationCore * const pAttributeCombination, DataSetAttributeCombination * const pValidationSet, const FractionalDataType * const aModelUpdateTensor, const size_t cTargetStates) {
   if(cTargetStates <= 1 << 1) {
      return ValidationSetTargetAttributeLoop<cInputBits, 1, countCompilerClassificationTargetStates>(pAttributeCombination, pValidationSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 2) {
      return ValidationSetTargetAttributeLoop<cInputBits, 2, countCompilerClassificationTargetStates>(pAttributeCombination, pValidationSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 4) {
      return ValidationSetTargetAttributeLoop<cInputBits, 4, countCompilerClassificationTargetStates>(pAttributeCombination, pValidationSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 8) {
      return ValidationSetTargetAttributeLoop<cInputBits, 8, countCompilerClassificationTargetStates>(pAttributeCombination, pValidationSet, aModelUpdateTensor, cTargetStates);
   } else if(cTargetStates <= 1 << 16) {
      return ValidationSetTargetAttributeLoop<cInputBits, 16, countCompilerClassificationTargetStates>(pAttributeCombination, pValidationSet, aModelUpdateTensor, cTargetStates);
   } else if(static_cast<uint64_t>(cTargetStates) <= uint64_t { 1 } << 32) {
      // if this is a 32 bit system, then m_cStates can't be 0x100000000 or above, because we would have checked that when converting the 64 bit numbers into size_t, and m_cStates will be promoted to a 64 bit number for the above comparison
      // if this is a 64 bit system, then this comparison is fine

      // TODO : perhaps we should change m_cStates into m_iStateMax so that we don't need to do the above promotion to 64 bits.. we can make it <= 0xFFFFFFFF.  Write a function to fill the lowest bits with ones for any number of bits

      return ValidationSetTargetAttributeLoop<cInputBits, 32, countCompilerClassificationTargetStates>(pAttributeCombination, pValidationSet, aModelUpdateTensor, cTargetStates);
   } else {
      // our interface doesn't allow more than 64 bits, so even if size_t was bigger then we don't need to examine higher
      static_assert(63 == CountBitsRequiredPositiveMax<IntegerDataType>(), "");
      return ValidationSetTargetAttributeLoop<cInputBits, 64, countCompilerClassificationTargetStates>(pAttributeCombination, pValidationSet, aModelUpdateTensor, cTargetStates);
   }
}

union CachedThreadResourcesUnion {
   CachedTrainingThreadResources<true> regression;
   CachedTrainingThreadResources<false> classification;

   CachedThreadResourcesUnion(const bool bRegression, const size_t cVectorLength) {
      LOG(TraceLevelInfo, "Entered CachedThreadResourcesUnion: bRegression=%u, cVectorLength=%zu", static_cast<unsigned int>(bRegression), cVectorLength);
      if(bRegression) {
         // member classes inside a union requre explicit call to constructor
         new(&regression) CachedTrainingThreadResources<true>(cVectorLength);
      } else {
         // member classes inside a union requre explicit call to constructor
         new(&classification) CachedTrainingThreadResources<false>(cVectorLength);
      }
      LOG(TraceLevelInfo, "Exited CachedThreadResourcesUnion");
   }

   ~CachedThreadResourcesUnion() {
      // TODO: figure out why this is being called, and if that is bad!
      //LOG(TraceLevelError, "ERROR ~CachedThreadResourcesUnion called.  It's union destructors should be called explicitly");

      // we don't have enough information here to delete this object, so we do it from our caller
      // we still need this destructor for a technicality that it might be called
      // if there were an excpetion generated in the initializer list which it is constructed in
      // but we have been careful to ensure that the class we are including it in doesn't thow exceptions in the
      // initializer list
   }
};

// TODO: rename this EbmTrainingState
class TmlState {
public:
   const bool m_bRegression;
   const size_t m_cTargetStates;

   const size_t m_cAttributeCombinations;
   AttributeCombinationCore ** const m_apAttributeCombinations;

   // TODO : can we internalize these so that they are not pointers and are therefore subsumed into our class
   DataSetAttributeCombination * m_pTrainingSet;
   DataSetAttributeCombination * m_pValidationSet;

   const size_t m_cSamplingSets;

   SamplingMethod ** m_apSamplingSets;
   SegmentedRegionCore<ActiveDataType, FractionalDataType> ** m_apCurrentModel;
   SegmentedRegionCore<ActiveDataType, FractionalDataType> ** m_apBestModel;

   FractionalDataType m_bestModelMetric;

   SegmentedRegionCore<ActiveDataType, FractionalDataType> * const m_pSmallChangeToModelOverwriteSingleSamplingSet;
   SegmentedRegionCore<ActiveDataType, FractionalDataType> * const m_pSmallChangeToModelAccumulatedFromSamplingSets;

   const size_t m_cAttributes;
   // TODO : in the future, we can allocate this inside a function so that even the objects inside are const
   AttributeInternalCore * const m_aAttributes;

   CachedThreadResourcesUnion m_cachedThreadResourcesUnion;

   TmlState(const bool bRegression, const size_t cTargetStates, const size_t cAttributes, const size_t cAttributeCombinations, const size_t cSamplingSets)
      : m_bRegression(bRegression)
      , m_cTargetStates(cTargetStates)
      , m_cAttributeCombinations(cAttributeCombinations)
      , m_apAttributeCombinations(0 == cAttributeCombinations ? nullptr : AttributeCombinationCore::AllocateAttributeCombinations(cAttributeCombinations))
      , m_pTrainingSet(nullptr)
      , m_pValidationSet(nullptr)
      , m_cSamplingSets(cSamplingSets)
      , m_apSamplingSets(nullptr)
      , m_apCurrentModel(nullptr)
      , m_apBestModel(nullptr)
      , m_bestModelMetric(FractionalDataType { std::numeric_limits<FractionalDataType>::infinity() })
      , m_pSmallChangeToModelOverwriteSingleSamplingSet(SegmentedRegionCore<ActiveDataType, FractionalDataType>::Allocate(k_cDimensionsMax, GetVectorLengthFlatCore(cTargetStates)))
      , m_pSmallChangeToModelAccumulatedFromSamplingSets(SegmentedRegionCore<ActiveDataType, FractionalDataType>::Allocate(k_cDimensionsMax, GetVectorLengthFlatCore(cTargetStates)))
      , m_cAttributes(cAttributes)
      , m_aAttributes(0 == cAttributes || IsMultiplyError(sizeof(AttributeInternalCore), cAttributes) ? nullptr : static_cast<AttributeInternalCore *>(malloc(sizeof(AttributeInternalCore) * cAttributes)))
      // we catch any errors in the constructor, so this should not be able to throw
      , m_cachedThreadResourcesUnion(bRegression, GetVectorLengthFlatCore(cTargetStates)) {
   }
   
   ~TmlState() {
      LOG(TraceLevelInfo, "Entered ~EbmTrainingState");

      if(m_bRegression) {
         // member classes inside a union requre explicit call to destructor
         LOG(TraceLevelInfo, "~EbmTrainingState identified as regression type");
         m_cachedThreadResourcesUnion.regression.~CachedTrainingThreadResources();
      } else {
         // member classes inside a union requre explicit call to destructor
         LOG(TraceLevelInfo, "~EbmTrainingState identified as classification type");
         m_cachedThreadResourcesUnion.classification.~CachedTrainingThreadResources();
      }

      SamplingWithReplacement::FreeSamplingSets(m_cSamplingSets, m_apSamplingSets);

      delete m_pTrainingSet;
      delete m_pValidationSet;

      AttributeCombinationCore::FreeAttributeCombinations(m_cAttributeCombinations, m_apAttributeCombinations);

      free(m_aAttributes);

      DeleteSegmentsCore(m_cAttributeCombinations, m_apCurrentModel);
      DeleteSegmentsCore(m_cAttributeCombinations, m_apBestModel);
      SegmentedRegionCore<ActiveDataType, FractionalDataType>::Free(m_pSmallChangeToModelOverwriteSingleSamplingSet);
      SegmentedRegionCore<ActiveDataType, FractionalDataType>::Free(m_pSmallChangeToModelAccumulatedFromSamplingSets);

      LOG(TraceLevelInfo, "Exited ~EbmTrainingState");
   }

   bool Initialize(const IntegerDataType randomSeed, const EbmAttribute * const aAttributes, const EbmAttributeCombination * const aAttributeCombinations, const IntegerDataType * attributeCombinationIndexes, const size_t cTrainingCases, const void * const aTrainingTargets, const IntegerDataType * const aTrainingData, const FractionalDataType * const aTrainingPredictionScores, const size_t cValidationCases, const void * const aValidationTargets, const IntegerDataType * const aValidationData, const FractionalDataType * const aValidationPredictionScores) {
      LOG(TraceLevelInfo, "Entered EbmTrainingState::Initialize");
      try {
         if(m_bRegression) {
            if(m_cachedThreadResourcesUnion.regression.IsError()) {
               LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize m_cachedThreadResourcesUnion.regression.IsError()");
               return true;
            }
         } else {
            if(m_cachedThreadResourcesUnion.classification.IsError()) {
               LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize m_cachedThreadResourcesUnion.classification.IsError()");
               return true;
            }
         }

         if(0 != m_cAttributes && nullptr == m_aAttributes) {
            LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize 0 != m_cAttributes && nullptr == m_aAttributes");
            return true;
         }

         if(UNLIKELY(0 != m_cAttributeCombinations && nullptr == m_apAttributeCombinations)) {
            LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize 0 != m_cAttributeCombinations && nullptr == m_apAttributeCombinations");
            return true;
         }

         if(UNLIKELY(nullptr == m_pSmallChangeToModelOverwriteSingleSamplingSet)) {
            LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == m_pSmallChangeToModelOverwriteSingleSamplingSet");
            return true;
         }

         if(UNLIKELY(nullptr == m_pSmallChangeToModelAccumulatedFromSamplingSets)) {
            LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == m_pSmallChangeToModelAccumulatedFromSamplingSets");
            return true;
         }

         LOG(TraceLevelInfo, "EbmTrainingState::Initialize starting attribute processing");
         if(0 != m_cAttributes) {
            EBM_ASSERT(!IsMultiplyError(m_cAttributes, sizeof(*aAttributes))); // if this overflows then our caller should not have been able to allocate the array
            const EbmAttribute * pAttributeInitialize = aAttributes;
            const EbmAttribute * const pAttributeEnd = &aAttributes[m_cAttributes];
            EBM_ASSERT(pAttributeInitialize < pAttributeEnd);
            size_t iAttributeInitialize = 0;
            do {
               static_assert(AttributeTypeCore::OrdinalCore == static_cast<AttributeTypeCore>(AttributeTypeOrdinal), "AttributeTypeCore::OrdinalCore must have the same value as AttributeTypeOrdinal");
               static_assert(AttributeTypeCore::NominalCore == static_cast<AttributeTypeCore>(AttributeTypeNominal), "AttributeTypeCore::NominalCore must have the same value as AttributeTypeNominal");
               EBM_ASSERT(AttributeTypeOrdinal == pAttributeInitialize->attributeType || AttributeTypeNominal == pAttributeInitialize->attributeType);
               AttributeTypeCore attributeTypeCore = static_cast<AttributeTypeCore>(pAttributeInitialize->attributeType);

               IntegerDataType countStates = pAttributeInitialize->countStates;
               EBM_ASSERT(1 <= countStates); // we can handle 1 == cStates even though that's a degenerate case that shouldn't be trained on (dimensions with 1 state don't contribute anything since they always have the same value)
               if(!IsNumberConvertable<size_t, IntegerDataType>(countStates)) {
                  LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize !IsNumberConvertable<size_t, IntegerDataType>(countStates)");
                  return true;
               }
               size_t cStates = static_cast<size_t>(countStates);
               if(1 == cStates) {
                  LOG(TraceLevelError, "ERROR EbmTrainingState::Initialize Our higher level caller should filter out features with a single state since these provide no useful information");
               }

               EBM_ASSERT(0 == pAttributeInitialize->hasMissing || 1 == pAttributeInitialize->hasMissing);
               bool bMissing = 0 != pAttributeInitialize->hasMissing;

               // this is an in-place new, so there is no new memory allocated, and we already knew where it was going, so we don't need the resulting pointer returned
               new (&m_aAttributes[iAttributeInitialize]) AttributeInternalCore(cStates, iAttributeInitialize, attributeTypeCore, bMissing);
               // we don't allocate memory and our constructor doesn't have errors, so we shouldn't have an error here

               EBM_ASSERT(0 == pAttributeInitialize->hasMissing); // TODO : implement this, then remove this assert
               EBM_ASSERT(AttributeTypeOrdinal == pAttributeInitialize->attributeType); // TODO : implement this, then remove this assert

               ++iAttributeInitialize;
               ++pAttributeInitialize;
            } while(pAttributeEnd != pAttributeInitialize);
         }
         LOG(TraceLevelInfo, "EbmTrainingState::Initialize done attribute processing");

         LOG(TraceLevelInfo, "EbmTrainingState::Initialize starting attribute combination processing");
         if(0 != m_cAttributeCombinations) {
            const IntegerDataType * pAttributeCombinationIndex = attributeCombinationIndexes;
            size_t iAttributeCombination = 0;
            do {
               const EbmAttributeCombination * const pAttributeCombinationInterop = &aAttributeCombinations[iAttributeCombination];

               IntegerDataType countAttributesInCombination = pAttributeCombinationInterop->countAttributesInCombination;
               EBM_ASSERT(0 <= countAttributesInCombination);
               if(!IsNumberConvertable<size_t, IntegerDataType>(countAttributesInCombination)) {
                  LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize !IsNumberConvertable<size_t, IntegerDataType>(countAttributesInCombination)");
                  return true;
               }
               size_t cAttributesInCombination = static_cast<size_t>(countAttributesInCombination);
               EBM_ASSERT(cAttributesInCombination <= m_cAttributes); // we don't allow duplicates, so we can't have more attributes in an attribute combination than we have attributes.
               size_t cSignificantAttributesInCombination = 0;
               const IntegerDataType * const pAttributeCombinationIndexEnd = pAttributeCombinationIndex + cAttributesInCombination;
               if(UNLIKELY(0 == cAttributesInCombination)) {
                  LOG(TraceLevelError, "ERROR EbmTrainingState::Initialize Our higher level caller should filter out AttributeCombinations with zero attributes since these provide no useful information for training");
               } else {
                  assert(nullptr != attributeCombinationIndexes);
                  const IntegerDataType * pAttributeCombinationIndexTemp = pAttributeCombinationIndex;
                  do {
                     const IntegerDataType indexAttributeInterop = *pAttributeCombinationIndexTemp;
                     EBM_ASSERT(0 <= indexAttributeInterop);
                     if(!IsNumberConvertable<size_t, IntegerDataType>(indexAttributeInterop)) {
                        LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize !IsNumberConvertable<size_t, IntegerDataType>(indexAttributeInterop)");
                        return true;
                     }
                     const size_t iAttributeForCombination = static_cast<size_t>(indexAttributeInterop);
                     EBM_ASSERT(iAttributeForCombination < m_cAttributes);
                     AttributeInternalCore * const pInputAttribute = &m_aAttributes[iAttributeForCombination];
                     if(LIKELY(1 != pInputAttribute->m_cStates)) {
                        // if we have only 1 state, then we can eliminate the attribute from consideration since the resulting tensor loses one dimension but is otherwise indistinquishable from the original data
                        ++cSignificantAttributesInCombination;
                     } else {
                        LOG(TraceLevelError, "ERROR EbmTrainingState::Initialize Our higher level caller should filter out AttributeCombination features with a single state since these provide no useful information");
                     }
                     ++pAttributeCombinationIndexTemp;
                  } while(pAttributeCombinationIndexEnd != pAttributeCombinationIndexTemp);

                  // TODO : we can allow more dimensions, if some of the dimensions have only 1 state
                  if(k_cDimensionsMax < cSignificantAttributesInCombination) {
                     // if we try to run with more than k_cDimensionsMax we'll exceed our memory capacity, so let's exit here instead
                     LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize k_cDimensionsMax < cSignificantAttributesInCombination");
                     return true;
                  }
               }

               AttributeCombinationCore * pAttributeCombination = AttributeCombinationCore::Allocate(cSignificantAttributesInCombination, iAttributeCombination);
               if(nullptr == pAttributeCombination) {
                  LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == pAttributeCombination");
                  return true;
               }
               // assign our pointer directly to our array right now so that we can't loose the memory if we decide to exit due to an error below
               m_apAttributeCombinations[iAttributeCombination] = pAttributeCombination;

               if(LIKELY(0 == cSignificantAttributesInCombination)) {
                  // move our index forward to the next attribute.  
                  // We won't be executing the loop below that would otherwise increment it by the number of attributes in this attribute combination
                  pAttributeCombinationIndex = pAttributeCombinationIndexEnd;
               } else {
                  assert(nullptr != attributeCombinationIndexes);
                  size_t cTensorStates = 1;
                  AttributeCombinationCore::AttributeCombinationEntry * pAttributeCombinationEntry = &pAttributeCombination->m_AttributeCombinationEntry[0];
                  do {
                     const IntegerDataType indexAttributeInterop = *pAttributeCombinationIndex;
                     EBM_ASSERT(0 <= indexAttributeInterop);
                     EBM_ASSERT((IsNumberConvertable<size_t, IntegerDataType>(indexAttributeInterop))); // this was checked above
                     const size_t iAttributeForCombination = static_cast<size_t>(indexAttributeInterop);
                     EBM_ASSERT(iAttributeForCombination < m_cAttributes);
                     const AttributeInternalCore * const pInputAttribute = &m_aAttributes[iAttributeForCombination];
                     const size_t cStates = pInputAttribute->m_cStates;
                     if(LIKELY(1 != cStates)) {
                        // if we have only 1 state, then we can eliminate the attribute from consideration since the resulting tensor loses one dimension but is otherwise indistinquishable from the original data
                        pAttributeCombinationEntry->m_pAttribute = pInputAttribute;
                        ++pAttributeCombinationEntry;
                        if(IsMultiplyError(cTensorStates, cStates)) {
                           // if this overflows, we definetly won't be able to allocate it
                           LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize IsMultiplyError(cTensorStates, cStates)");
                           return true;
                        }
                        cTensorStates *= cStates;
                     }
                     ++pAttributeCombinationIndex;
                  } while(pAttributeCombinationIndexEnd != pAttributeCombinationIndex);
                  // if cSignificantAttributesInCombination is zero, don't both initializing pAttributeCombination->m_cItemsPerBitPackDataUnit
                  const size_t cBitsRequiredMin = CountBitsRequiredCore(cTensorStates - 1);
                  pAttributeCombination->m_cItemsPerBitPackDataUnit = GetCountItemsBitPacked(cBitsRequiredMin);
               }
               ++iAttributeCombination;
            } while(iAttributeCombination < m_cAttributeCombinations);
         }
         LOG(TraceLevelInfo, "EbmTrainingState::Initialize finished attribute combination processing");

         size_t cVectorLength = GetVectorLengthFlatCore(m_cTargetStates);

         LOG(TraceLevelInfo, "Entered DataSetAttributeCombination for m_pTrainingSet");
         if(0 != cTrainingCases) {
            m_pTrainingSet = new (std::nothrow) DataSetAttributeCombination(true, !m_bRegression, !m_bRegression, m_cAttributeCombinations, m_apAttributeCombinations, cTrainingCases, aTrainingData, aTrainingTargets, aTrainingPredictionScores, cVectorLength);
            if(nullptr == m_pTrainingSet || m_pTrainingSet->IsError()) {
               LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == m_pTrainingSet || m_pTrainingSet->IsError()");
               return true;
            }
         }
         LOG(TraceLevelInfo, "Exited DataSetAttributeCombination for m_pTrainingSet %p", static_cast<void *>(m_pTrainingSet));

         LOG(TraceLevelInfo, "Entered DataSetAttributeCombination for m_pValidationSet");
         if(0 != cValidationCases) {
            m_pValidationSet = new (std::nothrow) DataSetAttributeCombination(m_bRegression, !m_bRegression, !m_bRegression, m_cAttributeCombinations, m_apAttributeCombinations, cValidationCases, aValidationData, aValidationTargets, aValidationPredictionScores, cVectorLength);
            if(nullptr == m_pValidationSet || m_pValidationSet->IsError()) {
               LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == m_pValidationSet || m_pValidationSet->IsError()");
               return true;
            }
         }
         LOG(TraceLevelInfo, "Exited DataSetAttributeCombination for m_pValidationSet %p", static_cast<void *>(m_pValidationSet));

         RandomStream randomStream(randomSeed);

         EBM_ASSERT(nullptr == m_apSamplingSets);
         if(0 != cTrainingCases) {
            m_apSamplingSets = SamplingWithReplacement::GenerateSamplingSets(&randomStream, m_pTrainingSet, m_cSamplingSets);
            if(UNLIKELY(nullptr == m_apSamplingSets)) {
               LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == m_apSamplingSets");
               return true;
            }
         }

         EBM_ASSERT(nullptr == m_apCurrentModel);
         EBM_ASSERT(nullptr == m_apBestModel);
         if(0 != m_cAttributeCombinations && (m_bRegression || 2 <= m_cTargetStates)) {
            m_apCurrentModel = InitializeSegmentsCore(m_cAttributeCombinations, m_apAttributeCombinations, cVectorLength);
            if(nullptr == m_apCurrentModel) {
               LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == m_apCurrentModel");
               return true;
            }
            m_apBestModel = InitializeSegmentsCore(m_cAttributeCombinations, m_apAttributeCombinations, cVectorLength);
            if(nullptr == m_apBestModel) {
               LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize nullptr == m_apBestModel");
               return true;
            }
         }

         if(m_bRegression) {
            if(0 != cTrainingCases) {
               InitializeResiduals<k_Regression>(cTrainingCases, aTrainingTargets, aTrainingPredictionScores, m_pTrainingSet->GetResidualPointer(), 0);
            }
            if(0 != cValidationCases) {
               InitializeResiduals<k_Regression>(cValidationCases, aValidationTargets, aValidationPredictionScores, m_pValidationSet->GetResidualPointer(), 0);
            }
         } else {
            if(2 == m_cTargetStates) {
               if(0 != cTrainingCases) {
                  InitializeResiduals<2>(cTrainingCases, aTrainingTargets, aTrainingPredictionScores, m_pTrainingSet->GetResidualPointer(), m_cTargetStates);
               }
            } else {
               if(0 != cTrainingCases) {
                  InitializeResiduals<k_DynamicClassification>(cTrainingCases, aTrainingTargets, aTrainingPredictionScores, m_pTrainingSet->GetResidualPointer(), m_cTargetStates);
               }
            }
         }
         
         LOG(TraceLevelInfo, "Exited EbmTrainingState::Initialize");
         return false;
      } catch (...) {
         // this is here to catch exceptions from RandomStream randomStream(randomSeed), but it could also catch errors if we put any other C++ types in here later
         LOG(TraceLevelWarning, "WARNING EbmTrainingState::Initialize exception");
         return true;
      }
   }
};

#ifndef NDEBUG
void CheckTargets(const size_t cTargetStates, const size_t cCases, const void * const aTargets) {
   if(0 != cCases) {
      if(0 == cTargetStates) {
         // regression!

         const FractionalDataType * pTarget = static_cast<const FractionalDataType *>(aTargets);
         const FractionalDataType * const pTargetEnd = pTarget + cCases;
         do {
            const FractionalDataType data = *pTarget;
            EBM_ASSERT(!std::isnan(data));
            EBM_ASSERT(!std::isinf(data));
            ++pTarget;
         } while(pTargetEnd != pTarget);
      } else {
         // classification

         const IntegerDataType * pTarget = static_cast<const IntegerDataType *>(aTargets);
         const IntegerDataType * const pTargetEnd = pTarget + cCases;
         do {
            const IntegerDataType data = *pTarget;
            EBM_ASSERT(0 <= data);
            EBM_ASSERT((IsNumberConvertable<size_t, IntegerDataType>(data))); // data must be lower than cTargetStates and cTargetStates fits into a size_t which we checked earlier
            EBM_ASSERT(static_cast<size_t>(data) < cTargetStates);
            ++pTarget;
         } while(pTargetEnd != pTarget);
      }
   }
}
#endif // NDEBUG

// a*PredictionScores = logOdds for binary classification
// a*PredictionScores = logWeights for multiclass classification
// a*PredictionScores = predictedValue for regression
TmlState * AllocateCore(bool bRegression, IntegerDataType randomSeed, IntegerDataType countAttributes, const EbmAttribute * attributes, IntegerDataType countAttributeCombinations, const EbmAttributeCombination * attributeCombinations, const IntegerDataType * attributeCombinationIndexes, IntegerDataType countTargetStates, IntegerDataType countTrainingCases, const void * trainingTargets, const IntegerDataType * trainingData, const FractionalDataType * trainingPredictionScores, IntegerDataType countValidationCases, const void * validationTargets, const IntegerDataType * validationData, const FractionalDataType * validationPredictionScores, IntegerDataType countInnerBags) {
   // randomSeed can be any value
   EBM_ASSERT(0 <= countAttributes);
   EBM_ASSERT(0 == countAttributes || nullptr != attributes);
   EBM_ASSERT(0 <= countAttributeCombinations);
   EBM_ASSERT(0 == countAttributeCombinations || nullptr != attributeCombinations);
   // attributeCombinationIndexes -> it's legal for attributeCombinationIndexes to be nullptr if there are no attributes indexed by our attributeCombinations.  AttributeCombinations can have zero attributes, so it could be legal for this to be null even if there are attributeCombinations
   EBM_ASSERT(bRegression && 0 == countTargetStates || !bRegression && (1 <= countTargetStates || 0 == countTargetStates && 0 == countTrainingCases && 0 == countValidationCases));
   EBM_ASSERT(0 <= countTrainingCases);
   EBM_ASSERT(0 == countTrainingCases || nullptr != trainingTargets);
   EBM_ASSERT(0 == countTrainingCases || 0 == countAttributes || nullptr != trainingData);
   // trainingPredictionScores can be null
   EBM_ASSERT(0 <= countValidationCases); // TODO: change this to make it possible to be 0 if the user doesn't want a validation set
   EBM_ASSERT(0 == countValidationCases || nullptr != validationTargets); // TODO: change this to make it possible to have no validation set
   EBM_ASSERT(0 == countValidationCases || 0 == countAttributes || nullptr != validationData); // TODO: change this to make it possible to have no validation set
   // validationPredictionScores can be null
   EBM_ASSERT(0 <= countInnerBags); // 0 means use the full set (good value).  1 means make a single bag (this is useless but allowed for comparison purposes).  2+ are good numbers of bag

   if(!IsNumberConvertable<size_t, IntegerDataType>(countAttributes)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore !IsNumberConvertable<size_t, IntegerDataType>(countAttributes)");
      return nullptr;
   }
   if(!IsNumberConvertable<size_t, IntegerDataType>(countAttributeCombinations)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore !IsNumberConvertable<size_t, IntegerDataType>(countAttributeCombinations)");
      return nullptr;
   }
   if(!IsNumberConvertable<size_t, IntegerDataType>(countTargetStates)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore !IsNumberConvertable<size_t, IntegerDataType>(countTargetStates)");
      return nullptr;
   }
   if(!IsNumberConvertable<size_t, IntegerDataType>(countTrainingCases)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore !IsNumberConvertable<size_t, IntegerDataType>(countTrainingCases)");
      return nullptr;
   }
   if(!IsNumberConvertable<size_t, IntegerDataType>(countValidationCases)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore !IsNumberConvertable<size_t, IntegerDataType>(countValidationCases)");
      return nullptr;
   }
   if(!IsNumberConvertable<size_t, IntegerDataType>(countInnerBags)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore !IsNumberConvertable<size_t, IntegerDataType>(countInnerBags)");
      return nullptr;
   }

   size_t cAttributes = static_cast<size_t>(countAttributes);
   size_t cAttributeCombinations = static_cast<size_t>(countAttributeCombinations);
   size_t cTargetStates = static_cast<size_t>(countTargetStates);
   size_t cTrainingCases = static_cast<size_t>(countTrainingCases);
   size_t cValidationCases = static_cast<size_t>(countValidationCases);
   size_t cInnerBags = static_cast<size_t>(countInnerBags);

   size_t cVectorLength = GetVectorLengthFlatCore(cTargetStates);

   if(IsMultiplyError(cVectorLength, cTrainingCases)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore IsMultiplyError(cVectorLength, cTrainingCases)");
      return nullptr;
   }
   if(IsMultiplyError(cVectorLength, cValidationCases)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore IsMultiplyError(cVectorLength, cValidationCases)");
      return nullptr;
   }

#ifndef NDEBUG
   CheckTargets(cTargetStates, cTrainingCases, trainingTargets);
   CheckTargets(cTargetStates, cValidationCases, validationTargets);
#endif // NDEBUG

   LOG(TraceLevelInfo, "Entered EbmTrainingState");
   TmlState * const pTmlState = new (std::nothrow) TmlState(bRegression, cTargetStates, cAttributes, cAttributeCombinations, cInnerBags);
   LOG(TraceLevelInfo, "Exited EbmTrainingState %p", static_cast<void *>(pTmlState));
   if(UNLIKELY(nullptr == pTmlState)) {
      LOG(TraceLevelWarning, "WARNING AllocateCore nullptr == pTmlState");
      return nullptr;
   }
   if(UNLIKELY(pTmlState->Initialize(randomSeed, attributes, attributeCombinations, attributeCombinationIndexes, cTrainingCases, trainingTargets, trainingData, trainingPredictionScores, cValidationCases, validationTargets, validationData, validationPredictionScores))) {
      LOG(TraceLevelWarning, "WARNING AllocateCore pTmlState->Initialize");
      delete pTmlState;
      return nullptr;
   }
   return pTmlState;
}

EBMCORE_IMPORT_EXPORT PEbmTraining EBMCORE_CALLING_CONVENTION InitializeTrainingRegression(IntegerDataType randomSeed, IntegerDataType countAttributes, const EbmAttribute * attributes, IntegerDataType countAttributeCombinations, const EbmAttributeCombination * attributeCombinations, const IntegerDataType * attributeCombinationIndexes, IntegerDataType countTrainingCases, const FractionalDataType * trainingTargets, const IntegerDataType * trainingData, const FractionalDataType * trainingPredictionScores, IntegerDataType countValidationCases, const FractionalDataType * validationTargets, const IntegerDataType * validationData, const FractionalDataType * validationPredictionScores, IntegerDataType countInnerBags) {
   LOG(TraceLevelInfo, "Entered InitializeTrainingRegression: randomSeed=%" IntegerDataTypePrintf ", countAttributes=%" IntegerDataTypePrintf ", attributes=%p, countAttributeCombinations=%" IntegerDataTypePrintf ", attributeCombinations=%p, attributeCombinationIndexes=%p, countTrainingCases=%" IntegerDataTypePrintf ", trainingTargets=%p, trainingData=%p, trainingPredictionScores=%p, countValidationCases=%" IntegerDataTypePrintf ", validationTargets=%p, validationData=%p, validationPredictionScores=%p, countInnerBags=%" IntegerDataTypePrintf, randomSeed, countAttributes, static_cast<const void *>(attributes), countAttributeCombinations, static_cast<const void *>(attributeCombinations), static_cast<const void *>(attributeCombinationIndexes), countTrainingCases, static_cast<const void *>(trainingTargets), static_cast<const void *>(trainingData), static_cast<const void *>(trainingPredictionScores), countValidationCases, static_cast<const void *>(validationTargets), static_cast<const void *>(validationData), static_cast<const void *>(validationPredictionScores), countInnerBags);
   PEbmTraining pEbmTraining = reinterpret_cast<PEbmTraining>(AllocateCore(true, randomSeed, countAttributes, attributes, countAttributeCombinations, attributeCombinations, attributeCombinationIndexes, 0, countTrainingCases, trainingTargets, trainingData, trainingPredictionScores, countValidationCases, validationTargets, validationData, validationPredictionScores, countInnerBags));
   LOG(TraceLevelInfo, "Exited InitializeTrainingRegression %p", static_cast<void *>(pEbmTraining));
   return pEbmTraining;
}

EBMCORE_IMPORT_EXPORT PEbmTraining EBMCORE_CALLING_CONVENTION InitializeTrainingClassification(IntegerDataType randomSeed, IntegerDataType countAttributes, const EbmAttribute * attributes, IntegerDataType countAttributeCombinations, const EbmAttributeCombination * attributeCombinations, const IntegerDataType * attributeCombinationIndexes, IntegerDataType countTargetStates, IntegerDataType countTrainingCases, const IntegerDataType * trainingTargets, const IntegerDataType * trainingData, const FractionalDataType * trainingPredictionScores, IntegerDataType countValidationCases, const IntegerDataType * validationTargets, const IntegerDataType * validationData, const FractionalDataType * validationPredictionScores, IntegerDataType countInnerBags) {
   LOG(TraceLevelInfo, "Entered InitializeTrainingClassification: randomSeed=%" IntegerDataTypePrintf ", countAttributes=%" IntegerDataTypePrintf ", attributes=%p, countAttributeCombinations=%" IntegerDataTypePrintf ", attributeCombinations=%p, attributeCombinationIndexes=%p, countTargetStates=%" IntegerDataTypePrintf ", countTrainingCases=%" IntegerDataTypePrintf ", trainingTargets=%p, trainingData=%p, trainingPredictionScores=%p, countValidationCases=%" IntegerDataTypePrintf ", validationTargets=%p, validationData=%p, validationPredictionScores=%p, countInnerBags=%" IntegerDataTypePrintf, randomSeed, countAttributes, static_cast<const void *>(attributes), countAttributeCombinations, static_cast<const void *>(attributeCombinations), static_cast<const void *>(attributeCombinationIndexes), countTargetStates, countTrainingCases, static_cast<const void *>(trainingTargets), static_cast<const void *>(trainingData), static_cast<const void *>(trainingPredictionScores), countValidationCases, static_cast<const void *>(validationTargets), static_cast<const void *>(validationData), static_cast<const void *>(validationPredictionScores), countInnerBags);
   PEbmTraining pEbmTraining = reinterpret_cast<PEbmTraining>(AllocateCore(false, randomSeed, countAttributes, attributes, countAttributeCombinations, attributeCombinations, attributeCombinationIndexes, countTargetStates, countTrainingCases, trainingTargets, trainingData, trainingPredictionScores, countValidationCases, validationTargets, validationData, validationPredictionScores, countInnerBags));
   LOG(TraceLevelInfo, "Exited InitializeTrainingClassification %p", static_cast<void *>(pEbmTraining));
   return pEbmTraining;
}

template<bool bRegression>
TML_INLINE CachedTrainingThreadResources<bRegression> * GetCachedThreadResources(TmlState * pTmlState);
template<>
TML_INLINE CachedTrainingThreadResources<false> * GetCachedThreadResources<false>(TmlState * pTmlState) {
   return &pTmlState->m_cachedThreadResourcesUnion.classification;
}
template<>
TML_INLINE CachedTrainingThreadResources<true> * GetCachedThreadResources<true>(TmlState * pTmlState) {
   return &pTmlState->m_cachedThreadResourcesUnion.regression;
}

// a*PredictionScores = logOdds for binary classification
// a*PredictionScores = logWeights for multiclass classification
// a*PredictionScores = predictedValue for regression
template<ptrdiff_t countCompilerClassificationTargetStates>
static FractionalDataType * GenerateModelUpdatePerTargetStates(TmlState * const pTmlState, const size_t iAttributeCombination, const FractionalDataType learningRate, const size_t cTreeSplitsMax, const size_t cCasesRequiredForSplitParentMin, const FractionalDataType * const aTrainingWeights, const FractionalDataType * const aValidationWeights, FractionalDataType * const pGainReturn) {
   // TODO remove this after we use aTrainingWeights and aValidationWeights into the GenerateModelUpdatePerTargetStates function
   UNUSED(aTrainingWeights);
   UNUSED(aValidationWeights);

   LOG(TraceLevelVerbose, "Entered GenerateModelUpdatePerTargetStates");

   if(nullptr != pGainReturn) {
      *pGainReturn = 0; // always set this, even on errors.  We might as well do it here at the top
   }

   const size_t cSamplingSetsAfterZero = (0 == pTmlState->m_cSamplingSets) ? 1 : pTmlState->m_cSamplingSets;
   CachedTrainingThreadResources<IsRegression(countCompilerClassificationTargetStates)> * const pCachedThreadResources = GetCachedThreadResources<IsRegression(countCompilerClassificationTargetStates)>(pTmlState);
   const AttributeCombinationCore * const pAttributeCombination = pTmlState->m_apAttributeCombinations[iAttributeCombination];
   const size_t cDimensions = pAttributeCombination->m_cAttributes;

   pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->SetCountDimensions(cDimensions);
   pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Reset();

   // if pTmlState->m_apSamplingSets is nullptr, then we should have zero training cases
   // we can't be partially constructed here since then we wouldn't have returned our state pointer to our caller
   EBM_ASSERT(!pTmlState->m_apSamplingSets == !pTmlState->m_pTrainingSet); // m_pTrainingSet and m_apSamplingSets should be the same null-ness in that they should either both be null or both be non-null (although different non-null values)
   FractionalDataType totalGain = 0;
   if(nullptr != pTmlState->m_apSamplingSets) {
      pTmlState->m_pSmallChangeToModelOverwriteSingleSamplingSet->SetCountDimensions(cDimensions);

      for(size_t iSamplingSet = 0; iSamplingSet < cSamplingSetsAfterZero; ++iSamplingSet) {
         FractionalDataType gain = 0;
         if(0 == pAttributeCombination->m_cAttributes) {
            if(TrainZeroDimensional<countCompilerClassificationTargetStates>(pCachedThreadResources, pTmlState->m_apSamplingSets[iSamplingSet], pTmlState->m_pSmallChangeToModelOverwriteSingleSamplingSet, pTmlState->m_cTargetStates)) {
               return nullptr;
            }
         } else if(1 == pAttributeCombination->m_cAttributes) {
            if(TrainSingleDimensional<countCompilerClassificationTargetStates>(pCachedThreadResources, pTmlState->m_apSamplingSets[iSamplingSet], pAttributeCombination, cTreeSplitsMax, cCasesRequiredForSplitParentMin, pTmlState->m_pSmallChangeToModelOverwriteSingleSamplingSet, &gain, pTmlState->m_cTargetStates)) {
               return nullptr;
            }
         } else {
            if(TrainMultiDimensional<countCompilerClassificationTargetStates, 0>(pCachedThreadResources, pTmlState->m_apSamplingSets[iSamplingSet], pAttributeCombination, pTmlState->m_pSmallChangeToModelOverwriteSingleSamplingSet, pTmlState->m_cTargetStates)) {
               return nullptr;
            }
         }
         totalGain += gain;
         // TODO : when we thread this code, let's have each thread take a lock and update the combined line segment.  They'll each do it while the others are working, so there should be no blocking and our final result won't require adding by the main thread
         if(pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Add(*pTmlState->m_pSmallChangeToModelOverwriteSingleSamplingSet)) {
            return nullptr;
         }
      }
      totalGain /= static_cast<FractionalDataType>(cSamplingSetsAfterZero);

      LOG(TraceLevelVerbose, "GenerateModelUpdatePerTargetStates done sampling set loop");

      // we need to divide by the number of sampling sets that we constructed this from.
      // We also need to slow down our growth so that the more relevant Attributes get a chance to grow first so we multiply by a user defined learning rate
      if(IsClassification(countCompilerClassificationTargetStates)) {
#ifdef EXPAND_BINARY_LOGITS
         constexpr bool bExpandBinaryLogits = true;
#else // EXPAND_BINARY_LOGITS
         constexpr bool bExpandBinaryLogits = false;
#endif // EXPAND_BINARY_LOGITS

         //if(0 <= k_iZeroResidual || 2 == pTmlState->m_cTargetStates && bExpandBinaryLogits) {
         //   EBM_ASSERT(2 <= pTmlState->m_cTargetStates);
         //   // TODO : for classification with residual zeroing, is our learning rate essentially being inflated as pTmlState->m_cTargetStates goes up?  If so, maybe we should divide by pTmlState->m_cTargetStates here to keep learning rates as equivalent as possible..  Actually, I think the real solution here is that 
         //   pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Multiply(learningRate / cSamplingSetsAfterZero * (pTmlState->m_cTargetStates - 1) / pTmlState->m_cTargetStates);
         //} else {
         //   // TODO : for classification, is our learning rate essentially being inflated as pTmlState->m_cTargetStates goes up?  If so, maybe we should divide by pTmlState->m_cTargetStates here to keep learning rates equivalent as possible
         //   pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Multiply(learningRate / cSamplingSetsAfterZero);
         //}

         constexpr bool bDividing = bExpandBinaryLogits && 2 == countCompilerClassificationTargetStates;
         if(bDividing) {
            pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Multiply(learningRate / cSamplingSetsAfterZero / 2);
         } else {
            pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Multiply(learningRate / cSamplingSetsAfterZero);
         }
      } else {
         pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Multiply(learningRate / cSamplingSetsAfterZero);
      }
   }

   if(0 != cDimensions) {
      // pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets was reset above, so it isn't expanded.  We want to expand it before calling ValidationSetInputAttributeLoop so that we can more efficiently lookup the results by index rather than do a binary search
      size_t acDivisionIntegersEnd[k_cDimensionsMax];
      size_t iDimension = 0;
      do {
         acDivisionIntegersEnd[iDimension] = pAttributeCombination->m_AttributeCombinationEntry[iDimension].m_pAttribute->m_cStates;
         ++iDimension;
      } while(iDimension < cDimensions);
      if(pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->Expand(acDivisionIntegersEnd)) {
         return nullptr;
      }
   }

   if(nullptr != pGainReturn) {
      *pGainReturn = totalGain;
   }

   LOG(TraceLevelVerbose, "Exited GenerateModelUpdatePerTargetStates");
   return pTmlState->m_pSmallChangeToModelAccumulatedFromSamplingSets->m_aValues;
}

template<ptrdiff_t iPossibleCompilerOptimizedTargetStates>
TML_INLINE FractionalDataType * CompilerRecursiveGenerateModelUpdate(const size_t cRuntimeTargetStates, TmlState * const pTmlState, const size_t iAttributeCombination, const FractionalDataType learningRate, const size_t cTreeSplitsMax, const size_t cCasesRequiredForSplitParentMin, const FractionalDataType * const aTrainingWeights, const FractionalDataType * const aValidationWeights, FractionalDataType * const pGainReturn) {
   EBM_ASSERT(IsClassification(iPossibleCompilerOptimizedTargetStates));
   if(iPossibleCompilerOptimizedTargetStates == cRuntimeTargetStates) {
      EBM_ASSERT(cRuntimeTargetStates <= k_cCompilerOptimizedTargetStatesMax);
      return GenerateModelUpdatePerTargetStates<iPossibleCompilerOptimizedTargetStates>(pTmlState, iAttributeCombination, learningRate, cTreeSplitsMax, cCasesRequiredForSplitParentMin, aTrainingWeights, aValidationWeights, pGainReturn);
   } else {
      return CompilerRecursiveGenerateModelUpdate<iPossibleCompilerOptimizedTargetStates + 1>(cRuntimeTargetStates, pTmlState, iAttributeCombination, learningRate, cTreeSplitsMax, cCasesRequiredForSplitParentMin, aTrainingWeights, aValidationWeights, pGainReturn);
   }
}

template<>
TML_INLINE FractionalDataType * CompilerRecursiveGenerateModelUpdate<k_cCompilerOptimizedTargetStatesMax + 1>(const size_t cRuntimeTargetStates, TmlState * const pTmlState, const size_t iAttributeCombination, const FractionalDataType learningRate, const size_t cTreeSplitsMax, const size_t cCasesRequiredForSplitParentMin, const FractionalDataType * const aTrainingWeights, const FractionalDataType * const aValidationWeights, FractionalDataType * const pGainReturn) {
   UNUSED(cRuntimeTargetStates);
   // it is logically possible, but uninteresting to have a classification with 1 target state, so let our runtime system handle those unlikley and uninteresting cases
   EBM_ASSERT(k_cCompilerOptimizedTargetStatesMax < cRuntimeTargetStates);
   return GenerateModelUpdatePerTargetStates<k_DynamicClassification>(pTmlState, iAttributeCombination, learningRate, cTreeSplitsMax, cCasesRequiredForSplitParentMin, aTrainingWeights, aValidationWeights, pGainReturn);
}

// we made this a global because if we had put this variable inside the EbmTrainingState object, then we would need to dereference that before getting the count.  By making this global we can send a log message incase a bad EbmTrainingState object is sent into us
// we only decrease the count if the count is non-zero, so at worst if there is a race condition then we'll output this log message more times than desired, but we can live with that
static unsigned int g_cLogGenerateModelUpdateParametersMessages = 10;

// TODO : we can make GenerateModelUpdate callable by multiple threads so that this step could be parallelized before making a decision and applying one of the updates.  Right now we're accessing scratch space in the pTmlState object, but we can move that to a thread resident object.  Do do this, we would need to have our caller allocate our tensor, but that is a manageable operation
EBMCORE_IMPORT_EXPORT FractionalDataType * EBMCORE_CALLING_CONVENTION GenerateModelUpdate(PEbmTraining ebmTraining, IntegerDataType indexAttributeCombination, FractionalDataType learningRate, IntegerDataType countTreeSplitsMax, IntegerDataType countCasesRequiredForSplitParentMin, const FractionalDataType * trainingWeights, const FractionalDataType * validationWeights, FractionalDataType * gainReturn) {
   LOG_COUNTED(&g_cLogGenerateModelUpdateParametersMessages, TraceLevelInfo, TraceLevelVerbose, "GenerateModelUpdate parameters: ebmTraining=%p, indexAttributeCombination=%" IntegerDataTypePrintf ", learningRate=%" FractionalDataTypePrintf ", countTreeSplitsMax=%" IntegerDataTypePrintf ", countCasesRequiredForSplitParentMin=%" IntegerDataTypePrintf ", trainingWeights=%p, validationWeights=%p, gainReturn=%p", static_cast<void *>(ebmTraining), indexAttributeCombination, learningRate, countTreeSplitsMax, countCasesRequiredForSplitParentMin, static_cast<const void *>(trainingWeights), static_cast<const void *>(validationWeights), static_cast<void *>(gainReturn));

   TmlState * pTmlState = reinterpret_cast<TmlState *>(ebmTraining);
   EBM_ASSERT(nullptr != pTmlState);

   EBM_ASSERT(0 <= indexAttributeCombination);
   EBM_ASSERT((IsNumberConvertable<size_t, IntegerDataType>(indexAttributeCombination))); // we wouldn't have allowed the creation of an attribute set larger than size_t
   size_t iAttributeCombination = static_cast<size_t>(indexAttributeCombination);
   EBM_ASSERT(iAttributeCombination < pTmlState->m_cAttributeCombinations);
   EBM_ASSERT(nullptr != pTmlState->m_apAttributeCombinations); // this is true because 0 < pTmlState->m_cAttributeCombinations since our caller needs to pass in a valid indexAttributeCombination to this function

   LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogEnterGenerateModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Entered GenerateModelUpdate");

   EBM_ASSERT(!std::isnan(learningRate));
   EBM_ASSERT(!std::isinf(learningRate));

   EBM_ASSERT(0 <= countTreeSplitsMax);
   size_t cTreeSplitsMax = static_cast<size_t>(countTreeSplitsMax);
   if(!IsNumberConvertable<size_t, IntegerDataType>(countTreeSplitsMax)) {
      // we can never exceed a size_t number of splits, so let's just set it to the maximum if we were going to overflow because it will generate the same results as if we used the true number
      cTreeSplitsMax = std::numeric_limits<size_t>::max();
   }

   EBM_ASSERT(0 <= countCasesRequiredForSplitParentMin); // if there is 1 case, then it can't be split, but we accept this input from our user
   size_t cCasesRequiredForSplitParentMin = static_cast<size_t>(countCasesRequiredForSplitParentMin);
   if(!IsNumberConvertable<size_t, IntegerDataType>(countCasesRequiredForSplitParentMin)) {
      // we can never exceed a size_t number of cases, so let's just set it to the maximum if we were going to overflow because it will generate the same results as if we used the true number
      cCasesRequiredForSplitParentMin = std::numeric_limits<size_t>::max();
   }

   EBM_ASSERT(nullptr == trainingWeights); // TODO : implement this later
   EBM_ASSERT(nullptr == validationWeights); // TODO : implement this later
   // validationMetricReturn can be nullptr

   FractionalDataType * aModelUpdateTensor;
   if(pTmlState->m_bRegression) {
      aModelUpdateTensor = GenerateModelUpdatePerTargetStates<k_Regression>(pTmlState, iAttributeCombination, learningRate, cTreeSplitsMax, cCasesRequiredForSplitParentMin, trainingWeights, validationWeights, gainReturn);
   } else {
      const size_t cTargetStates = pTmlState->m_cTargetStates;
      if(cTargetStates <= 1) {
         // if there is only 1 target state for classification, then we can predict the output with 100% accuracy.  The model is a tensor with zero length array logits, which means for our representation that we have zero items in the array total.
         // since we can predit the output with 100% accuracy, our gain will be 0.
         if(nullptr != gainReturn) {
            *gainReturn = 0;
         }
         LOG(TraceLevelWarning, "WARNING GenerateModelUpdate cTargetStates <= 1");
         return nullptr;
      }
      aModelUpdateTensor = CompilerRecursiveGenerateModelUpdate<2>(cTargetStates, pTmlState, iAttributeCombination, learningRate, cTreeSplitsMax, cCasesRequiredForSplitParentMin, trainingWeights, validationWeights, gainReturn);
   }

   if(nullptr != gainReturn) {
      EBM_ASSERT(*gainReturn <= 0.000000001);
      LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogExitGenerateModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Exited GenerateModelUpdate %" FractionalDataTypePrintf, *gainReturn);
   } else {
      LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogExitGenerateModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Exited GenerateModelUpdate no gain");
   }
   if(nullptr == aModelUpdateTensor) {
      LOG(TraceLevelWarning, "WARNING GenerateModelUpdate returned nullptr");
   }
   return aModelUpdateTensor;
}

// a*PredictionScores = logOdds for binary classification
// a*PredictionScores = logWeights for multiclass classification
// a*PredictionScores = predictedValue for regression
template<ptrdiff_t countCompilerClassificationTargetStates>
static IntegerDataType ApplyModelUpdatePerTargetStates(TmlState * const pTmlState, const size_t iAttributeCombination, const FractionalDataType * const aModelUpdateTensor, FractionalDataType * const pValidationMetricReturn) {
   LOG(TraceLevelVerbose, "Entered ApplyModelUpdatePerTargetStates");

   EBM_ASSERT(nullptr != pTmlState->m_apCurrentModel); // m_apCurrentModel can be null if there are no attributeCombinations (but we have an attribute combination index), or if the target has 1 or 0 states (which we check before calling this function), so it shouldn't be possible to be null
   EBM_ASSERT(nullptr != pTmlState->m_apBestModel); // m_apCurrentModel can be null if there are no attributeCombinations (but we have an attribute combination index), or if the target has 1 or 0 states (which we check before calling this function), so it shouldn't be possible to be null
   EBM_ASSERT(nullptr != aModelUpdateTensor); // aModelUpdateTensor is checked for nullptr before calling this function   

   pTmlState->m_apCurrentModel[iAttributeCombination]->AddExpanded(aModelUpdateTensor);

   const AttributeCombinationCore * const pAttributeCombination = pTmlState->m_apAttributeCombinations[iAttributeCombination];

   // if the count of training cases is zero, then pTmlState->m_pTrainingSet will be nullptr
   if(nullptr != pTmlState->m_pTrainingSet) {
      // TODO : move the target bits branch inside TrainingSetInputAttributeLoop to here outside instead of the attribute combination.  The target # of bits is extremely predictable and so we get to only process one sub branch of code below that.  If we do attribute combinations here then we have to keep in instruction cache a whole bunch of options
      TrainingSetInputAttributeLoop<1, countCompilerClassificationTargetStates>(pAttributeCombination, pTmlState->m_pTrainingSet, aModelUpdateTensor, pTmlState->m_cTargetStates);
   }

   FractionalDataType modelMetric = 0;
   if(nullptr != pTmlState->m_pValidationSet) {
      // if there is no validation set, it's pretty hard to know what the metric we'll get for our validation set
      // we could in theory return anything from zero to infinity or possibly, NaN (probably legally the best), but we return 0 here
      // because we want to kick our caller out of any loop it might be calling us in.  Infinity and NaN are odd values that might cause problems in
      // a caller that isn't expecting those values, so 0 is the safest option, and our caller can avoid the situation entirely by not calling
      // us with zero count validation sets

      // if the count of validation set is zero, then pTmlState->m_pValidationSet will be nullptr
      // if the count of training cases is zero, don't update the best model (it will stay as all zeros), and we don't need to update our non-existant training set either
      // C++ doesn't define what happens when you compare NaN to annother number.  It probably follows IEEE 754, but it isn't guaranteed, so let's check for zero cases in the validation set this better way   https://stackoverflow.com/questions/31225264/what-is-the-result-of-comparing-a-number-with-nan

      // TODO : move the target bits branch inside TrainingSetInputAttributeLoop to here outside instead of the attribute combination.  The target # of bits is extremely predictable and so we get to only process one sub branch of code below that.  If we do attribute combinations here then we have to keep in instruction cache a whole bunch of options

      modelMetric = ValidationSetInputAttributeLoop<1, countCompilerClassificationTargetStates>(pAttributeCombination, pTmlState->m_pValidationSet, aModelUpdateTensor, pTmlState->m_cTargetStates);

      // modelMetric is either logloss (classification) or rmse (regression).  In either case we want to minimize it.
      if(LIKELY(modelMetric < pTmlState->m_bestModelMetric)) {
         // we keep on improving, so this is more likely than not, and we'll exit if it becomes negative a lot
         pTmlState->m_bestModelMetric = modelMetric;

         // TODO : in the future don't copy over all SegmentedRegions.  We only need to copy the ones that changed, which we can detect if we use a linked list and array lookup for the same data structure
         size_t iModel = 0;
         size_t iModelEnd = pTmlState->m_cAttributeCombinations;
         do {
            if(pTmlState->m_apBestModel[iModel]->Copy(*pTmlState->m_apCurrentModel[iModel])) {
               if(nullptr != pValidationMetricReturn) {
                  *pValidationMetricReturn = 0; // on error set it to something instead of random bits
               }
               LOG(TraceLevelVerbose, "Exited ApplyModelUpdatePerTargetStates with memory allocation error in copy");
               return 1;
            }
            ++iModel;
         } while(iModel != iModelEnd);
      }
   }
   if(nullptr != pValidationMetricReturn) {
      *pValidationMetricReturn = modelMetric;
   }

   LOG(TraceLevelVerbose, "Exited ApplyModelUpdatePerTargetStates");
   return 0;
}

template<ptrdiff_t iPossibleCompilerOptimizedTargetStates>
TML_INLINE IntegerDataType CompilerRecursiveApplyModelUpdate(const size_t cRuntimeTargetStates, TmlState * const pTmlState, const size_t iAttributeCombination, const FractionalDataType * const aModelUpdateTensor, FractionalDataType * const pValidationMetricReturn) {
   EBM_ASSERT(IsClassification(iPossibleCompilerOptimizedTargetStates));
   if(iPossibleCompilerOptimizedTargetStates == cRuntimeTargetStates) {
      EBM_ASSERT(cRuntimeTargetStates <= k_cCompilerOptimizedTargetStatesMax);
      return ApplyModelUpdatePerTargetStates<iPossibleCompilerOptimizedTargetStates>(pTmlState, iAttributeCombination, aModelUpdateTensor, pValidationMetricReturn);
   } else {
      return CompilerRecursiveApplyModelUpdate<iPossibleCompilerOptimizedTargetStates + 1>(cRuntimeTargetStates, pTmlState, iAttributeCombination, aModelUpdateTensor, pValidationMetricReturn);
   }
}

template<>
TML_INLINE IntegerDataType CompilerRecursiveApplyModelUpdate<k_cCompilerOptimizedTargetStatesMax + 1>(const size_t cRuntimeTargetStates, TmlState * const pTmlState, const size_t iAttributeCombination, const FractionalDataType * const aModelUpdateTensor, FractionalDataType * const pValidationMetricReturn) {
   UNUSED(cRuntimeTargetStates);
   // it is logically possible, but uninteresting to have a classification with 1 target state, so let our runtime system handle those unlikley and uninteresting cases
   EBM_ASSERT(k_cCompilerOptimizedTargetStatesMax < cRuntimeTargetStates);
   return ApplyModelUpdatePerTargetStates<k_DynamicClassification>(pTmlState, iAttributeCombination, aModelUpdateTensor, pValidationMetricReturn);
}

// we made this a global because if we had put this variable inside the EbmTrainingState object, then we would need to dereference that before getting the count.  By making this global we can send a log message incase a bad EbmTrainingState object is sent into us
// we only decrease the count if the count is non-zero, so at worst if there is a race condition then we'll output this log message more times than desired, but we can live with that
static unsigned int g_cLogApplyModelUpdateParametersMessages = 10;

EBMCORE_IMPORT_EXPORT IntegerDataType EBMCORE_CALLING_CONVENTION ApplyModelUpdate(PEbmTraining ebmTraining, IntegerDataType indexAttributeCombination, const FractionalDataType * modelUpdateTensor, FractionalDataType * validationMetricReturn) {
   LOG_COUNTED(&g_cLogApplyModelUpdateParametersMessages, TraceLevelInfo, TraceLevelVerbose, "ApplyModelUpdate parameters: ebmTraining=%p, indexAttributeCombination=%" IntegerDataTypePrintf ", modelUpdateTensor=%p, validationMetricReturn=%p", static_cast<void *>(ebmTraining), indexAttributeCombination, static_cast<const void *>(modelUpdateTensor), static_cast<void *>(validationMetricReturn));

   TmlState * pTmlState = reinterpret_cast<TmlState *>(ebmTraining);
   EBM_ASSERT(nullptr != pTmlState);

   EBM_ASSERT(0 <= indexAttributeCombination);
   EBM_ASSERT((IsNumberConvertable<size_t, IntegerDataType>(indexAttributeCombination))); // we wouldn't have allowed the creation of an attribute set larger than size_t
   size_t iAttributeCombination = static_cast<size_t>(indexAttributeCombination);
   EBM_ASSERT(iAttributeCombination < pTmlState->m_cAttributeCombinations);
   EBM_ASSERT(nullptr != pTmlState->m_apAttributeCombinations); // this is true because 0 < pTmlState->m_cAttributeCombinations since our caller needs to pass in a valid indexAttributeCombination to this function

   LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogEnterApplyModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Entered ApplyModelUpdate");

   // modelUpdateTensor can be nullptr (then nothing gets updated)
   // validationMetricReturn can be nullptr

   if(nullptr == modelUpdateTensor) {
      if(nullptr != validationMetricReturn) {
         *validationMetricReturn = 0;
      }
      LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogExitApplyModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Exited ApplyModelUpdate from null modelUpdateTensor");
      return 0;
   }

   IntegerDataType ret;
   if(pTmlState->m_bRegression) {
      ret = ApplyModelUpdatePerTargetStates<k_Regression>(pTmlState, iAttributeCombination, modelUpdateTensor, validationMetricReturn);
   } else {
      const size_t cTargetStates = pTmlState->m_cTargetStates;
      if(cTargetStates <= 1) {
         // if there is only 1 target state for classification, then we can predict the output with 100% accuracy.  The model is a tensor with zero length array logits, which means for our representation that we have zero items in the array total.
         // since we can predit the output with 100% accuracy, our log loss is 0.
         if(nullptr != validationMetricReturn) {
            *validationMetricReturn = 0;
         }
         LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogExitApplyModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Exited ApplyModelUpdate from cTargetStates <= 1");
         return 0;
      }
      ret = CompilerRecursiveApplyModelUpdate<2>(cTargetStates, pTmlState, iAttributeCombination, modelUpdateTensor, validationMetricReturn);
   }
   if(0 != ret) {
      LOG(TraceLevelWarning, "WARNING ApplyModelUpdate returned %" IntegerDataTypePrintf, ret);
   }
   if(nullptr != validationMetricReturn) {
      EBM_ASSERT(0 <= *validationMetricReturn); // both log loss and RMSE need to be above zero
      LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogExitApplyModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Exited ApplyModelUpdate %" FractionalDataTypePrintf, *validationMetricReturn);
   } else {
      LOG_COUNTED(&pTmlState->m_apAttributeCombinations[iAttributeCombination]->m_cLogExitApplyModelUpdateMessages, TraceLevelInfo, TraceLevelVerbose, "Exited ApplyModelUpdate.  No validation pointer.");
   }
   return ret;
}

// we made this a global because if we had put this variable inside the EbmTrainingState object, then we would need to dereference that before getting the count.  By making this global we can send a log message incase a bad EbmTrainingState object is sent into us
// we only decrease the count if the count is non-zero, so at worst if there is a race condition then we'll output this log message more times than desired, but we can live with that
static unsigned int g_cLogTrainingStepParametersMessages = 10;

EBMCORE_IMPORT_EXPORT IntegerDataType EBMCORE_CALLING_CONVENTION TrainingStep(PEbmTraining ebmTraining, IntegerDataType indexAttributeCombination, FractionalDataType learningRate, IntegerDataType countTreeSplitsMax, IntegerDataType countCasesRequiredForSplitParentMin, const FractionalDataType * trainingWeights, const FractionalDataType * validationWeights, FractionalDataType * validationMetricReturn) {
   TmlState * pTmlState = reinterpret_cast<TmlState *>(ebmTraining);
   EBM_ASSERT(nullptr != pTmlState);

   if(!pTmlState->m_bRegression) {
      // we need to special handle this case because if we call GenerateModelUpdate, we'll get back a nullptr for the model (since there is no model) and we'll return 1 from this function.  We'd like to return 0 (success) here, so we handle it ourselves
      const size_t cTargetStates = pTmlState->m_cTargetStates;
      if(cTargetStates <= 1) {
         // if there is only 1 target state for classification, then we can predict the output with 100% accuracy.  The model is a tensor with zero length array logits, which means for our representation that we have zero items in the array total.
         // since we can predit the output with 100% accuracy, our gain will be 0.
         if(nullptr != validationMetricReturn) {
            *validationMetricReturn = 0;
         }
         LOG(TraceLevelWarning, "WARNING TrainingStep cTargetStates <= 1");
         return 0;
      }
   }

   FractionalDataType gain; // we toss this value, but we still need to get it
   FractionalDataType * pModelUpdateTensor = GenerateModelUpdate(ebmTraining, indexAttributeCombination, learningRate, countTreeSplitsMax, countCasesRequiredForSplitParentMin, trainingWeights, validationWeights, &gain);
   if(nullptr == pModelUpdateTensor) {
      EBM_ASSERT(nullptr == validationMetricReturn || 0 == *validationMetricReturn); // rely on GenerateModelUpdate to set the validationMetricReturn to zero on error
      return 1;
   }
   return ApplyModelUpdate(ebmTraining, indexAttributeCombination, pModelUpdateTensor, validationMetricReturn);
}

EBMCORE_IMPORT_EXPORT FractionalDataType * EBMCORE_CALLING_CONVENTION GetCurrentModel(PEbmTraining ebmTraining, IntegerDataType indexAttributeCombination) {
   LOG(TraceLevelInfo, "Entered GetCurrentModel: ebmTraining=%p, indexAttributeCombination=%" IntegerDataTypePrintf, static_cast<void *>(ebmTraining), indexAttributeCombination);

   TmlState * pTmlState = reinterpret_cast<TmlState *>(ebmTraining);
   EBM_ASSERT(nullptr != pTmlState);
   EBM_ASSERT(0 <= indexAttributeCombination);
   EBM_ASSERT((IsNumberConvertable<size_t, IntegerDataType>(indexAttributeCombination))); // we wouldn't have allowed the creation of an attribute set larger than size_t
   size_t iAttributeCombination = static_cast<size_t>(indexAttributeCombination);
   EBM_ASSERT(iAttributeCombination < pTmlState->m_cAttributeCombinations);

   if(nullptr == pTmlState->m_apCurrentModel) {
      // if pTmlState->m_apCurrentModel is nullptr, then either:
      //    1) m_cAttributeCombinations was 0, in which case this function would have undefined behavior since the caller needs to indicate a valid indexAttributeCombination, which is impossible, so we can do anything we like, include the below actions.
      //    2) m_cTargetStates was either 1 or 0 (and the learning type is classification), which is legal, which we need to handle here
      // for classification, if there is only 1 possible target state, then the probability of that state is 100%.  If there were logits in this model, they'd all be infinity, but you could alternatively think of this model as having zero logits, since the number of logits can be one less than the number of target cases.  A model with zero logits is empty, and has zero items.  We want to return a tensor with 0 items in it, so we could either return a pointer to some random memory that can't be accessed, or we can return nullptr.  We return a nullptr in the hopes that our caller will either handle it or throw a nicer exception.
      return nullptr;
   }

   SegmentedRegionCore<ActiveDataType, FractionalDataType> * pCurrentModel = pTmlState->m_apCurrentModel[iAttributeCombination];
   EBM_ASSERT(pCurrentModel->m_bExpanded); // the model should have been expanded at startup
   FractionalDataType * pRet = pCurrentModel->GetValuePointer();

   LOG(TraceLevelInfo, "Exited GetCurrentModel %p", static_cast<void *>(pRet));
   return pRet;
}

EBMCORE_IMPORT_EXPORT FractionalDataType * EBMCORE_CALLING_CONVENTION GetBestModel(PEbmTraining ebmTraining, IntegerDataType indexAttributeCombination) {
   LOG(TraceLevelInfo, "Entered GetBestModel: ebmTraining=%p, indexAttributeCombination=%" IntegerDataTypePrintf, static_cast<void *>(ebmTraining), indexAttributeCombination);

   TmlState * pTmlState = reinterpret_cast<TmlState *>(ebmTraining);
   EBM_ASSERT(nullptr != pTmlState);
   EBM_ASSERT(0 <= indexAttributeCombination);
   EBM_ASSERT((IsNumberConvertable<size_t, IntegerDataType>(indexAttributeCombination))); // we wouldn't have allowed the creation of an attribute set larger than size_t
   size_t iAttributeCombination = static_cast<size_t>(indexAttributeCombination);
   EBM_ASSERT(iAttributeCombination < pTmlState->m_cAttributeCombinations);

   if(nullptr == pTmlState->m_apBestModel) {
      // if pTmlState->m_apBestModel is nullptr, then either:
      //    1) m_cAttributeCombinations was 0, in which case this function would have undefined behavior since the caller needs to indicate a valid indexAttributeCombination, which is impossible, so we can do anything we like, include the below actions.
      //    2) m_cTargetStates was either 1 or 0 (and the learning type is classification), which is legal, which we need to handle here
      // for classification, if there is only 1 possible target state, then the probability of that state is 100%.  If there were logits in this model, they'd all be infinity, but you could alternatively think of this model as having zero logits, since the number of logits can be one less than the number of target cases.  A model with zero logits is empty, and has zero items.  We want to return a tensor with 0 items in it, so we could either return a pointer to some random memory that can't be accessed, or we can return nullptr.  We return a nullptr in the hopes that our caller will either handle it or throw a nicer exception.
      return nullptr;
   }

   SegmentedRegionCore<ActiveDataType, FractionalDataType> * pBestModel = pTmlState->m_apBestModel[iAttributeCombination];
   EBM_ASSERT(pBestModel->m_bExpanded); // the model should have been expanded at startup
   FractionalDataType * pRet = pBestModel->GetValuePointer();

   LOG(TraceLevelInfo, "Exited GetBestModel %p", static_cast<void *>(pRet));
   return pRet;
}

EBMCORE_IMPORT_EXPORT void EBMCORE_CALLING_CONVENTION CancelTraining(PEbmTraining ebmTraining) {
   LOG(TraceLevelInfo, "Entered CancelTraining: ebmTraining=%p", static_cast<void *>(ebmTraining));
   EBM_ASSERT(nullptr != ebmTraining);
   LOG(TraceLevelInfo, "Exited CancelTraining");
}

EBMCORE_IMPORT_EXPORT void EBMCORE_CALLING_CONVENTION FreeTraining(PEbmTraining ebmTraining) {
   LOG(TraceLevelInfo, "Entered FreeTraining: ebmTraining=%p", static_cast<void *>(ebmTraining));
   TmlState * pTmlState = reinterpret_cast<TmlState *>(ebmTraining);
   EBM_ASSERT(nullptr != pTmlState);
   delete pTmlState;
   LOG(TraceLevelInfo, "Exited FreeTraining");
}
