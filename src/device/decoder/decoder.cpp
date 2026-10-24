#include <QDebug>

#include "compat.h"
#include "decoder.h"
#include "videobuffer.h"

Decoder::Decoder(std::function<void(int, int, uint8_t*, uint8_t*, uint8_t*, int, int, int)> onFrame, QObject *parent)
    : IDecoder(parent)
    , m_vb(new VideoBuffer())
    , m_onFrame(onFrame)
{
    m_vb->init();
    connect(this, &Decoder::newFrame, this, &Decoder::onNewFrame, Qt::QueuedConnection);
    connect(m_vb, &VideoBuffer::updateFPS, this, &Decoder::updateFPS);
}

Decoder::~Decoder() {
    m_vb->deInit();
    delete m_vb;
}

bool Decoder::open()
{
    // codec
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("H.264 decoder not found");
        return false;
    }

    // codec context
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical("Could not allocate decoder context");
        return false;
    }
    // Frame threading adds one frame of latency per thread. Keep parallel
    // decoding within each frame, which improves throughput without queuing
    // future frames, and tell H.264 decoders this is a low-delay stream.
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->thread_type = FF_THREAD_SLICE;
    m_codecCtx->thread_count = 0; // let FFmpeg choose the slice worker count
    if (avcodec_open2(m_codecCtx, codec, NULL) < 0) {
        qCritical("Could not open H.264 codec");
        return false;
    }
    m_isCodecCtxOpen = true;
    return true;
}

void Decoder::close()
{
    if (m_vb) {
        m_vb->interrupt();
    }

    if (!m_codecCtx) {
        return;
    }
    if (m_isCodecCtxOpen) {
        avcodec_close(m_codecCtx);
    }
    avcodec_free_context(&m_codecCtx);
}

void Decoder::onVideoSessionChanged(const QSize &size)
{
    Q_UNUSED(size);
    if (m_codecCtx) {
        avcodec_flush_buffers(m_codecCtx);
    }
}

void Decoder::setRenderExpiredFrames(bool enabled)
{
    if (m_vb) {
        m_vb->setRenderExpiredFrames(enabled);
    }
}

bool Decoder::push(const AVPacket *packet)
{
    if (!m_codecCtx || !m_vb) {
        return false;
    }
#ifdef QTSCRCPY_LAVF_HAS_NEW_ENCODING_DECODING_API
    const auto drainFrames = [this]() {
        int decodedFrames = 0;
        for (;;) {
            // The render buffer swaps its decode and display frames after
            // every output, so fetch the current decode frame each iteration.
            AVFrame *decodingFrame = m_vb->decodingFrame();
            const int result = avcodec_receive_frame(m_codecCtx, decodingFrame);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return decodedFrames;
            }
            if (result < 0) {
                qCritical("Could not receive video frame: %d", result);
                return -1;
            }
            pushFrame();
            ++decodedFrames;
        }
    };

    int ret = avcodec_send_packet(m_codecCtx, packet);
    if (ret == AVERROR(EAGAIN)) {
        // The decoder still has output ready. Drain it before retrying this
        // packet instead of allowing decoded frames to queue behind input.
        if (drainFrames() < 0) {
            return false;
        }
        ret = avcodec_send_packet(m_codecCtx, packet);
    }
    if (ret < 0) {
        char errorbuf[255] = { 0 };
        av_strerror(ret, errorbuf, 254);
        qCritical("Could not send video packet: %s", errorbuf);
        return false;
    }
    if (drainFrames() < 0) {
        return false;
    }

#else
    AVFrame *decodingFrame = m_vb->decodingFrame();
    int gotPicture = 0;
    int len = -1;
    if (decodingFrame) {
        len = avcodec_decode_video2(m_codecCtx, decodingFrame, &gotPicture, packet);
    }
    if (len < 0) {
        qCritical("Could not decode video packet: %d", len);
        return false;
    }
    if (gotPicture) {
        pushFrame();
    }
#endif
    return true;
}

void Decoder::peekFrame(std::function<void (int, int, uint8_t *)> onFrame)
{
    if (!m_vb) {
        return;
    }
    m_vb->peekRenderedFrame(onFrame);
}

void Decoder::pushFrame()
{
    if (!m_vb) {
        return;
    }
    bool previousFrameSkipped = true;
    m_vb->offerDecodedFrame(previousFrameSkipped);
    if (previousFrameSkipped) {
        // the previous newFrame will consume this frame
        return;
    }
    emit newFrame();
}

void Decoder::onNewFrame() {
    if (!m_onFrame) {
        return;
    }

    m_vb->lock();
    const AVFrame *frame = m_vb->consumeRenderedFrame();
    m_onFrame(frame->width, frame->height, frame->data[0], frame->data[1], frame->data[2], frame->linesize[0], frame->linesize[1], frame->linesize[2]);
    m_vb->unLock();
}
