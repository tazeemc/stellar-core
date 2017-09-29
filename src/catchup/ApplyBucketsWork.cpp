// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyBucketsWork.h"
#include "bucket/Bucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "crypto/SecretKey.h"
#include "history/HistoryArchive.h"
#include "historywork/Progress.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/format.h"
#include "util/make_unique.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

ApplyBucketsWork::ApplyBucketsWork(
    Application& app, WorkParent& parent,
    std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
    HistoryArchiveState const& applyState)
    : Work(app, parent, std::string("apply-buckets"))
    , mBuckets(buckets)
    , mApplyState(applyState)
    , mApplying(false)
    , mLevel(BucketList::kNumLevels - 1)
    , mBucketApplyStart(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "start"}, "event"))
    , mBucketApplySuccess(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "success"}, "event"))
    , mBucketApplyFailure(app.getMetrics().NewMeter(
          {"history", "bucket-apply", "failure"}, "event"))
{
}

ApplyBucketsWork::~ApplyBucketsWork()
{
    clearChildren();
}

BucketLevel&
ApplyBucketsWork::getBucketLevel(size_t level)
{
    return mApp.getBucketManager().getBucketList().getLevel(level);
}

std::shared_ptr<Bucket const>
ApplyBucketsWork::getBucket(std::string const& hash)
{
    std::shared_ptr<Bucket const> b;
    if (isZero(hexToBin256(hash)))
    {
        b = std::make_shared<Bucket>();
    }
    else
    {
        auto i = mBuckets.find(hash);
        if (i != mBuckets.end())
        {
            b = i->second;
        }
        else
        {
            b = mApp.getBucketManager().getBucketByHash(hexToBin256(hash));
        }
    }
    assert(b);
    return b;
}

void
ApplyBucketsWork::onReset()
{
    mLevel = BucketList::kNumLevels - 1;
    mApplying = false;
    mSnapBucket.reset();
    mCurrBucket.reset();
    mSnapApplicator.reset();
    mCurrApplicator.reset();
}

void
ApplyBucketsWork::onStart()
{
    auto& level = getBucketLevel(mLevel);
    HistoryStateBucket const& i = mApplyState.currentBuckets.at(mLevel);
    if (mApplying || i.snap != binToHex(level.getSnap()->getHash()))
    {
        mSnapBucket = getBucket(i.snap);
        mSnapApplicator =
            make_unique<BucketApplicator>(mApp.getDatabase(), mSnapBucket);
        CLOG(DEBUG, "History") << "ApplyBuckets : starting level[" << mLevel
                               << "].snap = " << i.snap;
        mApplying = true;
        mBucketApplyStart.Mark();
    }
    if (mApplying || i.curr != binToHex(level.getCurr()->getHash()))
    {
        mCurrBucket = getBucket(i.curr);
        mCurrApplicator =
            make_unique<BucketApplicator>(mApp.getDatabase(), mCurrBucket);
        CLOG(DEBUG, "History") << "ApplyBuckets : starting level[" << mLevel
                               << "].curr = " << i.curr;
        mApplying = true;
        mBucketApplyStart.Mark();
    }
}

void
ApplyBucketsWork::onRun()
{
    if (mSnapApplicator && *mSnapApplicator)
    {
        mSnapApplicator->advance();
    }
    else if (mCurrApplicator && *mCurrApplicator)
    {
        mCurrApplicator->advance();
    }
    scheduleSuccess();
}

Work::State
ApplyBucketsWork::onSuccess()
{
    mApp.getCatchupManager().logAndUpdateCatchupStatus(true);

    if ((mSnapApplicator && *mSnapApplicator) ||
        (mCurrApplicator && *mCurrApplicator))
    {
        return WORK_RUNNING;
    }

    auto& level = getBucketLevel(mLevel);
    if (mSnapBucket)
    {
        level.setSnap(mSnapBucket);
        mBucketApplySuccess.Mark();
    }
    if (mCurrBucket)
    {
        level.setCurr(mCurrBucket);
        mBucketApplySuccess.Mark();
    }
    mSnapBucket.reset();
    mCurrBucket.reset();
    mSnapApplicator.reset();
    mCurrApplicator.reset();

    if (mLevel != 0)
    {
        --mLevel;
        CLOG(DEBUG, "History") << "ApplyBuckets : starting next level: "
                               << mLevel;
        return WORK_PENDING;
    }

    CLOG(DEBUG, "History") << "ApplyBuckets : done, restarting merges";
    mApp.getBucketManager().assumeState(mApplyState);
    return WORK_SUCCESS;
}

void
ApplyBucketsWork::onFailureRetry()
{
    mBucketApplyFailure.Mark();
    Work::onFailureRetry();
}

void
ApplyBucketsWork::onFailureRaise()
{
    mBucketApplyFailure.Mark();
    Work::onFailureRaise();
}
}
