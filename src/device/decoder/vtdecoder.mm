#include "vtdecoder.h"

#ifdef Q_OS_MACOS

#include <QDebug>
#include <QElapsedTimer>
#include <vector>

#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <dispatch/dispatch.h>

#define MAX_NALS 16

struct VTDecoder::Impl
{
    VTDecompressionSessionRef session = nullptr;
    CMVideoFormatDescriptionRef formatDesc = nullptr;

    int lastWidth = 0;
    int lastHeight = 0;

    dispatch_semaphore_t semaphore = nullptr;

    CVPixelBufferRef outputPixelBuffer = nullptr;
    OSStatus decodeStatus = noErr;

    quint32 renderedFrames = 0;
    QElapsedTimer fpsTimer;
    bool sessionCreated = false;

    // 缓存 SPS/PPS 字节用于分辨率变化检测
    std::vector<uint8_t> cachedSPS;
    std::vector<uint8_t> cachedPPS;
};

static void vtDecoderCallback(void *decompressionOutputRefCon,
                              void *sourceFrameRefCon,
                              OSStatus status, VTDecodeInfoFlags infoFlags,
                              CVImageBufferRef imageBuffer,
                              CMTime, CMTime)
{
    Q_UNUSED(sourceFrameRefCon);
    Q_UNUSED(infoFlags);

    auto *impl = static_cast<VTDecoder::Impl *>(decompressionOutputRefCon);
    impl->outputPixelBuffer = nullptr;

    if (status == noErr && imageBuffer) {
        CVPixelBufferRetain(imageBuffer);
        impl->outputPixelBuffer = imageBuffer;
    }
    impl->decodeStatus = status;
    dispatch_semaphore_signal(impl->semaphore);
}

static int startCodeLen(const uint8_t *p, const uint8_t *end)
{
    if (end - p >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return 4;
    if (end - p >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1) return 3;
    return 0;
}

static uint8_t* annexbToAVCC(const uint8_t *data, int size,
                              int &outAvccSize,
                              const uint8_t *&spsData, int &spsLen,
                              const uint8_t *&ppsData, int &ppsLen)
{
    spsData = ppsData = nullptr;
    spsLen = ppsLen = 0;
    outAvccSize = 0;

    struct { const uint8_t *start; int scLen; int nalSize; int type; } nals[MAX_NALS];
    int nalCount = 0;

    const uint8_t *p = data, *end = data + size;
    while (p < end && nalCount < MAX_NALS) {
        int sc = startCodeLen(p, end);
        if (sc == 0) { p++; continue; }

        const uint8_t *nalStart = p;
        const uint8_t *nalData = p + sc;
        const uint8_t *next = nalData;
        while (next < end && startCodeLen(next, end) == 0) next++;

        int totalSize = (int)(next - nalStart);
        int dataSize  = (int)(next - nalData);
        int type = nalData[0] & 0x1F;

        nals[nalCount++] = {nalStart, sc, totalSize, type};

        if (type == 7 && !spsData) { spsData = nalData; spsLen = dataSize; }
        if (type == 8 && !ppsData) { ppsData = nalData; ppsLen = dataSize; }

        p = next;
    }

    if (nalCount == 0) return nullptr;

    for (int i = 0; i < nalCount; i++) {
        outAvccSize += 4 + (nals[i].nalSize - nals[i].scLen);
    }

    uint8_t *avcc = (uint8_t *)malloc(outAvccSize);
    if (!avcc) return nullptr;

    uint8_t *dst = avcc;
    for (int i = 0; i < nalCount; i++) {
        int len = nals[i].nalSize - nals[i].scLen;
        dst[0] = (uint8_t)(len >> 24); dst[1] = (uint8_t)(len >> 16);
        dst[2] = (uint8_t)(len >> 8);  dst[3] = (uint8_t)len;
        dst += 4;
        memcpy(dst, nals[i].start + nals[i].scLen, len);
        dst += len;
    }
    return avcc;
}

static bool createFormatDesc(const uint8_t *sps, int slen,
                             const uint8_t *pps, int plen,
                             CMVideoFormatDescriptionRef &outDesc)
{
    const uint8_t *ptrs[2] = { sps, pps };
    size_t sizes[2] = { (size_t)slen, (size_t)plen };
    return CMVideoFormatDescriptionCreateFromH264ParameterSets(
        kCFAllocatorDefault, 2, ptrs, sizes, 4, &outDesc) == noErr;
}

static bool createVTSession(CMVideoFormatDescriptionRef fd, VTDecoder::Impl *impl)
{
    CFMutableDictionaryRef dstAttrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 3,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    int pf = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
    CFNumberRef pfNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pf);
    CFDictionarySetValue(dstAttrs, kCVPixelBufferPixelFormatTypeKey, pfNum);
    CFRelease(pfNum);
    CFDictionarySetValue(dstAttrs, kCVPixelBufferMetalCompatibilityKey, kCFBooleanTrue);
    CFDictionarySetValue(dstAttrs, kCVPixelBufferIOSurfacePropertiesKey, (CFDictionaryRef)@{});

    VTDecompressionOutputCallbackRecord cb;
    cb.decompressionOutputCallback = vtDecoderCallback;
    cb.decompressionOutputRefCon = impl;

    OSStatus st = VTDecompressionSessionCreate(kCFAllocatorDefault, fd,
        nullptr, dstAttrs, &cb, &impl->session);
    CFRelease(dstAttrs);

    return st == noErr;
}

static bool doDecode(VTDecoder::Impl *d, CMBlockBufferRef bb,
                     CMVideoFormatDescriptionRef fd, qint64 pts,
                     CVPixelBufferRef &outPB, int &outW, int &outH)
{
    CMSampleTimingInfo ti;
    ti.duration = kCMTimeInvalid;
    ti.presentationTimeStamp = CMTimeMake(pts, 1000000);
    ti.decodeTimeStamp = kCMTimeInvalid;

    size_t bs = CMBlockBufferGetDataLength(bb);
    size_t ss = bs;

    CMSampleBufferRef sb = nullptr;
    if (CMSampleBufferCreateReady(kCFAllocatorDefault, bb, fd, 1, 1, &ti, 1, &ss, &sb) != noErr)
        return false;

    OSStatus st = VTDecompressionSessionDecodeFrame(d->session, sb, 0, nullptr, nullptr);
    CFRelease(sb);

    if (st != noErr) {
        if (st == -12909) return false;
        if (st == kVTInvalidSessionErr) {
            qWarning("VTDecoder: invalid session");
        }
        return false;
    }

    if (dispatch_semaphore_wait(d->semaphore,
            dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC)) != 0) {
        qWarning("VTDecoder: decode timeout");
        return false;
    }

    if (d->decodeStatus != noErr || !d->outputPixelBuffer) return false;

    outPB = d->outputPixelBuffer;
    outW = (int)CVPixelBufferGetWidth(outPB);
    outH = (int)CVPixelBufferGetHeight(outPB);
    d->outputPixelBuffer = nullptr;
    return true;
}

// ═══════════════════ Public API ═══════════════════

VTDecoder::VTDecoder(QObject *parent) : QObject(parent), d(new Impl)
{
    d->fpsTimer.start();
}

VTDecoder::~VTDecoder() { close(); delete d; }

bool VTDecoder::isAvailable()
{
#ifdef __arm64__
    return true;
#else
    return false;
#endif
}

bool VTDecoder::open() { return true; }

void VTDecoder::close()
{
    if (d->session) { VTDecompressionSessionInvalidate(d->session); CFRelease(d->session); d->session = nullptr; }
    if (d->formatDesc) { CFRelease(d->formatDesc); d->formatDesc = nullptr; }
    if (d->semaphore) { dispatch_release(d->semaphore); d->semaphore = nullptr; }
    if (d->outputPixelBuffer) { CVPixelBufferRelease(d->outputPixelBuffer); d->outputPixelBuffer = nullptr; }
    d->cachedSPS.clear();
    d->cachedPPS.clear();
    d->sessionCreated = false;
    d->lastWidth = d->lastHeight = 0;
    d->renderedFrames = 0;
}

bool VTDecoder::decode(const unsigned char *data, int size, qint64 pts,
                       CVPixelBufferRef &outPixelBuffer, int &outWidth, int &outHeight)
{
    if (!data || size <= 0) return false;
    outPixelBuffer = nullptr;

    // —— Annex B → AVCC + 提取 SPS/PPS ——
    const uint8_t *spsData = nullptr, *ppsData = nullptr;
    int spsSize = 0, ppsSize = 0, avccSize = 0;

    uint8_t *avccData = annexbToAVCC(data, size, avccSize, spsData, spsSize, ppsData, ppsSize);
    if (!avccData) return false;

    // —— 检测 SPS/PPS 是否变化 ——
    bool spsChanged = false;
    if (spsData && spsSize > 0) {
        if (spsSize != (int)d->cachedSPS.size() ||
            memcmp(spsData, d->cachedSPS.data(), spsSize) != 0) {
            spsChanged = true;
            d->cachedSPS.assign(spsData, spsData + spsSize);
            if (ppsData && ppsSize > 0) {
                d->cachedPPS.assign(ppsData, ppsData + ppsSize);
            }
        }
    }

    // —— 懒初始化 / 分辨率重建 ——
    if (!d->sessionCreated || spsChanged) {
        if (!spsData || !ppsData) {
            free(avccData); return false;
        }

        // 创建新 formatDesc（可能包含新分辨率）
        CMVideoFormatDescriptionRef newFD = nullptr;
        if (!createFormatDesc(spsData, spsSize, ppsData, ppsSize, newFD)) {
            free(avccData); return false;
        }

        CMVideoDimensions dims = CMVideoFormatDescriptionGetDimensions(newFD);
        int newW = dims.width, newH = dims.height;

        if (!d->sessionCreated) {
            // 首次初始化
            d->formatDesc = newFD;
            d->lastWidth = newW; d->lastHeight = newH;
            m_currentWidth = newW; m_currentHeight = newH;

            if (!createVTSession(d->formatDesc, d)) {
                CFRelease(newFD); free(avccData); return false;
            }
            d->semaphore = dispatch_semaphore_create(0);
            d->sessionCreated = true;
            qInfo("VTDecoder: session created %dx%d", newW, newH);
        } else if (newW != d->lastWidth || newH != d->lastHeight) {
            // 分辨率变化
            qInfo("VTDecoder: resolution change %dx%d → %dx%d",
                  d->lastWidth, d->lastHeight, newW, newH);

            if (d->session) {
                VTDecompressionSessionInvalidate(d->session);
                CFRelease(d->session); d->session = nullptr;
            }
            if (d->formatDesc) {
                CFRelease(d->formatDesc);
            }
            d->formatDesc = newFD;
            d->lastWidth = newW; d->lastHeight = newH;
            m_currentWidth = newW; m_currentHeight = newH;

            if (!createVTSession(d->formatDesc, d)) {
                CFRelease(newFD); free(avccData); return false;
            }
        } else {
            // SPS 变了但分辨率不变（如 profile 变化），仅更新 formatDesc 引用
            if (d->formatDesc) CFRelease(d->formatDesc);
            d->formatDesc = newFD;
        }
    }

    if (!d->sessionCreated) {
        free(avccData); return false;
    }

    // —— 解码 ——
    CMBlockBufferRef bb = nullptr;
    if (CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, avccData, avccSize,
            kCFAllocatorMalloc, nullptr, 0, avccSize, 0, &bb) != kCMBlockBufferNoErr) {
        free(avccData); return false;
    }

    CVPixelBufferRef pb = nullptr;
    int w = 0, h = 0;
    bool ok = doDecode(d, bb, d->formatDesc, pts, pb, w, h);
    CFRelease(bb);

    if (!ok) return false;

    outPixelBuffer = pb;
    outWidth = w;
    outHeight = h;

    d->renderedFrames++;
    if (d->fpsTimer.elapsed() >= 1000) {
        quint32 fps = d->renderedFrames;
        d->renderedFrames = 0;
        d->fpsTimer.restart();
        emit updateFPS(fps);
    }

    return true;
}

#else
VTDecoder::VTDecoder(QObject *parent) : QObject(parent), d(nullptr) {}
VTDecoder::~VTDecoder() {}
bool VTDecoder::isAvailable() { return false; }
bool VTDecoder::open() { return false; }
void VTDecoder::close() {}
bool VTDecoder::decode(const unsigned char *, int, qint64, CVPixelBufferRef &o, int &w, int &h) { o = nullptr; return false; }
#endif
