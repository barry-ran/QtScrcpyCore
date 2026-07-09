#include <QDebug>

#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"
#include "vtdecoder.h"

#ifdef Q_OS_MACOS
#include <CoreVideo/CoreVideo.h>
#endif

Decoder::Decoder(std::function<void(int,int,uint8_t*,uint8_t*,uint8_t*,int,int,int)> onFrame, QObject *parent)
    : QObject(parent)
    , m_vb(new VideoBuffer())
    , m_onFrame(onFrame)
{
    m_vb->init();
    connect(this, &Decoder::newFrame, this, &Decoder::onNewFrame, Qt::QueuedConnection);
    connect(m_vb, &VideoBuffer::updateFPS, this, &Decoder::updateFPS);
}

Decoder::~Decoder()
{
    m_vb->deInit();
    delete m_vb;
#ifdef Q_OS_MACOS
    if (m_lastPixelBuffer) {
        CVPixelBufferRelease((CVPixelBufferRef)m_lastPixelBuffer);
    }
#endif
}

bool Decoder::open(int decodeMode)
{
#ifdef Q_OS_MACOS
    if (decodeMode == MODE_VT_METAL) {
        m_vtDecoder = new VTDecoder(this);
        if (m_vtDecoder->open()) {
            m_decodeMode = MODE_VT_METAL;
            connect(m_vtDecoder, &VTDecoder::updateFPS, this, &Decoder::updateFPS);
            qInfo("Decoder: VideoToolbox + Metal mode");
            return true;
        }
        qWarning("Decoder: VTDecoder open failed, fallback to FFmpeg");
        delete m_vtDecoder;
        m_vtDecoder = Q_NULLPTR;
    }
#endif

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("H.264 decoder not found");
        return false;
    }
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical("Could not allocate decoder context");
        return false;
    }
    if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
        qCritical("Could not open H.264 codec");
        return false;
    }
    m_isCodecCtxOpen = true;
    m_decodeMode = MODE_FFMPEG;
    return true;
}

void Decoder::close()
{
    if (m_vb) m_vb->interrupt();

    if (m_vtDecoder) {
        m_vtDecoder->close();
    }
#ifdef Q_OS_MACOS
    if (m_lastPixelBuffer) {
        CVPixelBufferRelease((CVPixelBufferRef)m_lastPixelBuffer);
        m_lastPixelBuffer = nullptr;
    }
#endif
    m_lastFrameWidth = m_lastFrameHeight = 0;

    if (m_codecCtx) {
        if (m_isCodecCtxOpen) avcodec_close(m_codecCtx);
        avcodec_free_context(&m_codecCtx);
    }
}

bool Decoder::pushVT(const AVPacket *packet)
{
#ifdef Q_OS_MACOS
    if (!m_vtDecoder || !packet) return false;

    CVPixelBufferRef pb = nullptr;
    int w = 0, h = 0;

    if (!m_vtDecoder->decode(packet->data, packet->size, packet->pts, pb, w, h) || !pb) {
        return false;
    }

    // 互斥更新最新帧 — 所有权转移
    {
        QMutexLocker lock(&m_pixelBufferMutex);
        if (m_lastPixelBuffer) {
            CVPixelBufferRelease((CVPixelBufferRef)m_lastPixelBuffer);
            m_lastPixelBuffer = nullptr;
        }
        m_lastPixelBuffer = pb;
        m_lastFrameWidth = w;
        m_lastFrameHeight = h;
    }

    // 非重复发出 newFrame 信号：仅当主线程空闲时才发
    // VT 硬件解码极快，不跳过帧，保持 GOP 连续避免马赛克
    if (m_frameInFlight.loadAcquire() == 0) {
        m_frameInFlight.storeRelease(1);
        emit newFrame();
    }
    // 当 m_frameInFlight > 0 时，主线程还在渲染上一帧。
    // 不发信号—主线程完成渲染后会检查 m_lastPixelBuffer，
    // pushVT 下一次调用会覆盖 m_lastPixelBuffer（在互斥锁内释放旧帧）。
    return true;
#else
    Q_UNUSED(packet);
    return false;
#endif
}

bool Decoder::push(const AVPacket *packet)
{
    if (m_decodeMode == MODE_VT_METAL) return pushVT(packet);

    if (!m_codecCtx || !m_vb) return false;

    AVFrame *decodingFrame = m_vb->decodingFrame();
#ifdef QTSCRCPY_LAVF_HAS_NEW_ENCODING_DECODING_API
    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret < 0) {
        char err[256] = {0};
        av_strerror(ret, err, 255);
        qCritical("send_packet: %s", err);
        return false;
    }
    if (decodingFrame && avcodec_receive_frame(m_codecCtx, decodingFrame) == 0) {
        pushFrame();
    } else if (ret != AVERROR(EAGAIN)) {
        qCritical("receive_frame: %d", ret);
        return false;
    }
#else
    int got = 0;
    int len = decodingFrame ? avcodec_decode_video2(m_codecCtx, decodingFrame, &got, packet) : -1;
    if (len < 0) { qCritical("decode: %d", len); return false; }
    if (got) pushFrame();
#endif
    return true;
}

void Decoder::peekFrame(std::function<void(int,int,uint8_t*)> onFrame)
{
#ifdef Q_OS_MACOS
    if (m_decodeMode == MODE_VT_METAL) {
        QMutexLocker lock(&m_pixelBufferMutex);
        CVPixelBufferRef pb = (CVPixelBufferRef)m_lastPixelBuffer;
        if (!pb) return;
        CVPixelBufferRetain(pb);
        lock.unlock();

        CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

        int w = (int)CVPixelBufferGetWidth(pb);
        int h = (int)CVPixelBufferGetHeight(pb);

        uint8_t *y  = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        uint8_t *uv = (uint8_t *)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t ys = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
        size_t uvs = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);

        uint8_t *rgb = new uint8_t[w * h * 4];
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                int yy  = y[row * ys + col];
                int uu  = uv[(row>>1) * uvs + (col & ~1)];
                int vv  = uv[(row>>1) * uvs + (col & ~1) + 1];

                float yf = (yy - 16) / 219.0f;
                float uf = (uu - 128) / 224.0f;
                float vf = (vv - 128) / 224.0f;

                int idx = (row * w + col) * 4;
                rgb[idx+0] = (uint8_t)qBound(0, (int)(255*(yf + 1.7927f*vf)), 255);
                rgb[idx+1] = (uint8_t)qBound(0, (int)(255*(yf - 0.2132f*uf - 0.5329f*vf)), 255);
                rgb[idx+2] = (uint8_t)qBound(0, (int)(255*(yf + 2.1124f*uf)), 255);
                rgb[idx+3] = 255;
            }
        }
        CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
        CVPixelBufferRelease(pb);

        onFrame(w, h, rgb);
        delete[] rgb;
        return;
    }
#endif
    if (m_vb) m_vb->peekRenderedFrame(onFrame);
}

void Decoder::pushFrame()
{
    if (!m_vb) return;
    bool skipped = true;
    m_vb->offerDecodedFrame(skipped);
    if (skipped) return;
    emit newFrame();
}

void Decoder::onNewFrame()
{
    if (m_decodeMode == MODE_VT_METAL) {
        if (!onMetalFrame) {
            m_frameInFlight.storeRelease(0);
            return;
        }

        // 取走 pixel buffer 所有权
        QMutexLocker lock(&m_pixelBufferMutex);
        void *pb = m_lastPixelBuffer;
        m_lastPixelBuffer = nullptr;  // 所有权转移
        int w = m_lastFrameWidth, h = m_lastFrameHeight;
        lock.unlock();

        if (!pb) {
            m_frameInFlight.storeRelease(0);
            return;
        }

        // 渲染接管生命周期：renderFrame 内部 Retain 给 GPU handler
        onMetalFrame(pb, w, h);

        // 释放 VT callback 持有的那一次 Retain
        // （如果 renderFrame Retain 了，GPU handler 会最终释放）
        CVPixelBufferRelease((CVPixelBufferRef)pb);

        // 背压清除：允许 pushVT 推送下一帧
        m_frameInFlight.storeRelease(0);
        return;
    }

    // FFmpeg 路径
    if (m_onFrame) {
        m_vb->lock();
        const AVFrame *frame = m_vb->consumeRenderedFrame();
        m_onFrame(frame->width, frame->height,
                  frame->data[0], frame->data[1], frame->data[2],
                  frame->linesize[0], frame->linesize[1], frame->linesize[2]);
        m_vb->unLock();
    }
}
