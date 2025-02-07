// Copyright (c) 2018 Microsoft Corporation
// Licensed under the MIT license.
// Author: Paul Koch <code@koch.ninja>

#ifndef TREE_NODE_H
#define TREE_NODE_H

#include <type_traits> // std::is_pod
#include <assert.h>
#include <stddef.h> // size_t, ptrdiff_t

#include "EbmInternal.h" // TML_INLINE
#include "Logging.h" // EBM_ASSERT & LOG
#include "SegmentedRegion.h"
#include "EbmStatistics.h"
#include "CachedThreadResources.h"
#include "AttributeInternal.h"
#include "SamplingWithReplacement.h"
#include "BinnedBucket.h"

template<bool bRegression>
class TreeNode;

template<bool bRegression>
TML_INLINE size_t GetTreeNodeSizeOverflow(size_t cVectorLength) {
   return IsMultiplyError(sizeof(PredictionStatistics<bRegression>), cVectorLength) ? true : IsAddError(sizeof(TreeNode<bRegression>) - sizeof(PredictionStatistics<bRegression>), sizeof(PredictionStatistics<bRegression>) * cVectorLength) ? true : false;
}
template<bool bRegression>
TML_INLINE size_t GetTreeNodeSize(size_t cVectorLength) {
   return sizeof(TreeNode<bRegression>) - sizeof(PredictionStatistics<bRegression>) + sizeof(PredictionStatistics<bRegression>) * cVectorLength;
}
template<bool bRegression>
TML_INLINE TreeNode<bRegression> * AddBytesTreeNode(TreeNode<bRegression> * pTreeNode, size_t countBytesAdd) {
   return reinterpret_cast<TreeNode<bRegression> *>(reinterpret_cast<char *>(pTreeNode) + countBytesAdd);
}
template<bool bRegression>
TML_INLINE TreeNode<bRegression> * GetLeftTreeNodeChild(TreeNode<bRegression> * pTreeNodeChildren, size_t countBytesTreeNode) {
   UNUSED(countBytesTreeNode);
   return pTreeNodeChildren;
}
template<bool bRegression>
TML_INLINE TreeNode<bRegression> * GetRightTreeNodeChild(TreeNode<bRegression> * pTreeNodeChildren, size_t countBytesTreeNode) {
   return AddBytesTreeNode<bRegression>(pTreeNodeChildren, countBytesTreeNode);
}

template<bool bRegression>
class TreeNodeData;

template<>
class TreeNodeData<false> {
   // classification version of the TreeNodeData

public:

   struct BeforeExaminationForPossibleSplitting {
      const BinnedBucket<false> * pBinnedBucketEntryFirst;
      const BinnedBucket<false> * pBinnedBucketEntryLast;
      size_t cCases;
   };

   struct AfterExaminationForPossibleSplitting {
      TreeNode<false> * pTreeNodeChildren;
      FractionalDataType splitGain; // put this at the top so that our priority queue can access it directly without adding anything to the pointer (this is slightly more efficient on intel systems at least)
      ActiveDataType divisionValue;
   };

   union TreeNodeDataUnion {
      // we can save precious L1 cache space by keeping only what we need
      BeforeExaminationForPossibleSplitting beforeExaminationForPossibleSplitting;
      AfterExaminationForPossibleSplitting afterExaminationForPossibleSplitting;

      static_assert(std::is_pod<BeforeExaminationForPossibleSplitting>::value, "BeforeSplit must be POD (Plain Old Data) if we are going to use it in a union!");
      static_assert(std::is_pod<AfterExaminationForPossibleSplitting>::value, "AfterSplit must be POD (Plain Old Data) if we are going to use it in a union!");
   };

   TreeNodeDataUnion m_UNION;
   PredictionStatistics<false> aPredictionStatistics[1];

   TML_INLINE size_t GetCases() const {
      return m_UNION.beforeExaminationForPossibleSplitting.cCases;
   }
   TML_INLINE void SetCases(size_t cCases) {
      m_UNION.beforeExaminationForPossibleSplitting.cCases = cCases;
   }
};

template<>
class TreeNodeData<true> {
   // regression version of the TreeNodeData
public:

   struct BeforeExaminationForPossibleSplitting {
      const BinnedBucket<true> * pBinnedBucketEntryFirst;
      const BinnedBucket<true> * pBinnedBucketEntryLast;
   };

   struct AfterExaminationForPossibleSplitting {
      TreeNode<true> * pTreeNodeChildren;
      FractionalDataType splitGain; // put this at the top so that our priority queue can access it directly without adding anything to the pointer (this is slightly more efficient on intel systems at least)
      ActiveDataType divisionValue;
   };

   union TreeNodeDataUnion {
      // we can save precious L1 cache space by keeping only what we need
      BeforeExaminationForPossibleSplitting beforeExaminationForPossibleSplitting;
      AfterExaminationForPossibleSplitting afterExaminationForPossibleSplitting;

      static_assert(std::is_pod<BeforeExaminationForPossibleSplitting>::value, "BeforeSplit must be POD (Plain Old Data) if we are going to use it in a union!");
      static_assert(std::is_pod<AfterExaminationForPossibleSplitting>::value, "AfterSplit must be POD (Plain Old Data) if we are going to use it in a union!");
   };

   TreeNodeDataUnion m_UNION;

   size_t m_cCases;
   PredictionStatistics<true> aPredictionStatistics[1];

   TML_INLINE size_t GetCases() const {
      return m_cCases;
   }
   TML_INLINE void SetCases(size_t cCases) {
      m_cCases = cCases;
   }
};

template<bool bRegression>
class TreeNode final : public TreeNodeData<bRegression> {
public:

   TML_INLINE bool IsSplittable(size_t cCasesRequiredForSplitParentMin) const {
      return this->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryLast != this->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryFirst && cCasesRequiredForSplitParentMin <= this->GetCases();
   }

   TML_INLINE FractionalDataType EXTRACT_GAIN_BEFORE_SPLITTING() {
      EBM_ASSERT(this->m_UNION.afterExaminationForPossibleSplitting.splitGain <= 0);
      return this->m_UNION.afterExaminationForPossibleSplitting.splitGain;
   }

   TML_INLINE void SPLIT_THIS_NODE() {
      this->m_UNION.afterExaminationForPossibleSplitting.splitGain = FractionalDataType { std::numeric_limits<FractionalDataType>::quiet_NaN() };
   }

   TML_INLINE void INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED() {
      // we aren't going to split this TreeNode because we can't.  We need to set the splitGain value here because otherwise it is filled with garbage that could be NaN (meaning the node was a branch)
      // we can't call INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED before calling SplitTreeNode because INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED sets m_UNION.afterExaminationForPossibleSplitting.splitGain and the m_UNION.beforeExaminationForPossibleSplitting values are needed if we had decided to call ExamineNodeForSplittingAndDetermineBestPossibleSplit
      this->m_UNION.afterExaminationForPossibleSplitting.splitGain = FractionalDataType { 0 };
   }

   TML_INLINE bool WAS_THIS_NODE_SPLIT() const {
      return std::isnan(this->m_UNION.afterExaminationForPossibleSplitting.splitGain);
   }

   template<ptrdiff_t countCompilerClassificationTargetStates>
   void ExamineNodeForPossibleSplittingAndDetermineBestSplitPoint(CachedTrainingThreadResources<bRegression> * const pCachedThreadResources, TreeNode<bRegression> * const pTreeNodeChildrenAvailableStorageSpaceCur, const size_t cTargetStates
#ifndef NDEBUG
      , const unsigned char * const aBinnedBucketsEndDebug
#endif // NDEBUG
   ) {
      LOG(TraceLevelVerbose, "Entered SplitTreeNode: this=%p, pTreeNodeChildrenAvailableStorageSpaceCur=%p", static_cast<void *>(this), static_cast<void *>(pTreeNodeChildrenAvailableStorageSpaceCur));

      static_assert(IsRegression(countCompilerClassificationTargetStates) == bRegression, "regression types must match");

      const size_t cVectorLength = GET_VECTOR_LENGTH(countCompilerClassificationTargetStates, cTargetStates);
      EBM_ASSERT(!GetTreeNodeSizeOverflow<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength)); // we're accessing allocated memory
      const size_t cBytesPerTreeNode = GetTreeNodeSize<bRegression>(cVectorLength);
      EBM_ASSERT(!GetBinnedBucketSizeOverflow<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength)); // we're accessing allocated memory
      const size_t cBytesPerBinnedBucket = GetBinnedBucketSize<bRegression>(cVectorLength);

      const BinnedBucket<bRegression> * pBinnedBucketEntryCur = this->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryFirst;
      const BinnedBucket<bRegression> * const pBinnedBucketEntryLast = this->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryLast;

      TreeNode<bRegression> * const pLeftChild1 = GetLeftTreeNodeChild<bRegression>(pTreeNodeChildrenAvailableStorageSpaceCur, cBytesPerTreeNode);
      pLeftChild1->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryFirst = pBinnedBucketEntryCur;
      TreeNode<bRegression> * const pRightChild1 = GetRightTreeNodeChild<bRegression>(pTreeNodeChildrenAvailableStorageSpaceCur, cBytesPerTreeNode);
      pRightChild1->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryLast = pBinnedBucketEntryLast;

      size_t cCasesLeft = pBinnedBucketEntryCur->cCasesInBucket;
      size_t cCasesRight = this->GetCases() - cCasesLeft;

      PredictionStatistics<bRegression> * const aSumPredictionStatisticsLeft = pCachedThreadResources->m_aSumPredictionStatistics1;
      FractionalDataType * const aSumResidualErrorsRight = pCachedThreadResources->m_aSumResidualErrors2;
      PredictionStatistics<bRegression> * const aSumPredictionStatisticsBest = pCachedThreadResources->m_aSumPredictionStatisticsBest;
      FractionalDataType BEST_nodeSplittingScore = 0;
      for(size_t iVector = 0; iVector < cVectorLength; ++iVector) {
         const FractionalDataType sumResidualErrorLeft = pBinnedBucketEntryCur->aPredictionStatistics[iVector].sumResidualError;
         const FractionalDataType sumResidualErrorRight = this->aPredictionStatistics[iVector].sumResidualError - sumResidualErrorLeft;

         BEST_nodeSplittingScore += EbmStatistics::ComputeNodeSplittingScore(sumResidualErrorLeft, cCasesLeft) + EbmStatistics::ComputeNodeSplittingScore(sumResidualErrorRight, cCasesRight);

         aSumPredictionStatisticsLeft[iVector].sumResidualError = sumResidualErrorLeft;
         aSumPredictionStatisticsBest[iVector].sumResidualError = sumResidualErrorLeft;
         aSumResidualErrorsRight[iVector] = sumResidualErrorRight;
         if(!bRegression) {
            FractionalDataType sumDenominator1 = pBinnedBucketEntryCur->aPredictionStatistics[iVector].GetSumDenominator();
            aSumPredictionStatisticsLeft[iVector].SetSumDenominator(sumDenominator1);
            aSumPredictionStatisticsBest[iVector].SetSumDenominator(sumDenominator1);
         }
      }

      EBM_ASSERT(0 <= BEST_nodeSplittingScore);
      const BinnedBucket<bRegression> * BEST_pBinnedBucketEntry = pBinnedBucketEntryCur;
      size_t BEST_cCasesLeft = cCasesLeft;
      for(pBinnedBucketEntryCur = GetBinnedBucketByIndex<bRegression>(cBytesPerBinnedBucket, pBinnedBucketEntryCur, 1); pBinnedBucketEntryLast != pBinnedBucketEntryCur; pBinnedBucketEntryCur = GetBinnedBucketByIndex<bRegression>(cBytesPerBinnedBucket, pBinnedBucketEntryCur, 1)) {
         ASSERT_BINNED_BUCKET_OK(cBytesPerBinnedBucket, pBinnedBucketEntryCur, aBinnedBucketsEndDebug);

         const size_t CHANGE_cCases = pBinnedBucketEntryCur->cCasesInBucket;
         cCasesLeft += CHANGE_cCases;
         cCasesRight -= CHANGE_cCases;

         FractionalDataType nodeSplittingScore = 0;
         for(size_t iVector = 0; iVector < cVectorLength; ++iVector) {
            if(!bRegression) {
               aSumPredictionStatisticsLeft[iVector].SetSumDenominator(aSumPredictionStatisticsLeft[iVector].GetSumDenominator() + pBinnedBucketEntryCur->aPredictionStatistics[iVector].GetSumDenominator());
            }

            const FractionalDataType CHANGE_sumResidualError = pBinnedBucketEntryCur->aPredictionStatistics[iVector].sumResidualError;
            const FractionalDataType sumResidualErrorLeft = aSumPredictionStatisticsLeft[iVector].sumResidualError + CHANGE_sumResidualError;
            const FractionalDataType sumResidualErrorRight = aSumResidualErrorsRight[iVector] - CHANGE_sumResidualError;

            aSumPredictionStatisticsLeft[iVector].sumResidualError = sumResidualErrorLeft;
            aSumResidualErrorsRight[iVector] = sumResidualErrorRight;

            // TODO : we can make this faster by doing the division in ComputeNodeSplittingScore after we add all the numerators
            const FractionalDataType nodeSplittingScoreOneVector = EbmStatistics::ComputeNodeSplittingScore(sumResidualErrorLeft, cCasesLeft) + EbmStatistics::ComputeNodeSplittingScore(sumResidualErrorRight, cCasesRight);
            EBM_ASSERT(0 <= nodeSplittingScore);
            nodeSplittingScore += nodeSplittingScoreOneVector;
         }
         EBM_ASSERT(0 <= nodeSplittingScore);

         if(UNLIKELY(BEST_nodeSplittingScore < nodeSplittingScore)) {
            // TODO : randomly choose a node if BEST_entropyTotalChildren == entropyTotalChildren, but if there are 3 choice make sure that each has a 1/3 probability of being selected (same as interview question to select a random line from a file)
            BEST_nodeSplittingScore = nodeSplittingScore;
            BEST_pBinnedBucketEntry = pBinnedBucketEntryCur;
            BEST_cCasesLeft = cCasesLeft;
            memcpy(aSumPredictionStatisticsBest, aSumPredictionStatisticsLeft, sizeof(*aSumPredictionStatisticsBest) * cVectorLength);
         }
      }

      TreeNode<bRegression> * const pLeftChild = GetLeftTreeNodeChild<bRegression>(pTreeNodeChildrenAvailableStorageSpaceCur, cBytesPerTreeNode);
      TreeNode<bRegression> * const pRightChild = GetRightTreeNodeChild<bRegression>(pTreeNodeChildrenAvailableStorageSpaceCur, cBytesPerTreeNode);

      pLeftChild->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryLast = BEST_pBinnedBucketEntry;
      pLeftChild->SetCases(BEST_cCasesLeft);

      const BinnedBucket<bRegression> * const BEST_pBinnedBucketEntryNext = GetBinnedBucketByIndex<bRegression>(cBytesPerBinnedBucket, BEST_pBinnedBucketEntry, 1);
      ASSERT_BINNED_BUCKET_OK(cBytesPerBinnedBucket, BEST_pBinnedBucketEntryNext, aBinnedBucketsEndDebug);

      pRightChild->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryFirst = BEST_pBinnedBucketEntryNext;
      size_t cCasesParent = this->GetCases();
      pRightChild->SetCases(cCasesParent - BEST_cCasesLeft);

      FractionalDataType originalParentScore = 0;
      for(size_t iVector = 0; iVector < cVectorLength; ++iVector) {
         pLeftChild->aPredictionStatistics[iVector].sumResidualError = aSumPredictionStatisticsBest[iVector].sumResidualError;
         if(!bRegression) {
            pLeftChild->aPredictionStatistics[iVector].SetSumDenominator(aSumPredictionStatisticsBest[iVector].GetSumDenominator());
         }

         const FractionalDataType sumResidualErrorParent = this->aPredictionStatistics[iVector].sumResidualError;
         originalParentScore += EbmStatistics::ComputeNodeSplittingScore(sumResidualErrorParent, cCasesParent);

         pRightChild->aPredictionStatistics[iVector].sumResidualError = sumResidualErrorParent - aSumPredictionStatisticsBest[iVector].sumResidualError;
         if(!bRegression) {
            pRightChild->aPredictionStatistics[iVector].SetSumDenominator(this->aPredictionStatistics[iVector].GetSumDenominator() - aSumPredictionStatisticsBest[iVector].GetSumDenominator());
         }
      }



      // IMPORTANT!! : we need to finish all our calls that use this->m_UNION.beforeExaminationForPossibleSplitting BEFORE setting anything in m_UNION.afterExaminationForPossibleSplitting as we do below this comment!  The call above to this->GetCases() needs to be done above these lines because it uses m_UNION.beforeExaminationForPossibleSplitting for classification!



      this->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren = pTreeNodeChildrenAvailableStorageSpaceCur;
      FractionalDataType splitGain = originalParentScore - BEST_nodeSplittingScore;
      if(UNLIKELY(std::isnan(splitGain))) {
         // it is possible that nodeSplittingScoreParent could reach infinity and BEST_nodeSplittingScore infinity, and the subtraction of those values leads to NaN
         // if gain became NaN via overlfow, that would be bad since we use NaN to indicate that a node has not been split
         splitGain = FractionalDataType { 0 };
      }
      this->m_UNION.afterExaminationForPossibleSplitting.splitGain = splitGain;
      this->m_UNION.afterExaminationForPossibleSplitting.divisionValue = (BEST_pBinnedBucketEntry->bucketValue + BEST_pBinnedBucketEntryNext->bucketValue) / 2;

      EBM_ASSERT(this->m_UNION.afterExaminationForPossibleSplitting.splitGain <= 0.0000000001); // within a set, no split should make our model worse.  It might in our validation set, but not within this set

      LOG(TraceLevelVerbose, "Exited SplitTreeNode: divisionValue=%zu, nodeSplittingScore=%" FractionalDataTypePrintf, static_cast<size_t>(this->m_UNION.afterExaminationForPossibleSplitting.divisionValue), this->m_UNION.afterExaminationForPossibleSplitting.splitGain);
   }

   // TODO: in theory, a malicious caller could overflow our stack if they pass us data that will grow a sufficiently deep tree.  Consider changing this recursive function to handle that
   // TODO: specialize this function for cases where we have hard coded vector lengths so that we don't have to pass in the cVectorLength parameter
   void Flatten(ActiveDataType ** const ppDivisions, FractionalDataType ** const ppValues, const size_t cVectorLength) const {
      // don't log this since we call it recursively.  Log where the root is called
      if(UNPREDICTABLE(WAS_THIS_NODE_SPLIT())) {
         EBM_ASSERT(!GetTreeNodeSizeOverflow<bRegression>(cVectorLength)); // we're accessing allocated memory
         const size_t cBytesPerTreeNode = GetTreeNodeSize<bRegression>(cVectorLength);
         const TreeNode<bRegression> * const pLeftChild = GetLeftTreeNodeChild<bRegression>(this->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode);
         pLeftChild->Flatten(ppDivisions, ppValues, cVectorLength);
         **ppDivisions = this->m_UNION.afterExaminationForPossibleSplitting.divisionValue;
         ++(*ppDivisions);
         const TreeNode<bRegression> * const pRightChild = GetRightTreeNodeChild<bRegression>(this->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode);
         pRightChild->Flatten(ppDivisions, ppValues, cVectorLength);
      } else {
         FractionalDataType * pValuesCur = *ppValues;
         FractionalDataType * const pValuesNext = pValuesCur + cVectorLength;
         *ppValues = pValuesNext;

         const PredictionStatistics<bRegression> * pPredictionStatistics = &this->aPredictionStatistics[0];
         do {
            FractionalDataType smallChangeToModel;
            if(bRegression) {
               smallChangeToModel = EbmStatistics::ComputeSmallChangeInRegressionPredictionForOneSegment(pPredictionStatistics->sumResidualError, this->GetCases());
            } else {
               smallChangeToModel = EbmStatistics::ComputeSmallChangeInClassificationLogOddPredictionForOneSegment(pPredictionStatistics->sumResidualError, pPredictionStatistics->GetSumDenominator());
            }
            *pValuesCur = smallChangeToModel;

            ++pPredictionStatistics;
            ++pValuesCur;
         } while(pValuesNext != pValuesCur);
      }
   }

   static_assert(std::is_pod<ActiveDataType>::value, "We want to keep our TreeNode compact and without a virtual pointer table for fitting in L1 cache as much as possible");
};
static_assert(std::is_pod<TreeNode<false>>::value, "We want to keep our TreeNode compact and without a virtual pointer table for fitting in L1 cache as much as possible");
static_assert(std::is_pod<TreeNode<true>>::value, "We want to keep our TreeNode compact and without a virtual pointer table for fitting in L1 cache as much as possible");

template<ptrdiff_t countCompilerClassificationTargetStates>
bool GrowDecisionTree(CachedTrainingThreadResources<IsRegression(countCompilerClassificationTargetStates)> * const pCachedThreadResources, const size_t cTargetStates, const size_t cBinnedBuckets, const BinnedBucket<IsRegression(countCompilerClassificationTargetStates)> * const aBinnedBucket, const size_t cCasesTotal, const PredictionStatistics<IsRegression(countCompilerClassificationTargetStates)> * const aSumPredictionStatistics, const size_t cTreeSplitsMax, const size_t cCasesRequiredForSplitParentMin, SegmentedRegionCore<ActiveDataType, FractionalDataType> * const pSmallChangeToModelOverwriteSingleSamplingSet, FractionalDataType * const pTotalGain
#ifndef NDEBUG
   , const unsigned char * const aBinnedBucketsEndDebug
#endif // NDEBUG
) {
   LOG(TraceLevelVerbose, "Entered GrowDecisionTree");

   const size_t cVectorLength = GET_VECTOR_LENGTH(countCompilerClassificationTargetStates, cTargetStates);

   EBM_ASSERT(nullptr != pTotalGain);
   EBM_ASSERT(1 <= cCasesTotal); // filter these out at the start where we can handle this case easily
   EBM_ASSERT(1 <= cBinnedBuckets); // cBinnedBuckets could only be zero if cCasesTotal.  We should filter out that special case at our entry point though!!
   if(UNLIKELY(cCasesTotal < cCasesRequiredForSplitParentMin || 1 == cBinnedBuckets || 0 == cTreeSplitsMax)) {
      // there will be no splits at all

      if(UNLIKELY(pSmallChangeToModelOverwriteSingleSamplingSet->SetCountDivisions(0, 0))) {
         LOG(TraceLevelWarning, "WARNING GrowDecisionTree pSmallChangeToModelOverwriteSingleSamplingSet->SetCountDivisions(0, 0)");
         return true;
      }

      // we don't need to call EnsureValueCapacity because by default we start with a value capacity of 2 * cVectorLength

      if(IsRegression(countCompilerClassificationTargetStates)) {
         FractionalDataType smallChangeToModel = EbmStatistics::ComputeSmallChangeInRegressionPredictionForOneSegment(aSumPredictionStatistics[0].sumResidualError, cCasesTotal);
         FractionalDataType * pValues = pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer();
         pValues[0] = smallChangeToModel;
      } else {
         EBM_ASSERT(IsClassification(countCompilerClassificationTargetStates));
         FractionalDataType * aValues = pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer();
         for(size_t iVector = 0; iVector < cVectorLength; ++iVector) {
            FractionalDataType smallChangeToModel = EbmStatistics::ComputeSmallChangeInClassificationLogOddPredictionForOneSegment(aSumPredictionStatistics[iVector].sumResidualError, aSumPredictionStatistics[iVector].GetSumDenominator());
            aValues[iVector] = smallChangeToModel;
         }
      }

      LOG(TraceLevelVerbose, "Exited GrowDecisionTree via not enough data to split");
      *pTotalGain = 0;
      return false;
   }

   // there will be at least one split

   if(GetTreeNodeSizeOverflow<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength)) {
      LOG(TraceLevelWarning, "WARNING GrowDecisionTree GetTreeNodeSizeOverflow<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength)");
      return true; // we haven't accessed this TreeNode memory yet, so we don't know if it overflows yet
   }
   const size_t cBytesPerTreeNode = GetTreeNodeSize<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength);
   EBM_ASSERT(!GetBinnedBucketSizeOverflow<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength)); // we're accessing allocated memory
   const size_t cBytesPerBinnedBucket = GetBinnedBucketSize<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength);

retry_with_bigger_tree_node_children_array:
   size_t cBytesBuffer2 = pCachedThreadResources->GetThreadByteBuffer2Size();
   const size_t cBytesInitialNeededAllocation = 3 * cBytesPerTreeNode; // we need 1 TreeNode for the root, 1 for the left child of the root and 1 for the right child of the root
   if(cBytesBuffer2 < cBytesInitialNeededAllocation) {
      // TODO : we can eliminate this check as long as we ensure that the ThreadByteBuffer2 is always initialized to be equal to the size of three TreeNodes (left and right) == GET_SIZEOF_ONE_TREE_NODE_CHILDREN(cBytesPerTreeNode)
      if(pCachedThreadResources->GrowThreadByteBuffer2(cBytesInitialNeededAllocation)) {
         LOG(TraceLevelWarning, "WARNING GrowDecisionTree pCachedThreadResources->GrowThreadByteBuffer2(cBytesInitialNeededAllocation)");
         return true;
      }
      cBytesBuffer2 = pCachedThreadResources->GetThreadByteBuffer2Size();
      EBM_ASSERT(cBytesInitialNeededAllocation <= cBytesBuffer2);
   }
   TreeNode<IsRegression(countCompilerClassificationTargetStates)> * pRootTreeNode = static_cast<TreeNode<IsRegression(countCompilerClassificationTargetStates)> *>(pCachedThreadResources->GetThreadByteBuffer2());

   pRootTreeNode->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryFirst = aBinnedBucket;
   pRootTreeNode->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryLast = GetBinnedBucketByIndex<IsRegression(countCompilerClassificationTargetStates)>(cBytesPerBinnedBucket, aBinnedBucket, cBinnedBuckets - 1);
   ASSERT_BINNED_BUCKET_OK(cBytesPerBinnedBucket, pRootTreeNode->m_UNION.beforeExaminationForPossibleSplitting.pBinnedBucketEntryLast, aBinnedBucketsEndDebug);
   pRootTreeNode->SetCases(cCasesTotal);

   memcpy(&pRootTreeNode->aPredictionStatistics[0], aSumPredictionStatistics, cVectorLength * sizeof(*aSumPredictionStatistics)); // copying existing mem

   pRootTreeNode->template ExamineNodeForPossibleSplittingAndDetermineBestSplitPoint<countCompilerClassificationTargetStates>(pCachedThreadResources, AddBytesTreeNode<IsRegression(countCompilerClassificationTargetStates)>(pRootTreeNode, cBytesPerTreeNode), cTargetStates
#ifndef NDEBUG
      , aBinnedBucketsEndDebug
#endif // NDEBUG
   );

   if(UNPREDICTABLE(PREDICTABLE(1 == cTreeSplitsMax) || UNPREDICTABLE(2 == cBinnedBuckets))) {
      // there will be exactly 1 split, which is a special case that we can return faster without as much overhead as the multiple split case

      assert(2 != cBinnedBuckets || !GetLeftTreeNodeChild<IsRegression(countCompilerClassificationTargetStates)>(pRootTreeNode->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode)->IsSplittable(cCasesRequiredForSplitParentMin) && !GetRightTreeNodeChild<IsRegression(countCompilerClassificationTargetStates)>(pRootTreeNode->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode)->IsSplittable(cCasesRequiredForSplitParentMin));

      if(UNLIKELY(pSmallChangeToModelOverwriteSingleSamplingSet->SetCountDivisions(0, 1))) {
         LOG(TraceLevelWarning, "WARNING GrowDecisionTree pSmallChangeToModelOverwriteSingleSamplingSet->SetCountDivisions(0, 1)");
         return true;
      }

      ActiveDataType * pDivisions = pSmallChangeToModelOverwriteSingleSamplingSet->GetDivisionPointer(0);
      pDivisions[0] = pRootTreeNode->m_UNION.afterExaminationForPossibleSplitting.divisionValue;

      // we don't need to call EnsureValueCapacity because by default we start with a value capacity of 2 * cVectorLength

      // TODO : we don't need to get the right and left pointer from the root.. we know where they will be
      const TreeNode<IsRegression(countCompilerClassificationTargetStates)> * const pLeftChild = GetLeftTreeNodeChild<IsRegression(countCompilerClassificationTargetStates)>(pRootTreeNode->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode);
      const TreeNode<IsRegression(countCompilerClassificationTargetStates)> * const pRightChild = GetRightTreeNodeChild<IsRegression(countCompilerClassificationTargetStates)>(pRootTreeNode->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode);

      FractionalDataType * const aValues = pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer();
      if(IsRegression(countCompilerClassificationTargetStates)) {
         aValues[0] = EbmStatistics::ComputeSmallChangeInRegressionPredictionForOneSegment(pLeftChild->aPredictionStatistics[0].sumResidualError, pLeftChild->GetCases());
         aValues[1] = EbmStatistics::ComputeSmallChangeInRegressionPredictionForOneSegment(pRightChild->aPredictionStatistics[0].sumResidualError, pRightChild->GetCases());
      } else {
         EBM_ASSERT(IsClassification(countCompilerClassificationTargetStates));
         for(size_t iVector = 0; iVector < cVectorLength; ++iVector) {
            aValues[iVector] = EbmStatistics::ComputeSmallChangeInClassificationLogOddPredictionForOneSegment(pLeftChild->aPredictionStatistics[iVector].sumResidualError, pLeftChild->aPredictionStatistics[iVector].GetSumDenominator());
            aValues[cVectorLength + iVector] = EbmStatistics::ComputeSmallChangeInClassificationLogOddPredictionForOneSegment(pRightChild->aPredictionStatistics[iVector].sumResidualError, pRightChild->aPredictionStatistics[iVector].GetSumDenominator());
         }
      }

      LOG(TraceLevelVerbose, "Exited GrowDecisionTree via one tree split");
      *pTotalGain = pRootTreeNode->EXTRACT_GAIN_BEFORE_SPLITTING();
      return false;
   }

   // it's very very likely that there will be more than 1 split below this point.  The only case where we wouldn't split below is if both our children nodes dont't have enough cases
   // to split, but that should be very rare

   // TODO: there are three types of queues that we should try out -> dyamically picking a stragety is a single predictable if statement, so shouldn't cause a lot of overhead
   //       1) When the data is the smallest(1-5 items), just iterate over all items in our TreeNode buffer looking for the best Node.  Zero the value on any nodes that have been removed from the queue.  For 1 or 2 instructions in the loop WITHOUT a branch we can probably save the pointer to the first TreeNode with data so that we can start from there next time we loop
   //       2) When the data is a tiny bit bigger and there are holes in our array of TreeNodes, we can maintain a pointer and value in a separate list and zip through the values and then go to the pointer to the best node.  Since the list is unordered, when we find a TreeNode to remove, we just move the last one into the hole
   //       3) The full fleged priority queue below
   size_t cSplits;
   try {
      std::priority_queue<TreeNode<IsRegression(countCompilerClassificationTargetStates)> *, std::vector<TreeNode<IsRegression(countCompilerClassificationTargetStates)> *>, CompareTreeNodeSplittingGain<IsRegression(countCompilerClassificationTargetStates)>> * pBestTreeNodeToSplit = &pCachedThreadResources->m_bestTreeNodeToSplit;

      // it is ridiculous that we need to do this in order to clear the tree (there is no "clear" function), but inside this queue is a chunk of memory, and we want to ensure that the chunk of memory stays in L1 cache, so we pop all the previous garbage off instead of allocating a new one!
      while(!pBestTreeNodeToSplit->empty()) {
         pBestTreeNodeToSplit->pop();
      }

      cSplits = 0;
      TreeNode<IsRegression(countCompilerClassificationTargetStates)> * pParentTreeNode = pRootTreeNode;

      // we skip 3 tree nodes.  The root, the left child of the root, and the right child of the root
      TreeNode<IsRegression(countCompilerClassificationTargetStates)> * pTreeNodeChildrenAvailableStorageSpaceCur = AddBytesTreeNode<IsRegression(countCompilerClassificationTargetStates)>(pRootTreeNode, cBytesInitialNeededAllocation);

      FractionalDataType totalGain = 0;

      goto skip_first_push_pop;

      do {
         // there is no way to get the top and pop at the same time.. would be good to get a better queue, but our code isn't bottlenecked by it
         pParentTreeNode = pBestTreeNodeToSplit->top();
         pBestTreeNodeToSplit->pop();

      skip_first_push_pop:

         // ONLY AFTER WE'VE POPPED pParentTreeNode OFF the priority queue is it considered to have been split.  Calling SPLIT_THIS_NODE makes it formal
         totalGain += pParentTreeNode->EXTRACT_GAIN_BEFORE_SPLITTING();
         pParentTreeNode->SPLIT_THIS_NODE();

         TreeNode<IsRegression(countCompilerClassificationTargetStates)> * const pLeftChild = GetLeftTreeNodeChild<IsRegression(countCompilerClassificationTargetStates)>(pParentTreeNode->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode);
         if(pLeftChild->IsSplittable(cCasesRequiredForSplitParentMin)) {
            TreeNode<IsRegression(countCompilerClassificationTargetStates)> * pTreeNodeChildrenAvailableStorageSpaceNext = AddBytesTreeNode<IsRegression(countCompilerClassificationTargetStates)>(pTreeNodeChildrenAvailableStorageSpaceCur, cBytesPerTreeNode << 1);
            if(cBytesBuffer2 < static_cast<size_t>(reinterpret_cast<char *>(pTreeNodeChildrenAvailableStorageSpaceNext) - reinterpret_cast<char *>(pRootTreeNode))) {
               if(pCachedThreadResources->GrowThreadByteBuffer2(cBytesPerTreeNode)) {
                  LOG(TraceLevelWarning, "WARNING GrowDecisionTree pCachedThreadResources->GrowThreadByteBuffer2(cBytesPerTreeNode)");
                  return true;
               }
               goto retry_with_bigger_tree_node_children_array;
            }
            // the act of splitting it implicitly sets INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED because splitting sets splitGain to a non-NaN value
            pLeftChild->template ExamineNodeForPossibleSplittingAndDetermineBestSplitPoint<countCompilerClassificationTargetStates>(pCachedThreadResources, pTreeNodeChildrenAvailableStorageSpaceCur, cTargetStates
#ifndef NDEBUG
               , aBinnedBucketsEndDebug
#endif // NDEBUG
            );
            pTreeNodeChildrenAvailableStorageSpaceCur = pTreeNodeChildrenAvailableStorageSpaceNext;
            pBestTreeNodeToSplit->push(pLeftChild);
         } else {
            // we aren't going to split this TreeNode because we can't.  We need to set the splitGain value here because otherwise it is filled with garbage that could be NaN (meaning the node was a branch)
            // we can't call INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED before calling SplitTreeNode because INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED sets m_UNION.afterExaminationForPossibleSplitting.splitGain and the m_UNION.beforeExaminationForPossibleSplitting values are needed if we had decided to call ExamineNodeForSplittingAndDetermineBestPossibleSplit
            pLeftChild->INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED();
         }

         TreeNode<IsRegression(countCompilerClassificationTargetStates)> * const pRightChild = GetRightTreeNodeChild<IsRegression(countCompilerClassificationTargetStates)>(pParentTreeNode->m_UNION.afterExaminationForPossibleSplitting.pTreeNodeChildren, cBytesPerTreeNode);
         if(pRightChild->IsSplittable(cCasesRequiredForSplitParentMin)) {
            TreeNode<IsRegression(countCompilerClassificationTargetStates)> * pTreeNodeChildrenAvailableStorageSpaceNext = AddBytesTreeNode<IsRegression(countCompilerClassificationTargetStates)>(pTreeNodeChildrenAvailableStorageSpaceCur, cBytesPerTreeNode << 1);
            if(cBytesBuffer2 < static_cast<size_t>(reinterpret_cast<char *>(pTreeNodeChildrenAvailableStorageSpaceNext) - reinterpret_cast<char *>(pRootTreeNode))) {
               if(pCachedThreadResources->GrowThreadByteBuffer2(cBytesPerTreeNode)) {
                  LOG(TraceLevelWarning, "WARNING GrowDecisionTree pCachedThreadResources->GrowThreadByteBuffer2(cBytesPerTreeNode)");
                  return true;
               }
               goto retry_with_bigger_tree_node_children_array;
            }
            // the act of splitting it implicitly sets INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED because splitting sets splitGain to a non-NaN value
            pRightChild->template ExamineNodeForPossibleSplittingAndDetermineBestSplitPoint<countCompilerClassificationTargetStates>(pCachedThreadResources, pTreeNodeChildrenAvailableStorageSpaceCur, cTargetStates
#ifndef NDEBUG
               , aBinnedBucketsEndDebug
#endif // NDEBUG
            );
            pTreeNodeChildrenAvailableStorageSpaceCur = pTreeNodeChildrenAvailableStorageSpaceNext;
            pBestTreeNodeToSplit->push(pRightChild);
         } else {
            // we aren't going to split this TreeNode because we can't.  We need to set the splitGain value here because otherwise it is filled with garbage that could be NaN (meaning the node was a branch)
            // we can't call INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED before calling SplitTreeNode because INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED sets m_UNION.afterExaminationForPossibleSplitting.splitGain and the m_UNION.beforeExaminationForPossibleSplitting values are needed if we had decided to call ExamineNodeForSplittingAndDetermineBestPossibleSplit
            pRightChild->INDICATE_THIS_NODE_EXAMINED_FOR_SPLIT_AND_REJECTED();
         }
         ++cSplits;
      } while(cSplits < cTreeSplitsMax && UNLIKELY(!pBestTreeNodeToSplit->empty()));
      // we DON'T need to call SetLeafAfterDone() on any items that remain in the pBestTreeNodeToSplit queue because everything in that queue has set a non-NaN nodeSplittingScore value

      // we might as well dump this value out to our pointer, even if later fail the function below.  If the function is failed, we make no guarantees about what we did with the value pointed to at *pTotalGain
      *pTotalGain = totalGain;
      EBM_ASSERT(static_cast<size_t>(reinterpret_cast<char *>(pTreeNodeChildrenAvailableStorageSpaceCur) - reinterpret_cast<char *>(pRootTreeNode)) <= cBytesBuffer2);
   } catch(...) {
      LOG(TraceLevelWarning, "WARNING GrowDecisionTree exception");
      return true;
   }

   if(UNLIKELY(pSmallChangeToModelOverwriteSingleSamplingSet->SetCountDivisions(0, cSplits))) {
      LOG(TraceLevelWarning, "WARNING GrowDecisionTree pSmallChangeToModelOverwriteSingleSamplingSet->SetCountDivisions(0, cSplits)");
      return true;
   }
   if(IsMultiplyError(cVectorLength, cSplits + 1)) {
      LOG(TraceLevelWarning, "WARNING GrowDecisionTree IsMultiplyError(cVectorLength, cSplits + 1)");
      return true;
   }
   if(UNLIKELY(pSmallChangeToModelOverwriteSingleSamplingSet->EnsureValueCapacity(cVectorLength * (cSplits + 1)))) {
      LOG(TraceLevelWarning, "WARNING GrowDecisionTree pSmallChangeToModelOverwriteSingleSamplingSet->EnsureValueCapacity(cVectorLength * (cSplits + 1)");
      return true;
   }
   ActiveDataType * pDivisions = pSmallChangeToModelOverwriteSingleSamplingSet->GetDivisionPointer(0);
   FractionalDataType * pValues = pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer();

   LOG(TraceLevelVerbose, "Entered Flatten");
   pRootTreeNode->Flatten(&pDivisions, &pValues, cVectorLength);
   LOG(TraceLevelVerbose, "Exited Flatten");

   EBM_ASSERT(pSmallChangeToModelOverwriteSingleSamplingSet->GetDivisionPointer(0) <= pDivisions);
   EBM_ASSERT(static_cast<size_t>(pDivisions - pSmallChangeToModelOverwriteSingleSamplingSet->GetDivisionPointer(0)) == cSplits);
   EBM_ASSERT(pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer() < pValues);
   EBM_ASSERT(static_cast<size_t>(pValues - pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer()) == cVectorLength * (cSplits + 1));

   LOG(TraceLevelVerbose, "Exited GrowDecisionTree via normal exit");
   return false;
}

// TODO : make variable ordering consistent with BinDataSet call below (put the attribute first since that's a definition that happens before the training data set)
template<ptrdiff_t countCompilerClassificationTargetStates>
bool TrainZeroDimensional(CachedTrainingThreadResources<IsRegression(countCompilerClassificationTargetStates)> * const pCachedThreadResources, const SamplingMethod * const pTrainingSet, SegmentedRegionCore<ActiveDataType, FractionalDataType> * const pSmallChangeToModelOverwriteSingleSamplingSet, const size_t cTargetStates) {
   LOG(TraceLevelVerbose, "Entered TrainZeroDimensional");

   const size_t cVectorLength = GET_VECTOR_LENGTH(countCompilerClassificationTargetStates, cTargetStates);
   if(GetBinnedBucketSizeOverflow<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength)) {
      // TODO : move this to initialization where we execute it only once (it needs to be in the attribute combination loop though)
      LOG(TraceLevelWarning, "WARNING TODO fill this in");
      return true;
   }
   const size_t cBytesPerBinnedBucket = GetBinnedBucketSize<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength);
   BinnedBucket<IsRegression(countCompilerClassificationTargetStates)> * const pBinnedBucket = static_cast<BinnedBucket<IsRegression(countCompilerClassificationTargetStates)> *>(pCachedThreadResources->GetThreadByteBuffer1(cBytesPerBinnedBucket));
   if(UNLIKELY(nullptr == pBinnedBucket)) {
      LOG(TraceLevelWarning, "WARNING TrainZeroDimensional nullptr == pBinnedBucket");
      return true;
   }
   memset(pBinnedBucket, 0, cBytesPerBinnedBucket);

   BinDataSetTrainingZeroDimensions<countCompilerClassificationTargetStates>(pBinnedBucket, pTrainingSet, cTargetStates);

   const PredictionStatistics<IsRegression(countCompilerClassificationTargetStates)> * const aSumPredictionStatistics = &pBinnedBucket->aPredictionStatistics[0];
   if(IsRegression(countCompilerClassificationTargetStates)) {
      FractionalDataType smallChangeToModel = EbmStatistics::ComputeSmallChangeInRegressionPredictionForOneSegment(aSumPredictionStatistics[0].sumResidualError, pBinnedBucket->cCasesInBucket);
      FractionalDataType * pValues = pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer();
      pValues[0] = smallChangeToModel;
   } else {
      EBM_ASSERT(IsClassification(countCompilerClassificationTargetStates));
      FractionalDataType * aValues = pSmallChangeToModelOverwriteSingleSamplingSet->GetValuePointer();
      for(size_t iVector = 0; iVector < cVectorLength; ++iVector) {
         FractionalDataType smallChangeToModel = EbmStatistics::ComputeSmallChangeInClassificationLogOddPredictionForOneSegment(aSumPredictionStatistics[iVector].sumResidualError, aSumPredictionStatistics[iVector].GetSumDenominator());
         aValues[iVector] = smallChangeToModel;
      }
   }

   LOG(TraceLevelVerbose, "Exited TrainZeroDimensional");
   return false;
}

// TODO : make variable ordering consistent with BinDataSet call below (put the attribute first since that's a definition that happens before the training data set)
template<ptrdiff_t countCompilerClassificationTargetStates>
bool TrainSingleDimensional(CachedTrainingThreadResources<IsRegression(countCompilerClassificationTargetStates)> * const pCachedThreadResources, const SamplingMethod * const pTrainingSet, const AttributeCombinationCore * const pAttributeCombination, const size_t cTreeSplitsMax, const size_t cCasesRequiredForSplitParentMin, SegmentedRegionCore<ActiveDataType, FractionalDataType> * const pSmallChangeToModelOverwriteSingleSamplingSet, FractionalDataType * const pTotalGain, const size_t cTargetStates) {
   LOG(TraceLevelVerbose, "Entered TrainSingleDimensional");

   EBM_ASSERT(1 == pAttributeCombination->m_cAttributes);
   size_t cTotalBuckets = pAttributeCombination->m_AttributeCombinationEntry[0].m_pAttribute->m_cStates;

   const size_t cVectorLength = GET_VECTOR_LENGTH(countCompilerClassificationTargetStates, cTargetStates);
   if(GetBinnedBucketSizeOverflow<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength)) {
      // TODO : move this to initialization where we execute it only once (it needs to be in the attribute combination loop though)
      LOG(TraceLevelWarning, "WARNING TODO fill this in");
      return true;
   }
   const size_t cBytesPerBinnedBucket = GetBinnedBucketSize<IsRegression(countCompilerClassificationTargetStates)>(cVectorLength);
   if(IsMultiplyError(cTotalBuckets, cBytesPerBinnedBucket)) {
      // TODO : move this to initialization where we execute it only once (it needs to be in the attribute combination loop though)
      LOG(TraceLevelWarning, "WARNING TODO fill this in");
      return true;
   }
   const size_t cBytesBuffer = cTotalBuckets * cBytesPerBinnedBucket;
   BinnedBucket<IsRegression(countCompilerClassificationTargetStates)> * const aBinnedBuckets = static_cast<BinnedBucket<IsRegression(countCompilerClassificationTargetStates)> *>(pCachedThreadResources->GetThreadByteBuffer1(cBytesBuffer));
   if(UNLIKELY(nullptr == aBinnedBuckets)) {
      LOG(TraceLevelWarning, "WARNING TrainSingleDimensional nullptr == aBinnedBuckets");
      return true;
   }
   // !!! VERY IMPORTANT: zero our one extra bucket for BuildFastTotals to use for multi-dimensional !!!!
   memset(aBinnedBuckets, 0, cBytesBuffer);

#ifndef NDEBUG
   const unsigned char * const aBinnedBucketsEndDebug = reinterpret_cast<unsigned char *>(aBinnedBuckets) + cBytesBuffer;
#endif // NDEBUG

   BinDataSetTraining<countCompilerClassificationTargetStates, 1>(aBinnedBuckets, pAttributeCombination, pTrainingSet, cTargetStates
#ifndef NDEBUG
      , aBinnedBucketsEndDebug
#endif // NDEBUG
   );

   PredictionStatistics<IsRegression(countCompilerClassificationTargetStates)> * const aSumPredictionStatistics = pCachedThreadResources->m_aSumPredictionStatistics;
   memset(aSumPredictionStatistics, 0, sizeof(*aSumPredictionStatistics) * cVectorLength); // can't overflow, accessing existing memory

   size_t cBinnedBuckets = pAttributeCombination->m_AttributeCombinationEntry[0].m_pAttribute->m_cStates;
   EBM_ASSERT(1 <= cBinnedBuckets); // this function can handle 1 == cStates even though that's a degenerate case that shouldn't be trained on (dimensions with 1 state don't contribute anything since they always have the same value)
   size_t cCasesTotal;
   cBinnedBuckets = CompressBinnedBuckets<countCompilerClassificationTargetStates>(pTrainingSet, cBinnedBuckets, aBinnedBuckets, &cCasesTotal, aSumPredictionStatistics, cTargetStates
#ifndef NDEBUG
      , aBinnedBucketsEndDebug
#endif // NDEBUG
   );

   EBM_ASSERT(1 <= cCasesTotal);
   EBM_ASSERT(1 <= cBinnedBuckets);

   bool bRet = GrowDecisionTree<countCompilerClassificationTargetStates>(pCachedThreadResources, cTargetStates, cBinnedBuckets, aBinnedBuckets, cCasesTotal, aSumPredictionStatistics, cTreeSplitsMax, cCasesRequiredForSplitParentMin, pSmallChangeToModelOverwriteSingleSamplingSet, pTotalGain
#ifndef NDEBUG
      , aBinnedBucketsEndDebug
#endif // NDEBUG
   );

   LOG(TraceLevelVerbose, "Exited TrainSingleDimensional");
   return bRet;
}

#endif // TREE_NODE_H
