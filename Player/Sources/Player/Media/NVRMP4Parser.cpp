
/** 
 * This file is part of Nokia OMAF implementation
 *
 * Copyright (c) 2018 Nokia Corporation and/or its subsidiary(-ies). All rights reserved.
 *
 * Contact: omaf@nokia.com
 *
 * This software, including documentation, is protected by copyright controlled by Nokia Corporation and/ or its
 * subsidiaries. All rights are reserved.
 *
 * Copying, including reproducing, storing, adapting or translating, any or all of this material requires the prior
 * written consent of Nokia.
 */
#include "Media/NVRMP4Parser.h"

#include <mp4vrfilereaderinterface.h>

#include "Media/NVRMP4MediaStream.h"
#include "Media/NVRMP4VideoStream.h"
#include "Media/NVRMP4AudioStream.h"
#include "Media/NVRMediaPacket.h"

#if OMAF_ENABLE_STREAM_VIDEO_PROVIDER
#include "Media/NVRMP4SegmentStreamer.h"
#endif

#include "VideoDecoder/NVRVideoDecoderManager.h"
#include "Foundation/NVRDeviceInfo.h"
#include "Foundation/NVRLogger.h"
#include "Foundation/NVRClock.h"
#include "Metadata/NVRCubemapDefinitions.h"

OMAF_NS_BEGIN
    OMAF_LOG_ZONE(MP4VRParser)

    MP4VRParser::MP4VRParser()
        : mAllocator(*MemorySystem::DefaultHeapAllocator())
        , mReader(OMAF_NULL)
        , mTimedMetadata(false)
        , mTracks(OMAF_NULL)
        , mMetaDataParser()
        , mFileStream()
#if OMAF_ENABLE_STREAM_VIDEO_PROVIDER
        , mMediaSegments(*MemorySystem::DefaultHeapAllocator())
        , mInitSegments()
        , mInitSegmentsContent()
#endif
        , mReaderMutex()
        , mClientAssignVideoStreamId(false)
    {
        mReader = MP4VR::MP4VRFileReaderInterface::Create();
    }

    MP4VRParser::~MP4VRParser()
    {
        mReader->close();
        MP4VR::MP4VRFileReaderInterface::Destroy(mReader);
        mReader = OMAF_NULL;
        OMAF_DELETE(mAllocator, mTracks);
        mFileStream.close();
#if OMAF_ENABLE_STREAM_VIDEO_PROVIDER
        if (!mSegmentIndexes.isEmpty())
        {
            for (SegmentIndexes::Iterator it = mSegmentIndexes.begin(); it != mSegmentIndexes.end(); ++it)
            {
                OMAF_DELETE(mAllocator, *it);
            }
            mSegmentIndexes.clear();
        }

        if (!mMediaSegments.isEmpty())
        {
            for (MediaSegmentMap::Iterator it = mMediaSegments.begin(); it != mMediaSegments.end(); ++it)
            {
                while (!(*it)->isEmpty())
                {
                    MP4SegmentStreamer *mediasegment = (*it)->front();
                    OMAF_DELETE(mAllocator, mediasegment);
                    (*it)->pop();
                }
                (*it)->clear();
                OMAF_DELETE(mAllocator, (*it));
            }
            mMediaSegments.clear();
        }

        if (!mInitSegments.isEmpty())
        {
            for (InitSegments::Iterator it = mInitSegments.begin(); it != mInitSegments.end(); ++it)
            {
                OMAF_DELETE(mAllocator, (*it).second);
            }
            mInitSegments.clear();
        }

        mInitSegmentsContent.clear();
#endif
    }


    Error::Enum MP4VRParser::parseVideoSources(MP4VideoStream& aVideoStream, SourceType::Enum aSourceType, BasicSourceInfo& aBasicSourceInfo)
    {
        MP4VR::PodvStereoVideoConfiguration stereoMode;
        if (mReader->getPropertyStereoVideoConfiguration(aVideoStream.getTrackId(), 0, stereoMode) == MP4VR::MP4VRFileReaderInterface::OK)
        {
            if (stereoMode == MP4VR::PodvStereoVideoConfiguration::TOP_BOTTOM_PACKING)
            {
                OMAF_LOG_D("Top-bottom stereo");
                // OMAF specifies that PackedContentInterpretationType is inferred to be 1, i.e. left is the first part of the frame
                aBasicSourceInfo.sourceDirection = SourceDirection::TOP_BOTTOM;
            }
            else if (stereoMode == MP4VR::PodvStereoVideoConfiguration::SIDE_BY_SIDE_PACKING)
            {
                OMAF_LOG_D("Side-by-side stereo");
                // OMAF specifies that PackedContentInterpretationType is inferred to be 1, i.e. left is the first part of the frame
                aBasicSourceInfo.sourceDirection = SourceDirection::LEFT_RIGHT;
            }
            else if (stereoMode == MP4VR::PodvStereoVideoConfiguration::TEMPORAL_INTERLEAVING)
            {
                return Error::FILE_NOT_SUPPORTED;
            }
            // else mono as set above
        }
        MP4VR::Rotation rotation;
        if (mReader->getPropertyRotation(aVideoStream.getTrackId(), 0, rotation) == MP4VR::MP4VRFileReaderInterface::OK)
        {
            aBasicSourceInfo.rotation.yaw = OMAF::toRadians((float32_t)rotation.yaw / 65536);
            aBasicSourceInfo.rotation.pitch = OMAF::toRadians((float32_t)rotation.pitch / 65536);
            aBasicSourceInfo.rotation.roll = OMAF::toRadians((float32_t)rotation.roll / 65536);
            if (aBasicSourceInfo.rotation.yaw != 0.f || aBasicSourceInfo.rotation.pitch != 0.f || aBasicSourceInfo.rotation.roll != 0.f)
            {
                aBasicSourceInfo.rotation.valid = true;
                OMAF_LOG_D("Rotation in degrees: (%f, %f, %f)", (float32_t)rotation.yaw / 65536, (float32_t)rotation.pitch / 65536, (float32_t)rotation.roll / 65536);
            }
        }

        if (aSourceType == SourceType::CUBEMAP)
        {
            MP4VR::RegionWisePackingProperty mp4Rwpk;
            if (mReader->getPropertyRegionWisePacking(aVideoStream.getTrackId(), 0, mp4Rwpk) == MP4VR::MP4VRFileReaderInterface::OK)
            {
                return mMetaDataParser.parseOmafCubemapRegionMetadata(mp4Rwpk, aBasicSourceInfo);
            }
            // else use previously obtained values, without RWPK. We come here only after OMAF projection metadata is detected so we have some info
        }
        else if (aSourceType == SourceType::EQUIRECTANGULAR_PANORAMA)
        {
            MP4VR::RegionWisePackingProperty mp4Rwpk;
            if (mReader->getPropertyRegionWisePacking(aVideoStream.getTrackId(), 0, mp4Rwpk) == MP4VR::MP4VRFileReaderInterface::OK)
            {
                return mMetaDataParser.parseOmafEquirectRegionMetadata(mp4Rwpk, aBasicSourceInfo);
            }
        }
        return Error::OK;
    }

    const CoreProviderSourceTypes& MP4VRParser::getVideoSourceTypes()
    {
        return mMetaDataParser.getVideoSourceTypes();
    }

    const CoreProviderSources& MP4VRParser::getVideoSources()
    {
        return mMetaDataParser.getVideoSources();
    }

    Error::Enum MP4VRParser::openInput(const PathName& mediaUri, MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {

        if (mFileStream.open(mediaUri) != Error::OK)
        {
            return Error::FILE_NOT_FOUND;
        }

        int32_t result = mReader->initialize(&mFileStream);

        if (result == MP4VR::MP4VRFileReaderInterface::ErrorCode::OK)
        {
            Error::Enum ret = prepareFile(audioStreams, videoStreams);
            if (ret != Error::OK)
            {
                return ret;
            }
            else
            {
                ret = readInitialMetadata(mediaUri, audioStreams, videoStreams);
                if (ret == Error::OK)
                {
                    if (!videoStreams.isEmpty())
                    {
                        MP4VideoStream* first = *videoStreams.begin();
                        mMediaInformation.width = first->getFormat()->width();
                        mMediaInformation.height = first->getFormat()->height();
                        mMediaInformation.frameRate = first->getFormat()->frameRate();
                        mMediaInformation.duration = first->getFormat()->durationUs();
                        mMediaInformation.numberOfFrames = first->getFrameCount();
                    }
                    else
                    {
                        mMediaInformation.fileType = MediaFileType::INVALID;
                    }
                }
                else
                {
                    mMediaInformation.fileType = MediaFileType::INVALID;
                }
                return ret;
            }
        }
        else if (result == MP4VR::MP4VRFileReaderInterface::ErrorCode::FILE_OPEN_ERROR)
        {
            return Error::FILE_NOT_FOUND;
        }
        else if (result == MP4VR::MP4VRFileReaderInterface::ErrorCode::FILE_READ_ERROR)
        {
            return Error::FILE_NOT_MP4;
        }
        else
        {
            return Error::FILE_OPEN_FAILED;
        }

    }

    Error::Enum MP4VRParser::prepareFile(MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams, uint32_t initSegmentId)
    {
        MP4VR::FourCC major;
        if (mReader->getMajorBrand(major, initSegmentId) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return Error::INVALID_DATA;
        }
        uint32_t version;
        if (mReader->getMinorVersion(version, initSegmentId) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return Error::INVALID_DATA;
        }
        MP4VR::DynArray<MP4VR::FourCC> brands;
        if (mReader->getCompatibleBrands(brands, initSegmentId) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return Error::INVALID_DATA;
        }

        FileFormatType::Enum fileFormat = FileFormatType::UNDEFINED;
        bool_t playable = false;
        
        for (MP4VR::FourCC* it = brands.begin(); it != brands.end(); it++)
        {
            if (*it == MP4VR::FourCC("mp41"))
            {
                // MP4 is an instance of the ISO Media File format.The general nature of the ISO Media File format is fully exercised by MP4.
                // The brand ‘mp41’ is defined as identifying version 1 of specification (ISO/IEC 14496-1:2001),
                playable = true;
            }
            else if (*it == MP4VR::FourCC("mp42"))
            {
                // In addition to above brand ‘mp42’ identifies specification version 14496-14:2003(E)
                playable = true;
            }
            else if (*it == MP4VR::FourCC("dash"))
            {
                playable = true;
            }
            else if ( *it == MP4VR::FourCC("isom") ||
                      *it == MP4VR::FourCC("avc1") || // avc1 indicates support also for isom brand (as it requires it).
                      *it == MP4VR::FourCC("iso2") || // iso2 requires support for all avc1 features. iso2 can be used in addition or instead of isom.
                      *it == MP4VR::FourCC("iso3") || // iso3 requires support for all features of iso2.
                      *it == MP4VR::FourCC("iso4") || // iso4 requires support for all features of iso3.
                      *it == MP4VR::FourCC("iso5") || // iso5 requires support for all features of iso4.
                      *it == MP4VR::FourCC("iso6") || // iso6 requires support for all features of iso5.
                      *it == MP4VR::FourCC("iso7") || // iso7 requires support for all features of iso6.
                      *it == MP4VR::FourCC("iso8") || // iso8 requires support for all features of iso7.
                      *it == MP4VR::FourCC("iso9")    // iso9 requires support for all features of iso8.
                     )
            {
                playable = true;
            }
            else if (*it == MP4VR::FourCC("hevi") ||
                     *it == MP4VR::FourCC("hevd"))
            {
                // one of the tracks follows OMAF HEVC viewport independent or dependent profile, ok
                fileFormat = FileFormatType::OMAF;
                playable = true;
            }
        }

        if (!playable)
        {
            OMAF_LOG_E("The file is not compatible: no 'isom'/'iso'+(1-9), 'dash' or 'mp41'/'mp42' brands found");
            return Error::FILE_NOT_SUPPORTED;
        }

        // reading MP4VR::FileInformation doesn't seem necessary, since it only indicates if the file has VR audio/video, which we know by other means
        // create streams
        Error::Enum ret = createStreams(audioStreams, videoStreams, fileFormat);

        if (ret != Error::OK)
        {
            return ret;
        }

        return ret;
    }

#if OMAF_ENABLE_STREAM_VIDEO_PROVIDER
    Error::Enum MP4VRParser::openSegmentedInput(MP4Segment* mp4Segment, bool_t aClientAssignVideoStreamId)
    {
        mClientAssignVideoStreamId = aClientAssignVideoStreamId;

        InitSegment initSegment(mp4Segment->getInitSegmentId(), OMAF_NEW(mAllocator, MP4SegmentStreamer)(mp4Segment));

        int32_t result = mReader->parseInitializationSegment(initSegment.second, initSegment.first);
        if (result != MP4VR::MP4VRFileReaderInterface::ErrorCode::OK)
        {
            OMAF_DELETE(mAllocator, initSegment.second);
            return Error::INVALID_DATA;
        }
        else
        {
            mInitSegments.add(initSegment);
            SegmentContent segmentContent;
            if (mp4Segment->getSegmentContent(segmentContent))
            { // works only for DASH segments, HLS returns false always
                mInitSegmentsContent.add(segmentContent);
            }
            return Error::OK;
            // we don't create streams yet at this point, because mp4reader can't provide all the necessary info, e.g. decoderConfigInfo
        }
    }

    Error::Enum MP4VRParser::addSegment(MP4Segment* mp4Segment, MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        bool initSegmentExists = false;
        for (InitSegments::Iterator it = mInitSegments.begin(); it != mInitSegments.end(); ++it)
        {
            if ((*it).first == mp4Segment->getInitSegmentId())
            {
                initSegmentExists = true;
            }
        }

        if (!initSegmentExists)
        {
            return Error::INVALID_STATE;
        }

        Mutex::ScopeLock readerLock(mReaderMutex);

        MP4SegmentStreamer* segment = OMAF_NEW(mAllocator, MP4SegmentStreamer)(mp4Segment);
        MediaSegmentMap::Iterator segmentQueue = mMediaSegments.find(mp4Segment->getInitSegmentId());
        if (segmentQueue == MediaSegmentMap::InvalidIterator)
        {// new media segment queue
            MediaSegments* mediasegments = OMAF_NEW(mAllocator, MediaSegments)();
            mMediaSegments.insert(mp4Segment->getInitSegmentId(), mediasegments);
            segmentQueue = mMediaSegments.find(mp4Segment->getInitSegmentId());
        }

        uint64_t earliestPTSinTS = UINT64_MAX;
        if (mp4Segment->isSubSegment())
        {
            // look for valid base PTS for this subsegment
            for (SegmentIndexes::Iterator it = mSegmentIndexes.begin(); it != mSegmentIndexes.end(); ++it)
            {
                if ((*it)->elements[0].segmentId != mp4Segment->getSegmentId())
                {
                    continue;
                }

                for (MP4VR::DynArray<MP4VR::SegmentInformation>::iterator subSegIt = (*it)->begin(); subSegIt != (*it)->end(); ++subSegIt)
                {
                    if (subSegIt != (*it)->begin() &&
                        subSegIt->startDataOffset == mp4Segment->rangeStartByte())
                    {
                        OMAF_LOG_D("parsing subsegment %d, startByte: %d, startPTSinMS: %d", mp4Segment->getSegmentId(), mp4Segment->rangeStartByte(), subSegIt->earliestPTSinTS*1000/subSegIt->timescale);
                        earliestPTSinTS = subSegIt->earliestPTSinTS;
                        break;
                    }
                }
            }
        }

        if (mReader->parseSegment(segment, mp4Segment->getInitSegmentId(), mp4Segment->getSegmentId(), earliestPTSinTS) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            OMAF_DELETE(mAllocator, segment);
            return Error::INVALID_DATA;
        }
        else
        {
            (*segmentQueue)->push(segment);
        }

        if (mTracks == OMAF_NULL)
        {
            SegmentContent segmentContent;
            mp4Segment->getSegmentContent(segmentContent);
            if (segmentContent.associatedToRepresentationId.getLength())
            {
                // this segment depends on other representation, but we don't yet have mediasegment for that representation. So don't prepare streams yet.
                bool initSegmentAssociationPresent = false;
                // just to make sure... enclosing if should cover this though.
                for (InitSegments::Iterator it = mInitSegments.begin(); it != mInitSegments.end(); ++it)
                {
                    if ((*it).first == segmentContent.associatedToInitializationSegmentId)
                    {
                        initSegmentAssociationPresent = true;
                    }
                }

                if (!initSegmentAssociationPresent)
                {// not ready yet... just return.
                    return Error::OK;
                }
            }

            Error::Enum ret = prepareFile(audioStreams, videoStreams, mp4Segment->getInitSegmentId());
            if (ret != Error::OK)
            {
                return ret;
            }
            else
            {
                // read metadata from the stream - URI-based default metadata need to be handled elsewhere
                return readInitialMetadata("", audioStreams, videoStreams);  // in DASH streaming, we support only 1 track per adaptation set - same with HLS
            }
        }
        else
        {
            updateStreams(audioStreams, videoStreams);
        }

        releaseUsedSegments(audioStreams, videoStreams);

        return Error::OK;
    }

    Error::Enum MP4VRParser::addSegmentIndex(MP4Segment* mp4Segment)
    {
        // No mutex on purpose - parseSegmentIndex isn't effected by other mReader usage
        uint32_t segmentId = mp4Segment->getSegmentId();
        OMAF_LOG_V("add SegmentIndex iD:%d", segmentId);

        // safety check for duplicates - shoudn't be needed if dashprovider is working correctly.
        for (SegmentIndexes::Iterator it = mSegmentIndexes.begin(); it != mSegmentIndexes.end(); ++it)
        {
            if ((*it)->elements[0].segmentId == segmentId)
            {// already have this segmend index for this segmentid so return
                OMAF_LOG_D("!!!!! Already have SegmentIndex for segment iD:%d !!!!!!", segmentId);
                return Error::OK;
            }
        }

        // Do some cleanup (since Download manager doesn't know about removed Media Segments just use constant max count for queue management
        if (mSegmentIndexes.getSize() >= SEGMENT_INDEX_COUNT_MAX)
        {
            for (SegmentIndexes::Iterator it = mSegmentIndexes.begin(); it != mSegmentIndexes.end(); ++it)
            {
                if (segmentId > SEGMENT_INDEX_COUNT_MAX)
                {// start clearing old ones once we get past rollover point and segment id is older than SEGMENT_INDEX_COUNT_MAX compared to new
                    if ((*it)->elements[0].segmentId <= (segmentId - SEGMENT_INDEX_COUNT_MAX))
                    {
                        OMAF_DELETE(mAllocator, *it);
                        mSegmentIndexes.remove(it);
                        continue;
                    }
                }

                // in case we have seeked backwards and have old segment indexes filling the array.
                if ((*it)->elements[0].segmentId > (segmentId + SEGMENT_INDEX_COUNT_MAX))
                {
                    OMAF_DELETE(mAllocator, *it);
                    mSegmentIndexes.remove(it);
                }
            }
        }

        MP4SegmentStreamer* segment = OMAF_NEW(mAllocator, MP4SegmentStreamer)(mp4Segment);
        MP4VR::DynArray<MP4VR::SegmentInformation>* segmentIndex = OMAF_NEW(mAllocator, MP4VR::DynArray<MP4VR::SegmentInformation>);

        if (mReader->parseSegmentIndex(segment, *segmentIndex) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            OMAF_DELETE(mAllocator, segmentIndex);
            OMAF_DELETE(mAllocator, segment);
            return Error::INVALID_DATA;
        }

        // check if there is no subsegments in segment index -> so don't store it and report upwards so download can be disabled.
        if (segmentIndex->size <= 1)
        {
            OMAF_DELETE(mAllocator, segmentIndex);
            OMAF_DELETE(mAllocator, segment);
            return Error::NOT_SUPPORTED;
        }

        // fix segmentId field in Segment Index:
        for (uint32_t i = 0; i < segmentIndex->size; ++i)
        {
            segmentIndex->elements[i].segmentId = segmentId;
        }

        mSegmentIndexes.add(segmentIndex);
        OMAF_DELETE(mAllocator, segment);

        return Error::OK;
    }

    Error::Enum MP4VRParser::hasSegmentIndexFor(uint32_t segmentId, uint64_t presentationTimeUs)
    {
        uint64_t pstInMs = presentationTimeUs / 1000;
        size_t noOfSubsegments = 0;
        for (SegmentIndexes::Iterator it = mSegmentIndexes.begin(); it != mSegmentIndexes.end(); ++it)
        {
            noOfSubsegments = (*it)->size;
            if ((*it)->elements[0].segmentId != segmentId ||
                 noOfSubsegments <= 1)
            {
                continue;
            }

            // loop through subsegments - skip first one (as we should then just download whole segment)
            for (MP4VR::DynArray<MP4VR::SegmentInformation>::iterator subSegIt = (*it)->begin(); subSegIt != (*it)->end(); ++subSegIt)
            {
                // debug helpers
                // uint64_t subSegStart = (subSegIt->earliestPTSinTS * 1000) / subSegIt->timescale;
                // uint64_t subSegEnd = ((subSegIt->earliestPTSinTS + subSegIt->durationInTS) * 1000) / subSegIt->timescale;

                if (subSegIt == (*it)->begin())
                { // always skip first subsegment as we don't want to use subsegment downloading for full segments.
                    continue;
                }

                if (subSegIt->earliestPTSinTS * 1000 / subSegIt->timescale <= pstInMs &&
                    (subSegIt->earliestPTSinTS + subSegIt->durationInTS) * 1000 / subSegIt->timescale > pstInMs)
                {
                    return Error::OK;
                }
            }
        }
        if (noOfSubsegments == 1)
        {// special case where there is no subsegments in segment, 0 is valid count if segmentindex hasn't been parsed yet
            return Error::NOT_SUPPORTED;
        }

        return Error::ITEM_NOT_FOUND;
    }

    Error::Enum MP4VRParser::getSegmentIndexFor(uint32_t segmentId, uint64_t presentationTimeUs, SubSegment& subSegment)
    {
        uint64_t pstInMs = presentationTimeUs / 1000;
        bool_t found = false;
        for (SegmentIndexes::Iterator it = mSegmentIndexes.begin(); it != mSegmentIndexes.end(); ++it)
        {
            if ((*it)->elements[0].segmentId != segmentId)
            {
                continue;
            }

            // loop through subsegments - skip first one (as we should then just download whole segment)
            for (MP4VR::DynArray<MP4VR::SegmentInformation>::iterator subSegIt = (*it)->begin(); subSegIt != (*it)->end(); ++subSegIt)
            {
                if (subSegIt == (*it)->begin())
                { // always skip first subsegment as we don't want to use subsegment downloading for full segments.
                    continue;
                }

                if (subSegIt->earliestPTSinTS * 1000 / subSegIt->timescale <= pstInMs &&
                    (subSegIt->earliestPTSinTS + subSegIt->durationInTS) * 1000 / subSegIt->timescale > pstInMs)
                {
                    found = true;
                    subSegment.segmentId = subSegIt->segmentId;
                    subSegment.earliestPresentationTimeMs = subSegIt->earliestPTSinTS * 1000/ subSegIt->timescale;
                    subSegment.startByte = subSegIt->startDataOffset;
                    subSegment.endByte = subSegIt->startDataOffset + subSegIt->dataSize - 1;  // http range requests are inclusive
                }
                if (found)
                {
                    subSegment.endByte = subSegIt->startDataOffset + subSegIt->dataSize - 1; // add also rest of the segment to byte range
                }
            }
            if (found)
            {
                return Error::OK;
            }
        }
        return Error::ITEM_NOT_FOUND;
    }

    void_t MP4VRParser::releaseUsedSegments(MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        Mutex::ScopeLock readerLock(mReaderMutex);

        // check if there are segments which are not needed any more
        if ((audioStreams.getSize() > 0 || videoStreams.getSize() > 0))
        {
            uint32_t oldestInUseSegmentId = OMAF_UINT32_MAX;

            for (size_t i = 0; i < videoStreams.getSize(); i++)
            {
                oldestInUseSegmentId = min(oldestInUseSegmentId, videoStreams[i]->getCurrentSegmentId());
            }

            for (size_t i = 0; i < audioStreams.getSize(); i++)
            {
                oldestInUseSegmentId = min(oldestInUseSegmentId, audioStreams[i]->getCurrentSegmentId());
            }

            if (!mMediaSegments.isEmpty())
            {
                for (MediaSegmentMap::Iterator it = mMediaSegments.begin(); it != mMediaSegments.end(); ++it)
                {
                    while(!(*it)->isEmpty())
                    {
                        MP4SegmentStreamer* mediasegment = (*it)->front();
                        if (mediasegment != OMAF_NULL && mediasegment->getSegmentId() < oldestInUseSegmentId)
                        {
                            if (!videoStreams.isEmpty())
                            {
                                OMAF_LOG_V("removing %d from %d", mediasegment->getSegmentId(), videoStreams[0]->getStreamId());
                            }
                            removeSegment(mediasegment->getInitSegmentId(), mediasegment->getSegmentId(), audioStreams, videoStreams);
                        }
                        else
                        {
                            break;
                        }
                    }
                }
            }
        }
    }

    Error::Enum MP4VRParser::removeSegment(uint32_t initSegmentId, uint32_t segmentId, MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        OMAF_LOG_V("removeSegment %d", segmentId);
        Mutex::ScopeLock readerLock(mReaderMutex);

        int32_t result = mReader->invalidateSegment(initSegmentId, segmentId);

        if (result == MP4VR::MP4VRFileReaderInterface::OK)
        {
            updateStreams(audioStreams, videoStreams);

            // remove the segment from queue
            //TODO replace if with the assert when mp4Reader returns error; now this would break the test run
            //OMAF_ASSERT(mSegments.getSize() > 0, "Segment queue mismatch");
            if (mMediaSegments.contains(initSegmentId) && (*mMediaSegments.find(initSegmentId))->isEmpty())
            {
                return Error::OPERATION_FAILED;
            }
            MP4SegmentStreamer *segment = (*mMediaSegments.find(initSegmentId))->front();
            if (segment->getSegmentId() == segmentId)
            {
                (*mMediaSegments.find(initSegmentId))->pop();
                OMAF_DELETE(mAllocator, segment);
            }
            else
            {
                return Error::OPERATION_FAILED;
            }
            // what if ids don't match??
            return Error::OK;
        }
        else
        {
            OMAF_LOG_D("Remove failed mp4 error %d", result);
            // Still remove the segment to avoid infinite loop
            if ((*mMediaSegments.find(initSegmentId))->getSize())
            {
                MP4SegmentStreamer *segment = (*mMediaSegments.find(initSegmentId))->front();
                if (segment->getSegmentId() == segmentId)
                {
                    (*mMediaSegments.find(initSegmentId))->pop();
                    OMAF_DELETE(mAllocator, segment);//TODO segment was allocated with mAllocator but the dashSegment inside it was allocated from heap??!!
                }
            }
            return Error::OPERATION_FAILED;
        }
    }

    bool_t MP4VRParser::getNewestSegmentId(uint32_t initSegmentId, uint32_t& segmentId)
    {
        Mutex::ScopeLock readerLock(mReaderMutex);
        for (MediaSegmentMap::Iterator it = mMediaSegments.begin(); it != mMediaSegments.end(); ++it)
        {
            if (!(*it)->isEmpty() &&
                (*it)->back()->getInitSegmentId() == initSegmentId)
            {
                segmentId = (*it)->back()->getSegmentId();
                return true;
            }
        }
        return false;
    }

    bool_t MP4VRParser::readyForSegment(MP4VideoStreams& videoStreams, uint32_t aSegmentId)
    {
        Mutex::ScopeLock readerLock(mReaderMutex);//needed??
        if (videoStreams.isEmpty())
        {
            return true;
        }
        if (videoStreams[0]->getCurrentSegmentId() == 0 || (aSegmentId - videoStreams[0]->getCurrentSegmentId() == 1 && videoStreams[0]->getSamplesLeft() < 3))
        {
            return true;
        }
        return false;
    }

    uint64_t MP4VRParser::getReadPositionUs(MP4VideoStreams& videoStreams, uint32_t& segmentIndex)
    {
        uint64_t currentPosMs = 0;
        for (size_t i = 0; i < videoStreams.getSize(); i++)
        {
            uint64_t pos = videoStreams[i]->getCurrentReadPositionMs();
            if (pos > currentPosMs)
            {
                currentPosMs = pos;
                segmentIndex = videoStreams[i]->getCurrentSegmentId();
            }
        }
        
        return currentPosMs*1000;
    }

    uint32_t MP4VRParser::releaseSegmentsUntil(uint32_t segmentId, MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        uint32_t unused = 0;
        Mutex::ScopeLock readerLock(mReaderMutex); // protects mReader operations and stream's sample table
        if (audioStreams.getSize() > 0 || videoStreams.getSize() > 0)
        {
            bool_t segmentCounted = false;
            if (!videoStreams.isEmpty() && videoStreams[0]->isCurrentSegmentInProgress())
            {
                OMAF_LOG_D("removing partially used segment");
                segmentCounted = true;
            }
            for (MediaSegmentMap::Iterator it = mMediaSegments.begin(); it != mMediaSegments.end(); ++it)
            {
                while (!(*it)->isEmpty() && (*it)->front()->getSegmentId() < segmentId)
                {
                    MP4SegmentStreamer *segment = (*it)->front();
                    if (!segmentCounted)
                    {
                        unused++;
                        OMAF_LOG_D("removing unused segment");
                    }
                    segmentCounted = false;
                    removeSegment(segment->getInitSegmentId(), segment->getSegmentId(), audioStreams, videoStreams);
                }
            }
        }
        return unused;
    }

    bool_t MP4VRParser::releaseAllSegments(MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        Mutex::ScopeLock readerLock(mReaderMutex); // protects mReader operations and stream's sample table

        if (!mSegmentIndexes.isEmpty())
        {
            for (SegmentIndexes::Iterator it = mSegmentIndexes.begin(); it != mSegmentIndexes.end(); ++it)
            {
                OMAF_DELETE(mAllocator, (*it));
            }
            mSegmentIndexes.clear();
        }

        mInitSegmentsContent.clear();

        if (!mMediaSegments.isEmpty())
        {
            for (MediaSegmentMap::Iterator mediaSegmentIt = mMediaSegments.begin(); mediaSegmentIt != mMediaSegments.end(); ++mediaSegmentIt)
            {
                if ((*mediaSegmentIt)->getSize() > 0 && (audioStreams.getSize() > 0 || videoStreams.getSize() > 0))
                {
                    uint32_t initSegmentId = (*mediaSegmentIt)->front()->getInitSegmentId();
                    while (!(*mediaSegmentIt)->isEmpty())
                    {
                        MP4SegmentStreamer *segment = (*mediaSegmentIt)->front();
                        // removes media segment and pops it from FixedQueue
                        removeSegment(initSegmentId, segment->getSegmentId(), audioStreams, videoStreams);
                    }
                    (*mediaSegmentIt)->clear();

                    for (InitSegments::Iterator initSegmentIt = mInitSegments.begin(); initSegmentIt != mInitSegments.end(); ++initSegmentIt)
                    {
                        if ((*initSegmentIt).first == initSegmentId)
                        {
                            // and then invalidate the init segment
                            mReader->invalidateInitializationSegment(initSegmentId);
                            // if fails, what can we do, we are invalidating data here, so we just ignore the error as we won't use this data any more anyways?

                            OMAF_DELETE(mAllocator, (*initSegmentIt).second);
                            mInitSegments.remove(*initSegmentIt);
                            break;
                        }
                    }
                }
            }
            return true;
        }
        return false;
    }

    bool_t MP4VRParser::hasSegmentIndexForSegmentId(uint32_t segmentId)
    {
        Mutex::ScopeLock readerLock(mReaderMutex);

        bool_t found = false;
        for (SegmentIndexes::Iterator segIndex = mSegmentIndexes.begin(); segIndex != mSegmentIndexes.end(); ++segIndex)
        {
            for (MP4VR::DynArray<MP4VR::SegmentInformation>::iterator it = (*segIndex)->begin(); it != (*segIndex)->end(); ++it)
            {
                if (it->segmentId == segmentId)
                {
                    found = true;
                }
            }
        }
        return found;
    }

#endif

    Error::Enum MP4VRParser::readFrame(MP4MediaStream& stream, bool_t& segmentChanged, int64_t currentTimeUs)
    {
        Mutex::ScopeLock readerLock(mReaderMutex); // protects mReader operations and stream's sample table

        MP4VRMediaPacket* packet = stream.getNextEmptyPacket();

        if (packet == OMAF_NULL)
        {
            // stream.mTrack may be NULL and then we can't read data
            return Error::END_OF_FILE;
        }

        uint32_t packetSize = (uint32_t)packet->bufferSize();
        uint32_t sample = 0;
        bool_t configChanged = false;
        uint32_t configId = 0;
        uint64_t sampleTimeMs = 0;

        packet->setSampleId(0); // initialize to 0, set to real sampleId later
        if (stream.IsVideo())
        {
            MP4VideoStream& videoStream = (MP4VideoStream&)stream;

            if (videoStream.getNextSample(sample, sampleTimeMs, configChanged, configId, segmentChanged) == Error::END_OF_FILE)
            {
                stream.returnEmptyPacket(packet);
                return Error::END_OF_FILE;
            }
            if (videoStream.isFrameLate(sampleTimeMs, (uint32_t)(currentTimeUs/1000)))
            {
                OMAF_LOG_D("stream %d frame is late (%llu - %llu), trying to skip", videoStream.getStreamId(), sampleTimeMs, currentTimeUs/1000);
                // too old frame, check if we have a reference frame nearby and seek to it
                // skip max 15 frames at a time (0.5 sec)
                uint64_t syncFrameTimeMs = 0;
                uint64_t targetTimeMs = (uint64_t)(currentTimeUs / 1000);
                uint32_t delta = videoStream.framesUntilNextSyncFrame(syncFrameTimeMs);
                OMAF_LOG_D("next ref frame after %u frames, time %llu target %llu", delta, syncFrameTimeMs, targetTimeMs);
                if (delta < OMAF_UINT32_MAX && syncFrameTimeMs <= targetTimeMs)
                {
                    // can skip the rest of the GOP
                    if (seekToSyncFrame(sample, &stream, SeekDirection::NEXT, segmentChanged) >= 0)
                    {
                        OMAF_LOG_D("stream %d skipped frames until next sync frame", videoStream.getStreamId());
                        bool_t tmpSegmentChanged; // if we switch segment, we switch it during the seek, not when reading the first sample after seek
                        if (stream.getNextSample(sample, sampleTimeMs, configChanged, configId, tmpSegmentChanged) == Error::END_OF_FILE)
                        {
                            stream.returnEmptyPacket(packet);

                            return Error::END_OF_FILE;
                        }
                    }
                    else
                    {
                        OMAF_LOG_D("Failed to seek %d", stream.getStreamId());
                    }
                }
            }
        }
        else
        {
            if (stream.getNextSample(sample, sampleTimeMs, configChanged, configId, segmentChanged) == Error::END_OF_FILE)
            {
                stream.returnEmptyPacket(packet);
                return Error::END_OF_FILE;
            }
        }

        packet->setReConfigRequired(false);

        if (configChanged)
        {
            MP4VR::DynArray<MP4VR::DecoderSpecificInfo> info;

            if (mReader->getDecoderConfiguration(stream.getTrackId(), sample, info) == MP4VR::MP4VRFileReaderInterface::ErrorCode::OK)
            {
                stream.getFormat()->setDecoderConfigInfo(info, configId);
                packet->setReConfigRequired(true);
            }
            // if it failed, we just keep using the previous config info (and hope for the best)
        }

        bool_t bytestreamMode = false;

        if (stream.IsVideo())
        {
            bytestreamMode = VideoDecoderManager::getInstance()->isByteStreamHeadersMode();
        }

        //OMAF_LOG_D("getTrackSampleData buffer size %d bytes", packetSize);
        int32_t result = mReader->getTrackSampleData(stream.getTrackId(), sample, (char_t*)(packet->buffer()), packetSize, bytestreamMode);

        if (result == MP4VR::MP4VRFileReaderInterface::MEMORY_TOO_SMALL_BUFFER)
        {
            // retry with reallocated larger buffer
            //OMAF_LOG_D("getTrackSampleData reallocate to %d bytes", packetSize);
            packet->resizeBuffer(packetSize);
            result = mReader->getTrackSampleData(stream.getTrackId(), sample, (char_t*)(packet->buffer()), packetSize, bytestreamMode);
        }
        //OMAF_LOG_D("getTrackSampleData returned %d bytes", packetSize);

        if (result != MP4VR::MP4VRFileReaderInterface::OK)
        {
            stream.returnEmptyPacket(packet);

            return Error::INVALID_DATA;
        }

        packet->setDataSize(packetSize);
        packet->setSampleId(sample);
        packet->setPresentationTimeUs(sampleTimeMs * 1000);

        uint32_t duration = 0;

        if (mReader->getSampleDuration(stream.getTrackId(), sample, duration) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return Error::INVALID_DATA;
        }

        packet->setDurationUs(duration * 1000);
        packet->setIsReferenceFrame(stream.isReferenceFrame());
        packet->setDecodingTimeUs(Clock::getMicroseconds());//we use current time as the decoding timestamp; it just indicates the decoding order. Prev solution (stream count) didn't work with alternate streams

        stream.storeFilledPacket(packet);

        if (stream.getMetadataStream() != OMAF_NULL)
        {
            bool_t changed = false;
            MP4MediaStream* invoStream = stream.getMetadataStream();
            
            MP4VR::TimestampIDPair sample;
            invoStream->peekNextSampleTs(sample);
            if (sample.timeStamp == sampleTimeMs)
            {
                readFrame(*invoStream, changed);
            }
        }

        if (result == MP4VR::MP4VRFileReaderInterface::OK)
        {
            return Error::OK;
        }
        else
        {
            return Error::INVALID_DATA;
        }
    }

    Error::Enum MP4VRParser::readInitialMetadata(const PathName& mediaUri, MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        bool_t videoMetadataMissing = true;
        bool_t audioMetadataMissing = true;

        Error::Enum result = Error::OK;
        bool_t removedColorTrack = false;

        uint32_t pixelsPerSecond = 0;

        for (MP4VideoStreams::ConstIterator it = videoStreams.begin(); it != videoStreams.end(); ++it)
        {
            const MediaFormat* format = (*it)->getFormat();
            float32_t frameRate = 0.0f;

            // If framerate is zeroish, assume it's not valid and use 30
            if (format->frameRate() < 1.0f)
            {
                frameRate = 30.0f;
            }
            else
            {
                frameRate = format->frameRate();
            }
            pixelsPerSecond += (uint32_t)(format->width() * format->height() * frameRate);
        }

        uint32_t maxDecodedPixelsPerSecond = DeviceInfo::maxDecodedPixelCountPerSecond();

        if (pixelsPerSecond > maxDecodedPixelsPerSecond && videoStreams.getSize() > 1)
        {
            OMAF_LOG_W("Can't support all the video tracks in the file so removing some of the tracks");

            pixelsPerSecond *= 0.95f; // allow 5% margin e.g. due to framerate inaccuracy
            while (pixelsPerSecond > DeviceInfo::maxDecodedPixelCountPerSecond() && !videoStreams.isEmpty())
            {
                MP4VideoStream* stream = videoStreams.back();
                const MediaFormat* format = stream->getFormat();
                float32_t frameRate = 0.0f;
                // If framerate is zeroish, assume it's not valid and use 30
                if (format->frameRate() < 1.0f)
                {
                    frameRate = 30.0f;
                }
                else
                {
                    frameRate = format->frameRate();
                }
                pixelsPerSecond -= (uint32_t)(format->width() * format->height() * frameRate);
                videoStreams.remove(stream);
                OMAF_DELETE(mAllocator, stream);
                removedColorTrack = true;
                if (videoStreams.isEmpty())
                {
                    OMAF_LOG_E("No video streams left!!");
                    return Error::FILE_NOT_SUPPORTED;
                }
            }
        }

        if (!mediaUri.isEmpty())
        {
            // check OMAF properties on offline mp4 clips
            // only single stream support for now, but OMAF doesn't require support for more than 1
            BasicSourceInfo sourceInfo;
            MP4VR::ProjectionFormatProperty projectionFormat;
            for (MP4VR::TrackInformation* track = mTracks->begin(); track != mTracks->end(); track++)
            {
                if (track->trackId == videoStreams[0]->getTrackId())
                {
                    if (mReader->getPropertyProjectionFormat(track->trackId, track->sampleProperties[0].sampleId, projectionFormat) == MP4VR::MP4VRFileReaderInterface::OK)
                    {
                        if (projectionFormat.format == MP4VR::OmafProjectionType::EQUIRECTANGULAR)
                        {
                            OMAF_LOG_D("Equirectangular");
                            sourceInfo.sourceType = SourceType::EQUIRECTANGULAR_PANORAMA;
                        }
                        else if (projectionFormat.format == MP4VR::OmafProjectionType::CUBEMAP)
                        {
                            OMAF_LOG_D("Cubemap");
                            sourceInfo.sourceType = SourceType::CUBEMAP;
                        }

                        sourceInfo.sourceDirection = SourceDirection::MONO;
                        // Default OMAF face order is "LFRDBU", with 2nd row faces rotated right
                        sourceInfo.faceOrder = DEFAULT_OMAF_FACE_ORDER;       // a coded representation of LFRDBU
                        sourceInfo.faceOrientation = DEFAULT_OMAF_FACE_ORIENTATION;  // a coded representation of 000111. A merge of enum of individual faces / face sections, that can be many

                        Error::Enum propertiesOk = parseVideoSources(*videoStreams[0], sourceInfo.sourceType, sourceInfo);
                        if (propertiesOk != Error::OK)
                        {
                            return propertiesOk;
                        }
                        // if there was no useful source info, we use the default values (mono, no/default RWPK)
                        videoMetadataMissing = false;
                        sourceid_t sourceId = 0;
                        mMetaDataParser.setVideoMetadata(sourceInfo, sourceId, videoStreams[0]->getDecoderConfig());
                        if (sourceInfo.rotation.valid)
                        {
                            mMetaDataParser.setRotation(sourceInfo.rotation);
                        }
                        mMediaInformation.fileType = MediaFileType::MP4_OMAF;
                    }
                }
            }
        }

        if (audioMetadataMissing)
        {
            if (!audioStreams.isEmpty())
            {
                // in normal AAC case use standard MP4 ChannelLayout metadata or default configuration
                if (!readAACAudioMetadata(*audioStreams[0]))
                {
                    return Error::FILE_NOT_SUPPORTED;
                }
            }
        }


        if (mediaUri.isEmpty())
        {
            return Error::OK_SKIPPED;
        }
        if (videoMetadataMissing)
        {
            if (!videoStreams.isEmpty())
            {
                BasicSourceInfo sourceInfo;

                if (videoStreams[0]->getFormat()->getTrackVRFeatures() & MP4VR::TrackFeatureEnum::VRFeature::HasVRGoogleStereoscopic3D)
                {
                    uint32_t trackId = videoStreams[0]->getTrackId();
                    for (MP4VR::TrackInformation* track = mTracks->begin(); track != mTracks->end(); track++)
                    {
                        if (track->trackId == trackId && track->sampleProperties.size)
                        {
                            sourceInfo.sourceDirection = SourceDirection::MONO;
                            MP4VR::StereoScopic3DProperty stereoScopic3Dproperty(MP4VR::StereoScopic3DProperty::MONOSCOPIC);
                            if (mReader->getPropertyStereoScopic3D(trackId, track->sampleProperties[0].sampleId, stereoScopic3Dproperty) == MP4VR::MP4VRFileReaderInterface::OK)
                            {
                                switch (stereoScopic3Dproperty)
                                {
                                case MP4VR::StereoScopic3DProperty::STEREOSCOPIC_TOP_BOTTOM:
                                    sourceInfo.sourceDirection = SourceDirection::TOP_BOTTOM;
                                    break;
                                case MP4VR::StereoScopic3DProperty::STEREOSCOPIC_LEFT_RIGHT:
                                    sourceInfo.sourceDirection = SourceDirection::LEFT_RIGHT;
                                    break;
                                case MP4VR::StereoScopic3DProperty::MONOSCOPIC:
                                case MP4VR::StereoScopic3DProperty::STEREOSCOPIC_STEREO_CUSTOM:
                                default:
                                    break; // defaults to MONO
                                }
                            }

                            if (videoStreams[0]->getFormat()->getTrackVRFeatures() & MP4VR::TrackFeatureEnum::VRFeature::HasVRGoogleV2SpericalVideo)
                            {
                                MP4VR::SphericalVideoV2Property sphericalVideoV2Property = {};
                                if (mReader->getPropertySphericalVideoV2(trackId, track->sampleProperties[0].sampleId, sphericalVideoV2Property) == MP4VR::MP4VRFileReaderInterface::OK)
                                {
                                    sourceInfo.sourceType = SourceType::EQUIRECTANGULAR_PANORAMA; // only support EQUIRECTANGULAR_PANORAMA for now.
                                }
                            }
                            else if (videoStreams[0]->getFormat()->getTrackVRFeatures() & MP4VR::TrackFeatureEnum::VRFeature::HasVRGoogleV1SpericalVideo)
                            {
                                MP4VR::SphericalVideoV1Property sphericalVideoV1Property = {};
                                if (mReader->getPropertySphericalVideoV1(trackId, track->sampleProperties[0].sampleId, sphericalVideoV1Property) == MP4VR::MP4VRFileReaderInterface::OK)
                                {
                                    sourceInfo.sourceType = SourceType::EQUIRECTANGULAR_PANORAMA; // v1 only support EQUIRECTANGULAR_PANORAMA.
                                }
                            }
                            sourceid_t sourceId = 0;
                            mMetaDataParser.setVideoMetadata(sourceInfo, sourceId, videoStreams[0]->getDecoderConfig());
                            break;
                        }
                    }
                }
                else if (mMetaDataParser.parseUri(mediaUri.getData(), sourceInfo))
                {
                    if (sourceInfo.sourceDirection == SourceDirection::DUAL_TRACK)
                    {
                        if (videoStreams.getSize() == 2)
                        {
                            BasicSourceInfo data;
                            data.sourceDirection = SourceDirection::DUAL_TRACK;
                            data.sourceType = SourceType::EQUIRECTANGULAR_PANORAMA;
                            mMetaDataParser.setVideoMetadataPackageStereo(0, 1, videoStreams[0]->getDecoderConfig(), videoStreams[1]->getDecoderConfig(), data);
                        }
                        else
                        {
                            // 2-track stereo, but the device is not capable of playing it so play the other one only
                            mMetaDataParser.setVideoMetadataPackageMono(0, videoStreams[0]->getDecoderConfig(), SourceType::EQUIRECTANGULAR_PANORAMA);
                        }
                    }
                    else
                    {
                        sourceid_t sourceId = 0;
                        mMetaDataParser.setVideoMetadata(sourceInfo, sourceId, videoStreams[0]->getDecoderConfig());
                    }
                }
                else if (videoStreams[0]->getFormat()->width() == videoStreams[0]->getFormat()->height())
                {
                    // assume top-bottom stereo
                    sourceInfo.sourceDirection = SourceDirection::TOP_BOTTOM;
                    sourceInfo.sourceType = SourceType::EQUIRECTANGULAR_PANORAMA;

                    sourceid_t sourceId = 0;
                    mMetaDataParser.setVideoMetadata(sourceInfo, sourceId, videoStreams[0]->getDecoderConfig());
                }
                else
                {
                    // no extension == mono
                    sourceInfo.sourceDirection = SourceDirection::MONO;
                    sourceInfo.sourceType = SourceType::EQUIRECTANGULAR_PANORAMA;

                    sourceid_t sourceId = 0;
                    mMetaDataParser.setVideoMetadata(sourceInfo, sourceId, videoStreams[0]->getDecoderConfig());
                }
            }
        }
        return result;
    }

    bool_t MP4VRParser::readAACAudioMetadata(MP4AudioStream& stream)
    {
        // there is none, use default
        return true;

    }

    bool_t MP4VRParser::seekToUs(uint64_t& seekPosUs, MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams, SeekDirection::Enum mode, SeekAccuracy::Enum accuracy)
    {
        Mutex::ScopeLock readerLock(mReaderMutex);

        uint64_t seekPosMs = (seekPosUs / 1000);
        uint64_t videoFinalSeekTimeMs = 0;
        int64_t videoSyncFrameTimeMs = -1;
        bool_t segmentChanged; // not used in this context
        OMAF_LOG_D("Seek: seekToUs stream global seek target: seekPosMs ms: %lld", seekPosMs);

        for (size_t i = 0; i < videoStreams.getSize(); i++)
        {
            uint32_t sampleId = 0;
            if (videoStreams[i]->findSampleIdByTime(seekPosMs, videoFinalSeekTimeMs, sampleId))
            {
                OMAF_LOG_D("Seek: seekToUs video seek target: videoFinalSeekTimeMs ms: %lld, sampleId: %d", videoFinalSeekTimeMs, sampleId);
                // seek by time; find first sample whose timestamp is larger or equal than the seeking time (we could rewind by 1, but since we anyway seek back to the prev sync frame it would be useless detail)
                videoSyncFrameTimeMs = seekToSyncFrame(sampleId, videoStreams[i], mode, segmentChanged);

                // Check the desired seek accuracy and if the device is fast enough
                if (accuracy == SeekAccuracy::NEAREST_SYNC_FRAME || !DeviceInfo::deviceSupports2VideoTracks())
                {
                    videoFinalSeekTimeMs = videoSyncFrameTimeMs;
                }
                if (mTimedMetadata && videoStreams[i]->getMetadataStream() != OMAF_NULL)
                {
                    uint64_t vtmdSeekTimeMs = 0;
                    if (videoStreams[i]->getMetadataStream()->findSampleIdByTime(videoFinalSeekTimeMs, vtmdSeekTimeMs, sampleId))
                    {
                        videoStreams[i]->getMetadataStream()->setNextSampleId(sampleId, segmentChanged);
                        OMAF_LOG_D("Seek: seekToUs video metadata sample: %d, timestamp: %d", vtmdSeekTimeMs, sampleId);
                    }
                }
                seekPosUs = videoFinalSeekTimeMs * 1000;
            }
        }

        uint64_t audioFinalSeekTimeMs = 0;
        bool audioSeeked = false;
        for (size_t i = 0; i < audioStreams.getSize(); i++)
        {
            uint32_t sampleId = 0;
            if (audioStreams[i]->findSampleIdByTime(seekPosMs, audioFinalSeekTimeMs, sampleId))
            {
                audioStreams[i]->setNextSampleId(sampleId, segmentChanged);
                audioSeeked = true;
                OMAF_LOG_D("Seek: seekToUs audio seek target: audioFinalSeekTimeMs ms: %lld, sampleId: %d", audioFinalSeekTimeMs, sampleId);
                if (mTimedMetadata && audioStreams[i]->getMetadataStream() != OMAF_NULL)
                {
                    uint64_t audioMetaFinalSeekTimeMs = 0;
                    if (audioStreams[i]->getMetadataStream()->findSampleIdByTime(audioFinalSeekTimeMs, audioMetaFinalSeekTimeMs, sampleId))
                    {
                        audioStreams[i]->getMetadataStream()->setNextSampleId(sampleId, segmentChanged);
                        OMAF_LOG_D("Seek: seekToUs audio metadata timestamp: %lld, sampleId: %d, ", audioMetaFinalSeekTimeMs, sampleId);
                    }
                }
                seekPosUs = audioFinalSeekTimeMs * 1000;
            }
        }

        if (videoSyncFrameTimeMs < 0 && !audioSeeked)
        {
            // didn't find the requested position => we run to the end of the clip?
            OMAF_LOG_W("Cannot seek to time %lld", seekPosUs);
            return false;
        }
        return true;
    }

    bool_t MP4VRParser::seekToFrame(int32_t seekFrameNr, uint64_t& seekPosUs, MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        Mutex::ScopeLock readerLock(mReaderMutex);
        bool_t segmentChanged;  // not used in this context
        int64_t finalSeekTimeMs = -1;
        OMAF_LOG_D("seekToFrame frame: %d", seekFrameNr);

        for (size_t i = 0; i < videoStreams.getSize(); i++)
        {
            uint32_t sampleId = 0;
            if (videoStreams[i]->findSampleIdByIndex(seekFrameNr, sampleId))
            {
                // this works only if mode is "seek to previous sync frame" - but do we really need any other mode?
                // seek by time; find first sample whose timestamp is larger or equal than the seeking time (we could rewind by 1, but since we anyway seek back to the prev sync frame it would be useless detail)
                finalSeekTimeMs = seekToSyncFrame(sampleId, videoStreams[i], SeekDirection::PREVIOUS, segmentChanged);

                if (mTimedMetadata && videoStreams[i]->getMetadataStream() != OMAF_NULL)
                {
                    uint64_t vtmdSeekTimeMs = 0;
                    if (videoStreams[i]->getMetadataStream()->findSampleIdByTime(finalSeekTimeMs, vtmdSeekTimeMs, sampleId))
                    {
                        videoStreams[i]->getMetadataStream()->setNextSampleId(sampleId, segmentChanged);
                        OMAF_LOG_D("seekToFrame video metadata sample: %d, timestamp: %d", sampleId, vtmdSeekTimeMs);
                    }
                }
            }
        }
        if (finalSeekTimeMs < 0)
        {
            // didn't find the requested position => we run to the end of the clip?
            OMAF_LOG_W("Cannot seek to frame %d", seekFrameNr);
            return false;
        }
        for (size_t i = 0; i < audioStreams.getSize(); i++)
        {
            uint32_t sampleId = 0;
            uint64_t finalAudioSeekTime = 0;
            if (audioStreams[i]->findSampleIdByTime(finalSeekTimeMs, finalAudioSeekTime, sampleId))
            {
                audioStreams[i]->setNextSampleId(sampleId, segmentChanged);
                OMAF_LOG_D("seekToFrame audio sample: %d, timestamp: %d" , sampleId, finalAudioSeekTime);
                if (mTimedMetadata && audioStreams[i]->getMetadataStream() != OMAF_NULL)
                {
                    if (audioStreams[i]->getMetadataStream()->findSampleIdByTime(finalSeekTimeMs, finalAudioSeekTime, sampleId))
                    {
                        audioStreams[i]->getMetadataStream()->setNextSampleId(sampleId, segmentChanged);
                        OMAF_LOG_D("seekToFrame audio metadata sample: %d, timestamp: %d", sampleId, finalAudioSeekTime);
                    }
                }
            }
        }

        seekPosUs = finalSeekTimeMs * 1000;
        return true;
    }


    int64_t MP4VRParser::seekToSyncFrame(uint32_t sampleId, MP4MediaStream* stream, SeekDirection::Enum direction, bool_t& segmentChanged)
    {
        int64_t finalSeekTimeMs = -1;
        uint32_t syncSample = 0;
        if (direction == SeekDirection::PREVIOUS)
        {
            if (mReader->getSyncSampleId(stream->getTrackId(), sampleId, MP4VR::SeekDirection::PREVIOUS, syncSample) != MP4VR::MP4VRFileReaderInterface::OK || syncSample == 0xffffffff)
            {
                return -1;
            }
        }
        else
        {
            if (mReader->getSyncSampleId(stream->getTrackId(), sampleId, MP4VR::SeekDirection::NEXT, syncSample) != MP4VR::MP4VRFileReaderInterface::OK || syncSample == 0xffffffff)
            {
                return -1;
            }
        }
        stream->setNextSampleId(syncSample, segmentChanged);
        MP4VR::DynArray<uint64_t> timestampsSample;
        if (mReader->getTimestampsOfSample(stream->getTrackId(), syncSample, timestampsSample) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return -1;
        }
        finalSeekTimeMs = timestampsSample[0];
        OMAF_LOG_D("Seek: seekToSyncFrame: sampleid: %d, %lld ms", syncSample, finalSeekTimeMs);

        return finalSeekTimeMs;
    }

    uint64_t MP4VRParser::getNextVideoTimestampUs(MP4MediaStream& stream)
    {
        Mutex::ScopeLock readerLock(mReaderMutex);

        uint32_t sample = 0;
        if (stream.peekNextSample(sample) != Error::OK)
        {
            return 0;
        }

        MP4VR::DynArray<uint64_t> timestamps;
        if (mReader->getTimestampsOfSample(stream.getTrackId(), sample, timestamps) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return 0;
        }
        if (timestamps.size > 0)
        {
            return (timestamps[0]) * 1000;
        }
        else
        {
            return 0;
        }
    }



    Error::Enum MP4VRParser::createStreams(MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams, FileFormatType::Enum fileFormat)
    {
        bool_t hasExtractor = false;
        bool_t hasMetadata = false;
        if (mTracks != OMAF_NULL)
        {
            OMAF_DELETE(mAllocator, mTracks);
        }
        mTracks = OMAF_NEW(mAllocator, MP4VR::DynArray<MP4VR::TrackInformation>);
        if (mTracks == OMAF_NULL)
        {
            return Error::OUT_OF_MEMORY;
        }
        if (mReader->getTrackInformations(*mTracks) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return Error::INVALID_DATA;
        }
        
        for (MP4VR::TrackInformation* track = mTracks->begin(); track != mTracks->end(); track++)
        {
            if (track->sampleProperties.size == 0)
            {
                // ignore the track. It may be part of multi-res extractor case, where we may need to have init for all possible tracks, but have data only in relevant tracks
                continue;
            }
            MP4VR::FourCC codecFourCC;
            if (mReader->getDecoderCodeType(track->trackId, track->sampleProperties[0].sampleId, codecFourCC) != MP4VR::MP4VRFileReaderInterface::OK)
            {
                return Error::INVALID_DATA;
            }
            if (codecFourCC == MP4VR::FourCC("hvc2"))
            {
                hasExtractor = true;
            }
        }

        int32_t streamId = 0;
        for (MP4VR::TrackInformation* track = mTracks->begin(); track != mTracks->end(); track++)
        {
            MediaFormat* format = OMAF_NULL;
            MP4VR::FourCC codecFourCC;
            if (track->sampleProperties.size == 0)
            {
                // ignore the track. It may be part of multi-res extractor case, where we may need to have init for all possible tracks, but have data only in relevant tracks
                continue;
            }
            if (mReader->getDecoderCodeType(track->trackId, track->sampleProperties[0].sampleId, codecFourCC) != MP4VR::MP4VRFileReaderInterface::OK)
            {
                return Error::INVALID_DATA;
            }
            float64_t durationInSecs = 0.0;
            if (mReader->getPlaybackDurationInSecs(track->trackId, durationInSecs) != MP4VR::MP4VRFileReaderInterface::OK)
            {
                return Error::INVALID_DATA;
            }

            if ((hasExtractor && codecFourCC == MP4VR::FourCC("hvc2"))
                || (!hasExtractor && (codecFourCC == MP4VR::FourCC("avc1") || codecFourCC == MP4VR::FourCC("avc3") || codecFourCC == MP4VR::FourCC("hev1") || codecFourCC == MP4VR::FourCC("hvc1"))))
            {
                format = OMAF_NEW(mAllocator, MediaFormat)(MediaFormat::Type::IsVideo, track->vrFeatures, codecFourCC.value, UNKNOWN_MIME_TYPE);

                if (format == OMAF_NULL)
                {
                    return Error::OUT_OF_MEMORY;
                }
                format->setDuration((int64_t)(1000000LL * durationInSecs));
                format->setFrameRate((float32_t)(track->sampleProperties.size / durationInSecs));
                // Note! decspecinfo may need to be updated (can change in a normal file or in segmented only within initialization segment)!
                // Info about it is currently available at TrackInformation.sampleProperties.sampleDescriptionIndex when reading frames
                MP4VR::DynArray<MP4VR::DecoderSpecificInfo> info;
                if (mReader->getDecoderConfiguration(track->trackId, track->sampleProperties[0].sampleId, info) != MP4VR::MP4VRFileReaderInterface::ErrorCode::OK)
                {
                    return Error::INVALID_DATA;
                }
                format->setDecoderConfigInfo(info, track->sampleProperties[0].sampleDescriptionIndex);
                uint32_t height;
                if (mReader->getHeight(track->trackId, track->sampleProperties[0].sampleId, height) != MP4VR::MP4VRFileReaderInterface::OK)
                {
                    return Error::INVALID_DATA;
                }
                format->setHeight(height);
                uint32_t width;
                if (mReader->getWidth(track->trackId, track->sampleProperties[0].sampleId, width) != MP4VR::MP4VRFileReaderInterface::OK)
                {
                    return Error::INVALID_DATA;
                }
                format->setWidth(width);

                MP4VideoStream* stream = OMAF_NEW(mAllocator, MP4VideoStream)(format);
                if (stream == OMAF_NULL)
                {
                    OMAF_DELETE(mAllocator, format);
                    return Error::OUT_OF_MEMORY;
                }

                stream->setTrack(track);

                // Read timestamp array for the stream. Note! the timestamps array is not in the same order as data samples are for video, so with video it is used only with some special cases, e.g. seeking, not when reading video.
                stream->setTimestamps(mReader);

                if (!mClientAssignVideoStreamId)
                {
                    stream->setStreamId(VideoDecoderManager::getInstance()->generateUniqueStreamID());
                }

                if (track->hasTypeInformation)
                {
                    OMAF_LOG_D("Major track type: %s", track->type.majorBrand.value);
                    bool compatible = false;
                    for (MP4VR::FourCC* it = track->type.compatibleBrands.begin(); it != track->type.compatibleBrands.end(); it++)
                    {
                        OMAF_LOG_V("Compatible track type contains: %s", (*it).value);
                        if ((*it) == MP4VR::FourCC("hevi") || (*it) == MP4VR::FourCC("hevd"))
                        {
                            //OK
                            compatible = true;
                            break;
                        }
                    }
                    if (!compatible)
                    {
                        OMAF_LOG_W("No compatible track type found!");
                        OMAF_DELETE(mAllocator, stream);
                        continue;
                    }
                }
                // Note! ttyp is relevant if there are more than 1 track - or possibly if there are more than 1 track types. In other cases the brand(s) is in ftyp

                MP4VR::SchemeTypesProperty schemeTypes;
                if (mReader->getPropertySchemeTypes(track->trackId, track->sampleProperties[0].sampleId, schemeTypes) == MP4VR::MP4VRFileReaderInterface::ErrorCode::OK)
                {
                    OMAF_LOG_D("Main scheme type %s", schemeTypes.mainScheme.type.value);
                    bool compatible = false;
                    for (MP4VR::SchemeType* it = schemeTypes.compatibleSchemeTypes.begin(); it != schemeTypes.compatibleSchemeTypes.end(); it++)
                    {
                        OMAF_LOG_D("Compatible scheme type contains: %s", (*it).type.value);
                        if ((*it).type == MP4VR::FourCC("podv"))
                        {
                            compatible = true;
                        }
                        else if ((*it).type == MP4VR::FourCC("erpv") ||
                                (*it).type == MP4VR::FourCC("ercm"))
                        {
                            // equirect or packed equirect/cubemap. Inherits podv + adds more restrictions
                            compatible = true;
                        }
                    }
                    if (!compatible)
                    {
                        // still check the main scheme
                        if (schemeTypes.mainScheme.type == MP4VR::FourCC("podv"))
                        {
                            // We are accepting this alone too, since there are files like this, although it is against OMAF spec. The compatible scheme types should contain podv and either erpv/ercm
                            compatible = true;
                        }
                    }
                    if (!compatible)
                    {
                        // unsupported
                        OMAF_LOG_W("Unsupported track scheme type");
                        OMAF_DELETE(mAllocator, stream);
                        continue;
                    }
                }

                videoStreams.add(stream);
            }
            else if (track->features & MP4VR::TrackFeatureEnum::IsAudioTrack)
            {
                format = OMAF_NEW(mAllocator, MediaFormat)(MediaFormat::Type::IsAudio,
                                                            track->vrFeatures,
                                                            codecFourCC.value,
                                                            UNKNOWN_MIME_TYPE);
                if (format == OMAF_NULL)
                {
                    return Error::OUT_OF_MEMORY;
                }
                format->setDuration((int64_t)(1000000LL * durationInSecs));
                // Note! decspecinfo may need to be updated (can change in a normal file or in segmented only within initialization segment)!
                // Info about it is currently available at TrackInformation.sampleProperties.sampleDescriptionIndex when reading frames
                MP4VR::DynArray<MP4VR::DecoderSpecificInfo> info;
                if (mReader->getDecoderConfiguration(track->trackId, track->sampleProperties[0].sampleId, info) != MP4VR::MP4VRFileReaderInterface::ErrorCode::OK)
                {
                    return Error::INVALID_DATA;
                }
                format->setDecoderConfigInfo(info, track->sampleProperties[0].sampleDescriptionIndex);
                MP4AudioStream* stream = OMAF_NEW(mAllocator, MP4AudioStream)(format);
                if (stream == OMAF_NULL)
                {
                    OMAF_DELETE(mAllocator, format);
                    return Error::OUT_OF_MEMORY;
                }

                stream->setTrack(track);

#if OMAF_ENABLE_STREAM_VIDEO_PROVIDER
                SegmentContent segmentContent = {};
                for (InitSegmentsContent::Iterator it = mInitSegmentsContent.begin(); it != mInitSegmentsContent.end(); ++it)
                {
                    if (track->initSegmentId == it->initializationSegmentId)
                    {
                        segmentContent = *it;
                    }
                }
#endif
                stream->setTimestamps(mReader);

                stream->setStreamId(VideoDecoderManager::getInstance()->generateUniqueStreamID());
                audioStreams.add(stream);
            }
            else if (track->features & MP4VR::TrackFeatureEnum::IsMetadataTrack)
            {
                hasMetadata = true;
                // handle it later, as in theory the order of tracks could be such that the associated track is not yet processed
            }

        }

        if (hasMetadata)
        {
            for (MP4VR::TrackInformation* track = mTracks->begin(); track != mTracks->end(); track++)
            {
                if (track->features & MP4VR::TrackFeatureEnum::IsMetadataTrack)
                {
                    MP4VR::FourCC codecFourCC;
                    if (mReader->getDecoderCodeType(track->trackId, track->sampleProperties[0].sampleId, codecFourCC) != MP4VR::MP4VRFileReaderInterface::OK)
                    {
                        return Error::INVALID_DATA;
                    }
                    if (codecFourCC != MP4VR::FourCC("invo"))
                    {
                        // skip other than initial viewing orientation tracks
                        continue;
                    }

                    float64_t durationInSecs = 0.0;
                    if (mReader->getPlaybackDurationInSecs(track->trackId, durationInSecs) != MP4VR::MP4VRFileReaderInterface::OK)
                    {
                        return Error::INVALID_DATA;
                    }
                    MediaFormat* format = OMAF_NEW(mAllocator, MediaFormat)(MediaFormat::Type::IsMeta, track->vrFeatures, codecFourCC.value, UNKNOWN_MIME_TYPE);
                    if (format == OMAF_NULL)
                    {
                        return Error::OUT_OF_MEMORY;
                    }
                    format->setDuration((int64_t)(1000000LL * durationInSecs));

                    MP4MediaStream* stream = OMAF_NEW(mAllocator, MP4MediaStream)(format);
                    if (stream == OMAF_NULL)
                    {
                        OMAF_DELETE(mAllocator, format);
                        continue;
                    }
                    stream->setStreamId(streamId++);

                    stream->setTrack(track);

                    stream->setTimestamps(mReader);

                    mTimedMetadata = true;

#if OMAF_ENABLE_STREAM_VIDEO_PROVIDER
                    SegmentContent segmentContent = {};
                    for (InitSegmentsContent::Iterator it = mInitSegmentsContent.begin(); it != mInitSegmentsContent.end(); ++it)
                    {
                        if (track->initSegmentId == it->initializationSegmentId)
                        {
                            segmentContent = *it;
                        }
                    }
#endif
                    bool_t assigned = false;
#if OMAF_ENABLE_STREAM_VIDEO_PROVIDER
                    // assign based on MPD
                    if (segmentContent.associatedToRepresentationId.getLength())
                    {
                        if (segmentContent.type.matches(MediaContent::Type::METADATA_INVO))
                        {
                            for (size_t i = 0; i < videoStreams.getSize(); i++)
                            {
                                videoStreams[i]->setMetadataStream(stream);
                                assigned = true;
                                break; // set only for first
                            }
                        }
                    }
                    else
#endif
                    {
                        // add to stream
                        for (uint32_t ref = 0; ref < track->referenceTrackIds.size; ref++)
                        {
                            if (track->referenceTrackIds[ref].trackIds.size > 0 &&
                                track->referenceTrackIds[ref].type == MP4VR::FourCC("cdsc"))
                            {
                                for (size_t i = 0; i < videoStreams.getSize(); i++)
                                {
                                    if (videoStreams[i]->matchAndAssignMetadataStream(stream, track->referenceTrackIds[ref].trackIds))
                                    {
                                        assigned = true;
                                        break;
                                    }
                                }
                                break;
                            }
                        }
                    }
                    if (!assigned)
                    {
                        // we didn't find the track this is referring to. Cleanup
                        OMAF_DELETE(mAllocator, stream);
                    }

                }
            }
        }
        if (videoStreams.isEmpty() && audioStreams.isEmpty())
        {
            return Error::NOT_SUPPORTED;
        }
        return Error::OK;
    }

    // update streams with updated track information (after new segment loaded / old invalidated)
    Error::Enum MP4VRParser::updateStreams(MP4AudioStreams& audioStreams, MP4VideoStreams& videoStreams)
    {
        // clear track from streams
        for (MP4VideoStreams::Iterator it = videoStreams.begin(); it != videoStreams.end(); ++it)
        {
            (*it)->updateTrack(OMAF_NULL);
        }
        for (MP4AudioStreams::Iterator it = audioStreams.begin(); it != audioStreams.end(); ++it)
        {
            (*it)->setTrack(OMAF_NULL);
        }

        if (mTracks == OMAF_NULL)
        {
            mTracks = OMAF_NEW(mAllocator, MP4VR::DynArray<MP4VR::TrackInformation>);
            if (mTracks == OMAF_NULL)
            {
                return Error::OUT_OF_MEMORY;
            }
        }

        if (mReader->getTrackInformations(*mTracks) != MP4VR::MP4VRFileReaderInterface::OK)
        {
            return Error::SEGMENT_CHANGE_FAILED;
        }

        for (MP4VR::TrackInformation* track = mTracks->begin(); track != mTracks->end(); track++)
        {
            if (track->features & MP4VR::TrackFeatureEnum::IsVideoTrack)
            {
                for (size_t i = 0; i < videoStreams.getSize(); i++)
                {
                    if (videoStreams[i]->updateTrack(track))
                    {
                        videoStreams[i]->setTimestamps(mReader);
                        break;
                    }
                }
            }
            else if (track->features & MP4VR::TrackFeatureEnum::IsAudioTrack)
            {
                for (size_t i = 0; i < audioStreams.getSize(); i++)
                {
                    if (audioStreams[i]->getTrackId() == track->trackId)
                    {
                        audioStreams[i]->setTrack(track);
                        audioStreams[i]->setTimestamps(mReader);
                    }
                }
            }
            else if (track->features & MP4VR::TrackFeatureEnum::IsMetadataTrack)
            {
                bool_t found = false;
                for (size_t i = 0; i < audioStreams.getSize(); i++)
                {
                    if (audioStreams[i]->getMetadataStream()->getTrackId() == track->trackId)
                    {
                        // Update timestamp array of the metadata stream
                        audioStreams[i]->getMetadataStream()->setTrack(track);
                        audioStreams[i]->getMetadataStream()->setTimestamps(mReader);
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    for (size_t i = 0; i < videoStreams.getSize(); i++)
                    {
                        // Update timestamp array of the metadata stream
                        if (videoStreams[i]->updateMetadataStream(track, mReader))
                        {
                            break;
                        }
                    }
                }
            }
        }

        return Error::OK;
    }

    MetadataParser& MP4VRParser::getMetadataParser()
    {
        return mMetaDataParser;
    }

    const MediaInformation& MP4VRParser::getMediaInformation() const
    {
        return mMediaInformation;
    }

    bool_t MP4VRParser::isEOS(const MP4AudioStreams& aAudioStreams, const MP4VideoStreams& aVideoStreams) const
    {
        // to be used only with offline files, not with segmented input
        for (MP4AudioStreams::ConstIterator it = aAudioStreams.begin(); it != aAudioStreams.end(); ++it)
        {
            uint32_t sample;
            if ((*it)->peekNextSample(sample) == Error::END_OF_FILE)
            {
                return true;
            }
        }

        for (MP4VideoStreams::ConstIterator it = aVideoStreams.begin(); it != aVideoStreams.end(); ++it)
        {
            uint32_t sample;
            if ((*it)->peekNextSample(sample) == Error::END_OF_FILE)
            {
                return true;
            }
        }

        return false;
    }
OMAF_NS_END
