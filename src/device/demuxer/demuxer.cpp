#include <QDebug>
#include <QTime>

#include "compat.h"
#include "demuxer.h"
#include "videosocket.h"

#define HEADER_SIZE 12

#define SC_PACKET_FLAG_CONFIG    (UINT64_C(1) << 63)
#define SC_PACKET_FLAG_KEY_FRAME (UINT64_C(1) << 62)

#define SC_PACKET_PTS_MASK (SC_PACKET_FLAG_KEY_FRAME - 1)

typedef qint32 (*ReadPacketFunc)(void *, quint8 *, qint32);

Demuxer::Demuxer(QObject *parent)
    : QThread(parent)
{}

Demuxer::~Demuxer() {}

static void avLogCallback(void *avcl, int level, const char *fmt, va_list vl)
{
    Q_UNUSED(avcl)
    Q_UNUSED(vl)

    QString localFmt = QString::fromUtf8(fmt);
    localFmt.prepend("[FFmpeg] ");
    switch (level) {
    case AV_LOG_PANIC:
    case AV_LOG_FATAL:
        qFatal("%s", localFmt.toUtf8().data());
        break;
    case AV_LOG_ERROR:
        qCritical() << localFmt.toUtf8();
        break;
    case AV_LOG_WARNING:
        qWarning() << localFmt.toUtf8();
        break;
    case AV_LOG_INFO:
        qInfo() << localFmt.toUtf8();
        break;
    case AV_LOG_DEBUG:
        // qDebug() << localFmt.toUtf8();
        break;
    }

    // do not forward others, which are too verbose
    return;
}

bool Demuxer::init()
{
#ifdef QTSCRCPY_LAVF_REQUIRES_REGISTER_ALL
    av_register_all();
#endif
    if (avformat_network_init()) {
        return false;
    }
    av_log_set_callback(avLogCallback);
    return true;
}

void Demuxer::deInit()
{
    avformat_network_deinit(); // ignore failure
}

void Demuxer::installVideoSocket(VideoSocket *videoSocket)
{
    videoSocket->moveToThread(this);
    m_videoSocket = videoSocket;
}

void Demuxer::setFrameSize(const QSize &frameSize)
{
    m_frameSize = frameSize;
}

static quint32 bufferRead32be(quint8 *buf)
{
    return static_cast<quint32>((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

static quint64 bufferRead64be(quint8 *buf)
{
    quint32 msb = bufferRead32be(buf);
    quint32 lsb = bufferRead32be(&buf[4]);
    return (static_cast<quint64>(msb) << 32) | lsb;
}

qint32 Demuxer::recvData(quint8 *buf, qint32 bufSize)
{
    if (!buf || !m_videoSocket) {
        return 0;
    }

    qint32 len = m_videoSocket->subThreadRecvData(buf, bufSize);
    return len;
}

bool Demuxer::startDecode()
{
    if (!m_videoSocket) {
        return false;
    }
    start();
    return true;
}

void Demuxer::stopDecode()
{
    wait();
}

void Demuxer::run()
{
    m_codecCtx = Q_NULLPTR;
    m_parser = Q_NULLPTR;
    AVPacket *packet = Q_NULLPTR;

    // codec
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        qCritical("H.264 decoder not found");
        goto runQuit;
    }

    // codeCtx
    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical("Could not allocate codec context");
        goto runQuit;
    }
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->width = m_frameSize.width();
    m_codecCtx->height = m_frameSize.height();
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    m_parser = av_parser_init(AV_CODEC_ID_H264);
    if (!m_parser) {
        qCritical("Could not initialize parser");
        goto runQuit;
    }

    // We must only pass complete frames to av_parser_parse2()!
    // It's more complicated, but this allows to reduce the latency by 1 frame!
    m_parser->flags |= PARSER_FLAG_COMPLETE_FRAMES;

    packet = av_packet_alloc();
    if (!packet) {
        qCritical("OOM");
        goto runQuit;
    }

    for (;;) {
        bool ok = recvPacket(packet);
        if (!ok) {
            // end of stream
            break;
        }

        ok = pushPacket(packet);
        av_packet_unref(packet);
        if (!ok) {
            // cannot process packet (error already logged)
            break;
        }
    }

    qDebug("End of frames");

    if (m_pending) {
        av_packet_free(&m_pending);
    }

    av_packet_free(&packet);

    av_parser_close(m_parser);

runQuit:
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }

    if (m_videoSocket) {
        m_videoSocket->close();
        delete m_videoSocket;
        m_videoSocket = Q_NULLPTR;
    }

    emit onStreamStop();
}

bool Demuxer::recvPacket(AVPacket *packet)
{
    // The video stream contains raw packets, without time information. When we
    // record, we retrieve the timestamps separately, from a "meta" header
    // added by the server before each raw packet.
    //
    // The "meta" header length is 12 bytes:
    // [. . . . . . . .|. . . .]. . . . . . . . . . . . . . . ...
    //  <-------------> <-----> <-----------------------------...
    //        PTS        packet        raw packet
    //                    size
    //
    // It is followed by <packet_size> bytes containing the packet/frame.
    //
    // The most significant bits of the PTS are used for packet flags:
    //
    //  byte 7   byte 6   byte 5   byte 4   byte 3   byte 2   byte 1   byte 0
    // CK...... ........ ........ ........ ........ ........ ........ ........
    // ^^<------------------------------------------------------------------->
    // ||                                PTS
    // | `- config packet
    //  `-- key frame

    quint8 header[HEADER_SIZE];
    qint32 r = recvData(header, HEADER_SIZE);
    if (r < HEADER_SIZE) {
        return false;
    }

    quint64 ptsFlags = bufferRead64be(header);
    quint32 len = bufferRead32be(&header[8]);
    Q_ASSERT(len);

    if (av_new_packet(packet, static_cast<int>(len))) {
        qCritical("Could not allocate packet");
        return false;
    }

    r = recvData(packet->data, static_cast<qint32>(len));
    if (r < 0 || static_cast<quint32>(r) < len) {
        av_packet_unref(packet);
        return false;
    }

    if (ptsFlags & SC_PACKET_FLAG_CONFIG) {
        packet->pts = AV_NOPTS_VALUE;
    } else {
        packet->pts = ptsFlags & SC_PACKET_PTS_MASK;
    }

    if (ptsFlags & SC_PACKET_FLAG_KEY_FRAME) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    packet->dts = packet->pts;
    return true;
}

bool Demuxer::pushPacket(AVPacket *packet)
{
    bool isConfig = packet->pts == AV_NOPTS_VALUE;

    // A config packet must not be decoded immetiately (it contains no
    // frame); instead, it must be concatenated with the future data packet.
    if (m_pending || isConfig) {
        qint32 offset;
        if (m_pending) {
            offset = m_pending->size;
            if (av_grow_packet(m_pending, packet->size)) {
                qCritical("Could not grow packet");
                return false;
            }
        } else {
            offset = 0;
            m_pending = av_packet_alloc();
            if (av_new_packet(m_pending, packet->size)) {
                av_packet_free(&m_pending);
                qCritical("Could not create packet");
                return false;
            }
        }

        memcpy(m_pending->data + offset, packet->data, static_cast<unsigned int>(packet->size));

        if (!isConfig) {
            // prepare the concat packet to send to the decoder
            m_pending->pts = packet->pts;
            m_pending->dts = packet->dts;
            m_pending->flags = packet->flags;
            packet = m_pending;
        }
    }

    if (isConfig) {
        // config packet
        bool ok = processConfigPacket(packet);
        if (!ok) {
            return false;
        }
    } else {
        // data packet
        bool ok = parse(packet);

        if (m_pending) {
            // the pending packet must be discarded (consumed or error)
            av_packet_free(&m_pending);
        }

        if (!ok) {
            return false;
        }
    }
    return true;
}

bool Demuxer::processConfigPacket(AVPacket *packet)
{
    emit getConfigFrame(packet);
    return true;
}

bool Demuxer::parse(AVPacket *packet)
{
    quint8 *inData = packet->data;
    int inLen = packet->size;
    quint8 *outData = Q_NULLPTR;
    int outLen = 0;
    int r = av_parser_parse2(m_parser, m_codecCtx, &outData, &outLen, inData, inLen, AV_NOPTS_VALUE, AV_NOPTS_VALUE, -1);

    // PARSER_FLAG_COMPLETE_FRAMES is set
    Q_ASSERT(r == inLen);
    (void)r;
    Q_ASSERT(outLen == inLen);

    if (m_parser->key_frame == 1) {
        packet->flags |= AV_PKT_FLAG_KEY;
    }

    bool ok = processFrame(packet);
    if (!ok) {
        qCritical("Could not process frame");
        return false;
    }

    return true;
}

bool Demuxer::processFrame(AVPacket *packet)
{
    packet->dts = packet->pts;
    emit getFrame(packet);
    return true;
}
